/* THE MESSAGE-PREFIX DOCUMENT: tworoot's trStage without the query gate or the fragment. One `message` listener
   keeps a first message's `mpPre` in a script-level binding and, on a later message carrying `mpPost`, writes
   the kept prefix joined with it into markup. The joined value's identity is the earlier message's member, so
   a payload placed there must survive to a later delivery before it reaches the sink. Nothing in the page
   posts a message. Every witness fetch carries constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGPREFIX_H
#define ENGINE_HOST_FIXTURE_MSGPREFIX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGPREFIX[] =
    "<!doctype html><html><body>"
    "<div id='mpOut'></div>"
    "<script>"
    "var mpPrefix = null;"
    "window.addEventListener('message', function mpStage(e) {"
    "  var d = e.data;"
    "  if (!d || typeof d !== 'object') return;"
    "  if (typeof d.mpPre === 'string') { mpPrefix = d.mpPre; fetch('/api/mp-prefix'); return; }"
    "  if (mpPrefix !== null && typeof d.mpPost === 'string') {"
    "    fetch('/api/mp-emit');"
    "    document.getElementById('mpOut').innerHTML = mpPrefix + d.mpPost;"
    "  }"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
