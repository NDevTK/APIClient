/* THE KEY-DOMAIN DOCUMENT: reads keyed by an unknown String whose leading bytes the page wrote, jQuery's expando
   shape (`"kd" + Math.random()`). On an object no link of whose chain holds a key in that domain the read misses
   in every world, so no arm reads it present (kd-a1, kd-a2). A slot written through the same key reads back
   what was written (kd-b3). An object holding a key the domain contains (`kdNear`), or a name spent on another
   unknown key (`location.hash`), keeps both arms (kd-a4, kd-a5). The objects are built by `new Object()` so no
   published record is involved. The same holds of a Document and a WindowProxy, whose named properties are
   asked of the domain by a resting walk: no supported name or own key in it, so no hit arm (kd-a6, kd-a7, kd-a11);
   an `img` named in the domain keeps both (kd-a8, kd-a9), and so does a global whose name lies in it (kd-a10).
   No global variable of this page begins with a domain's prefix (`zq`, `zi`, `zg`). jQuery's own expando
   (`"jQuery" + ("3.7.1" + Math.random()).replace(/\D/g, "")`) is at least nine units long with only digits after
   its prefix, so the global `jQuery` (too short, kd-a12) and `znNotDigits` (a non-digit after `zn`, kd-a13) are
   outside it, and a global `jQuery123` keeps the hit arm (kd-a14). One of test_forced.c's exploring documents
   (TF_DOCS); it ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_KEYDOMAIN_H
#define ENGINE_HOST_FIXTURE_KEYDOMAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_KEYDOMAIN[] =
    "<!doctype html><html><body>"
    "<img name=\"ziNamedImageWhoseNameLiesInTheDomainOfTheKey\">"
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
    "var kdMissKey = 'zq' + Math.random();"
    "if (document[kdMissKey]) fetch('/api/kd-a6'); else fetch('/api/kd-b6');"
    "if (window[kdMissKey]) fetch('/api/kd-a7'); else fetch('/api/kd-b7');"
    "var kdImgKey = 'zi' + Math.random();"
    "if (document[kdImgKey]) fetch('/api/kd-a8'); else fetch('/api/kd-b8');"
    "if (window[kdImgKey]) fetch('/api/kd-a9'); else fetch('/api/kd-b9');"
    "var kdGlobKey = 'zg' + Math.random();"
    "window.zgGlobalWhoseNameLiesInTheDomainOfTheKey = 1;"
    "if (window[kdGlobKey]) fetch('/api/kd-a10'); else fetch('/api/kd-b10');"
    "if (document[kdGlobKey]) fetch('/api/kd-a11'); else fetch('/api/kd-b11');"
    "var kdJqKey = 'jQuery' + ('3.7.1' + Math.random()).replace(/\\D/g, '');"
    "window.jQuery = 1;"
    "if (window[kdJqKey]) fetch('/api/kd-a12'); else fetch('/api/kd-b12');"
    "var kdClsKey = 'zn' + ('3.7.1' + Math.random()).replace(/\\D/g, '');"
    "window.znNotDigits = 1;"
    "if (window[kdClsKey]) fetch('/api/kd-a13'); else fetch('/api/kd-b13');"
    "window.jQuery123 = 1;"
    "if (window[kdJqKey]) fetch('/api/kd-a14'); else fetch('/api/kd-b14');"
    "var kdOther = new Object(); kdOther[location.hash] = 1;"
    "if (kdOther[kdKey]) fetch('/api/kd-a5'); else fetch('/api/kd-b5');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
