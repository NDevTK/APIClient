/* THE FORKED-FRAME RECLAMATION DOCUMENT: the script forks on the fragment first; in the '#rf' arm alone it reads a
   srcless <iframe>, which materializes the frame's realm inside that timeline after its fork, then removes the
   frame. Two tasks later, after the removal's queued destruction of the frame's Document has run, it calls the
   fixture's `gcNow()` and reports `childRealmCount()`, the agent's live child realms: the removed frame's realm is
   reachable from nothing once its Document is destroyed, so the collection has freed it. The other arm never reads
   the frame.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REALMFREE_H
#define ENGINE_HOST_FIXTURE_REALMFREE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REALMFREE[] =
    "<!doctype html><html><body>"
    "<iframe id='rff'></iframe>"
    "<script>"
    "function rfArm() {"
    "  var f = document.getElementById('rff');"
    "  if (location.hash === '#rf') {"
    "    (function () { var d = f.contentDocument; d.body.appendChild(d.createElement('p')); })();"
    "    f.remove();"
    "    f = null;"
    "    setTimeout(function rfWait() {"
    "      setTimeout(function rfCount() { gcNow(); fetch('/api/rf-count?n=' + childRealmCount()); }, 0);"
    "    }, 0);"
    "  } else {"
    "    fetch('/api/rf-other');"
    "  }"
    "}"
    "rfArm();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
