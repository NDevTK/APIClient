/* THE MESSAGE-HOST DOCUMENT: four `message` listeners, each comparing the sender's origin with a constant no
   host spells — one carrying `@` (userinfo), `#` (a fragment), `:` (a port) and an empty label (`..`) — and
   writing its own data member into markup when the comparison holds. A constant that is no host contributes
   no label to a composed origin, so each listener's candidates are the attacker's site alone. Nothing in the
   page posts a message.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGHOST_H
#define ENGINE_HOST_FIXTURE_MSGHOST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGHOST[] =
    "<!doctype html><html><body>"
    "<div id='mhA'></div><div id='mhB'></div><div id='mhC'></div><div id='mhD'></div>"
    "<script>"
    "window.addEventListener('message', function mhAt(e) {"
    "  if (e.origin.indexOf('trusted.example@evil.example') === -1) return;"
    "  document.getElementById('mhA').innerHTML = e.data.atMarkup;"
    "});"
    "window.addEventListener('message', function mhHash(e) {"
    "  if (e.origin.indexOf('trusted.example#frag') === -1) return;"
    "  document.getElementById('mhB').innerHTML = e.data.hashMarkup;"
    "});"
    "window.addEventListener('message', function mhPort(e) {"
    "  if (e.origin.indexOf('trusted.example:8443') === -1) return;"
    "  document.getElementById('mhC').innerHTML = e.data.portMarkup;"
    "});"
    "window.addEventListener('message', function mhDots(e) {"
    "  if (!e.origin.endsWith('trusted..example')) return;"
    "  document.getElementById('mhD').innerHTML = e.data.dotsMarkup;"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
