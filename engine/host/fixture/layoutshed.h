/* The layout shed document: a page that forks on `location.hash` and in each arm reads a box (CSSOM View §6
   getBoundingClientRect) whose block layout takes many of a resting drive's activations, then reports its top.
   test_forced.c arms its forced pressure act (TF_DOCS's `shed`): at every slice's pressure point every resting
   layout job pressure may shed is shed (engine_force_layout_shed), so each arm re-derives what its job had laid out.
   Run with `--pressure-act none` it is its own no-pressure twin, whose schedule and work must be the act's.
   One of test_forced.c's exploring documents (TF_DOCS); it ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_LAYOUTSHED_H
#define ENGINE_HOST_FIXTURE_LAYOUTSHED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

/* Sixty-four rows of eight blocks with a line of text each, laid out before the last block's top is known. One
   block of each row holds an inline-block, whose shrink-to-fit width (CSS 2.2 §10.3.9) reads css-sizing-3 §5.1's
   measurement of its run, so a shed job holds intrinsic size answers as well as line box fills. */
#define TF_LS_8 "<div>ls</div><div>ls</div><div>ls</div><div><span style=\"display:inline-block\">ls</span></div>" \
                "<div>ls</div><div>ls</div><div>ls</div><div>ls</div>"
#define TF_LS_64 TF_LS_8 TF_LS_8 TF_LS_8 TF_LS_8 TF_LS_8 TF_LS_8 TF_LS_8 TF_LS_8
#define TF_LS_512 TF_LS_64 TF_LS_64 TF_LS_64 TF_LS_64 TF_LS_64 TF_LS_64 TF_LS_64 TF_LS_64

static const char HTML_LAYOUTSHED[] =
    "<!doctype html><html><body>"
    TF_LS_512
    "<div id=lsLast>ls</div>"
    "<script>"
    "var lsArm = location.hash === '#ls' ? 'a' : 'b';"
    "var lsTop = document.getElementById('lsLast').getBoundingClientRect().top;"
    "fetch('/api/ls-top?arm=' + lsArm + '&y=' + Math.round(lsTop));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
