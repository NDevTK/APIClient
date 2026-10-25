/* THE CLOBBER DOCUMENT: a banner from the query is filtered and written to innerHTML, and a later script reads
   `window.clCfg` and navigates to `clCfg.url`. The filter refuses event handlers and nested browsing contexts
   (a script element inserted by innerHTML never runs), so the banner cannot fire by itself; what it can still
   do is name elements. HTML §7.2.2.3 "Named access on the Window object" makes two elements with
   `id=clCfg` a collection on `window.clCfg`, and the one also named `url` its `url` member, whose
   stringification is its href. So the PoC is markup, and it fires at the navigation, not at the markup write.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CLOBBER_H
#define ENGINE_HOST_FIXTURE_CLOBBER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CLOBBER[] =
    "<!doctype html><html><body>"
    "<div id='clBanner'></div>"
    "<script>"
    "var clQ = new URLSearchParams(location.search).get('banner');"
    "if (clQ) {"
    "  var clLow = clQ.toLowerCase();"
    "  if (clLow.indexOf('on') < 0 && clLow.indexOf('frame') < 0 && clLow.indexOf('object') < 0 &&"
    "      clLow.indexOf('embed') < 0) {"
    "    document.getElementById('clBanner').innerHTML = clQ;"
    "  }"
    "}"
    "</script>"
    "<script>"
    "if (window.clCfg) {"
    "  fetch('/api/cl-clobbered');"
    "  location.href = clCfg.url + '';"
    "} else {"
    "  fetch('/api/cl-plain');"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
