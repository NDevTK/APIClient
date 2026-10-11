/* THE FRAGMENT FORK DOCUMENT: a flow forked while HTML §13.4 "Parsing HTML fragments" is part-way through its
   markup. The fixture's zone refuses `/api/cold/held.js` (test_forced.c's reply arm), so flow_decline_fork forks
   this flow into a waiting arm and a failure arm at its next pick, and the setter's markup is long enough that
   the pick lands while the parse is still feeding bytes. Both arms then finish the same parse. One of
   test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGFORK_H
#define ENGINE_HOST_FIXTURE_FRAGFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGFORK[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "fetch('/api/cold/held.js').catch(function () {});"
    "var ffd = document.createElement('div');"
    "ffd.innerHTML = '<p>' + 'x'.repeat(200000) + '</p><i>tail</i>';"
    "fetch('/api/fragfork?v=' + ffd.textContent.length + '&t=' + ffd.lastChild.textContent);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
