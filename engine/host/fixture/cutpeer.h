/* THE LONG-BOOT REFERENCED DOCUMENT: cutboot's setup and fragment sink in a document that embeds a cross-site
   frame. The frame is markup, so the parse creates its child navigable (HTML §7.3.1.3 Child navigables) before
   anything forks, and the peer hosting it holds a WindowProxy for this document, its parent, which makes this
   document one a peer references for the whole run.
   A candidate of the fragment can still start at a snapshot taken before its first read: what a peer routes
   while the snapshot is held waits on it for the arms forked off it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUTPEER_H
#define ENGINE_HOST_FIXTURE_CUTPEER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUTPEER[] =
    "<!doctype html><html><body>"
    "<iframe src='https://other.test/cutpeer'></iframe>"
    "<div id='cpOut'></div>"
    "<script>"
    "var cpAcc = 0;"
    "for (var cpI = 0; cpI < 4000000; cpI++) cpAcc = (cpAcc + cpI) % 1000003;"
    "document.getElementById('cpOut').innerHTML = decodeURIComponent(location.hash.slice(1));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
