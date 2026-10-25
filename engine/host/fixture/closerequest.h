/* THE CLOSE-REQUEST DOCUMENT: a modal dialog shown at load, whose `close` listener fetches only once the dialog
   is closed. Nothing in the page closes it, so the witness is reached through the modelled potential close
   request (HTML §6.10.1), the person's Escape, and a drive of the listener alone finds the dialog still open.
   The question is whose act the witness request is: the close request is the victim's act performed by this
   tool, so the request must read as the tool's (driven), never as the page's own. The witness carries a
   constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CLOSEREQUEST_H
#define ENGINE_HOST_FIXTURE_CLOSEREQUEST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CLOSEREQUEST[] =
    "<!doctype html><html><body>"
    "<dialog id='crDialog'>crBody</dialog>"
    "<script>"
    "var crDialog = document.getElementById('crDialog');"
    "crDialog.addEventListener('close', function crOnClose() {"
    "  if (!crDialog.open) fetch('/api/cr-closed');"
    "});"
    "crDialog.showModal();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
