/* THE POC-GATES DOCUMENT: one sink's path pins two other components of the address beside the query value that
   reaches it, and another sink's path pins a stored value. The first PoC must state the fragment and the second
   query tuple, each through its own transform: the fragment `#/r&d` is placed raw, while the tuple `mode` must
   carry `r&d` serialized as `r%26d`, or its `&` would end the tuple. The second sink is reached only where
   this origin's own storage holds `beta`, which no attacker writes, so its finding is a precondition.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_POCGATES_H
#define ENGINE_HOST_FIXTURE_POCGATES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_POCGATES[] =
    "<!doctype html><html><body>"
    "<div id='pgOut'></div><div id='pgPre'></div>"
    "<script>"
    "var pgParams = new URLSearchParams(location.search);"
    "if (localStorage.getItem('pgMode') === 'beta') {"
    "  fetch('/api/pg-stored');"
    "  document.getElementById('pgPre').innerHTML = window.name;"
    "} else if (location.hash === '#/r&d' && pgParams.get('mode') === 'r&d') {"
    "  fetch('/api/pg-gated');"
    "  document.getElementById('pgOut').innerHTML = pgParams.get('q');"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
