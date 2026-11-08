/* THE LARGE GET-ANIMATIONS DOCUMENT (`gabig`): Web Animations' Animatable `getAnimations({ subtree: true })` over
   a container of eight thousand elements, one of which runs CSS Animations' `gbSpin`. The call's style change
   event visits every element, which is several work quanta of computed-style work, so the walk must rest
   between elements rather than hold the thread: the rows read the census of its rests and the elements it
   visited, and `n` is the one CSSAnimation the subtree holds.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GABIG_H
#define ENGINE_HOST_FIXTURE_GABIG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GABIG[] =
    "<!doctype html><html><head><style>"
    "@keyframes gbSpin { from { opacity: 0; } to { opacity: 1; } }"
    ".gb { animation: gbSpin 1s linear infinite; } i { color: red; } div i { color: blue; }"
    "</style></head><body><div id=gbBox></div>"
    "<script>"
    "var gbBox = document.getElementById('gbBox'), gbN = -1, gbE = '';"
    "gbBox.innerHTML = '<i></i>'.repeat(4000) + '<b class=gb></b>' + '<i></i>'.repeat(4000);"
    "try { gbN = gbBox.getAnimations({ subtree: true }).length; } catch (e) { gbE = e.name; }"
    "fetch('/api/gabig?n=' + gbN + '&e=' + gbE);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
