#!/usr/bin/env python3
"""range_server.py - a directory HTTP server that honours `Range: bytes=N-` (206), which
python -m http.server does not. Used by tools/test_hdtex.sh to exercise resumed downloads.
    python tools/range_server.py <dir> <port> [--no-range]
--no-range answers every request with the whole file (200), like a server without Range
support, to test the start-over path."""
import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    no_range = False

    def do_GET(self):
        path = self.translate_path(self.path)
        rng = self.headers.get("Range")
        if self.no_range or not rng or not os.path.isfile(path) or not rng.startswith("bytes="):
            return super().do_GET()
        size = os.path.getsize(path)
        start = int(rng[6:].split("-")[0])
        if start >= size:
            self.send_response(416)
            self.end_headers()
            return
        self.send_response(206)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, size - 1, size))
        self.send_header("Content-Length", str(size - start))
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(start)
            while True:
                chunk = f.read(1 << 16)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def log_message(self, *a):
        pass


def main():
    d, port = sys.argv[1], int(sys.argv[2])
    Handler.no_range = "--no-range" in sys.argv
    os.chdir(d)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
