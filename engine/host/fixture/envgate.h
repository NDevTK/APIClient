/* THE ENVIRONMENT-GATE DOCUMENT: `navigator.language` and `matchMedia('(max-width: 600px)')` each gate two
   branches with an address of their own. Both are environment values (core/frame/navigator.c and
   core/css/media_query.c mint them opaque for control flow, with a desktop Chrome's value as the example), so
   every arm of each gate runs, not just the one the modelled machine would take. Each address is a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ENVGATE_H
#define ENGINE_HOST_FIXTURE_ENVGATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ENVGATE[] =
    "<!doctype html><html><body>"
    "<script>"
    "if (navigator.language.slice(0, 2) === 'de') fetch('/api/eg-lang-de');"
    "else fetch('/api/eg-lang-other');"
    "</script>"
    "<script>"
    "if (matchMedia('(max-width: 600px)').matches) fetch('/api/eg-mq-narrow');"
    "else fetch('/api/eg-mq-wide');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
