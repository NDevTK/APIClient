/* THE REFLECTED-INTEGER DOCUMENT: HTML §2.6.1's `unsigned long` and `long` setters over fields of a fetched JSON
   reply, which reach the setter as unknowns whose examples are the served values. `canvas.width` takes an
   integer field and then a non-numeric one (Web IDL §3.2.4.9 ConvertToInt makes NaN 0), and `li.value` takes a
   negative one; each is read back through its getter and through the content attribute.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REFLECTINT_H
#define ENGINE_HOST_FIXTURE_REFLECTINT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REFLECTINT[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var c = await (await fetch('/api/rint')).json();"
    "  var cv = document.createElement('canvas');"
    "  cv.width = c.n; var n = cv.width, na = cv.getAttribute('width');"
    "  cv.width = c.s; var s = cv.width, sa = cv.getAttribute('width');"
    "  var li = document.createElement('li');"
    "  li.value = c.m; var m = li.value;"
    "  fetch('/api/rintback?n=' + n + '&na=' + na + '&s=' + s + '&sa=' + sa + '&m=' + m);"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
