/* THE MESSAGE-ORIGIN DOCUMENT: four `message` listeners, each writing `e.data.markup` into its own element when
   the data's `kind` is 'show', behind an origin check. An attacker meets moSub's `indexOf('trusted.example')`
   with a subdomain of its own site carrying the string, and moSuffix's `endsWith('trusted.example')` with a
   registrable domain of its own ending in it; no origin it owns meets moDotSuffix's
   `endsWith('.trusted.example')` (a subdomain of another site) or moExact's `https://trusted.example`. Each
   listener reports its entry before its check. Nothing in the page posts a message.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGORIGIN_H
#define ENGINE_HOST_FIXTURE_MSGORIGIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGORIGIN[] =
    "<!doctype html><html><body>"
    "<div id='moA'></div><div id='moB'></div><div id='moC'></div><div id='moD'></div>"
    "<script>"
    "window.addEventListener('message', function moSub(e) {"
    "  fetch('/api/mo-sub-entered');"
    "  if (e.origin.indexOf('trusted.example') === -1) return;"
    "  if (e.data && e.data.kind === 'show') document.getElementById('moA').innerHTML = e.data.markup;"
    "});"
    "window.addEventListener('message', function moSuffix(e) {"
    "  fetch('/api/mo-suffix-entered');"
    "  if (!e.origin.endsWith('trusted.example')) return;"
    "  if (e.data && e.data.kind === 'show') document.getElementById('moC').innerHTML = e.data.markup;"
    "});"
    "window.addEventListener('message', function moDotSuffix(e) {"
    "  fetch('/api/mo-dotsuffix-entered');"
    "  if (!e.origin.endsWith('.trusted.example')) return;"
    "  if (e.data && e.data.kind === 'show') document.getElementById('moD').innerHTML = e.data.markup;"
    "});"
    "window.addEventListener('message', function moExact(e) {"
    "  fetch('/api/mo-exact-entered');"
    "  if (e.origin !== 'https://trusted.example') return;"
    "  if (e.data && e.data.kind === 'show') document.getElementById('moB').innerHTML = e.data.markup;"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
