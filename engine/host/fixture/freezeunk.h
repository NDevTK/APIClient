/* THE FREEZE-UNKNOWN DOCUMENT: ECMAScript §7.3.15 SetIntegrityLevel ( obj, level ) over a stored record with no
   stored value, whose own keys are unknown. Step 1 makes the record non-extensible before step 3 enumerates it,
   so on the arm where it holds a member that member is named on a record that is already non-extensible, and
   step 5 freezes it: /api/fz-keys reports one key and isFrozen true there, and none on the empty arm. One of
   test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FREEZEUNK_H
#define ENGINE_HOST_FIXTURE_FREEZEUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FREEZEUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var fz = JSON.parse(localStorage.getItem('fz') || '{}');"
    "if (fz !== null && typeof fz === 'object' && !Array.isArray(fz)) {"
    "  Object.freeze(fz);"
    "  fetch('/api/fz-keys?n=' + Object.keys(fz).length + '&f=' + Object.isFrozen(fz));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
