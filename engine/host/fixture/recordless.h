/* THE RECORDLESS-FRAME DOCUMENT: a branch on an unknown (an injected global the document does not spell, which
   solver/absent.c mints) inside each callee the engine runs on its chain with no continuation record — a setter body, an iterator's
   `next` driven by for-of, the same `next` driven by a generator's `yield*`, and an iterator's `return` on a
   for-of `break`. Each branch forks the flow with that callee's frame on the chain, so each arm's address is
   reached only if the fork could copy the frame.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RECORDLESS_H
#define ENGINE_HOST_FIXTURE_RECORDLESS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RECORDLESS[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rlU = window.__tfRlRole;"
    "var rlO = { set v(x) { if (rlU === 'adm') fetch('/api/rl/setter-a'); else fetch('/api/rl/setter-b'); } };"
    "rlO.v = 1;"
    "function rlIter(a, b) {"
    "  var n = 0;"
    "  return { [Symbol.iterator]() { return this; },"
    "           next() { if (n++) return { done: true };"
    "                    if (rlU === 'adm') fetch(a); else fetch(b);"
    "                    return { value: 1, done: false }; } };"
    "}"
    "for (var rlE of rlIter('/api/rl/next-a', '/api/rl/next-b')) {}"
    "function* rlGen() { yield* rlIter('/api/rl/star-a', '/api/rl/star-b'); }"
    "for (var rlF of rlGen()) {}"
    "var rlR = { [Symbol.iterator]() { return this; }, next() { return { value: 1, done: false }; },"
    "            return() { if (rlU === 'adm') fetch('/api/rl/return-a'); else fetch('/api/rl/return-b');"
    "                       return {}; } };"
    "for (var rlG of rlR) break;"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
