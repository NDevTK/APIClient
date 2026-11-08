/* THE SVG TRANSFORMABLE DOCUMENT (`svgtransformable`): css-transforms-1 §2 Terminology's second category of
   transformable element and §4's used transform-origin, read through getComputedStyle. `#r` and `#g` are SVG
   renderable elements and `#lg` a paint server, so §4's `Applies to: transformable elements` holds and the
   resolved value is the used one, `0 0` by §4's UA rule for SVG elements without a CSS layout box. `#ts` is a
   descendant of a text content element and `#d` a never-rendered `desc`, outside both categories, so theirs is
   the computed value, the same UA rule's. The outermost `#s` and the `div` `#b` are the first category: the
   initial `50% 50%` of a 100px square, and `10px 25%` of a 40x20 border box. Spaces are written as `_`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SVGTRANSFORMABLE_H
#define ENGINE_HOST_FIXTURE_SVGTRANSFORMABLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SVGTRANSFORMABLE[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<svg id=s width=\"100\" height=\"100\">"
    "<g id=g><rect id=r width=\"10\" height=\"10\" style=\"transform:rotate(10deg)\"/></g>"
    "<text>x<tspan id=ts style=\"transform:scale(2)\">y</tspan></text>"
    "<linearGradient id=lg style=\"transform:scale(2)\"/>"
    "<desc id=d style=\"transform:scale(2)\"></desc>"
    "</svg>"
    "<div id=b style=\"width:40px;height:20px;transform-origin:10px 25%\"></div>"
    "<script>"
    "function so(id) { return getComputedStyle(document.getElementById(id)).transformOrigin.replace(/ /g, '_'); }"
    "fetch('/api/svgtransformable?r=' + so('r') + '&g=' + so('g') + '&lg=' + so('lg') + '&ts=' + so('ts') +"
    "      '&d=' + so('d') + '&s=' + so('s') + '&b=' + so('b'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
