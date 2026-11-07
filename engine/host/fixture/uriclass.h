/* THE URI-CLASS DOCUMENT: encodeURIComponent and encodeURI over unknown input, whose results hold only the units
   ECMA-262 §19.2.6.5 Encode leaves unescaped or writes for an escape, asked of by a RegExp.
   - `#` is in neither set for encodeURIComponent, so `/#/.test` is false in every world and only that arm runs;
   - `%` begins every escape, so `/%/.test` may answer either way and both arms run;
   - `#` is in encodeURI's extra unescaped set, so over encodeURI both arms of `/#/.test` run.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_URICLASS_H
#define ENGINE_HOST_FIXTURE_URICLASS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_URICLASS[] =
    "<!doctype html><html><body>"
    "<script>"
    "var ucS = location.search.slice(1);"
    "var ucC = encodeURIComponent(ucS);"
    "fetch(/#/.test(ucC) ? '/api/uc-hash-yes' : '/api/uc-hash-no');"
    "fetch(/%/.test(ucC) ? '/api/uc-pct-yes' : '/api/uc-pct-no');"
    "fetch(/#/.test(encodeURI(ucS)) ? '/api/uc-uri-yes' : '/api/uc-uri-no');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
