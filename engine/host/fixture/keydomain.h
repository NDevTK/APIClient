/* THE KEY-DOMAIN DOCUMENT: reads keyed by an unknown String whose leading bytes the page wrote, jQuery's expando
   shape (`"kd" + Math.random()`). On an object no link of whose chain holds a key in that domain the read misses
   in every world, so no arm reads it present (kd-a1, kd-a2). A slot written through the same key reads back
   what was written (kd-b3). An object holding a key the domain contains (`kdNear`), or a name spent on another
   unknown key (`location.hash`), keeps both arms (kd-a4, kd-a5). The objects are built by `new Object()` so no
   published record is involved. One of test_forced.c's exploring documents (TF_DOCS); it ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_KEYDOMAIN_H
#define ENGINE_HOST_FIXTURE_KEYDOMAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_KEYDOMAIN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var kdKey = 'kd' + Math.random();"
    "var kdPlain = new Object();"
    "if (kdPlain[kdKey]) fetch('/api/kd-a1'); else fetch('/api/kd-b1');"
    "var kdEl = document.createElement('div');"
    "if (kdEl[kdKey]) fetch('/api/kd-a2'); else fetch('/api/kd-b2');"
    "var kdHeld = new Object(); kdHeld[kdKey] = 1;"
    "if (kdHeld[kdKey]) fetch('/api/kd-b3'); else fetch('/api/kd-a3');"
    "var kdNear = new Object(); kdNear.kdNear = 1;"
    "if (kdNear[kdKey]) fetch('/api/kd-a4'); else fetch('/api/kd-b4');"
    "var kdOther = new Object(); kdOther[location.hash] = 1;"
    "if (kdOther[kdKey]) fetch('/api/kd-a5'); else fetch('/api/kd-b5');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
