/* The CSS parse twin document: a page that forks on `location.hash`; in each arm it turns on one large style sheet
   (its media from `print` to `all`), resolves a style over it, and reports how far the arm's clock (HR-TIME §7.1
   now(), a function of the work the arm's path retired) moved across the resolution. Arm `b` first spins for
   several times the parse's work, so arm `a` has run the parse whole before `b` asks the parse table for the same
   emission. Each loop is a yield poll, where the interpreter banks the work charged before it into the clock.
   Each arm is charged the parse its own path asked for, so the two report one duration, to within one of
   HR-TIME §4's 100 microsecond grid steps that the two ends of each are floored onto.
   One of test_forced.c's exploring documents (TF_DOCS); it ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_CSSPARSETWIN_H
#define ENGINE_HOST_FIXTURE_CSSPARSETWIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

/* Four thousand distinct rules, about 200 KiB emitted, so the sheet's parse runs in a few dozen work units and
   charges about a millisecond of the clock, ten of HR-TIME §4's 100 microsecond grid steps. */
#define TF_CPT_R(p) ".cpt" p " .x{color:rgb(1,2,3);margin:1px 2px}"
#define TF_CPT_10(p) TF_CPT_R(p "0") TF_CPT_R(p "1") TF_CPT_R(p "2") TF_CPT_R(p "3") TF_CPT_R(p "4") \
    TF_CPT_R(p "5") TF_CPT_R(p "6") TF_CPT_R(p "7") TF_CPT_R(p "8") TF_CPT_R(p "9")
#define TF_CPT_100(p) TF_CPT_10(p "0") TF_CPT_10(p "1") TF_CPT_10(p "2") TF_CPT_10(p "3") TF_CPT_10(p "4") \
    TF_CPT_10(p "5") TF_CPT_10(p "6") TF_CPT_10(p "7") TF_CPT_10(p "8") TF_CPT_10(p "9")
#define TF_CPT_1000(p) TF_CPT_100(p "0") TF_CPT_100(p "1") TF_CPT_100(p "2") TF_CPT_100(p "3") TF_CPT_100(p "4") \
    TF_CPT_100(p "5") TF_CPT_100(p "6") TF_CPT_100(p "7") TF_CPT_100(p "8") TF_CPT_100(p "9")

static const char HTML_CSSPARSETWIN[] =
    "<!doctype html><html><head><style media=print>"
    TF_CPT_1000("a") TF_CPT_1000("b") TF_CPT_1000("c") TF_CPT_1000("d")
    "#cptEl{color:rgb(9,8,7)}</style></head><body>"
    "<div id=cptEl>cpt</div>"
    "<script>"
    "var cptArm = location.hash === '#cpt' ? 'a' : 'b';"
    "for (var cptI = 0, cptN = cptArm === 'b' ? 100000 : 3; cptI < cptN; cptI++) {}"
    "var cptT0 = performance.now();"
    "document.styleSheets[0].media.mediaText = 'all';"
    "var cptColor = getComputedStyle(document.getElementById('cptEl')).color;"
    "for (var cptJ = 0; cptJ < 3; cptJ++) {}"
    "var cptT1 = performance.now();"
    "fetch('/api/cpt-now?arm=' + cptArm + '&c=' + (cptColor === 'rgb(9, 8, 7)' ? 'y' : 'n') +"
    "      '&d=' + Math.round((cptT1 - cptT0) * 10));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef TF_CPT_1000
#undef TF_CPT_100
#undef TF_CPT_10
#undef TF_CPT_R

#endif
