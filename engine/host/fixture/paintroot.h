/* THE PAINTED ROOT DOCUMENT: HTML §8.1.7.3 update the rendering's step 22 paints a document whose document
   element's `class` is an unknown with no example (`state.*`), under an author rule keyed on a class. Painting
   reads the root's computed `display`, whose author match cannot decide `[class~="pr-on"]`: the paint asks the
   root's style under a resting drive and forks the predicate, rather than building the cascade where no arm can
   be kept. The witness fetch runs from a task queued after a frame was painted.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PAINTROOT_H
#define ENGINE_HOST_FIXTURE_PAINTROOT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PAINTROOT[] =
    "<!doctype html><html><head><style>.pr-on { background: #00f }</style></head><body>"
    "<script>"
    "document.documentElement.className = state.prCls;"
    "requestAnimationFrame(function () {"
    "  setTimeout(function () { fetch('/api/paintroot?w=prPAINTED'); }, 0);"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
