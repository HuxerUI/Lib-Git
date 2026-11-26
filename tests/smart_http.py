"""Run the integration fixtures against loopback Git Smart HTTP and HTTPS remotes.

The first receive-pack request reaches Git and updates the ref. The server drops
its response so the client has to inspect the remote to learn what happened.
The HTTPS fixture redirects to a different host name, then checks known
accounts and presents account-specific branches.
"""

import base64
import http.server
import os
import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import urllib.parse


def receive_body(handler):
    if handler.headers.get("Transfer-Encoding", "").lower() != "chunked":
        return handler.rfile.read(int(handler.headers.get("Content-Length", "0")))
    result = bytearray()
    while True:
        header = handler.rfile.readline()
        length = int(header.split(b";", 1)[0].strip(), 16)
        if length == 0:
            while handler.rfile.readline() not in (b"\r\n", b"\n", b""):
                pass
            return bytes(result)
        result.extend(handler.rfile.read(length))
        handler.rfile.read(2)


def run(executable, git, openssl=None):
    with tempfile.TemporaryDirectory(prefix="huxerui-git-http-") as directory:
        root = pathlib.Path(directory)
        repository = root / "remote.git"
        subprocess.run([git, "init", "--bare", "--initial-branch=main", repository], check=True)
        subprocess.run([git, "-C", repository, "config", "http.receivepack", "true"], check=True)
        accounts = {
            "publisher": "publisher-token",
            "clone": "clone-token",
            "personal": "personal-token",
            "work": "work-token",
            "manual": "manual-token",
        }
        authorized = {
            "Basic " + base64.b64encode(f"{name}:{token}".encode()).decode(): name
            for name, token in accounts.items()
        }

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def do_GET(self):
                self.dispatch()

            def do_POST(self):
                self.dispatch()

            def dispatch(self):
                request = urllib.parse.urlsplit(self.path)
                body = receive_body(self)
                path = request.path
                username = "fixture"
                if openssl:
                    if self.headers.get("Host", "").startswith("localhost:"):
                        if self.headers.get("Authorization"):
                            self.server.redirect_received_credentials = True
                        self.send_response(307)
                        self.send_header("Location", f"https://127.0.0.1:{self.server.server_port}{self.path}")
                        self.send_header("Content-Length", "0")
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.close_connection = True
                        return
                    username = authorized.get(self.headers.get("Authorization"))
                    push_endpoint = path.startswith("/push.git/")
                    if not username or (username == "publisher") != push_endpoint:
                        self.send_response(401)
                        self.send_header("WWW-Authenticate", 'Basic realm="fixture"')
                        self.send_header("Content-Length", "0")
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.close_connection = True
                        return
                    self.server.accounts_seen.add(username)
                    if push_endpoint:
                        path = "/remote.git" + path[len("/push.git"):]
                env = dict(os.environ, GIT_PROJECT_ROOT=directory, GIT_HTTP_EXPORT_ALL="1",
                           REQUEST_METHOD=self.command, PATH_INFO=path, QUERY_STRING=request.query,
                           CONTENT_TYPE=self.headers.get("Content-Type", ""), CONTENT_LENGTH=str(len(body)),
                           REMOTE_USER=username)
                protocol = self.headers.get("Git-Protocol")
                if protocol:
                    env["HTTP_GIT_PROTOCOL"] = protocol
                if openssl:
                    hidden = ["refs/heads/account-personal", "refs/heads/account-work"]
                    selected = f"refs/heads/account-{username}"
                    hidden = [ref for ref in hidden if ref != selected]
                    env["GIT_CONFIG_COUNT"] = str(len(hidden))
                    for index, ref in enumerate(hidden):
                        env[f"GIT_CONFIG_KEY_{index}"] = "uploadpack.hideRefs"
                        env[f"GIT_CONFIG_VALUE_{index}"] = ref
                result = subprocess.run([git, "http-backend"], input=body, capture_output=True, env=env, check=True)
                if openssl and path.endswith("/git-receive-pack"):
                    for account in ("personal", "work"):
                        subprocess.run([git, "-C", repository, "update-ref",
                                        f"refs/heads/account-{account}", "refs/heads/main"], check=True)
                if not openssl and path.endswith("/git-receive-pack"):
                    self.server.dropped_response = True
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return

                header, boundary, payload = result.stdout.partition(b"\r\n\r\n")
                if not boundary:
                    header, boundary, payload = result.stdout.partition(b"\n\n")
                headers = dict(line.decode("latin-1").split(": ", 1) for line in header.splitlines())
                status = int(headers.pop("Status", "200").split()[0])
                self.send_response(status)
                for name, value in headers.items():
                    if name.lower() not in ("connection", "content-length"):
                        self.send_header(name, value)
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(payload)
                self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.dropped_response = False
        server.redirect_received_credentials = False
        server.accounts_seen = set()
        if openssl:
            cert = root / "ca.pem"
            key = root / "key.pem"
            subprocess.run(
                [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                 "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
                 "-keyout", key, "-out", cert],
                check=True, capture_output=True,
            )
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(cert, key)
            server.socket = tls.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            if openssl:
                base = f"https://127.0.0.1:{server.server_port}"
                redirect = f"https://localhost:{server.server_port}"
                run_result = subprocess.run(
                    [executable, "--credentials", base, redirect, cert], timeout=50,
                )
                if run_result.returncode == 77:
                    return 77
                run_result.check_returncode()
                if server.redirect_received_credentials:
                    raise RuntimeError("The redirect source received credentials for the target")
                if server.accounts_seen != set(accounts):
                    raise RuntimeError("The server did not receive every expected account")
            else:
                subprocess.run([executable, f"http://127.0.0.1:{server.server_port}/remote.git"], check=True, timeout=50)
                if not server.dropped_response:
                    raise RuntimeError("No receive-pack response was dropped")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    sys.exit(run(*sys.argv[1:]))
