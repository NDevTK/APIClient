/* THE PAGE-WRITE DOCUMENT: members of records an inline script built (published records, solver/absent.c) that
   the page's own program writes before reading. `s.b = 1` and a read of `s.b` in one script are one unit, so the
   write is proof by run-to-completion and the falsy arm must not exist. `q.b` and `r.b` are written by one
   listener and read by another, across a task boundary, so the falsy arm stays and its source names the
   assumption. `__PWFLAGS.admin` is a served literal the page only reads: it still forks. One of test_forced.c's
   exploring documents (TF_DOCS); it ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PAGEWRITE_H
#define ENGINE_HOST_FIXTURE_PAGEWRITE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PAGEWRITE[] =
    "<!doctype html><html><body>"
    "<script>"
    "window.__PWFLAGS = { admin: false };"
    "var q = {};"
    "document.addEventListener('DOMContentLoaded', function () { q.b = 1; });"
    "window.addEventListener('load', function () { if (!q.b) fetch('/api/pw-a1'); else fetch('/api/pw-b1'); });"
    "var r = {};"
    "window.addEventListener('load', function () { r.b = 1; });"
    "window.addEventListener('load', function () { if (!r.b) fetch('/api/pw-a2'); else fetch('/api/pw-b2'); });"
    "var s = {};"
    "s.b = 1;"
    "if (!s.b) fetch('/api/pw-a3'); else fetch('/api/pw-b3');"
    "</script>"
    "<script>"
    "if (__PWFLAGS.admin) fetch('/api/pw-flagadmin'); else fetch('/api/pw-flaguser');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
