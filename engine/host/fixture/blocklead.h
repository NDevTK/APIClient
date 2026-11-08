/* THE BLOCK-LED DOCUMENT (`blocklead`): a block container whose first child is block-level, followed by an
   atomic inline and 20,001 empty inline boxes (test_forced.c's COLLREST_10000, twice). CSS 2.2 §9.4.2's
   establishing condition is answered at the first child, so the stack walk's style ask over the child sequence
   (core/layout/box_tree.h) is what reaches the 20,002 inline children, under getBoundingClientRect's resting
   drive; §9.2.1.1 wraps them in one anonymous block box. `font-size:0` empties every strut, so the line is the
   30px image on its baseline below the 7px block: Chrome reads the last box at `x` 20, `y` 37 and the
   container's `offsetHeight` 37.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_BLOCKLEAD_H
#define ENGINE_HOST_FIXTURE_BLOCKLEAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_BLOCKLEAD[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=blBox style=\"font-size:0\"><div style=\"height:7px\"></div><img width=20 height=30>"
    COLLREST_10000 COLLREST_10000 "<i id=blLast></i></div>"
    "<script>"
    "var blR = document.getElementById('blLast').getBoundingClientRect();"
    "fetch('/api/blocklead?x=' + blR.left + '&y=' + blR.top + '&h=' + document.getElementById('blBox').offsetHeight);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
