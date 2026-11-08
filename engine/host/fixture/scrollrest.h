/* THE SCROLL-REST DOCUMENT (`scrollrest`): CSSOM VIEW §6.1's scroll a target into view over the viewport, for a
   target below 20,000 one-pixel block boxes (test_forced.c's COLLREST_10000, twice, each `<i>` made a block).
   The viewport's scrolling area walk is seeded with the document element's margin edge and step 2.2 reads the
   target's border box, and both are CSS 2.1 §9.4.1's walk over those boxes, which a resting drive must hold.
   The target is 10px tall at 20,000px in a 20,010px document and a 720px viewport: Chrome scrolls the window
   to 19,290.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SCROLLREST_H
#define ENGINE_HOST_FIXTURE_SCROLLREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCROLLREST[] =
    "<!doctype html><html><head><style>#srPre i{display:block;height:1px}</style></head>"
    "<body style=\"margin:0\"><div id=srPre>" COLLREST_10000 COLLREST_10000 "</div>"
    "<div id=srT style=\"height:10px\"></div>"
    "<script>"
    "document.getElementById('srT').scrollIntoView();"
    "fetch('/api/scrollintorest?y=' + window.scrollY);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
