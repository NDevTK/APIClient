/* THE REPLACED FLEX ITEM DOCUMENT: an outermost `svg` (SVG 2 §8.12) as a flex item with a preferred aspect
   ratio, sized by css-flexbox-1 §9.2's arm B — "a preferred aspect ratio, a used flex basis of content, and a
   definite cross size" — and floored by §4.5's transferred size suggestion. One of test_forced.c's exploring
   documents (its TF_DOCS table). #f1 is a real page's icon (`width="1em" height="1em"` and a `viewBox`), whose
   declared main size is arm A; #f2 and #f4 are arm B in a row and a column; #f3 shrinks in a 30px row and
   stops at its transferred size; #f5 has a declared main size below its transferred one, which caps the
   minimum. Chrome 155.0.8059.84 and Firefox 158.0.1 give every value expected. It ends with TF_CANVAS_INK,
   which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXREPLACED_H
#define ENGINE_HOST_FIXTURE_FLEXREPLACED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLEXREPLACED[] =
    "<!doctype html><html>"
    "<body style=\"margin:0;font-size:15px\">"
    "<div style=\"display:flex;width:300px\"><svg id=f1 width=\"1em\" height=\"1em\" viewBox=\"0 0 24 24\">"
    "</svg></div>"
    "<div style=\"display:flex;width:300px;align-items:flex-start\"><svg id=f2 style=\"height:20px\" "
    "viewBox=\"0 0 24 12\"></svg></div>"
    "<div style=\"display:flex;width:30px;align-items:flex-start\"><svg id=f3 style=\"height:20px\" "
    "viewBox=\"0 0 24 12\"></svg></div>"
    "<div style=\"display:flex;flex-direction:column;width:300px;align-items:flex-start\"><svg id=f4 "
    "style=\"width:40px\" viewBox=\"0 0 24 12\"></svg></div>"
    "<div style=\"display:flex;width:300px;align-items:flex-start\"><svg id=f5 style=\"height:20px;width:30px\" "
    "viewBox=\"0 0 24 12\"></svg></div>"
    "<script>"
    "function Z(i){var r=document.getElementById(i).getBoundingClientRect();return r.width+'x'+r.height;}"
    "var q='',i;for(i=1;i<=5;i++)q+=(i>1?'&':'')+'f'+i+'='+Z('f'+i);"
    "fetch('/api/flexreplaced?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
