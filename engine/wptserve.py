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
import signal
import socket
import tempfile
import time
import uuid

root = os.path.abspath(sys.argv[1])
port = int(sys.argv[2])
if port == 0:
    # THE PORT HAS TO BE KNOWN BEFORE THE CONFIG IS BUILT, not after the server binds. A `.sub.html` substitutes
    # `{{ports[http][0]}}` into the origins it then fetches, so a config carrying 0 while the socket carried an
    # ephemeral port would hand every such test an address that answers nothing. Reserve one here and pass the
    # same number to both.
    _s = socket.socket()
    _s.bind(("127.0.0.1", 0))
    port = _s.getsockname()[1]
    _s.close()

# THE CORPUS IS WRITTEN AGAINST FOUR PORTS, not one. common/get-host-info.sub.js — which most cross-origin
# tests in html/browsers name in their META block — substitutes ports[http][0], ports[http][1], ports[https][0]
# and ports[https][1]. A config declaring ONE http port does not merely give a wrong answer for the others:
# wptserve's `sub` pipe indexes the list and raises IndexError, so the SERVER answers 500 for the helper
# itself. Every test that merely imports it then runs with `get_host_info()` undefined or with literal
# `{{hosts[alt][]}}` text in its origins, which is why cross-origin `window.open` reported "the URL to open is
# not a URL" and every test built on one timed out. One missing port in a config silenced a family of tests.
_extra = []
for _ in range(3):
    _s = socket.socket()
    _s.bind(("127.0.0.1", 0))
    _extra.append(_s.getsockname()[1])
    _s.close()

sys.path.insert(0, os.path.join(root, "tools"))
import localpaths  # noqa: F401  — puts tools/third_party/* on the path, which is how wpt itself does it

from wptserve import server, handlers, stash, routes as default_routes, config as wptconfig

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
                               # THE SECOND HTTP PORT IS SERVED; the two https ports are DECLARED and not
                               # served, and that difference is deliberate. A declared-but-unserved port makes
                               # a test that actually fetches https fail to connect — one honest failure in the
                               # test that reaches for it. Leaving it out of the config instead makes the
                               # shared helper 500, which reports nothing for every test that imports it,
                               # including the ones that never touch https. Serving them needs TLS and a
                               # certificate, which is the next thing to build here.
                               ports={"http": [port, _extra[0]], "https": [_extra[1], _extra[2]]},
                               # THE HTTPS PORTS ONLY SURVIVE THE CONFIG IF SSL IS CONFIGURED. ConfigBuilder
                               # PRUNES a scheme it has no certificate for, so `ports={"https": [...]}` alone
                               # left the list empty and `{{ports[https][0]}}` raised IndexError exactly as a
                               # missing port did — the config accepted the argument and dropped it. Naming
                               # the openssl generator keeps them, and the binary is present.
                               ssl={"type": "openssl",
                                    "openssl": {"openssl_binary": "openssl",
                                                "base_path": os.path.join(tempfile.mkdtemp(), "certs"),
                                                "force_regenerate": False,
                                                "duration": 30,
                                                "base_conf_path": None}},
                               check_subdomains=False,
                               subdomains={"www", "www1", "www2", "天気の良い日", "élève"},
                               not_subdomains={"nonexistent"})

# THE STASH IS A SERVER, AND WITHOUT IT A HANDLER THAT USES ONE ANSWERS 500. `request.server.stash` reads its
# address out of WPT_STASH_CONFIG, which nothing but tools/serve/serve.py's `with stash.StashServer(...)` ever
# sets — so every handler that persists a value between two requests raised KeyError inside wptserve and the
# corpus got a 500 whose body was a Python traceback. That is the same defect as serving a `.py` handler's
# SOURCE: the test compares its answer against something that is not the server's answer and reports the ENGINE
# as wrong. `xhr/resources/authentication.py` is the worked example — its whole subject is the 401 challenge it
# only issues once per token, which it remembers in the stash — so thirteen `send-authentication-*` files were
# measured against a traceback. serve.py's own two lines, with its own address-and-authkey shape.
_stash_port = socket.socket()
_stash_port.bind(("127.0.0.1", 0))
_stash_addr = ("127.0.0.1", _stash_port.getsockname()[1])
_stash_port.close()

with stash.StashServer(_stash_addr, authkey=str(uuid.uuid4())), _cfg as cfg:
    httpd = server.WebTestHttpd(host="127.0.0.1", port=port, doc_root=root, routes=routes,
                                config=cfg, use_ssl=False, key_file=None, certificate=None)
    httpd.start()
    # THE SECOND HTTP PORT ANSWERS TOO. `{{ports[http][1]}}` is how the corpus names "same host, different
    # origin"; a port the config declares and nothing listens on would hand those tests an address that
    # answers nothing — the same defect the reservation above exists to avoid, one index along.
    httpd2 = server.WebTestHttpd(host="127.0.0.1", port=_extra[0], doc_root=root, routes=routes,
                                 config=cfg, use_ssl=False, key_file=None, certificate=None)
    httpd2.start()
    sys.stdout.write("READY %d\n" % httpd.port)
    sys.stdout.flush()
# WebTestHttpd.start() runs the server on its own thread and returns, so this one blocks until it is killed.
# There is no join to call — `start`, `get_url` and `stop` are the whole of its surface.
    # AND SIGTERM UNWINDS IT, because the stash is a CHILD PROCESS now and the driver stops this server by
    # killing it. Python's default SIGTERM disposition dies without running a single `__exit__`, so the manager
    # the StashServer forked would outlive every gate run and accumulate one orphan per run on a shared box.
    # Raising SystemExit from the handler runs the `with` blocks' exits, and `manager.shutdown()` with them.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
