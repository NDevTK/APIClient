/* THE POSITIONING CONTAINING BLOCK DOCUMENT (`willchange`): css-position-3 §2.1's establishment by `will-change`
   (css-will-change-1 §2) and by `transform`, replit's shape. Every ancestor sits 50px from the left edge and
   every positioned child has `left: 10px`, so a child whose containing block the ancestor establishes is 60px
   from the edge and one placed against the initial containing block or the viewport is 10px. `will-change:
   transform` makes `#a1`'s; `will-change: opacity, scroll-position` names nothing that would (`#a2`);
   `will-change: filter` makes a fixed child's (`#f3`); `will-change: position` does not, since `position` makes
   no fixed positioning containing block (`#f4`); `transform: translateX(0px)` does (`#f5`); a declared
   `filter: none` is the initial value and establishes nothing (`#a6`).
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WILLCHANGE_H
#define ENGINE_HOST_FIXTURE_WILLCHANGE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define WC_ROW(style, id, pos) \
    "<div style=\"margin-left:50px;height:20px;" style "\"><div id=" id " style=\"position:" pos \
    ";left:10px;top:0;width:5px;height:5px\"></div></div>"

static const char HTML_WILLCHANGE[] =
    "<!doctype html><html><body style=\"margin:0\">"
    WC_ROW("will-change:transform", "a1", "absolute")
    WC_ROW("will-change:opacity, scroll-position", "a2", "absolute")
    WC_ROW("will-change:filter", "f3", "fixed")
    WC_ROW("will-change:position", "f4", "fixed")
    WC_ROW("transform:translateX(0px)", "f5", "fixed")
    WC_ROW("filter:none", "a6", "absolute")
    "<script>"
    "function wcL(id) { return document.getElementById(id).getBoundingClientRect().left; }"
    "fetch('/api/willchange?a1=' + wcL('a1') + '&a2=' + wcL('a2') + '&f3=' + wcL('f3') + '&f4=' + wcL('f4') +"
    "      '&f5=' + wcL('f5') + '&a6=' + wcL('a6'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef WC_ROW

#endif
