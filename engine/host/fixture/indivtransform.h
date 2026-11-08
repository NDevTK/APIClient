/* THE INDIVIDUAL TRANSFORMS DOCUMENT (`indivtransform`): css-transforms-2 §5 "Individual Transform Properties:
   the translate, scale, and rotate properties", composed by §6 "Current Transformation Matrix" ahead of
   `transform` and inside the origin bracket. `#t` translate: 10px 20px; `#r` rotate: 90deg about the initial
   origin; `#s` scale: 2 about its centre; `#o` translate, rotate and transform together on a 20x10 box at
   (300, 0), which §6's order maps to (315, 5) and 10 wide. The three computed values are read back. Rounded,
   and spaces written as `_`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INDIVTRANSFORM_H
#define ENGINE_HOST_FIXTURE_INDIVTRANSFORM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INDIVTRANSFORM[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=t style=\"position:absolute;left:0;top:0;width:10px;height:10px;translate:10px 20px\"></div>"
    "<div id=r style=\"position:absolute;left:100px;top:100px;width:40px;height:20px;rotate:90deg\"></div>"
    "<div id=s style=\"position:absolute;left:200px;top:0;width:10px;height:10px;scale:2\"></div>"
    "<div id=o style=\"position:absolute;left:300px;top:0;width:20px;height:10px;translate:10px;rotate:90deg;"
    "transform:translateX(10px)\"></div>"
    "<script>"
    "function it(id, k) { return Math.round(document.getElementById(id).getBoundingClientRect()[k]); }"
    "function ic(id, p) { return getComputedStyle(document.getElementById(id))[p].replace(/ /g, '_'); }"
    "fetch('/api/indivtransform?t=' + it('t', 'left') + '&tt=' + it('t', 'top') + '&r=' + it('r', 'left') +"
    "      '&rt=' + it('r', 'top') + '&rw=' + it('r', 'width') + '&s=' + it('s', 'left') + '&sw=' + it('s', 'width') +"
    "      '&o=' + it('o', 'left') + '&ot=' + it('o', 'top') + '&ow=' + it('o', 'width') +"
    "      '&ct=' + ic('t', 'translate') + '&cr=' + ic('r', 'rotate') + '&cs=' + ic('s', 'scale'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
