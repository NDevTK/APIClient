/* THE PERSPECTIVE DOCUMENT (`perspective`): css-transforms-2 §8 "The perspective Property" and §9 "The
   perspective-origin Property", whose perspective matrix (§4.1.1 "Perspective") is pre-multiplied into each
   child's matrix (§4.1.4 "Accumulated 3D Transformation Matrix Computation"). `#c` is translateZ(50px) under
   `perspective: 100px` with the origin at its own centre, so it scales by d/(d − Z) = 2 about it; `#d` is the
   same under `perspective-origin: right 50px bottom 50px`, (450, 150) in the page. `#k` is an absolutely
   positioned child of a static box with `perspective`, which is its containing block. Computed and resolved
   values are read back. Rounded, spaces written as `_`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PERSPECTIVE_H
#define ENGINE_HOST_FIXTURE_PERSPECTIVE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PERSPECTIVE[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=p style=\"position:absolute;left:0;top:0;width:200px;height:200px;perspective:100px\">"
    "<div id=c style=\"position:absolute;left:50px;top:50px;width:100px;height:100px;transform:translateZ(50px)\">"
    "</div></div>"
    "<div id=q style=\"position:absolute;left:300px;top:0;width:200px;height:200px;perspective:100px;"
    "perspective-origin:right 50px bottom 50px\">"
    "<div id=d style=\"position:absolute;left:0;top:0;width:100px;height:100px;transform:translateZ(50px)\">"
    "</div></div>"
    "<div id=w style=\"margin-top:300px;width:50px;height:50px;perspective:0\">"
    "<div id=k style=\"position:absolute;left:5px;top:5px;width:1px;height:1px\"></div></div>"
    "<script>"
    "function pr(id, k) { return Math.round(document.getElementById(id).getBoundingClientRect()[k]); }"
    "function pc(id, p) { return getComputedStyle(document.getElementById(id))[p].replace(/ /g, '_'); }"
    "fetch('/api/perspective?c=' + pr('c', 'left') + '&cw=' + pr('c', 'width') + '&ct=' + pr('c', 'top') +"
    "      '&d=' + pr('d', 'left') + '&dw=' + pr('d', 'width') + '&dt=' + pr('d', 'top') +"
    "      '&k=' + pr('k', 'left') + '&kt=' + pr('k', 'top') +"
    "      '&pp=' + pc('p', 'perspective') + '&wp=' + pc('w', 'perspective') + '&cp=' + pc('c', 'perspective') +"
    "      '&po=' + pc('p', 'perspectiveOrigin') + '&qo=' + pc('q', 'perspectiveOrigin'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
