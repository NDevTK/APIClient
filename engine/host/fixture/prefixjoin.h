/* THE PREFIX-JOIN DOCUMENT: msgprefix's shape (a first message's member kept in a script-level binding, written
   into markup joined with a later message's member) under the two factors that each kept its candidates from
   reaching the sink. pjFrag joins the decoded fragment between the two members, so the written value is also a
   function of an address component no message carries. pjTwo is one of two `message` listeners whose drives can
   share a chain with pjOther's, a listener that turns away every message that is not an object holding a String
   `oTail`. Nothing in the page posts a message. Every witness fetch carries constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PREFIXJOIN_H
#define ENGINE_HOST_FIXTURE_PREFIXJOIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PREFIXJOIN[] =
    "<!doctype html><html><body>"
    "<div id='pjFragOut'></div><div id='pjTwoOut'></div>"
    "<script>"
    "var pjFragPrefix = null, pjTwoPrefix = null;"
    "window.addEventListener('message', function pjOther(e) {"
    "  var d = e.data;"
    "  if (!d || typeof d !== 'object' || typeof d.oTail !== 'string') return;"
    "  fetch('/api/pj-other');"
    "});"
    "window.addEventListener('message', function pjFrag(e) {"
    "  var d = e.data;"
    "  if (!d || typeof d !== 'object') return;"
    "  if (typeof d.fPre === 'string') { pjFragPrefix = d.fPre; fetch('/api/pj-frag-prefix'); return; }"
    "  if (pjFragPrefix !== null && typeof d.fPost === 'string') {"
    "    fetch('/api/pj-frag-emit');"
    "    document.getElementById('pjFragOut').innerHTML ="
    "      pjFragPrefix + decodeURIComponent(location.hash.slice(1)) + d.fPost;"
    "  }"
    "});"
    "window.addEventListener('message', function pjTwo(e) {"
    "  var d = e.data;"
    "  if (!d || typeof d !== 'object') return;"
    "  if (typeof d.tPre === 'string') { pjTwoPrefix = d.tPre; fetch('/api/pj-two-prefix'); return; }"
    "  if (pjTwoPrefix !== null && typeof d.tPost === 'string') {"
    "    fetch('/api/pj-two-emit');"
    "    document.getElementById('pjTwoOut').innerHTML = pjTwoPrefix + d.tPost;"
    "  }"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
