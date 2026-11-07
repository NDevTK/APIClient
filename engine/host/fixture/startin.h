/* THE START-IN DOCUMENT (`startin`): File System Access §3.2.2's `startIn`, a `(WellKnownDirectory or
   FileSystemHandle)` member, over absent application state, tldraw's shape. Web IDL §3.2.25 sends a value that
   is no FileSystemHandle to §3.2.18, so the unknown forks into the six WellKnownDirectory worlds, where the
   picker is shown, and the one where the string is none of them, a TypeError. The witness carries the
   rejection's name, or `shown` where the picker resolved.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STARTIN_H
#define ENGINE_HOST_FIXTURE_STARTIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STARTIN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var _sicfg = { dir: state.siDir };"
    "(async function () {"
    "  var r;"
    "  try { await showDirectoryPicker({ startIn: _sicfg.dir }); r = 'shown'; }"
    "  catch (e) { r = e.name; }"
    "  fetch('/api/startin?r=' + r);"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
