/* The shared-module documents: a classic script branches over an absent global, so two worlds stand before the
   root module script that follows, and each world's module run sends `/api/ms-mod` with its arm's letter.
   `modshare` holds one module row; its rows read what the module's compile was charged to. One of
   test_forced.c's exploring documents (its TF_DOCS table); it ends with TF_CANVAS_INK, which test_forced.c
   defines before including this. */
#ifndef ENGINE_HOST_FIXTURE_MODSHARE_H
#define ENGINE_HOST_FIXTURE_MODSHARE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MODSHARE[] =
    "<!doctype html><html><body>"
    "<script>var msArm = window.__tfMsFlag ? 'A' : 'P'; fetch('/api/ms-arm?w=' + msArm);</script>"
    "<script type='module'>"
    "const msTable = [1, 2, 3, 5, 8, 13, 21, 34].map((v, i) => v * (i + 1));"
    "function msSum(a) { let s = 0; for (const v of a) s += v; return s; }"
    "fetch('/api/ms-mod?w=' + msArm + '&s=' + msSum(msTable));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
