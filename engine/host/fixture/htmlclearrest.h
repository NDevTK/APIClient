/* THE HTML-CLEAR-REST DOCUMENT: a `<div>` box whose one child is a `<div>` holding a named `<img>` (display:none),
   an `<i>` with an id and 100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice). The page forks on
   `location.hash`: the example's arm sets the box's innerHTML, whose HTML §8.5.4 step 5 replace all removes the
   inner div with remove's walks of its 100,002 children, more work than a quantum, so the setter rests inside
   itself; the other arm, which the scheduler can run while the first is parked mid-setter, reads the inner div in
   place with its children and both names present. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HTMLCLEARREST_H
#define ENGINE_HOST_FIXTURE_HTMLCLEARREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HTMLCLEARREST[] =
    "<!doctype html><html><body>"
    "<div id=hcBox><div id=hcOld><img name=hcNamed style=display:none><i id=hcInner></i>"
    COLLREST_ITEMS COLLREST_ITEMS "</div></div>"
    "<script>"
    "var hcBox = document.getElementById('hcBox'), hcOld = document.getElementById('hcOld');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/hc-kept?p=' + (hcOld.parentNode === hcBox ? 1 : 0) + '&c=' + hcOld.childNodes.length +"
    "        '&n=' + (document.hcNamed ? 1 : 0) + '&w=' + (window.hcInner ? 1 : 0));"
    "} else {"
    "  hcBox.innerHTML = '<b>done</b>';"
    "  fetch('/api/hc-done?c=' + hcBox.childNodes.length + '&t=' + (hcBox.firstChild.textContent === 'done' ? 1 : 0) +"
    "        '&o=' + (hcOld.parentNode === null ? 1 : 0) + '&n=' + (document.hcNamed === undefined ? 1 : 0) +"
    "        '&w=' + (window.hcInner === undefined ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
