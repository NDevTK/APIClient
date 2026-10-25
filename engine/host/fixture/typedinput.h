/* THE TYPED-INPUT DOCUMENT: an `input` listener on a text field writes the field's value to innerHTML. Nothing
   dispatches the event, so the handler is reached only as a drive, and the value is what a person typed into
   that element: the PoC must name the element and the typing, which no part of the document's address
   carries. The witness fetch precedes the write and carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TYPEDINPUT_H
#define ENGINE_HOST_FIXTURE_TYPEDINPUT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TYPEDINPUT[] =
    "<!doctype html><html><body>"
    "<input id='tiField' type='text'><div id='tiPreview'></div>"
    "<script>"
    "document.getElementById('tiField').addEventListener('input', function tiOnInput(e) {"
    "  fetch('/api/ti-input');"
    "  document.getElementById('tiPreview').innerHTML = document.getElementById('tiField').value;"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
