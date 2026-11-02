/* THE REQUEST-ROUTE DOCUMENT: one sink two drive chains reach. A button's click listener sends a credentialed
   request to an address a query value supplies from its fourth click on, a request-class finding whose proof is
   the four clicks. `rrPrime`, a function the page never calls, sets the count to three, so the chain that drives
   `rrPrime` and then the listener reaches the sink in two drives, where the listener's own chain takes four
   events. `rrPrime` also sends a request of its own and is built first, so the orphan take, which hands over
   bodies that spell a request first and the earliest built among those, offers it before the listener. A browser
   never calls `rrPrime`, so only the listener's chain can carry the PoC, whichever chain the search's detection
   saw first.
   Exploring documents of test_forced.c (its TF_DOCS table); each ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_REQUESTROUTE_H
#define ENGINE_HOST_FIXTURE_REQUESTROUTE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REQUESTROUTE[] =
    "<!doctype html><html><body>"
    "<button id='rrGoBtn'>Go</button>"
    "<script>"
    "var rrQ = new URLSearchParams(location.search);"
    "var rrClicks = 0;"
    "function rrPrime() { rrClicks = 3; fetch('/api/rr-prime'); }"
    "document.getElementById('rrGoBtn').addEventListener('click', function () {"
    "  rrClicks++;"
    "  if (rrClicks >= 4) fetch(rrQ.get('go'), { credentials: 'include' });"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

/* THE REQUEST-REDRIVE DOCUMENT: `requestroute` without `rrPrime`. The one chain reaching the sink drives the same
   listener four times, so a candidate of the search replays a chain whose members repeat one body. */
static const char HTML_REQUESTREDRIVE[] =
    "<!doctype html><html><body>"
    "<button id='rdGoBtn'>Go</button>"
    "<script>"
    "var rdQ = new URLSearchParams(location.search);"
    "var rdClicks = 0;"
    "document.getElementById('rdGoBtn').addEventListener('click', function () {"
    "  rdClicks++;"
    "  if (rdClicks === 4) fetch(rdQ.get('go'), { credentials: 'include' });"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
