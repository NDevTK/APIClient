/* THE 2D TRANSFORM DOCUMENT (`transform2d`): css-transforms-1 §2's transformation matrix with §4's
   transform-origin, §5's reference box and §12's matrices for every 2D function. Each box is absolutely
   positioned on a margin-free body, so its untransformed border box is the numbers in its style:
   `#r` rotate(90deg) about the initial origin `50% 50%`; `#o` scale(0.5) about `right bottom`; `#l` scale(2)
   translate(5px) about `0 0`; `#k` skewX(45deg); `#m` matrix(); `#pc` a child of a box rotated half a turn;
   `#c` scale(0.5) about the content box's corner under `transform-box: content-box`; `#t` the 2D members of
   css-transforms-2's 3D functions; `#a` rotate(180deg) about `top left`, the swapped keyword order.
   Rounded because sin, cos and tan of these angles are not exact in binary floating point.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TRANSFORM2D_H
#define ENGINE_HOST_FIXTURE_TRANSFORM2D_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TRANSFORM2D[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=r style=\"position:absolute;left:100px;top:100px;width:40px;height:20px;transform:rotate(90deg)\">"
    "</div>"
    "<div id=o style=\"position:absolute;left:0;top:200px;width:40px;height:40px;transform-origin:right bottom;"
    "transform:scale(0.5)\"></div>"
    "<div id=l style=\"position:absolute;left:200px;top:200px;width:20px;height:20px;transform-origin:0 0;"
    "transform:scale(2) translate(5px)\"></div>"
    "<div id=k style=\"position:absolute;left:300px;top:0;width:20px;height:20px;transform:skewX(45deg)\"></div>"
    "<div id=m style=\"position:absolute;left:0;top:300px;width:10px;height:10px;transform-origin:0 0;"
    "transform:matrix(2, 0, 0, 3, 10, 20)\"></div>"
    "<div id=p style=\"position:absolute;left:0;top:400px;width:100px;height:100px;transform:rotate(0.5turn)\">"
    "<div id=pc style=\"width:10px;height:10px\"></div></div>"
    "<div id=c style=\"position:absolute;left:200px;top:300px;width:40px;height:40px;padding:10px;"
    "transform-box:content-box;transform-origin:0 0;transform:scale(0.5)\"></div>"
    "<div id=t style=\"position:absolute;left:400px;top:0;width:10px;height:10px;"
    "transform:translate3d(5px, 6px, 0) translateZ(0) scale3d(1, 1, 1) rotateZ(0deg)\"></div>"
    "<div id=a style=\"position:absolute;left:400px;top:100px;width:20px;height:10px;transform-origin:top left;"
    "transform:rotate(180deg)\"></div>"
    "<script>"
    "function t2(id, k) { return Math.round(document.getElementById(id).getBoundingClientRect()[k]); }"
    "fetch('/api/transform2d?r=' + t2('r', 'left') + '&rt=' + t2('r', 'top') + '&rw=' + t2('r', 'width') +"
    "      '&o=' + t2('o', 'left') + '&ot=' + t2('o', 'top') + '&ow=' + t2('o', 'width') +"
    "      '&l=' + t2('l', 'left') + '&lw=' + t2('l', 'width') + '&k=' + t2('k', 'left') + '&kw=' + t2('k', 'width') +"
    "      '&m=' + t2('m', 'left') + '&mt=' + t2('m', 'top') + '&mh=' + t2('m', 'height') +"
    "      '&pc=' + t2('pc', 'left') + '&pct=' + t2('pc', 'top') + '&c=' + t2('c', 'left') + '&cw=' + t2('c', 'width') +"
    "      '&t=' + t2('t', 'left') + '&tt=' + t2('t', 'top') + '&a=' + t2('a', 'left') + '&at=' + t2('a', 'top'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
