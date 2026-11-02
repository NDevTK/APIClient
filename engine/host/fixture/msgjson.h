/* THE MESSAGE-JSON DOCUMENT: one `message` listener that checks no origin, parses its data as JSON and writes the
   parsed value's `markup` member into an element when its `kind` member is 'render'. HTML §9.3.3 Posting
   messages hands the listener the structured-clone value the attacker posted, so the attacker posts the String
   whose ParseJSON value holds both members. Nothing in the page posts a message.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGJSON_H
#define ENGINE_HOST_FIXTURE_MSGJSON_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGJSON[] =
    "<!doctype html><html><body>"
    "<div id='mjA'></div>"
    "<script>"
    "window.addEventListener('message', function mjRender(e) {"
    "  var d = JSON.parse(e.data);"
    "  if (d.kind === 'render') document.getElementById('mjA').innerHTML = d.markup;"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
