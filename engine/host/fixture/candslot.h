/* THE CANDIDATE-OWN-SLOT DOCUMENT: the fragment, decoded, is tested for its length and, where it is longer than
   three units, written to innerHTML; a branch over an absent global follows the write. The exploring flow forks at
   the length test, so the path an @S candidate for that sink is seeded on holds a decision over the candidate's own
   source before the sink, and a decision over the absent global after it. The witness fetches carry constants.
   Two more documents put the sink behind an arm a breakout cannot take: the parity of the fragment's first code
   unit, odd, where every markup breakout opens on `<` (even). In `candcontra` each arm then tests its own absent
   global, so a candidate's next question differs from its path's; in `candweak` nothing is asked after the parity
   test, so the contradiction meets no further recorded question. `candoutcome` asks each arm's next question
   through a step machine instead (JSON.parse's outcome over `window.name`, a source the candidate does not
   deliver, and over a slice of it), so the contradiction is met at a machine's outcome ask.
   `candarrive` puts an arrival's slot on the path too: inside the length test the page posts a request the
   unconfigured zone declines (the provider's `/api/decl` rule), and both of that request's reactions test the
   absent global and write the sink, so the path reads own-source slot, the decline's arrival, program question.
   test_forced.c's exploring documents (its TF_DOCS table). Each ends with TF_CANVAS_INK, which test_forced.c
   defines before including this. */
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

static const char HTML_CANDCONTRA[] =
    "<!doctype html><html><body>"
    "<div id='cco'></div>"
    "<script>"
    "function ccRun() {"
    "  var h = decodeURIComponent(location.hash.slice(1));"
    "  if (h.charCodeAt(0) % 2) {"
    "    if (window.ccOdd) fetch('/api/cc-odd');"
    "    document.getElementById('cco').innerHTML = h;"
    "  } else {"
    "    if (window.ccEven) fetch('/api/cc-even');"
    "  }"
    "}"
    "ccRun();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_CANDOUTCOME[] =
    "<!doctype html><html><body>"
    "<div id='coo'></div>"
    "<script>"
    "function coRun() {"
    "  var h = decodeURIComponent(location.hash.slice(1));"
    "  if (h.charCodeAt(0) % 2) {"
    "    try { JSON.parse(window.name); } catch (e) {}"
    "    document.getElementById('coo').innerHTML = h;"
    "  } else {"
    "    try { JSON.parse(window.name.slice(1)); } catch (e) {}"
    "  }"
    "}"
    "coRun();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";
static const char HTML_CANDARRIVE[] =
    "<!doctype html><html><body>"
    "<div id='cao'></div>"
    "<script>"
    "function caRun() {"
    "  var h = decodeURIComponent(location.hash.slice(1));"
    "  if (h.length > 3) {"
    "    var next = function () {"
    "      if (window.caFlag) fetch('/api/ca-flag'); else fetch('/api/ca-plain');"
    "      document.getElementById('cao').innerHTML = h;"
    "    };"
    "    fetch('/api/decl/ca', { method: 'POST', body: 'x' }).then(next, next);"
    "  }"
    "}"
    "caRun();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_CANDWEAK[] =
    "<!doctype html><html><body>"
    "<div id='cwo'></div>"
    "<script>"
    "function cwRun() {"
    "  var h = decodeURIComponent(location.hash.slice(1));"
    "  if (h.charCodeAt(0) % 2) document.getElementById('cwo').innerHTML = h;"
    "}"
    "cwRun();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
