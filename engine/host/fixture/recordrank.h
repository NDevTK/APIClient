/* THE RECORD-FACT DOCUMENTS: requests spanning the facts a record states (solver/endpoint.h's `credentials`,
   `destination`, `attackerNeeds` and `assumes`).
   `recordrank`, served with no opener policy and no framing refusal:
   - a credentialed POST to its own origin whose address and body carry `?q=`;
   - an uncredentialed GET to another origin carrying the document's address;
   - a request behind an absent global whose truthy arm the engine takes as present, and one composed from an
     absent global read past its throw;
   - an ungated request, and requests rooted only in `window.name`, only in the referrer, or in a message's data;
   - one request site sent both from `?q=` and from a message's data.
   `recordrankframe` is served with `Cross-Origin-Opener-Policy: same-origin` and `X-Frame-Options: DENY`, so no
   route lets an attacker page post it a message, and its message listener sends a request.
   Exploring documents of test_forced.c (its TF_DOCS table); each ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_RECORDRANK_H
#define ENGINE_HOST_FIXTURE_RECORDRANK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RECORDRANK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rrq = new URLSearchParams(location.search).get('q');"
    "if (rrq !== null) fetch('/api/rr-post?q=' + encodeURIComponent(rrq),"
    " { method: 'POST', credentials: 'include', body: rrq });"
    "fetch('https://rr-third.test/c?u=' + encodeURIComponent(location.href), { credentials: 'omit' });"
    "if (window.__rrUser) fetch('/api/rr-me');"
    "fetch('/api/rr-plain');"
    "if (window.name) fetch('/api/rr-name?n=' + encodeURIComponent(window.name));"
    "if (document.referrer) fetch('/api/rr-ref?r=' + encodeURIComponent(document.referrer));"
    "function rrBoth(v) { fetch('/api/rr-both?v=' + encodeURIComponent(v)); }"
    "if (rrq !== null) rrBoth(rrq);"
    "addEventListener('message', function (e) {"
    "  fetch('/api/rr-msg?d=' + encodeURIComponent(e.data));"
    "  rrBoth(e.data);"
    "});"
    "</script>"
    "<script>"
    "fetch('/api/rr-acct?i=' + __rrAcct.id);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_RECORDRANKFRAME[] =
    "<!doctype html><html><body>"
    "<script>"
    "addEventListener('message', function (e) { fetch('/api/rrf-msg?d=' + encodeURIComponent(e.data)); });"
    "fetch('/api/rrf-plain');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
