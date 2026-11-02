/* THE ENVIRONMENT-SINK DOCUMENT: a string derived from Math.random, an environment unknown, and one derived from the
   URL's fragment, an attacker source, each reach the markup sink through innerHTML. Only the second is a value an
   attacker sets, so exactly one @S search opens, and the first is counted as an arrival no declared source roots
   (`_sinkUndeclared`). Each write is followed by a request, the reachability witness its row reads. One of
   test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ENVSINK_H
#define ENGINE_HOST_FIXTURE_ENVSINK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ENVSINK[] =
    "<!doctype html><html><body>"
    "<div id='esEnv'></div><div id='esAtk'></div>"
    "<script>"
    "var esTag = String(Math.random()).slice(2);"
    "document.getElementById('esEnv').innerHTML = esTag;"
    "fetch('/api/es-env');"
    "document.getElementById('esAtk').innerHTML = location.hash.slice(1);"
    "fetch('/api/es-atk');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
