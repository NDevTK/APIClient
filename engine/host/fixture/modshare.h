/* The shared-module documents: a classic script branches over an absent global, so two worlds stand before the
   root module script that follows, and each world's module run sends `/api/ms-mod` with its arm's letter.
   `modshare` holds one module row; its rows read what the module's compile was charged to and how many times
   its body was parsed. `modshare2` holds two module rows whose bodies have equal length and differ in one
   character, the control: two bodies are two parses. `modsharebig` holds one module row whose body is longer
   than several quanta of parse, so every world begins it before any finishes. test_forced.c's exploring
   documents (its TF_DOCS table); each ends with TF_CANVAS_INK, which test_forced.c defines before including
   this. */
#ifndef ENGINE_HOST_FIXTURE_MODSHARE_H
#define ENGINE_HOST_FIXTURE_MODSHARE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define MS_FORK "<script>var msArm = window.__tfMsFlag ? 'A' : 'P'; fetch('/api/ms-arm?w=' + msArm);</script>"
#define MS_MODULE_OPEN "<script type='module'>"
#define MS_BODY(tag)                                                                 \
    "const msTable = [1, 2, 3, 5, 8, 13, 21, 34].map((v, i) => v * (i + 1));"        \
    "function msSum(a) { let s = 0; for (const v of a) s += v; return s; }"          \
    "fetch('/api/" tag "?w=' + msArm + '&s=' + msSum(msTable));"
#define MS_X16(s) s s s s s s s s s s s s s s s s
#define MS_PAD MS_X16(MS_X16(MS_X16("msN = (msN * 31 + 7) % 1000003;\n")))

static const char HTML_MODSHARE[] =
    "<!doctype html><html><body>"
    MS_FORK
    MS_MODULE_OPEN MS_BODY("ms-mod") "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_MODSHARE2[] =
    "<!doctype html><html><body>"
    MS_FORK
    MS_MODULE_OPEN MS_BODY("m2-one") "</script>"
    MS_MODULE_OPEN MS_BODY("m2-two") "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_MODSHAREBIG[] =
    "<!doctype html><html><body>"
    MS_FORK
    MS_MODULE_OPEN "let msN = 1;\n" MS_PAD "fetch('/api/mb-mod?w=' + msArm + '&n=' + msN);" "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
