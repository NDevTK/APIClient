/* THE MESSAGE-ORIGIN DOCUMENT: two `message` listeners, each writing `e.data.html` into its own element when the
   data's `kind` is 'show', behind two origin checks. moSub admits an origin holding `trusted.example` anywhere
   (`indexOf`), which an attacker meets with an origin it owns (a subdomain of its own site carrying the
   string); moExact admits only `https://trusted.example`, another site's origin, which no sender the attacker
   owns presents. Each listener reports its entry before its check. Nothing in the page posts a message.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGORIGIN_H
#define ENGINE_HOST_FIXTURE_MSGORIGIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGORIGIN[] =
    "<!doctype html><html><body>"
    "<div id='moA'></div><div id='moB'></div>"
    "<script>"
    "window.addEventListener('message', function moSub(e) {"
    "  fetch('/api/mo-sub-entered');"
    "  if (e.origin.indexOf('trusted.example') === -1) return;"
    "  if (e.data && e.data.kind === 'show') document.getElementById('moA').innerHTML = e.data.html;"
    "});"
    "window.addEventListener('message', function moExact(e) {"
    "  fetch('/api/mo-exact-entered');"
    "  if (e.origin !== 'https://trusted.example') return;"
    "  if (e.data && e.data.kind === 'show') document.getElementById('moB').innerHTML = e.data.html;"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
