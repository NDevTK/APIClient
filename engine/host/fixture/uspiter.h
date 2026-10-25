/* THE URLSEARCHPARAMS-ITERATION DOCUMENT: a URLSearchParams parsed from `location.search` is walked by for-of,
   by forEach and by keys(), then mutated and read. Its value pairs are a function of the unknown query, so
   every walk forks per index (a world with no pair there, and one with a pair whose name and value derive from
   the query), and a read after a mutation derives from the mutated list, as does that list sent as a request
   body. Witness fetches carry constants,
   except the iteration counts, which are concrete in each world.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_USPITER_H
#define ENGINE_HOST_FIXTURE_USPITER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_USPITER[] =
    "<!doctype html><html><body>"
    "<div id='uspOut'></div>"
    "<script>"
    "var uspP = new URLSearchParams(location.search);"
    "var uspSeen = 0;"
    "for (var uspE of uspP) {"
    "  uspSeen++;"
    "  if (uspE[0] === 'next') fetch('/api/uspi-key');"
    "  if (uspSeen > 1) break;"
    "}"
    "fetch('/api/uspi-done?n=' + uspSeen);"
    "</script>"
    "<script>"
    "var uspQ = new URLSearchParams(location.search);"
    "uspQ.forEach(function uspEach(v, k) { if (v === 'on') fetch('/api/uspf-val'); });"
    "var uspK = uspQ.keys().next();"
    "fetch('/api/uspk-first?done=' + (uspK.done ? 'yes' : 'no'));"
    "</script>"
    "<script>"
    "var uspM = new URLSearchParams(location.search);"
    "uspM.append('a', '1');"
    "uspM.delete('b');"
    "uspM.set('c', 'yes');"
    "if (uspM.get('mode') === 'admin') fetch('/api/uspm-get');"
    "document.getElementById('uspOut').innerHTML = uspM.get('q');"
    "fetch('/api/uspb', { method: 'POST', body: uspM });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
