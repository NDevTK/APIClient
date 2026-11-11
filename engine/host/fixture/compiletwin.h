/* The compile twin documents: a page that forks on `location.hash` in its first script, and in each arm reports
   how far the arm's clock (HR-TIME §7.1 now(), a function of the work the arm's path retired) moved from the end of
   that script to the start of the next, a program of about 150 KiB whose compile is the span's work. One arm first
   spins for several times the compile's work, so the other has compiled the program whole before it reaches the
   row and finds the body's parse held. `compiletwin`'s second script is classic and arm `b` spins;
   `compiletwinmod`'s is a module and arm `a` spins, so the two documents take the two orders. Each arm is charged
   the compile its own path asked for, so the two report one duration, to within one of HR-TIME §4's 100
   microsecond grid steps that the two ends of each are floored onto. Each loop is a yield poll, where the
   interpreter banks the work charged before it into the clock.
   test_forced.c's exploring documents (TF_DOCS); each ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_COMPILETWIN_H
#define ENGINE_HOST_FIXTURE_COMPILETWIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define TF_CTW_X16(s) s s s s s s s s s s s s s s s s
#define TF_CTW_PAD TF_CTW_X16(TF_CTW_X16(TF_CTW_X16("ctwV = (ctwV * 31 + 7) % 1000003;\n")))
#define TF_CTW_FORK(spin)                                                                             \
    "<script>var ctwArm = location.hash === '#ctw' ? 'a' : 'b';"                                     \
    "for (var ctwI = 0, ctwN = ctwArm === '" spin "' ? 200000 : 3; ctwI < ctwN; ctwI++) {}"          \
    "var ctwT0 = performance.now();</script>"
#define TF_CTW_BODY(path)                                                                             \
    "for (var ctwJ = 0; ctwJ < 3; ctwJ++) {}\n"                                                       \
    "var ctwT1 = performance.now();\n"                                                                \
    "var ctwV = 1;\n"                                                                                 \
    TF_CTW_PAD                                                                                        \
    "fetch('" path "?arm=' + ctwArm + '&d=' + Math.round((ctwT1 - ctwT0) * 10) + '&v=' + ctwV);"

static const char HTML_COMPILETWIN[] =
    "<!doctype html><html><body>"
    TF_CTW_FORK("b")
    "<script>" TF_CTW_BODY("/api/ctw-cls") "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_COMPILETWINMOD[] =
    "<!doctype html><html><body>"
    TF_CTW_FORK("a")
    "<script type=module>" TF_CTW_BODY("/api/ctw-mod") "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef TF_CTW_BODY
#undef TF_CTW_FORK
#undef TF_CTW_PAD
#undef TF_CTW_X16

#endif
