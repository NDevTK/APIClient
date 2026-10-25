/* THE FILE-PARSE (DROP) DOCUMENT: the fileparse-fetch format's parser fed by a file the person drops on the
   page: the `drop` handler takes `dataTransfer.files[0]`, reads it with `arrayBuffer()`, and walks the same
   FPX1 format into `new Function`. The drop is the victim's act and the file is the attacker's bytes, so the
   sink's source is the dropped file and the PoC names the drop. Nothing dispatches the drop. Each witness
   fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSEDROP_H
#define ENGINE_HOST_FIXTURE_FILEPARSEDROP_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_DROP[] =
    "<!doctype html><html><body>"
    "<div id='fpdZone' style='height: 100px'></div>"
    "<script>"
    "function fpdParse(buf) {"
    "  var dv = new DataView(buf);"
    "  if (dv.getUint32(0) !== 0x46505831 || dv.getUint8(4) !== 1) { fetch('/api/fpd-badmagic'); return null; }"
    "  var len = dv.getUint8(5);"
    "  var body = new TextDecoder().decode(new Uint8Array(buf, 6, len));"
    "  fetch('/api/fpd-parsed');"
    "  return new Function('x', body);"
    "}"
    "var fpdZone = document.getElementById('fpdZone');"
    "fpdZone.addEventListener('dragover', function fpdOnDragOver(e) { e.preventDefault(); });"
    "fpdZone.addEventListener('drop', function fpdOnDrop(e) {"
    "  e.preventDefault();"
    "  fetch('/api/fpd-drop');"
    "  e.dataTransfer.files[0].arrayBuffer().then(function (buf) {"
    "    fetch('/api/fpd-read');"
    "    fpdParse(buf);"
    "  });"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
