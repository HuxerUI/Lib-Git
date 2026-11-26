"""Run the repository fixture against a loopback Git Smart HTTP remote.

The first receive-pack request reaches Git and updates the ref. The server drops
its response so the client has to inspect the remote to learn what happened.
"""

import http.server
import os
import pathlib
import socket
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


def run(executable, git):
    with tempfile.TemporaryDirectory(prefix="huxerui-git-http-") as directory:
        root = pathlib.Path(directory)
        repository = root / "remote.git"
        subprocess.run([git, "init", "--bare", "--initial-branch=main", repository], check=True)
        subprocess.run([git, "-C", repository, "config", "http.receivepack", "true"], check=True)

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
                env = dict(os.environ, GIT_PROJECT_ROOT=directory, GIT_HTTP_EXPORT_ALL="1",
                           REQUEST_METHOD=self.command, PATH_INFO=request.path, QUERY_STRING=request.query,
                           CONTENT_TYPE=self.headers.get("Content-Type", ""), CONTENT_LENGTH=str(len(body)),
                           REMOTE_USER="fixture")
                protocol = self.headers.get("Git-Protocol")
                if protocol:
                    env["HTTP_GIT_PROTOCOL"] = protocol
                result = subprocess.run([git, "http-backend"], input=body, capture_output=True, env=env, check=True)
                if request.path.endswith("/git-receive-pack"):
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
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            subprocess.run([executable, f"http://127.0.0.1:{server.server_port}/remote.git"], check=True, timeout=50)
            if not server.dropped_response:
                raise RuntimeError("No receive-pack response was dropped")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    run(*sys.argv[1:])
