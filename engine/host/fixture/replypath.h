/* THE REPLYPATH DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). A network answer lands
   at its due point on the asking flow's own path (solver/engine.h's engine_reply_clock), so where a flow takes
   it does not depend on what its siblings run or when the host paid. The page forks three arms on the fragment:
   `fast` and `late` run the same measurement (a fetch beside a chain of message tasks, counting the tasks run
   before the reply's reaction), and `busy` burns work and then asks for /api/rpathbusy. test_forced.c's provider
   answers the `fast` request at once and holds the `late` one until /api/rpathbusy is answered, so the two
   measurements differ only in their sibling and their payment. Tags are one length, so the two paths retire
   the same work, and each chain stops at 400 tasks, so a driven handler ends too. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REPLYPATH_H
#define ENGINE_HOST_FIXTURE_REPLYPATH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REPLYPATH[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "function rpBurn(k) { for (var i = 0; i < 20000; i++) k += i; return k; }"
    "function rpMeasure(tag) {"
    " var n = 0, got = false, mc = new MessageChannel();"
    " mc.port1.onmessage = function () { if (got || n === 400) return; n++; rpBurn(0); mc.port2.postMessage(0); };"
    " fetch('/api/rpath' + tag).then(function () { got = true;"
    "  fetch((tag === 'fast' ? '/api/rpathnfast' : '/api/rpathnlate') + '?n=' + n); });"
    " mc.port2.postMessage(0);"
    "}"
    "function rpBusy() {"
    " var k = 0, mc = new MessageChannel();"
    " mc.port1.onmessage = function () { rpBurn(0); if (++k < 60) mc.port2.postMessage(0);"
    "  else fetch('/api/rpathbusy'); };"
    " mc.port2.postMessage(0);"
    "}"
    "var rpHash = location.hash;"
    "if (rpHash === '#busy') rpBusy(); else if (rpHash === '#late') rpMeasure('late'); else rpMeasure('fast');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
