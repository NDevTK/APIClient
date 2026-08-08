"""WPT'S OWN SERVER, launched against the pinned corpus.

WHY. A test's `.py` path is a wptserve HANDLER, not a file: the server imports it and calls its `main`, which is
how a test that uploads a form reads back what the server received. engine/wpt.mjs used to serve the corpus off
disk, so those files compared their upload against a Python source file and reported the ENGINE as wrong; then
they were reported as a gate limitation, which was honest about the number but wrong about the cause — the
handlers are right here and their dependencies are vendored in tools/third_party. So the server runs.

It is wptserve's OWN handlers, not a reimplementation. The routes below are the ones tools/serve/serve.py
installs for plain HTTP; using them rather than writing a file server means the corpus is served exactly as it
is written to be served — including the rewrites (/resources/WebIDLParser.js is the webidl2 library under its
historical name) that engine/wpt.mjs previously carried as a hand-copied table.

Usage:  python3 engine/wptserve.py <wpt-root> <port>
It prints `READY <port>` on stdout once bound, and serves until killed.
"""
import sys
import os

root = os.path.abspath(sys.argv[1])
port = int(sys.argv[2])
if port == 0:
    # THE PORT HAS TO BE KNOWN BEFORE THE CONFIG IS BUILT, not after the server binds. A `.sub.html` substitutes
    # `{{ports[http][0]}}` into the origins it then fetches, so a config carrying 0 while the socket carried an
    # ephemeral port would hand every such test an address that answers nothing. Reserve one here and pass the
    # same number to both.
    import socket
    _s = socket.socket()
    _s.bind(("127.0.0.1", 0))
    port = _s.getsockname()[1]
    _s.close()

sys.path.insert(0, os.path.join(root, "tools"))
import localpaths  # noqa: F401  — puts tools/third_party/* on the path, which is how wpt itself does it

from wptserve import server, handlers, routes as default_routes, config as wptconfig

# The routes tools/serve/serve.py builds for a plain HTTP virtual host, in its order: the rewrites and special
# paths it declares, then the python-script handler for `.py`, then the file handler for everything else.
routes = default_routes.routes

# THE HOST NAMES THE CORPUS IS WRITTEN AGAINST. A `.sub.html` is a TEMPLATE: wptserve's `sub` pipe replaces
# `{{host}}`, `{{hosts[alt][]}}` and `{{ports[http][0]}}` with the names and ports the run is using, which is
# how a cross-origin test names an origin it can actually reach. Served with wptserve's DEFAULT config those
# substitutions raise — the default browser_host is "localhost" and there are no alternate hosts at all — so
# every .sub.html answered 500 and the runner saw a file the server would not serve. These are the names
# tools/serve/serve.py declares, which is what the corpus was written for; a second authority on the same
# loopback port is exactly what a hosts-file mapping gives a real browser.
class _Log:
    def _n(self, *a, **k): pass
    debug = info = warning = error = critical = _n

_cfg = wptconfig.ConfigBuilder(_Log(),
                               browser_host="web-platform.test",
                               alternate_hosts={"alt": "not-web-platform.test"},
                               doc_root=root,
                               ports={"http": [port]},
                               check_subdomains=False,
                               subdomains={"www", "www1", "www2", "天気の良い日", "élève"},
                               not_subdomains={"nonexistent"})

with _cfg as cfg:
    httpd = server.WebTestHttpd(host="127.0.0.1", port=port, doc_root=root, routes=routes,
                                config=cfg, use_ssl=False, key_file=None, certificate=None)
    httpd.start()
    sys.stdout.write("READY %d\n" % httpd.port)
    sys.stdout.flush()
# WebTestHttpd.start() runs the server on its own thread and returns, so this one blocks until it is killed.
# There is no join to call — `start`, `get_url` and `stop` are the whole of its surface.
    try:
        while True:
            __import__("time").sleep(3600)
    except KeyboardInterrupt:
        pass
