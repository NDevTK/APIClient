/* THE HTML-REST DOCUMENT: an empty target `<div>` and markup for a `<div>` holding a named `<img>` (display:none),
   an `<i>` with an id and 100,000 empty `<i>`. The page forks on `location.hash`: the example's arm sets the
   target's innerHTML, whose HTML §8.5.4 step 5 replace all inserts the parsed box with insert step 7's walks of
   its 100,002 children, more work than a quantum, so the setter rests inside itself; the other arm, which the
   scheduler can run while the first is parked mid-setter, reads the target empty and both names absent. Each arm
   reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HTMLREST_H
#define ENGINE_HOST_FIXTURE_HTMLREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HTMLREST[] =
    "<!doctype html><html><body>"
    "<div id=hrTarget></div>"
    "<script>"
    "var hrT = document.getElementById('hrTarget');"
    "var hrM = '<div id=hrBox><img name=hrNamed style=display:none><i id=hrInner></i>' + '<i></i>'.repeat(100000) +"
    "          '</div>';"
    "if (location.hash === '#keep') {"
    "  fetch('/api/hr-kept?t=' + hrT.childNodes.length + '&n=' + (document.hrNamed === undefined ? 1 : 0) +"
    "        '&w=' + (window.hrInner === undefined ? 1 : 0));"
    "} else {"
    "  hrT.innerHTML = hrM;"
    "  var hrB = document.getElementById('hrBox');"
    "  fetch('/api/hr-done?p=' + (hrB && hrB.parentNode === hrT ? 1 : 0) + '&c=' + (hrB ? hrB.childNodes.length : 0) +"
    "        '&n=' + (document.hrNamed ? 1 : 0) + '&w=' + (window.hrInner ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
