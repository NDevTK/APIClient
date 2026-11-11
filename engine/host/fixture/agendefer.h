/* THE ASYNC-GENERATOR DEFERRAL FORK DOCUMENT: a branch on an unknown (an injected global the document does not
   spell, which solver/absent.c mints) inside page code an async generator's queue runs while its drive is
   deferred: a promise's `constructor` getter read by §27.9.3.9 AsyncGeneratorAwaitReturn's PromiseResolve, and
   an iterator result's inherited `then` getter read by the resolve of §27.9.3.5 AsyncGeneratorCompleteStep. Each
   fork copies the deferred drive, so each arm's settle-time address is reached only if the sibling got its own.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_AGENDEFER_H
#define ENGINE_HOST_FIXTURE_AGENDEFER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_AGENDEFER[] =
    "<!doctype html><html><body>"
    "<script>"
    "var adU = window.__tfAgenDeferRole, adRet = 'none', adStep = 'none', adMark = {};"
    "async function* adRetGen() { yield 1; }"
    "var adP = Promise.resolve(7);"
    "Object.defineProperty(adP, 'constructor', { get: function () {"
    "  if (adU === 'adm') adRet = 'a'; else adRet = 'b';"
    "  return Promise;"
    "} });"
    "adRetGen().return(adP).then(function () { fetch('/api/ad/ret-' + adRet); });"
    "async function* adStepGen() { yield adMark; }"
    "Object.defineProperty(Object.prototype, 'then', { configurable: true, get: function () {"
    "  if (this.value === adMark) { if (adU === 'adm') adStep = 'a'; else adStep = 'b'; }"
    "  return undefined;"
    "} });"
    "adStepGen().next().then(function () { fetch('/api/ad/step-' + adStep); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
