/* THE UPGRADE-SALE DOCUMENT: `customElements.define` upgrades a parsed `<x-cs>` (HTML §4.13.5 "Upgrades"), and
   its constructor forks on absent application state and then grows the heap, so whichever arm is parked is
   parked mid-Construct with step 10's active custom element constructor map entry and construction stack
   entry in its own world, and RAM pressure sells it (the document is served with a small share, its TF_DOCS
   row). A sale frees a flow that is not switched in, so nothing it entered is the world the pager runs in, and
   the surviving arm finishes its own upgrade and a direct `new XCs()`.
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
    "    this.mode = _cucfg.hold ? 'held' : 'free';"
    "    var a = [];"
    "    for (var i = 0; i < 400000; i++) a.push({ i: i, s: 'v' + i });"
    "    this.n = a.length;"
    "  }"
    "}"
    "customElements.define('x-cs', XCs);"
    "fetch('/api/cusell?m=' + document.getElementById('csEl').mode + '&n=' + (new XCs() instanceof XCs));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
