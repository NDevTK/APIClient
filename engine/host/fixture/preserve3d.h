/* THE PRESERVE-3D DOCUMENT (`preserve3d`): css-transforms-2 §7 "The transform-style Property" and §4.1.4
   "Accumulated 3D Transformation Matrix Computation". `#a` is rotateY(-45deg) inside a rotateY(45deg) parent
   with `transform-style: preserve-3d`, both about the same centre, so the accumulated 4x4 is the identity and
   the box keeps its 100px width; `#b` is the same under a flat parent, flattened twice to cos²45° = 50px; `#c`
   is the same under a preserve-3d parent with `overflow: hidden`, a §7.1 grouping property that makes the used
   value flat. Rounded, spaces written as `_`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PRESERVE3D_H
#define ENGINE_HOST_FIXTURE_PRESERVE3D_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define P3_PAIR(left, style, id)                                                                             \
    "<div id=" id "p style=\"position:absolute;left:" left ";top:0;width:100px;height:100px;"                \
    "transform:rotateY(45deg);" style "\"><div id=" id " style=\"position:absolute;left:0;top:0;width:100px;" \
    "height:100px;transform:rotateY(-45deg)\"></div></div>"

static const char HTML_PRESERVE3D[] =
    "<!doctype html><html><body style=\"margin:0\">"
    P3_PAIR("100px", "transform-style:preserve-3d", "a")
    P3_PAIR("300px", "", "b")
    P3_PAIR("500px", "transform-style:preserve-3d;overflow:hidden", "c")
    "<script>"
    "function p3(id, k) { return Math.round(document.getElementById(id).getBoundingClientRect()[k]); }"
    "function p3c(id, p) { return getComputedStyle(document.getElementById(id))[p]; }"
    "fetch('/api/preserve3d?a=' + p3('a', 'left') + '&aw=' + p3('a', 'width') +"
    "      '&b=' + p3('b', 'left') + '&bw=' + p3('b', 'width') + '&c=' + p3('c', 'left') + '&cw=' + p3('c', 'width') +"
    "      '&as=' + p3c('ap', 'transformStyle') + '&bs=' + p3c('bp', 'transformStyle') +"
    "      '&cs=' + p3c('cp', 'transformStyle'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef P3_PAIR

#endif
