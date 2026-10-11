/* THE SRCDOC DOCUMENT: HTML §4.8.5's iframe srcdoc documents and §2.4.3's about base URL.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SRCDOC_H
#define ENGINE_HOST_FIXTURE_SRCDOC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SRCDOC[] =
    "<!doctype html><html>"
    "<body>"
    /* ---- HTML §4.8.5 "The `iframe` element"'s SRCDOC DOCUMENTS — the class whose §2.4.3 arm the asserts in
       core/dom/document.c are about (step 1's "Assert: document's about base URL is non-null." and the pairing
       checked at the record's birth); without a srcdoc document no run can reach them.
       IT IS MARKUP AND NOT SCRIPT, for the realm budget: a frame a `<script>` created would be created by every
       flow that reached it, while markup is created once by the parse, before anything forks. Every address
       is an absolute literal, reported by the endpoint surface as written. THE ROWS, in localising order:
         /api/sdrun    the armed control — the srcdoc Document was built and its script ran.
         /api/sdbase   §4.2.3 "The base element" freezes `/api/` against §2.4.3's fallback base URL, whose
                       step 1 for this class is "Return document's about base URL"; read through
                       `new Request('sdbase').url`, Fetch's own parse against the API base URL.
         /api/sdparse  DOMParser inside the frame: addressed `about:srcdoc` with a NULL about base URL and not
                       of §4.8.5's class (that is said only of a srcdoc NAVIGATION's Document).
         /api/sdclone  `document.cloneNode(true)`, the same creation by its other caller, written first so a
                       realm with no DOMParser cannot take it down. */
    "<iframe id=sdf srcdoc=\"<base href='/api/'>"
    "<script>"
    "fetch('https://x.test/api/sdrun?v=ran');"
    "fetch('https://x.test/api/sdbase?v=' + (new Request('sdbase').url === 'https://x.test/api/sdbase'"
    " ? 'rel' : 'wrong'));"
    "var sdc = document.cloneNode(true);"
    "fetch('https://x.test/api/sdclone?v=' + (sdc !== document ? 'cloned' : 'wrong'));"
    "var sdp = new DOMParser().parseFromString('<p id=sdpp>x</p>', 'text/html');"
    "fetch('https://x.test/api/sdparse?v=' + (sdp.getElementById('sdpp') ? 'built' : 'wrong'));"
    "</script>\"></iframe>"
    /* AND THE SECOND FRAME IS THE OTHER DIRECTION: `<iframe src="about:srcdoc">` takes §4.8.5's `Otherwise`
       arm, while §7.4.2.2 "Beginning navigation" still gives it an about base URL ("Set documentState's about
       base URL to initiatorBaseURLSnapshot."), so §2.4.3 must reach step 3 and answer its OWN address.
       ITS ROWS READ §4.5's `URL` BEFORE THE BASE URL: a frame whose navigation has not landed still holds its
       initial `about:blank` Document, which §2.4.3 step 2 correctly answers with the creator's base URL, so the
       base URL alone cannot tell that from a wrong step 1. The arms are `initial` (not landed: the schedule),
       `ownaddr` (the standard), `inherited` (an address-shaped step 1) and `otheraddr` (neither); the rows
       assert NOT `inherited`, accepting `initial`.
       The sync read is that frame's reachability witness; the timer read is the subject, a timer rather than
       a `load` handler because an absent `load` would be a silence where an arm can be a value. */
    "<iframe id=sdo src=\"about:srcdoc\"></iframe>"
    "<script>"
    "var sdo = document.getElementById('sdo');"
    "function sdoRead(d) {"
    " return (d.URL === 'about:blank') ? 'initial'"
    "  : (d.URL !== 'about:srcdoc') ? 'otheraddr'"
    "  : (d.baseURI === 'about:srcdoc') ? 'ownaddr' : 'inherited'; }"
    "fetch('/api/sdsync?v=' + sdoRead(sdo.contentWindow.document));"
    "setTimeout(function(){"
    " fetch('/api/sdotherwise?v=' + sdoRead(sdo.contentWindow.document)); }, 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
