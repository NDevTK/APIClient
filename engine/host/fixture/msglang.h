/* THE MESSAGE-LANGUAGE DOCUMENT: a `message` listener hands `e.data.s` to File System Access §3.2.1's process
   accept types as a suffix and renders `e.data.h` into markup only when the picker rejects with a SecurityError,
   which §3.1 step 4 does in the unactivated world once every suffix is valid. So the sink is reached only in
   the world where *validate a suffix* accepted the unknown, and a composed message proves it only when its `s`
   is a String that language holds: the shortest strings and an omitted member are all refused.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGLANG_H
#define ENGINE_HOST_FIXTURE_MSGLANG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGLANG[] =
    "<!doctype html><html><body>"
    "<div id='mlOut'></div>"
    "<script>"
    "window.addEventListener('message', async function mlOnMessage(e) {"
    "  var d = e.data;"
    "  if (d === null || typeof d !== 'object') return;"
    "  try {"
    "    await showOpenFilePicker({ types: [{ accept: { 'text/plain': [d.s] } }] });"
    "  } catch (ex) {"
    "    if (ex.name === 'SecurityError') document.getElementById('mlOut').innerHTML = d.h;"
    "  }"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
