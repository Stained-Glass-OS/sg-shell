#!/usr/bin/env python3
# store-httpd.py PORT: store-check's web server -- python3 -m http.server's, from the current folder, except
# that a path holding /Throttled/ is answered 429 (Retry-After: 1) as many times as the file ./throttle
# says (each 429 counts it down), as GitHub answers a busy address
import http.server, os, sys

class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if '/Throttled/' in self.path:
            try: n = int(open('throttle').read().strip() or 0)
            except (OSError, ValueError): n = 0
            if n > 0:
                open('throttle', 'w').write(str(n - 1))
                self.send_response(429); self.send_header('Retry-After', '1'); self.send_header('Content-Length', '0'); self.end_headers()
                return
        super().do_GET()

http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
