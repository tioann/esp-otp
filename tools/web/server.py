#!/usr/bin/env python3
"""Tiny static web server for the esp-otp browser console (tools/web/index.html).

Serves the page over HTTP from http://localhost:<port>/ — a "secure context",
which the Web Serial and Web Bluetooth APIs require. The page talks to a real
device directly (Web Serial over the C3's USB-Serial/JTAG, or Web Bluetooth over
the management GATT service); this server only hands out the static files, so any
static server over https/localhost works just as well.

    tools/web/server.py          # serve on http://localhost:8000/
    # open http://localhost:8000/ , pick a transport, Connect

No third-party dependencies (stdlib only).
"""
import argparse
import os
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class Handler(SimpleHTTPRequestHandler):
    # Quieter logs: skip the default per-request line.
    def log_message(self, fmt, *args):
        pass


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="esp-otp web console static server")
    ap.add_argument("--http-port", type=int, default=8000, help="HTTP listen port")
    ap.add_argument("--dir", default=here, help="directory of static files to serve")
    args = ap.parse_args()

    handler = partial(Handler, directory=args.dir)
    httpd = ThreadingHTTPServer(("127.0.0.1", args.http_port), handler)
    print(f"serving {args.dir} on http://localhost:{args.http_port}/")
    print("  open the page and pick Device (Web Serial) or Device (Web Bluetooth)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
