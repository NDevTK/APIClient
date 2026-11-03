/* THE CLOSURE-FLAG DOCUMENT: flagflow's two-handler shape with the flag held in an IIFE's closure and set by a
   `message` listener whose data must equal 'cf-unlock'. The click handler writes the query's `q` parameter to
   innerHTML only while the flag is set, so the one sink needs a message, a URL parameter and a click, the
   message before the click. The click without the message is the control and reaches no sink. Nothing in the
   page posts a message or dispatches the click. Every witness fetch carries constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CLOSUREFLAG_H
#define ENGINE_HOST_FIXTURE_CLOSUREFLAG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CLOSUREFLAG[] =
    "<!doctype html><html><body>"
    "<button id='cfShow'>show</button><div id='cfOut'></div>"
    "<script>"
    "(function () {"
    "  var armed = false;"
    "  window.addEventListener('message', function cfMessage(e) {"
    "    if (e.data === 'cf-unlock') { armed = true; fetch('/api/cf-armed'); }"
    "  });"
    "  document.getElementById('cfShow').addEventListener('click', function cfShowClick() {"
    "    if (armed) {"
    "      fetch('/api/cf-show-armed');"
    "      document.getElementById('cfOut').innerHTML = new URLSearchParams(location.search).get('q');"
    "    } else {"
    "      fetch('/api/cf-show-unarmed');"
    "    }"
    "  });"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
