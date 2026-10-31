/* THE REPLACED GRID ITEM DOCUMENT: an outermost `svg` (SVG 2 §8.12) as a grid item in a 100px column, sized by
   css-grid-1 §6.2 "Grid Item Sizing" — `normal` with a preferred aspect ratio by the block-level replaced
   rules, any other self-alignment by "Use natural size", `stretch` by the stretch-fit size — and contributing
   its CSS 2.1 §10.6.2 height to an `auto` row. One of test_forced.c's exploring documents (its TF_DOCS table).
   Each row reports the svg's border box and its offset in the grid, WxH@X_Y. Chrome 155.0.8059.84 and Firefox
   158.0.1 give every value expected. Cross-engine note, left out: a bare `<svg>` (no ratio, no natural size)
   is 300 x 150 in both browsers where §6.2's `normal` row reads as stretch. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GRIDREPLACED_H
#define ENGINE_HOST_FIXTURE_GRIDREPLACED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define GRIDREPLACED_ROW(cstyle, svg) \
    "<div class=g style=\"" cstyle "\">" svg "</div>"

static const char HTML_GRIDREPLACED[] =
    "<!doctype html><html>"
    "<head><style>.g{display:grid;grid-template-columns:100px;width:100px}</style></head>"
    "<body style=\"margin:0\">"
    GRIDREPLACED_ROW("align-items:center", "<svg id=q1 viewBox=\"0 0 24 12\"></svg>")
    GRIDREPLACED_ROW("align-items:start;justify-items:start", "<svg id=q2 viewBox=\"0 0 24 12\"></svg>")
    GRIDREPLACED_ROW("", "<svg id=q3 viewBox=\"0 0 24 12\"></svg>")
    GRIDREPLACED_ROW("align-items:center", "<svg id=q4 width=\"40\" viewBox=\"0 0 24 12\"></svg>")
    GRIDREPLACED_ROW("justify-items:center", "<svg id=q5 height=\"30\" viewBox=\"0 0 24 12\"></svg>")
    GRIDREPLACED_ROW("grid-template-rows:80px;align-items:stretch;justify-items:stretch",
                     "<svg id=q6 viewBox=\"0 0 24 12\"></svg>")
    "<script>"
    "function Z(i){var e=document.getElementById(i),r=e.getBoundingClientRect(),"
    "p=e.parentElement.getBoundingClientRect();return r.width+'x'+r.height+'@'+(r.left-p.left)+'_'+(r.top-p.top);}"
    "var q='',i;for(i=1;i<=6;i++)q+=(i>1?'&':'')+'q'+i+'='+Z('q'+i);"
    "fetch('/api/gridreplaced?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef GRIDREPLACED_ROW
#endif
