/* THE STORED-XSS DOCUMENT: two values a previous visit stored are read on load and written to innerHTML, one
   from localStorage and one from a cookie. Neither is in this document's address, so a reproduction must first
   store the value and then load the page: the PoC states the store step before the load.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STOREDXSS_H
#define ENGINE_HOST_FIXTURE_STOREDXSS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STOREDXSS[] =
    "<!doctype html><html><body>"
    "<div id='sxLs'></div><div id='sxCk'></div>"
    "<script>"
    "var sxNote = localStorage.getItem('sxNote');"
    "if (sxNote !== null) {"
    "  fetch('/api/sx-ls-read');"
    "  document.getElementById('sxLs').innerHTML = sxNote;"
    "}"
    "</script>"
    "<script>"
    "var sxJar = document.cookie;"
    "var sxAt = sxJar.indexOf('sxBanner=');"
    "if (sxAt >= 0) {"
    "  fetch('/api/sx-ck-read');"
    "  document.getElementById('sxCk').innerHTML = decodeURIComponent(sxJar.slice(sxAt + 9).split(';')[0]);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
