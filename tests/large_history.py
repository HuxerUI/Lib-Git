"""Generate a local 100,000-commit fixture and exercise the preview's history viewport."""

import pathlib
import subprocess
import sys
import tempfile


def run(executable, git):
    with tempfile.TemporaryDirectory(prefix="huxerui-git-history-") as folder:
        repository = pathlib.Path(folder)
        subprocess.run([git, "init", "--initial-branch=main", repository], check=True)
        with subprocess.Popen([git, "-C", repository, "fast-import", "--quiet"], stdin=subprocess.PIPE) as ingest:
            for n in range(1, 100001):
                subject = "Large history tip" if n == 100000 else f"Commit {n}"
                message = subject.encode()
                header = (f"commit refs/heads/main\nmark :{n}\n"
                          f"committer Fixture <fixture@example.test> {1700000000 + n} +0000\n"
                          f"data {len(message)}\n").encode()
                ingest.stdin.write(header + message + b"\n")
                if n > 1:
                    ingest.stdin.write(f"from :{n - 1}\n".encode())
                ingest.stdin.write(b"\n")
            ingest.stdin.close()
            if ingest.wait(timeout=45):
                raise RuntimeError("Could not create large history fixture")
        subprocess.run([executable, "--large-history", repository], check=True, timeout=25)


if __name__ == "__main__":
    run(*sys.argv[1:])
