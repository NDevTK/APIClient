/* THE SHADOW-ATTACH-REST DOCUMENT: a shadow host whose declarative shadow root holds a `<slot>`, which the host's
   `<span slot=a>` is assigned to, a `<b>` and 100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice), all
   children of the root. The page forks on `location.hash`: the example's arm calls the host's
   attachShadow({mode: 'open'}), whose DOM §4.9 attach a shadow root step 4.3 removes the declarative root's
   100,002 children, more work than a quantum, so the call rests inside itself; a second call then throws step
   4.2's "NotSupportedError", the root no longer being declarative. The other arm, which the scheduler can run
   while the first is parked mid-call, reads the root's children in place and the span still assigned. Each arm
   reports what it read. The host is display:none, as fixture/slotrest.h's is.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SHADOWATTACHREST_H
#define ENGINE_HOST_FIXTURE_SHADOWATTACHREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SHADOWATTACHREST[] =
    "<!doctype html><html><body>"
    "<div id=saHost style=display:none><template shadowrootmode=open><slot name=a></slot><b></b>"
    COLLREST_ITEMS COLLREST_ITEMS "</template><span id=saSpan slot=a>x</span></div>"
    "<script>"
    "var saHost = document.getElementById('saHost'), saRoot = saHost.shadowRoot;"
    "var saSpan = document.getElementById('saSpan');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/sa-kept?c=' + saRoot.childNodes.length + '&a=' + (saSpan.assignedSlot !== null ? 1 : 0));"
    "} else {"
    "  var saWas = saSpan.assignedSlot !== null ? 1 : 0, saFirst = saRoot.firstChild;"
    "  var saR = saHost.attachShadow({mode: 'open'}), saT = 'none';"
    "  try { saHost.attachShadow({mode: 'open'}); } catch (e) { saT = e.name; }"
    "  fetch('/api/sa-done?s=' + (saR === saRoot ? 1 : 0) + '&c=' + saRoot.childNodes.length + '&w=' + saWas +"
    "        '&a=' + (saSpan.assignedSlot === null ? 1 : 0) + '&p=' + (saFirst.parentNode === null ? 1 : 0) +"
    "        '&t=' + saT);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
