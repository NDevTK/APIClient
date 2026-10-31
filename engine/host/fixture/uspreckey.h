/* THE URLSEARCHPARAMS-RECORD-KEY DOCUMENT: a posted message's member names a property of a plain object, so
   the object's one key is an unknown with no example, and URL §6.2's constructor converts that object as a
   Web IDL §3.2.23 record. Step 4.2.1 converts the key the cursor hands on, the unknown itself, so the
   serialized list is an unknown too and the page's comparison of it with "admin=v" has a world where it holds.
   The witness carries the comparison's answer.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_USPRECKEY_H
#define ENGINE_HOST_FIXTURE_USPRECKEY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_USPRECKEY[] =
    "<!doctype html><html><body>"
    "<script>"
    "window.addEventListener('message', function (e) {"
    "  var d = e.data, o = {};"
    "  if (d === null || typeof d !== 'object') return;"
    "  o[d.k] = 'v';"
    "  var q = '' + new URLSearchParams(o);"
    "  fetch('/api/usk?eq=' + (q === 'admin=v'));"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
