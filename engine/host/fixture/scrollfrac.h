/* The fractional root scrolling area document: CSSOM VIEW §6 "Extensions to the Element Interface"'
   `scrollHeight` of the root element over a 2000.5px block, longer than the viewport, so step 4 answers the
   height of the viewport scrolling area, a real length. §6 declares the member `long`, so the length crosses
   as an integer through the nearest-integer conversion §6's other extents take (element_view_length_long):
   2001. Read from the spec. One of test_forced.c's TF_DOCS; it ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_SCROLLFRAC_H
#define ENGINE_HOST_FIXTURE_SCROLLFRAC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCROLLFRAC[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div style=\"height:2000.5px\"></div>"
    "<script>"
    "fetch('/api/scrollfrac?rh='+document.documentElement.scrollHeight);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
