/* THE REPLACED FLEX ITEM DOCUMENT (`flexratio`): css-flexbox-1 §9.4 step 7's hypothetical cross size of a replaced
   item in a `row` container, CSS 2.1 §10.6.2's height over the item's used main size. Each container is
   `align-items: flex-start`, so its auto height is that cross size. `#f1` holds a 100px-wide 2:1 svg (50px),
   `#f2` a 60px-wide canvas, whose 300x150 bitmap is 2:1 (30px); `#f3` a canvas flexed from 0 to 120px (60px, not
   its natural 150px) and `#f4` a 2:1 svg with only a viewBox flexed to 200px (100px), replit's shape.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXRATIO_H
#define ENGINE_HOST_FIXTURE_FLEXRATIO_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define FR_ROW(id, width, item) \
    "<div id=" id " style=\"display:flex;align-items:flex-start;width:" width "\">" item "</div>"

static const char HTML_FLEXRATIO[] =
    "<!doctype html><html><body style=\"margin:0\">"
    FR_ROW("f1", "200px", "<svg viewBox=\"0 0 2 1\" style=\"width:100px\"></svg>")
    FR_ROW("f2", "200px", "<canvas style=\"width:60px\"></canvas>")
    FR_ROW("f3", "120px", "<canvas style=\"flex:1 1 0;min-width:0\"></canvas>")
    FR_ROW("f4", "200px", "<svg viewBox=\"0 0 2 1\" style=\"flex:1 1 0;min-width:0\"></svg>")
    "<script>"
    "function frH(id) { return document.getElementById(id).offsetHeight; }"
    "fetch('/api/flexratio?f1=' + frH('f1') + '&f2=' + frH('f2') + '&f3=' + frH('f3') + '&f4=' + frH('f4'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef FR_ROW

#endif
