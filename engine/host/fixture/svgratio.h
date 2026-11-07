/* THE RATIO-ONLY REPLACED DOCUMENT (`svgratio`): css-sizing-3 §5.1's first replaced arm, an outermost `svg` with
   a `viewBox` of 2:1 and no natural size (SVG 2 §8.12). Its min-content size is zero and its max-content size is
   a <length> `min-width`, or a `min-height` through the ratio, the larger of the two, else the initial
   containing block's width. Each box's width is that max-content size: `#b1`-`#b4` are `inline-block`s around
   a block-level svg, `#i` one around an inline svg, and `#g` a float in a 5000px block. `#z`, at `min-content`,
   is the zero. Replit reaches the arm through a flex item (css-flexbox-1 §9.9's intrinsic main size); a flex
   item of this shape is refused at css-flexbox-1 §9.4's step 7 before its container's width can be read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SVGRATIO_H
#define ENGINE_HOST_FIXTURE_SVGRATIO_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define SR_SVG(style) "<svg viewBox=\"0 0 2 1\" style=\"" style "\"></svg>"

static const char HTML_SVGRATIO[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div><div id=b1 style=\"display:inline-block\">" SR_SVG("display:block;min-width:40px") "</div></div>"
    "<div><div id=b2 style=\"display:inline-block\">" SR_SVG("display:block;min-height:30px") "</div></div>"
    "<div><div id=b3 style=\"display:inline-block\">" SR_SVG("display:block;min-width:40px;min-height:30px")
    "</div></div>"
    "<div><div id=b4 style=\"display:inline-block\">" SR_SVG("display:block;min-width:40px;min-height:10px")
    "</div></div>"
    "<div style=\"font-size:0\"><div id=i style=\"display:inline-block\">" SR_SVG("min-height:30px")
    "</div></div>"
    "<div style=\"overflow:hidden\"><div style=\"width:5000px\"><div id=g style=\"float:left\">"
    SR_SVG("display:block") "</div></div></div>"
    "<div id=z style=\"width:min-content\">" SR_SVG("display:block") "</div>"
    "<script>"
    "function srW(id) { return document.getElementById(id).offsetWidth; }"
    "fetch('/api/svgratio?b1=' + srW('b1') + '&b2=' + srW('b2') + '&b3=' + srW('b3') + '&b4=' + srW('b4') +"
    "      '&i=' + srW('i') + '&g=' + (srW('g') - document.documentElement.clientWidth) +"
    "      '&z=' + srW('z'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef SR_SVG

#endif
