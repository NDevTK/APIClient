/* THE HTML-BYTES-REST DOCUMENT: an empty target `<div>` and markup for one `<b>` holding 800,000 characters of
   text. The page forks on `location.hash`: the example's arm sets the target's innerHTML, whose HTML §13.4 parse
   is fed a byte per step and charged for its bytes at the HTML parse's rate, more work than a quantum for this
   markup, so the setter rests inside its feed; the other arm reads the target empty. Each arm reports what it
   read. The markup holds two nodes, so the reading is the feed's and not a walk's.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HTMLBYTESREST_H
#define ENGINE_HOST_FIXTURE_HTMLBYTESREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HTMLBYTESREST[] =
    "<!doctype html><html><body>"
    "<div id=hbTarget style=display:none></div>"
    "<script>"
    "var hbT = document.getElementById('hbTarget');"
    "var hbM = '<b>' + 'x'.repeat(800000) + '</b>';"
    "if (location.hash === '#keep') {"
    "  fetch('/api/hb-kept?t=' + hbT.childNodes.length);"
    "} else {"
    "  hbT.innerHTML = hbM;"
    "  fetch('/api/hb-done?c=' + hbT.childNodes.length + '&l=' + hbT.firstChild.firstChild.data.length);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
