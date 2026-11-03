/* THE TWO-ROOT DOCUMENT: under `?mode=dev` two `message` listeners write markup whose value joins the decoded
   fragment with message data, so a payload may ride either part and the one sink value is rooted in two (and in
   the second listener three) attacker sources. trJoin writes the fragment followed by one message's `trTail`.
   trStage needs two messages: one whose `trPre` it keeps in a script-level binding, and a later one whose
   `trPost` it joins after that prefix and the fragment. Nothing in the page posts a message. Every witness
   fetch carries constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TWOROOT_H
#define ENGINE_HOST_FIXTURE_TWOROOT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TWOROOT[] =
    "<!doctype html><html><body>"
    "<div id='trOut'></div><div id='trOut2'></div>"
    "<script>"
    "var trPrefix = null;"
    "if (new URLSearchParams(location.search).get('mode') === 'dev') {"
    "  fetch('/api/tr-listen');"
    "  window.addEventListener('message', function trJoin(e) {"
    "    var d = e.data;"
    "    if (!d || typeof d !== 'object' || typeof d.trTail !== 'string') return;"
    "    fetch('/api/tr-join');"
    "    document.getElementById('trOut').innerHTML = decodeURIComponent(location.hash.slice(1)) + d.trTail;"
    "  });"
    "  window.addEventListener('message', function trStage(e) {"
    "    var d = e.data;"
    "    if (!d || typeof d !== 'object') return;"
    "    if (typeof d.trPre === 'string') { trPrefix = d.trPre; fetch('/api/tr-prefix'); return; }"
    "    if (trPrefix !== null && typeof d.trPost === 'string') {"
    "      fetch('/api/tr-emit');"
    "      document.getElementById('trOut2').innerHTML ="
    "        trPrefix + decodeURIComponent(location.hash.slice(1)) + d.trPost;"
    "    }"
    "  });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
