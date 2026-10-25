/* THE ASYNC-GENERATOR FORK DOCUMENT: a branch on an unknown (an injected global the document does not spell,
   which solver/absent.c mints) inside an async generator's body while `for await` drives it, and inside a
   default parameter while a call binds it. Each fork copies an async generator frame that is live on the
   chain, so each arm's address is reached only if the sibling got its own record.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_AGENFORK_H
#define ENGINE_HOST_FIXTURE_AGENFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_AGENFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var afU = window.__tfAgenRole;"
    "async function* afBody() {"
    "  if (afU === 'adm') fetch('/api/af/body-a'); else fetch('/api/af/body-b');"
    "  yield 1;"
    "  fetch('/api/af/after-yield');"
    "}"
    "(async function () { for await (var afV of afBody()) {} })();"
    "async function* afParam(p = (afU === 'adm' ? fetch('/api/af/param-a') : fetch('/api/af/param-b'))) {"
    "  yield 1;"
    "}"
    "afParam().next();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
