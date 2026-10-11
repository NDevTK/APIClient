/* THE WINDOW CLOSE DOCUMENT: HTML §7.2.2.1 "Opening and closing windows"' close() steps over a same-origin
   popup this document opened, so step 6 asks §7.3.2.2 "Related browsing contexts"' familiar with and its first
   step answers. One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK,
   which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WINDOWCLOSE_H
#define ENGINE_HOST_FIXTURE_WINDOWCLOSE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WINDOWCLOSE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* `S` when `closed` reads true, `s` when close() was refused, `E<name>` when it threw.
       The popup is a top-level traversable created by web content, so step 6's script-closable conjunct holds,
       and nothing here is sandboxed, so its sandboxing conjunct holds. The incumbent is familiar with the popup
       by the algorithm's step 1, same origin. Step 6.1 sets is closing before the task that definitely closes
       the traversable is queued, and the `closed` getter ("true if this's navigable is null or its is closing
       is true") reads it on the next line. */
    "var _wc = '';"
    "try { var _wcs = open(); _wcs.close(); _wc = _wcs.closed === true ? 'S' : 's'; } catch (e) { _wc = 'E' + e.name; }"
    "fetch('/api/winclose?v=' + _wc);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
