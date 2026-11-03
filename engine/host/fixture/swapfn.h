/* THE FUNCTION-REPLACEMENT DOCUMENT: one click handler replaces the helper another click handler calls, so the
   markup sink exists only in a world where the replacing drive precedes the calling one. Two spellings of the
   helper: a script-level binding `render` reassigned from handler A, and a method `ui.render` on an object
   reachable only through an IIFE's closure, reassigned from that closure's own handler. Each calling handler
   reads the fragment through a different member (`slice` and `substring`), so the two writes are two @S
   records. Without the replacement each helper writes textContent and reaches no sink; that is the control.
   Every witness fetch carries constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SWAPFN_H
#define ENGINE_HOST_FIXTURE_SWAPFN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SWAPFN[] =
    "<!doctype html><html><body>"
    "<button id='swSwap'>swap</button><button id='swShow'>show</button><div id='swOut'></div>"
    "<button id='sw2Swap'>swap</button><button id='sw2Show'>show</button><div id='sw2Out'></div>"
    "<script>"
    "var swOut = document.getElementById('swOut');"
    "function render(s) { fetch('/api/sw-text'); swOut.textContent = s; }"
    "document.getElementById('swSwap').addEventListener('click', function swSwapClick() {"
    "  render = function (s) { fetch('/api/sw-markup'); swOut.innerHTML = s; };"
    "  fetch('/api/sw-swapped');"
    "});"
    "document.getElementById('swShow').addEventListener('click', function swShowClick() {"
    "  render(decodeURIComponent(location.hash.slice(1)));"
    "});"
    "(function () {"
    "  var out = document.getElementById('sw2Out');"
    "  var ui = { render: function (s) { fetch('/api/sw2-text'); out.textContent = s; } };"
    "  document.getElementById('sw2Swap').addEventListener('click', function sw2SwapClick() {"
    "    ui.render = function (s) { fetch('/api/sw2-markup'); out.innerHTML = s; };"
    "    fetch('/api/sw2-swapped');"
    "  });"
    "  document.getElementById('sw2Show').addEventListener('click', function sw2ShowClick() {"
    "    ui.render(decodeURIComponent(location.hash.substring(1)));"
    "  });"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
