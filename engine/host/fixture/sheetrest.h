/* THE SHEET-REST DOCUMENT: a style sheet whose only rule's declaration block is several CSS parse rest units
   long (solver/rest_unit.c's REST_UNIT_CSS_PARSE_BYTES), so css_sheet_parse.c's run suspends inside that block
   and the resume that completes the sheet runs no later rule's prelude. The rule's 1001 declarations are read
   back through CSSOM and the cascade: a resumed run answers the same sheet an unsuspended one does.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SHEETREST_H
#define ENGINE_HOST_FIXTURE_SHEETREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

/* Ten, a hundred and a thousand distinct custom properties, each about 30 bytes. */
#define SHEETREST_D(p, d) "--sr" p d ": hsl(46deg, 10%, 96%);"
#define SHEETREST_10(p) SHEETREST_D(p, "0") SHEETREST_D(p, "1") SHEETREST_D(p, "2") SHEETREST_D(p, "3") \
    SHEETREST_D(p, "4") SHEETREST_D(p, "5") SHEETREST_D(p, "6") SHEETREST_D(p, "7") SHEETREST_D(p, "8")     \
    SHEETREST_D(p, "9")
#define SHEETREST_100(p) SHEETREST_10(p "0") SHEETREST_10(p "1") SHEETREST_10(p "2") SHEETREST_10(p "3")      \
    SHEETREST_10(p "4") SHEETREST_10(p "5") SHEETREST_10(p "6") SHEETREST_10(p "7") SHEETREST_10(p "8")      \
    SHEETREST_10(p "9")
#define SHEETREST_1000 SHEETREST_100("0") SHEETREST_100("1") SHEETREST_100("2") SHEETREST_100("3")           \
    SHEETREST_100("4") SHEETREST_100("5") SHEETREST_100("6") SHEETREST_100("7") SHEETREST_100("8")           \
    SHEETREST_100("9")

static const char HTML_SHEETREST[] =
    "<!doctype html><html><head><style>#srEl{" SHEETREST_1000 "width:7px}</style></head><body>"
    "<div id=srEl></div>"
    "<script>"
    "var srRule = document.styleSheets[0].cssRules[0];"
    "fetch('/api/sheetrest?n=' + document.styleSheets[0].cssRules.length + '&d=' + srRule.style.length +"
    "      '&w=' + srRule.style.width + '&c=' + getComputedStyle(document.getElementById('srEl')).width);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef SHEETREST_1000
#undef SHEETREST_100
#undef SHEETREST_10
#undef SHEETREST_D

#endif
