/* THE BEHIND-THE-VIEWER DOCUMENT (`perspectivew`): css-transforms-2 §4.2 "Processing of Perspective-Transformed
   Boxes"' corners at w <= 0, as CSSOM View §6's client rectangles map them. Each box is 100px square at (100,
   100) with `transform-origin: 0 0`. `#n` is perspective(64px) translateZ(128px), w = -1 at every corner; `#z`
   is translateZ(64px) under the same, w = 0; `#m` is a matrix3d() whose w is 0.5 - 0.01x, negative past x = 50.
   Expected values are Chrome 153.0.8010.12's getBoundingClientRect. Rounded.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PERSPECTIVEW_H
#define ENGINE_HOST_FIXTURE_PERSPECTIVEW_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define PW_BOX(id, t) \
    "<div id=" id " style=\"position:absolute;left:100px;top:100px;width:100px;height:100px;" \
    "transform-origin:0 0;transform:" t "\"></div>"

static const char HTML_PERSPECTIVEW[] =
    "<!doctype html><html><body style=\"margin:0\">"
    PW_BOX("n", "perspective(64px) translateZ(128px)")
    PW_BOX("z", "perspective(64px) translateZ(64px)")
    PW_BOX("m", "matrix3d(1,0,0,-0.01, 0,1,0,0, 0,0,1,0, 0,0,0,0.5)")
    "<script>"
    "function pw(id) { var r = document.getElementById(id).getBoundingClientRect();"
    "                  return [r.x, r.y, r.width, r.height].map(Math.round).join('_'); }"
    "fetch('/api/perspectivew?n=' + pw('n') + '&z=' + pw('z') + '&m=' + pw('m'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef PW_BOX

#endif
