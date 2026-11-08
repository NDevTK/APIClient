/* THE CSS ANIMATION EVENTS DOCUMENT (`gaevents`): CSS Animations Level 2 §5.1's event dispatch over a `<p>` running
   two 50ms iterations of `geFade`. Update the rendering generates and plays the animation, and the frames after it
   cross its phases, so the document receives `animationstart` (elapsedTime 0), `animationiteration` (0.05) and
   `animationend` (0.1), bubbling from the owning element, each carrying the CSSAnimation that fired it. The
   page also constructs one AnimationEvent to check the dictionary.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GAEVENTS_H
#define ENGINE_HOST_FIXTURE_GAEVENTS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GAEVENTS[] =
    "<!doctype html><html><head><style>"
    "@keyframes geFade { from { opacity: 0; } to { opacity: 1; } }"
    ".ge { animation: geFade 50ms linear 2; }"
    "</style></head><body><p class=ge id=geP>x</p>"
    "<script>"
    "var geSeen = [], geOk = true, geC = new AnimationEvent('x', { animationName: 'n', elapsedTime: 2 });"
    "function geOn(e) {"
    "  geSeen.push(e.type.slice(9) + ':' + e.elapsedTime);"
    "  geOk = geOk && e.target.id === 'geP' && e.animationName === 'geFade' && e.animation instanceof CSSAnimation;"
    "  if (e.type === 'animationend')"
    "    fetch('/api/gaevents?seen=' + geSeen.join(',') + '&ok=' + geOk + '&c=' + geC.animationName + geC.elapsedTime);"
    "}"
    "['animationstart', 'animationiteration', 'animationend'].forEach(function (t) {"
    "  document.addEventListener(t, geOn); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
