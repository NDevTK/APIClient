/* The frame pair document. One of test_forced.c's exploring documents (its TF_DOCS table). Two requests are
   issued together beside an animation frame callback, and the host answers both in one payment. HTML §8.1.7.3
   Processing model determines a rendering opportunity from the display and the page, so the frame can fall
   before the first arrival, between the two, or after both, as a timer's deadline can (fixture/races.h's pair).
   Each reaction and the callback report the order they ran in. It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAMEPAIR_H
#define ENGINE_HOST_FIXTURE_FRAMEPAIR_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAMEPAIR[] =
    "<!doctype html><html><body>"
    "<script>"
    "var fpSeen = [];"
    "function fpDone(t) { fpSeen.push(t); if (fpSeen.length === 3) fetch('/api/fprorder?v=' + fpSeen.join('-')); }"
    "fetch('/api/fpra').then(function () { fpDone('a'); });"
    "fetch('/api/fprb').then(function () { fpDone('b'); });"
    "requestAnimationFrame(function () { fpDone('frame'); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
