#!/usr/bin/env python3
"""Serves a built site (website/_site) on localhost for preview and flashing.

    serve.py [--port 8137]

Web Serial works on http://localhost, so the flasher can be used from here.
Build the site first: build_site.py --out website/_site
"""

import argparse
import functools
import http.server
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, "_site")

# Explicit types: on Windows, http.server takes them from the registry, where
# .js is not always a JavaScript type, and browsers refuse such ES modules.
TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".jpg": "image/jpeg",
    ".png": "image/png",
    ".bin": "application/octet-stream",
    ".txt": "text/plain; charset=utf-8",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    def guess_type(self, path):
        ext = os.path.splitext(path)[1].lower()
        return TYPES.get(ext) or super().guess_type(path)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    ap = argparse.ArgumentParser(description="Serve website/_site on localhost.")
    ap.add_argument("--port", type=int, default=8137)
    args = ap.parse_args()
    if not os.path.isfile(os.path.join(SITE, "index.html")):
        print("No built site in %s. Run build_site.py --out website/_site first." % SITE, file=sys.stderr)
        return 1
    handler = functools.partial(Handler, directory=SITE)
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print("Serving %s at http://localhost:%d/  (Ctrl+C stops)" % (SITE, args.port), flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
