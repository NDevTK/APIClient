/* THE FILE-PICKER-ACCEPT-KEY DOCUMENT: File System Access §3.2.1's process accept types over an `accept` record
   that is a posted message's data, so its keys are unknown (a message an attacker posts carries no example to
   read them from). Step 2.1.1 parses each key as a MIME type and
   steps 2.1.2-2.1.3 refuse a failure or a type with parameters with a TypeError; a valid key passes on to its
   suffixes and the call then reaches §3.1 step 4, whose unactivated world rejects with a SecurityError. The
   witness carries the rejection's name, marked `-key` where this engine's message names the key's refusal,
   then the record's own key count as the page's Object.keys reads it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FPAKEY_H
#define ENGINE_HOST_FIXTURE_FPAKEY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FPAKEY[] =
    "<!doctype html><html><body>"
    "<script>"
    "window.addEventListener('message', async function (e) {"
    "  var acc = e.data, got;"
    "  if (acc === null || typeof acc !== 'object') return;"
    "  try {"
    "    await showOpenFilePicker({ types: [{ accept: acc }] });"
    "    got = 'picked';"
    "  } catch (ex) { got = ex.name + (/accept key/.test(ex.message) ? '-key' : ''); }"
    "  fetch('/api/fpk-pick?got=' + got + '-' + Object.keys(acc).length);"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
