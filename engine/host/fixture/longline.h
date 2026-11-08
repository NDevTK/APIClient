/* THE LONG LINE DOCUMENT (`longline`): CSS 2.2 §9.4.2's inline formatting context over one line holding an
   atomic inline and 20,001 empty inline boxes (test_forced.c's COLLREST_10000, twice), more line items between
   two code point positions than a work quantum prices, so the line box fill rests between them inside
   getBoundingClientRect. The count stays small beside that because test_forced.c's paint selftests lay this
   document out again on jobs of their own. The box's
   `font-size:0` empties every strut, so the line is the 30px image on its baseline and the last box is a
   zero-width point on the baseline after it: Chrome reads `x` 20, `y` 30 and the box's `offsetHeight` 30. Its
   declared `height` equals that line, so painting its line boxes is not served by a walk its own size asked. The
   body keeps its font, since test_forced.c's paint selftests append text to this document's body.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LONGLINE_H
#define ENGINE_HOST_FIXTURE_LONGLINE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LONGLINE[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=llBox style=\"font-size:0;height:30px\"><img width=20 height=30>" COLLREST_10000 COLLREST_10000
    "<i id=llLast></i></div>"
    "<script>"
    "var llR = document.getElementById('llLast').getBoundingClientRect();"
    "fetch('/api/longline?x=' + llR.left + '&y=' + llR.top + '&h=' + document.getElementById('llBox').offsetHeight);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
