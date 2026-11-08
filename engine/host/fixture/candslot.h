/* THE CANDIDATE-OWN-SLOT DOCUMENT: the fragment, decoded, is tested for its length and, where it is longer than
   three units, written to innerHTML; a branch over an absent global follows the write. The exploring flow forks at
   the length test, so the path an @S candidate for that sink is seeded on holds a decision over the candidate's own
   source before the sink, and a decision over the absent global after it. The witness fetches carry constants.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CANDSLOT_H
#define ENGINE_HOST_FIXTURE_CANDSLOT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CANDSLOT[] =
    "<!doctype html><html><body>"
    "<div id='cso'></div>"
    "<script>"
    "function csRun() {"
    "  var h = decodeURIComponent(location.hash.slice(1));"
    "  if (h.length > 3) {"
    "    document.getElementById('cso').innerHTML = h;"
    "    if (window.csFlag) fetch('/api/cs-flag'); else fetch('/api/cs-plain');"
    "  }"
    "}"
    "csRun();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
