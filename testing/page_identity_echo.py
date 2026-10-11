"""Header-echo server for testing/page_identity_probe.cjs, on three loopback ports.

A = http://localhost:PA is the page's origin; its /page sends `Referrer-Policy: unsafe-url` and a
`connect-src` naming A and B, and registers a service worker that logs every /echo fetch event to /swlog.
B = http://127.0.0.1:PB is cross-site and CSP-allowed; C = http://127.0.0.1:PC is cross-site and outside
the page's connect-src. B and C share a host, so they share cookies (cookies do not isolate by port).
/setcookies sets None/Lax/Strict/unspecified cookies on its host; /setpart sets a Partitioned one.
/echo records the request's raw header list in arrival order; /log returns every record, /clear empties it.

Run:  python3 -I testing/page_identity_echo.py 18431 18432 18433
"""
import http.server, json, sys, threading, urllib.parse

PA, PB, PC = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
LOG = []
LOCK = threading.Lock()

PAGE = """<!doctype html><meta charset=utf-8><title>relay probe</title>
<script>
window.swReady = (async () => {
  const reg = await navigator.serviceWorker.register('/sw.js', {scope: '/'});
  await navigator.serviceWorker.ready;
  if (!navigator.serviceWorker.controller)
    await new Promise(r => navigator.serviceWorker.addEventListener('controllerchange', r, {once: true}));
  return !!navigator.serviceWorker.controller;
})();
window.pageFetch = async (url, init) => {
  try { const r = await fetch(url, init); return {status: r.status}; }
  catch (e) { return {error: String(e)}; }
};
window.pageXhr = (url, withCredentials) => new Promise(res => {
  const x = new XMLHttpRequest(); x.open('GET', url); x.withCredentials = withCredentials;
  x.onload = () => res({status: x.status}); x.onerror = () => res({error: 'xhr error'}); x.send();
});
</script><p>probe page</p>"""

SW = """self.addEventListener('install', e => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', e => {
  const u = new URL(e.request.url);
  if (!u.pathname.startsWith('/echo')) return;
  const q = new URLSearchParams({id: u.searchParams.get('id') || '', mode: e.request.mode,
    cred: e.request.credentials, dest: e.request.destination, ref: e.request.referrer,
    refpol: e.request.referrerPolicy, client: e.clientId || ''});
  e.waitUntil(fetch('/swlog?' + q.toString()));
});"""


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def _cors(self):
        o = self.headers.get("Origin")
        if not o:
            return []
        return [("Access-Control-Allow-Origin", o), ("Access-Control-Allow-Credentials", "true"), ("Vary", "Origin")]

    def _record(self, kind):
        u = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        with LOCK:
            LOG.append({"kind": kind, "port": self.server.server_port, "method": self.command,
                        "path": self.path, "id": q.get("id", ""), "q": q,
                        "headers": [[k, v] for k, v in self.headers.items()]})

    def do_OPTIONS(self):
        self._record("preflight")
        acrh = self.headers.get("Access-Control-Request-Headers", "")
        self._send(204, b"", self._cors() + [("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE"),
                                             ("Access-Control-Allow-Headers", acrh)])

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)
        self.do_GET()

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        p = u.path
        tag = {PA: "A", PB: "B", PC: "C"}[self.server.server_port]
        if p == "/page":
            self._send(200, PAGE.encode(), [("Content-Type", "text/html"),
                                            ("Referrer-Policy", "unsafe-url"),
                                            ("Content-Security-Policy",
                                             "connect-src 'self' http://127.0.0.1:%d" % PB)])
        elif p == "/sw.js":
            self._send(200, SW.encode(), [("Content-Type", "application/javascript")])
        elif p == "/setcookies":
            hs = [("Content-Type", "text/plain"),
                  ("Set-Cookie", tag + "none=1; SameSite=None; Secure; Path=/"),
                  ("Set-Cookie", tag + "lax=1; SameSite=Lax; Path=/"),
                  ("Set-Cookie", tag + "strict=1; SameSite=Strict; Path=/"),
                  ("Set-Cookie", tag + "def=1; Path=/")]
            self._send(200, b"set", hs)
        elif p == "/setpart":
            self._record("setpart")
            self._send(200, b"part", self._cors() + [("Content-Type", "text/plain"),
                       ("Set-Cookie", tag + "part=1; SameSite=None; Secure; Partitioned; Path=/")])
        elif p == "/echo":
            self._record("echo")
            self._send(200, b'{"ok":true}', self._cors() + [("Content-Type", "application/json")])
        elif p == "/swlog":
            self._record("swlog")
            self._send(204)
        elif p == "/log":
            with LOCK:
                b = json.dumps(LOG).encode()
            self._send(200, b, [("Content-Type", "application/json")])
        elif p == "/clear":
            with LOCK:
                LOG.clear()
            self._send(204)
        else:
            self._send(404, b"nf")


for port in (PA, PB, PC):
    s = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
    threading.Thread(target=s.serve_forever, daemon=True).start()
print("serving", PA, PB, PC, flush=True)
threading.Event().wait()
