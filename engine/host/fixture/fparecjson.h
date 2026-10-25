/* THE FILE-PICKER-ACCEPT-JSON DOCUMENT: File System Access §3.2.1's process accept types over an `accept` record
   whose value is a field of a fetched JSON reply. Web IDL §3.2.23 converts that value as §3.2.25's
   `(USVString or sequence<USVString>)`, so the field is one suffix or a list of them, and validate a suffix
   decides each: an invalid one rejects with a TypeError, while a valid one (or an empty list) passes step 2 and
   reaches §3.1 step 4, whose unactivated world rejects with a SecurityError. The witness carries the rejection's
   name, or 'picked'.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FPARECJSON_H
#define ENGINE_HOST_FIXTURE_FPARECJSON_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FPARECJSON[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var cfg = await (await fetch('/api/fpa-cfg')).json();"
    "  var got;"
    "  try {"
    "    await showOpenFilePicker({ types: [{ accept: { 'text/plain': cfg.exts } }] });"
    "    got = 'picked';"
    "  } catch (e) { got = e.name; }"
    "  fetch('/api/fpa-pick?got=' + got);"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
