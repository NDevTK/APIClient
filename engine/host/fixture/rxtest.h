/* THE REGEXP-TEST DOCUMENT: RegExp.prototype.test and exec over a subject that is unknown input, alone or after
   units the page wrote, as a request library asks whether a URL already holds a `?` before appending its query.
   - the fragment itself may or may not hold a `?`, so both arms of `/\?/.test` run;
   - `/^\/\//` cannot match a subject that begins `/a`, so `test` is false and `exec` is null in every world,
     and only that arm runs.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXTEST_H
#define ENGINE_HOST_FIXTURE_RXTEST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXTEST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rtQ = encodeURIComponent(location.search.slice(1));"
    "var rtH = location.hash.slice(1);"
    "fetch(/\\?/.test(rtH) ? '/api/rt-open-yes' : '/api/rt-open-no');"
    "fetch(/^\\/\\//.test('/api/rt-path/' + rtQ) ? '/api/rt-anch-yes' : '/api/rt-anch-no');"
    "fetch(/^\\/\\//.exec('/api/rt-exec/' + rtQ) === null ? '/api/rt-exec-null' : '/api/rt-exec-match');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
