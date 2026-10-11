/* THE OPENER CLOSE DOCUMENT: HTML §7.2.2.1 "Opening and closing windows"' close() steps over a cross-origin
   popup this document opened, so step 6's familiar with (§7.3.2.2 "Related browsing contexts") holds only
   through the popup's opener link. One of test_forced.c's exploring documents (its TF_DOCS table). It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WINDOWOPENERCLOSE_H
#define ENGINE_HOST_FIXTURE_WINDOWOPENERCLOSE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WINDOWOPENERCLOSE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* `X` when `closed` reads true, `x` when close() was refused, `E<name>` when it threw.
       Steps 1, 2 and 4 of familiar with fail (another origin, not the incumbent's top-level browsing context,
       no ancestor), and step 3 holds because the popup is an auxiliary browsing context whose opener browsing
       context is this document's, with which the incumbent is same origin. So the walk climbs the popup's
       `opener` link before it answers. Step 6.1 then sets is closing and queues the task that definitely
       closes the traversable. Another instance holds the popup's document, so after this document's fetch
       is issued that task sends a `windowproxy.close` record and parks on the peer's answer. */
    "var _wo = '';"
    "try { var _wox = open('https://other.test/p'); _wox.close(); _wo = _wox.closed === true ? 'X' : 'x'; }"
    " catch (e) { _wo = 'E' + e.name; }"
    "fetch('/api/winopclose?v=' + _wo);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
