/* THE UPGRADE-SALE DOCUMENT: `customElements.define` upgrades a parsed `<x-cs>` (HTML §4.13.5 "Upgrades"), and
   its constructor forks on absent application state. One arm stays inside the constructor in a long loop, so
   it parks mid-Construct with step 10's active custom element constructor map entry and construction stack
   entry in its own world; the other arm returns, finishes the define and grows the heap in rounds until RAM
   pressure sells the parked arm (the document is served with a small share, its TF_DOCS row). A sale frees a
   flow that is not switched in, so nothing it entered is the world the pager runs in.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_CEUPSALE_H
#define ENGINE_HOST_FIXTURE_CEUPSALE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CEUPSALE[] =
    "<!doctype html><html><body>"
    "<x-cs id=csEl></x-cs>"
    "<script>"
    "var _cucfg = { hold: state.ceHold };"
    "class XCs extends HTMLElement {"
    "  constructor() {"
    "    super();"
    "    if (_cucfg.hold) {"
    "      var t = 0;"
    "      for (var i = 0; i < 100000000; i++) t = (t + i) % 7919;"
    "      this.mode = 'held' + t;"
    "    } else {"
    "      this.mode = 'free';"
    "    }"
    "  }"
    "}"
    "customElements.define('x-cs', XCs);"
    "if (!_cucfg.hold) {"
    "  (async function () {"
    "    var a = [];"
    "    for (var k = 0; k < 40; k++) {"
    "      for (var i = 0; i < 20000; i++) a.push({ i: i, s: 'v' + k + '.' + i });"
    "      await fetch('/api/cugrow?k=' + k);"
    "    }"
    "    fetch('/api/cusell?m=' + document.getElementById('csEl').mode +"
    "          '&n=' + (new XCs() instanceof XCs));"
    "  })();"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
