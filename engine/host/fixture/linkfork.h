/* THE LINK-FORK DOCUMENT: a module worker links two module graphs while the page forks the flow beside it. The
   page builds two chains of 2000 Blob URL module scripts, each importing the next (fixture/importchain.h's shape):
   `lfOk`, and `lfBad`, whose deepest module imports a binding its dependency does not export. The worker imports
   the first and then the second and posts each outcome; the page reads the URL's fragment against one literal
   per task, an attacker source, so each test forks the flow, the worker agent's state included
   (flow_agents_fork). Every arm links the first chain to its full depth, and the second link's SyntaxError takes
   ECMAScript §16.2.1.6.1.2 Link ( ) step 4 in that arm's own world.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_LINKFORK_H
#define ENGINE_HOST_FIXTURE_LINKFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LINKFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "function lfChain(n, base) {"
    "  var u = URL.createObjectURL(new Blob([base], { type: 'text/javascript' }));"
    "  for (var i = 1; i < n; i++)"
    "    u = URL.createObjectURL(new Blob(['import { depth as d } from \"' + u + '\"; export var depth = d + 1;'],"
    "                                     { type: 'text/javascript' }));"
    "  return u;"
    "}"
    "var lfZ = URL.createObjectURL(new Blob(['export var z = 0;'], { type: 'text/javascript' }));"
    "var lfOk = lfChain(2000, 'export var depth = 0;');"
    "var lfBad = lfChain(2000, 'import { nosuch } from \"' + lfZ + '\"; export var depth = nosuch;');"
    "var lfW = new Worker(URL.createObjectURL(new Blob(["
    "  'self.onmessage = function (e) {'"
    "  + ' import(e.data.ok).then(function (m) {'"
    "  + '  self.postMessage(\"ok=\" + (m.depth + 1));'"
    "  + '  return import(e.data.bad);'"
    "  + ' }).then(function () { self.postMessage(\"bad=linked\"); },'"
    "  + '         function (x) { self.postMessage(\"bad=\" + x.name); });'"
    "  + '};'"
    "], { type: 'text/javascript' })), { type: 'module' });"
    "var lfSeen = [];"
    "lfW.onmessage = function (e) {"
    "  lfSeen.push(e.data);"
    "  if (lfSeen.length === 2) fetch('/api/lkfork?' + lfSeen.join('&'));"
    "};"
    "lfW.postMessage({ ok: lfOk, bad: lfBad });"
    "var lfT = 0;"
    "function lfTick() {"
    "  if (lfSeen.length === 2 || lfT >= 40) return;"
    "  if (location.hash === '#t' + lfT) fetch('/api/lfarm?t=' + lfT);"
    "  lfT++;"
    "  setTimeout(lfTick, 0);"
    "}"
    "lfTick();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
