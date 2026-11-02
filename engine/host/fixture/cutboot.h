/* THE LONG-BOOT FRAGMENT DOCUMENT: about forty million units of work of page setup run before the one read of
   `location.hash`, whose decoded value is written to markup and fires. A candidate that re-runs the document from
   the pre-boot baseline pays the setup again before its payload can matter; one that starts at a snapshot taken
   before the fragment's first read pays none of it. `_firstFireWork` is the engine work retired at the first
   finding, the number the two are compared by.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUTBOOT_H
#define ENGINE_HOST_FIXTURE_CUTBOOT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUTBOOT[] =
    "<!doctype html><html><body>"
    "<div id='cbOut'></div>"
    "<script>"
    "var cbAcc = 0;"
    "for (var cbI = 0; cbI < 4000000; cbI++) cbAcc = (cbAcc + cbI) % 1000003;"
    "document.getElementById('cbOut').innerHTML = decodeURIComponent(location.hash.slice(1));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
