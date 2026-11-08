/* THE 3D TRANSFORM DOCUMENT (`transform3d`): css-transforms-2 §6 "Current Transformation Matrix"'s 4x4 over
   §16's matrices, flattened into each parent's plane (§7's initial `transform-style: flat`). Each box is
   absolutely positioned on a margin-free body: `#z` translate3d(10px, 20px, 30px), whose Z is dropped by the
   flattening; `#x` rotateX(60deg) about its centre, half its height; `#y` rotateY(60deg), half its width; `#p`
   perspective(100px) translateZ(50px), w = 1 - 50/100, twice its size about its centre; `#fc` the
   translateZ(100px) child of a rotateX(60deg) parent, flattened into the parent's plane before the parent's
   matrix maps it. `#z`'s resolved value is §2.1's matrix3d(). Rounded, and spaces and commas written as `_`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TRANSFORM3D_H
#define ENGINE_HOST_FIXTURE_TRANSFORM3D_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TRANSFORM3D[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=z style=\"position:absolute;left:0;top:0;width:10px;height:10px;"
    "transform:translate3d(10px, 20px, 30px)\"></div>"
    "<div id=x style=\"position:absolute;left:100px;top:0;width:40px;height:40px;transform:rotateX(60deg)\">"
    "</div>"
    "<div id=y style=\"position:absolute;left:300px;top:0;width:20px;height:10px;transform:rotateY(60deg)\">"
    "</div>"
    "<div id=p style=\"position:absolute;left:200px;top:0;width:20px;height:20px;"
    "transform:perspective(100px) translateZ(50px)\"></div>"
    "<div style=\"position:absolute;left:0;top:100px;width:40px;height:40px;transform:rotateX(60deg)\">"
    "<div id=fc style=\"width:40px;height:40px;transform:translateZ(100px)\"></div></div>"
    "<script>"
    "function t3(id, k) { return Math.round(document.getElementById(id).getBoundingClientRect()[k]); }"
    "fetch('/api/transform3d?z=' + t3('z', 'left') + '&zt=' + t3('z', 'top') + '&x=' + t3('x', 'top') +"
    "      '&xh=' + t3('x', 'height') + '&xw=' + t3('x', 'width') + '&y=' + t3('y', 'left') +"
    "      '&yw=' + t3('y', 'width') + '&p=' + t3('p', 'left') + '&pt=' + t3('p', 'top') +"
    "      '&pw=' + t3('p', 'width') + '&fc=' + t3('fc', 'top') + '&fch=' + t3('fc', 'height') +"
    "      '&zr=' + getComputedStyle(document.getElementById('z')).transform.replace(/[ ,]+/g, '_'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
