#!/usr/bin/env python3
"""Dev server for credentialed-fetch testing.

Serves testing/fixtures/ statically (so a fixture page can load a real CDN library
and call a local API) AND exposes a cookie-auth JSON API under /api/. The API is
GET-only and mutation-free on purpose: a well-designed server never changes state
on GET, so the analyzer can replay a learned GET endpoint credentialed (with the
session cookie) to fetch the REAL authenticated reply without risking an account
action. POST/PUT/DELETE are rejected (405) — those the analyzer must only RECORD,
never issue.

  GET  /api/<x>   -> 200 {authed:true, user:{...}, next:"/api/.."} when the session
                     cookie is present (the logged-in reply), else 401 {authed:false}.
  any response    -> Set-Cookie: sid=test  (so a same-origin API GET is authed).

Run:  python testing/devserver.py [port]   (default 8777)
"""
import json, os, sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

FIX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=FIX, **k)

    def log_message(self, *a):
        pass

    def _cors(self):
        o = self.headers.get("Origin")
        if o:
            self.send_header("Access-Control-Allow-Origin", o)
            self.send_header("Access-Control-Allow-Credentials", "true")

    def end_headers(self):
        # Hand the browser a session cookie on EVERY response so a later API GET
        # (same-origin from the fixture, or the analyzer's credentialed replay)
        # carries it and gets the authenticated reply.
        self.send_header("Set-Cookie", "sid=test; Path=/; SameSite=Lax")
        super().end_headers()

    def do_GET(self):
        if self.path.startswith("/api/"):
            authed = "sid=test" in (self.headers.get("Cookie") or "")
            self.send_response(200 if authed else 401)
            self.send_header("Content-Type", "application/json")
            self._cors()
            self.end_headers()
            self.wfile.write(json.dumps({
                "authed": authed,
                "path": self.path,
                "user": {"id": 7, "name": "alice", "role": "admin"} if authed else None,
                "next": "/api/profile/7" if authed else None,
            }).encode())
            return
        super().do_GET()

    def do_POST(self):
        # GET-only surface: never mutate. The analyzer must RECORD, not issue, these.
        self.send_response(405)
        self._cors()
        self.end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8777
    print("devserver on http://127.0.0.1:%d (fixtures + cookie-auth /api GET)" % port, flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
