/* THE CYCLIC REPLACED WIDTH DOCUMENT: css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes"
   for a replaced box (a 200x100 canvas) whose `width: 50%` is cyclic inside a shrink-to-fit inline-block. Its
   max-content contribution takes `width` as `auto` (200, the natural width) and its min-content contribution
   resolves the percentage against zero (0), so CSS 2.1 §10.3.9's shrink-to-fit width is min(max(0, available),
   200): 50 in a 50px container and 200 in a 500px one, with the canvas at half of each. The steps answer
   a=50 ac=25 b=200 bc=100.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CYCLICREPLACED_H
#define ENGINE_HOST_FIXTURE_CYCLICREPLACED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CYCLICREPLACED[] =
    "<!doctype html><html><body>"
    "<div style=\"width:50px\"><span id=crA style=\"display:inline-block\">"
    "<canvas id=crAc width=200 height=100 style=\"width:50%\"></canvas></span></div>"
    "<div style=\"width:500px\"><span id=crB style=\"display:inline-block\">"
    "<canvas id=crBc width=200 height=100 style=\"width:50%\"></canvas></span></div>"
    "<script>"
    "function crW(id) { return Math.round(document.getElementById(id).getBoundingClientRect().width); }"
    "fetch('/api/crwidth?a=' + crW('crA') + '&ac=' + crW('crAc') + '&b=' + crW('crB') + '&bc=' + crW('crBc'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
