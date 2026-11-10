#!/usr/bin/env python3
"""Serves the built browser lab on localhost with the cross-origin isolation headers.

Usage: web/lab/serve.py [--port 8642] [--dir build/web-lab/site]

COOP same-origin plus COEP require-corp make the page cross-origin isolated, which SharedArrayBuffer
and wasm threads need; localhost counts as a secure context, so WebGPU, OPFS and WebHID work without
TLS. The hosted build must send the same two headers.
"""
import argparse
import functools
import http.server
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".json": "application/json",
        ".bin": "application/octet-stream",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        if "chunk-" not in (args[0] if args else ""):
            super().log_message(fmt, *args)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8642)
    parser.add_argument("--dir", default=str(ROOT / "build" / "web-lab" / "site"))
    args = parser.parse_args()
    site = Path(args.dir)
    if not (site / "index.html").exists():
        raise SystemExit(f"{site} has no index.html; run web/lab/build.sh first")
    handler = functools.partial(Handler, directory=str(site))
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"Browser lab: http://localhost:{args.port}/  (serving {site})", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
