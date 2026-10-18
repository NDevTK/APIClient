/* THE CLIENT EXTENT DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). CSSOM VIEW §6's
   `clientHeight` of a box with twelve 10px children and 5px of padding is its padding edge, 130px, and that
   box's §10.6.3 content-based height lays out more boxes than one activation of a resting drive takes, so the
   read rests at least once (`_layoutClientExtentRests`). An `inline-block` holding children 10px to 80px wide
   with 3px of padding is 86px wide by §10.3.9's shrink-to-fit. A `span` is inline, so step 1 answers 0.
   Chrome 155 gives these numbers for the same markup. It ends with TF_CANVAS_INK, which test_forced.c defines
   before including it. */
#ifndef ENGINE_HOST_FIXTURE_CLIENTEXTENT_H
#define ENGINE_HOST_FIXTURE_CLIENTEXTENT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define CE_ROW "<div style=\"height:10px\"></div>"
#define CE_COL(w) "<div style=\"width:" w "px;height:5px\"></div>"

static const char HTML_CLIENTEXTENT[] =
    "<!doctype html><html>"
    "<body>"
    "<div id=ce1 style=\"padding:5px\">" CE_ROW CE_ROW CE_ROW CE_ROW CE_ROW CE_ROW CE_ROW CE_ROW CE_ROW CE_ROW
    CE_ROW CE_ROW "</div>"
    "<div><div id=ce2 style=\"display:inline-block;padding:3px\">" CE_COL("10") CE_COL("20") CE_COL("30")
    CE_COL("40") CE_COL("50") CE_COL("60") CE_COL("70") CE_COL("80") "</div></div>"
    "<p><span id=ce3>x</span></p>"
    "<script>fetch('/api/cext?h=' + document.getElementById('ce1').clientHeight"
    " + '&w=' + document.getElementById('ce2').clientWidth + '&s=' + document.getElementById('ce3').clientWidth);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef CE_ROW
#undef CE_COL

#endif
