/* THE MESSAGE-HOST DOCUMENT: five `message` listeners, each writing its own data member into markup when the
   sender's origin holds a constant. Four constants spell no host — one carrying `@` (userinfo), `#` (a
   fragment), `:` (a port) and an empty label (`..`) — so they contribute no label to a composed origin and each
   of those listeners' candidates is the attacker's site alone. The fifth is an IPv4 address: its labels join
   the site as a domain, while the name `x` prefixed to it ends in a number and is no domain. A sixth listener
   checks no origin and parses its data as JSON, writing the parsed value's markup member when its `kind` member
   is 'render': the attacker posts the String whose ParseJSON value carries both. Nothing in the page posts a
   message.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGHOST_H
#define ENGINE_HOST_FIXTURE_MSGHOST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGHOST[] =
    "<!doctype html><html><body>"
    "<div id='mhA'></div><div id='mhB'></div><div id='mhC'></div><div id='mhD'></div><div id='mhE'></div>"
    "<div id='mhF'></div>"
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
    "window.addEventListener('message', function mhIp(e) {"
    "  if (e.origin.indexOf('10.0.0.1') === -1) return;"
    "  document.getElementById('mhE').innerHTML = e.data.ipMarkup;"
    "});"
    "window.addEventListener('message', function mhJson(e) {"
    "  var d = JSON.parse(e.data);"
    "  if (d.kind === 'render') document.getElementById('mhF').innerHTML = d.jsonMarkup;"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
