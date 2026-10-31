/* THE TRAVERSE NAVIGATE EVENT DOCUMENT: HTML §7.4.6.1 "Updating the traversable"'s apply the history step, whose
   step 5 fires a traverse navigate event (§7.2.6.10.4) before a `history.back()` is applied. The page pushes an
   entry, then listens for `navigate` on the Navigation and goes back: the event is a "traverse", cancelable
   (§7.2.6.10.4 inner step 8: a top-level traversable, same document, user involvement "none"), and its
   `preventDefault()` cancels the traversal, so the document stays at the pushed entry's `#b`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TRAVNAV_H
#define ENGINE_HOST_FIXTURE_TRAVNAV_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TRAVNAV[] =
    "<!doctype html><html><body>"
    "<script>"
    "history.pushState({a:1},'','#b');"
    "navigation.addEventListener('navigate',function(e){"
    "  fetch('/api/travnav?t='+e.navigationType+'&c='+e.cancelable+'&s='+e.destination.sameDocument);"
    "  e.preventDefault();"
    "});"
    "history.back();"
    "setTimeout(function(){fetch('/api/travnavend?h='+encodeURIComponent(location.hash));},200);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
