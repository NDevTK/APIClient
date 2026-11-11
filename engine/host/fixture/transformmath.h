/* The transform math document (`transformmath`): css-values-4 §10 "Mathematical Expressions" in every argument
   type css-transforms-1 §7.1 and css-transforms-2 §12.2 give a transform function, and in css-transforms-2 §5's
   individual properties. `#r` an `<angle>` over a `var()`, which openlibrary's `--fan-spread` writes; `#s` a
   scale's `<number>` and `<percentage>`; `#m` matrix() numbers, one a quotient of two lengths; `#e` a number
   whose `em` leaf is the element's own 10px; `#k` skewX(); `#a` rotate3d()'s numbers and a `turn` angle; `#i`
   the rotate and scale properties; `#x` and `#y` math functions of the wrong type, which drop the declaration.
   Rounded, and spaces written as `_`. One of test_forced.c's exploring documents (its TF_DOCS table). It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TRANSFORMMATH_H
#define ENGINE_HOST_FIXTURE_TRANSFORMMATH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TRANSFORMMATH[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=r style=\"position:absolute;left:100px;top:100px;width:40px;height:20px;--s:2;"
    "transform:rotate(calc(-10deg - 40deg * var(--s)))\"></div>"
    "<div id=s style=\"position:absolute;left:200px;top:0;width:10px;height:10px;"
    "transform:scale(calc(1 + 1), calc(150% + 50%))\"></div>"
    "<div id=m style=\"position:absolute;left:0;top:300px;width:10px;height:10px;transform-origin:0 0;"
    "transform:matrix(calc(4 / 2), 0, 0, calc(6px / 2px), 10, 20)\"></div>"
    "<div id=e style=\"position:absolute;left:300px;top:0;width:10px;height:10px;font-size:10px;"
    "transform:scale(calc(3em / 10px))\"></div>"
    "<div id=k style=\"position:absolute;left:400px;top:0;width:20px;height:20px;transform:skewX(calc(90deg / 2))\">"
    "</div>"
    "<div id=a style=\"position:absolute;left:500px;top:100px;width:40px;height:20px;"
    "transform:rotate3d(0, 0, calc(2 - 1), calc(0.25turn))\"></div>"
    "<div id=i style=\"rotate:calc(0.25turn - 180deg);scale:calc(1 + 1)\"></div>"
    "<div id=x style=\"transform:rotate(calc(1px + 1deg))\"></div>"
    "<div id=y style=\"transform:scale(calc(1deg))\"></div>"
    "<script>"
    "function tm(id, k) { return Math.round(document.getElementById(id).getBoundingClientRect()[k]); }"
    "function tc(id, p) { return getComputedStyle(document.getElementById(id))[p].replace(/ /g, '_'); }"
    "fetch('/api/transformmath?r=' + tm('r', 'left') + '&rt=' + tm('r', 'top') + '&rw=' + tm('r', 'width') +"
    "      '&rc=' + tc('r', 'transform') + '&s=' + tm('s', 'left') + '&sw=' + tm('s', 'width') +"
    "      '&sh=' + tm('s', 'height') + '&m=' + tm('m', 'left') + '&mt=' + tm('m', 'top') +"
    "      '&mw=' + tm('m', 'width') + '&mh=' + tm('m', 'height') + '&e=' + tm('e', 'left') +"
    "      '&ew=' + tm('e', 'width') + '&k=' + tm('k', 'left') + '&kw=' + tm('k', 'width') +"
    "      '&a=' + tm('a', 'left') + '&at=' + tm('a', 'top') + '&aw=' + tm('a', 'width') +"
    "      '&ir=' + tc('i', 'rotate') + '&is=' + tc('i', 'scale') + '&x=' + tc('x', 'transform') +"
    "      '&y=' + tc('y', 'transform'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
