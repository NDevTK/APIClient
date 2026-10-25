/* THE REPLY-ROLE DOCUMENT: a fetched JSON reply decides `user.role === 'admin'`, and only the admin arm builds
   an address. The host serves `/api/rrme` as a viewer (TF_SERVED), so the admin address exists only on an arm
   the solver forced against the observed reply: it must be learned with its computed parts and graded
   `forced`, while the viewer arm's address is graded by the reply it really read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REPLYROLE_H
#define ENGINE_HOST_FIXTURE_REPLYROLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REPLYROLE[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var user = await (await fetch('/api/rrme')).json();"
    "  if (user.role === 'admin') {"
    "    var section = ['audit', 'log'].join('-');"
    "    fetch('/api/rradmin/' + section + '?uid=' + user.id + '&org=' + user.org.toUpperCase());"
    "  } else {"
    "    fetch('/api/rrviewer?uid=' + user.id);"
    "  }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
