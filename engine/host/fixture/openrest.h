/* THE OPEN-REST DOCUMENT: a `<div>` box holding a named `<img>` (display:none), an `<i>` with an id and 100,000
   empty `<i>` (test_forced.c's COLLREST_ITEMS, twice), which the page's program moves into an `<iframe>`'s
   initial about:blank document, then adds an `x` listener to that document, the box and the frame's Window. The
   page forks on `location.hash`: the example's arm calls the frame document's open(), whose HTML §8.4.1 step 9
   erases the listeners of every shadow-including inclusive descendant and whose step 11 replace all removes the
   document element with remove's walks of the box's 100,002 children, more work than a quantum, so the call
   rests inside itself; it then writes a `<p>` and closes. The other arm, which the scheduler can run while the
   first is parked mid-open, reads the box in place, both names present and all three listeners firing. Each arm
   reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_OPENREST_H
#define ENGINE_HOST_FIXTURE_OPENREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_OPENREST[] =
    "<!doctype html><html><body>"
    "<div id=orBox><img name=orNamed style=display:none><i id=orInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<script>"
    "var orF = document.createElement('iframe'); document.body.appendChild(orF);"
    "var orD = orF.contentDocument, orW = orF.contentWindow, orN = 0;"
    "var orBox = document.getElementById('orBox');"
    "orD.body.appendChild(orBox);"
    "orD.addEventListener('x', function () { orN += 1; });"
    "orBox.addEventListener('x', function () { orN += 10; });"
    "orW.addEventListener('x', function () { orN += 100; });"
    "function orFire() {"
    "  orN = 0; orD.dispatchEvent(new Event('x')); orBox.dispatchEvent(new Event('x'));"
    "  orW.dispatchEvent(new Event('x')); return orN;"
    "}"
    "if (location.hash === '#keep') {"
    "  fetch('/api/or-kept?p=' + (orBox.parentNode === orD.body ? 1 : 0) + '&c=' + orBox.childNodes.length +"
    "        '&n=' + (orD.orNamed ? 1 : 0) + '&w=' + (orW.orInner ? 1 : 0) + '&l=' + orFire());"
    "} else {"
    "  var orR = orD.open();"
    "  var orE = (orR === orD ? 1 : 0) + '-' + orD.childNodes.length + '-' + (orD.documentElement === null ? 1 : 0);"
    "  var orL = orFire();"
    "  orD.write('<p id=orP>x</p>'); orD.close();"
    "  fetch('/api/or-done?e=' + orE + '&c=' + orBox.childNodes.length + '&k=' + (orBox.isConnected ? 1 : 0) +"
    "        '&n=' + (orD.orNamed === undefined ? 1 : 0) + '&w=' + (orW.orInner === undefined ? 1 : 0) +"
    "        '&l=' + orL + '&q=' + (orD.body && orD.body.firstChild && orD.body.firstChild.id === 'orP' ? 1 : 0) +"
    "        '&p=' + (orD.getElementById('orP') !== null ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
