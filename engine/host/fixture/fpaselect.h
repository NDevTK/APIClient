/* THE FILE-PICKER-SELECTION DOCUMENT: File System Access §3.2.1's filter (process accept types step 2.2) over an
   `accept` record that is a posted message's data, asked by §3.3 step 7.6's prompt of a file the page itself
   saved. A save picker under `suggestedName` creates `a.txt` in the starting directory; the open picker that
   follows excludes the all-files option, so `a.txt` is offered exactly where the record's unknown key or
   unknown suffixes admit it. The witness carries the picked file's name and the record's own value as the page
   then reads it, or the rejection's name marked `-none` where the prompt had nothing to offer, then the
   record's key count.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FPASELECT_H
#define ENGINE_HOST_FIXTURE_FPASELECT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FPASELECT[] =
    "<!doctype html><html><body>"
    "<script>"
    "window.addEventListener('message', async function (e) {"
    "  var acc = e.data, got;"
    "  if (acc === null || typeof acc !== 'object') return;"
    "  try {"
    "    await showSaveFilePicker({ suggestedName: 'a.txt' });"
    "    var hs = await showOpenFilePicker({ types: [{ accept: acc }], excludeAcceptAllOption: true });"
    "    got = 'picked-' + hs[0].name + '-' + acc[Object.keys(acc)[0]];"
    "  } catch (ex) { got = ex.name + (/without a selection/.test(ex.message) ? '-none' : ''); }"
    "  fetch('/api/fps-pick?got=' + got + '-' + Object.keys(acc).length);"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
