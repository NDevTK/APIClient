/* THE TWO-MESSAGE DOCUMENT: one handler whose sink is reached only on a second routed message, after the first
   set the state that opens it. One of test_forced.c's exploring documents (its TF_DOCS table). A cross-origin
   peer posts twice once the page asks `/api/xdmready` (test_forced.c's fixture_route_gadget_post).
   The first delivery's `e.data === 'xdmstep'` arm sets `_xdmOk`; the second delivery's `e.data` then reaches
   markup and, first, a `d` parameter. The two deliveries are two attacker inputs, so a pin made on the first
   says nothing about the second: `d` must never read as the first delivery's pinned `xdmstep`. Each arm emits a
   constant `v` to `/api/xdmgate`, the reachability witness its row is read beside. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_XDMULTI_H
#define ENGINE_HOST_FIXTURE_XDMULTI_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_XDMULTI[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var _xdmSinkEl = document.createElement('div'); document.body.appendChild(_xdmSinkEl);"
    "var _xdmOk = false;"
    "window.addEventListener('message', function(e){"
    " if (!_xdmOk) { if (e.data === 'xdmstep') { _xdmOk = true; fetch('/api/xdmgate?v=xdmSet'); } return; }"
    " fetch('/api/xdmgate?v=xdmSecond&d=' + e.data); _xdmSinkEl.innerHTML = e.data; });"
    "fetch('/api/xdmready');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
