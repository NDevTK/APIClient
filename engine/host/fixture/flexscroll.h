/* THE COLUMN-FLEX SCROLL DOCUMENT: a `column` flex container centres an <h2> of text, so the heading is a flex item
   that does not stretch and its width is css-flexbox-1 §9.4 "Cross Size Determination" step 7's fit-content over
   its intrinsic inline sizes. A script then calls window.scrollTo(), whose CSSOM VIEW §4 steps measure the
   viewport's scrolling area under a resting drive, laying the heading's line out, and fetches a witness after it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXSCROLL_H
#define ENGINE_HOST_FIXTURE_FLEXSCROLL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLEXSCROLL[] =
    "<!doctype html><html><body>"
    "<div style='display:flex;flex-direction:column;align-items:center'><h2>A centred heading</h2></div>"
    "<script>"
    "function fxsAfterScroll() { fetch('/api/fxs-scrolled'); }"
    "window.scrollTo(0, 0);"
    "fxsAfterScroll();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
