/* THE STORED-REQUEST DOCUMENT: requests whose address, header or body carry a value a previous task or load of
   this page stored from an attacker source. A stored value is attacker input only where this page itself
   stored it from one of those, so each request is security-relevant only through the page's own write, and its
   record states that chain as storedxss's PoC does: the write's delivery, and for a later load the attacker
   opening the address again.
   - A load carrying `?ref=` saves it as localStorage's `srRef`; a later load sends it in a request's address.
   - A load carrying a fragment saves it as `srTok`; a later load sends it in a POST's header and body.
   - A load carrying `?note=` saves it as `srNote`, and a timer task of the same load sends it in an address.
   - `srNever` is a key the page never writes, sent in an address: a precondition, so a plain API record.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STOREDREQ_H
#define ENGINE_HOST_FIXTURE_STOREDREQ_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STOREDREQ[] =
    "<!doctype html><html><body>"
    "<script>"
    "var srRef = localStorage.getItem('srRef');"
    "if (srRef !== null) {"
    "  fetch('/api/sr-track?r=' + encodeURIComponent(srRef));"
    "} else {"
    "  var srAsk = new URLSearchParams(location.search).get('ref');"
    "  if (srAsk !== null) {"
    "    localStorage.setItem('srRef', srAsk);"
    "    fetch('/api/sr-ref-saved');"
    "  }"
    "}"
    "</script>"
    "<script>"
    "var srTok = localStorage.getItem('srTok');"
    "if (srTok !== null) {"
    "  fetch('/api/sr-sync', { method: 'POST', headers: { 'X-Sr-Tok': srTok }, body: srTok });"
    "} else if (location.hash.length > 1) {"
    "  localStorage.setItem('srTok', decodeURIComponent(location.hash.slice(1)));"
    "  fetch('/api/sr-tok-saved');"
    "}"
    "</script>"
    "<script>"
    "var srNoteAsk = new URLSearchParams(location.search).get('note');"
    "if (srNoteAsk !== null) localStorage.setItem('srNote', srNoteAsk);"
    "setTimeout(function () {"
    "  var srNote = localStorage.getItem('srNote');"
    "  if (srNote !== null) fetch('/api/sr-note?n=' + encodeURIComponent(srNote));"
    "}, 0);"
    "</script>"
    "<script>"
    "var srNever = localStorage.getItem('srNever');"
    "if (srNever !== null) fetch('/api/sr-never?k=' + encodeURIComponent(srNever));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
