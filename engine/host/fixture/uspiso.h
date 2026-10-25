/* THE URL-STATE ISOLATION DOCUMENT: a URLSearchParams and a URL, both built before the fork on
   `location.hash`, are mutated on one arm only — `set()` on each, the URL's through its searchParams so §6.2's
   update steps write its query — and both arms read them in a later script. The mutating arm is the one the
   example takes, so it runs first; the other arm must still read the values as built. Each witness carries the
   values it read. The hash is read once, before the write, so every world the solver builds over it is one
   consistent choice of hash for the whole document.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_USPISO_H
#define ENGINE_HOST_FIXTURE_USPISO_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_USPISO[] =
    "<!doctype html><html><body>"
    "<script>"
    "var isoP = new URLSearchParams('mode=base');"
    "var isoU = new URL('https://x.test/usiso?mode=base');"
    "var isoKeep = location.hash === '#keep';"
    "if (!isoKeep) {"
    "  isoP.set('mode', 'changed');"
    "  isoU.searchParams.set('mode', 'changed');"
    "}"
    "</script>"
    "<script>"
    "if (isoKeep)"
    "  fetch('/api/usiso-kept?p=' + isoP.get('mode') + '&u=' + encodeURIComponent(isoU.search));"
    "else"
    "  fetch('/api/usiso-set?p=' + isoP.get('mode') + '&u=' + encodeURIComponent(isoU.search));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
