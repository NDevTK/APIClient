/* THE TWIN-READ DOCUMENTS: one source read twice in one document. `twinread` reaches its eval sinks only where a
   derived read of the address carries the payload while a plain read of the same component is empty, which no
   one delivered URL produces: a fired record there is a re-fire world holding two values of one source.
   `onceread` is the control, the same derived reads with no second read, which must fire.
   Exploring documents of test_forced.c (its TF_DOCS table); each ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_TWINREAD_H
#define ENGINE_HOST_FIXTURE_TWINREAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TWINREAD[] =
    "<!doctype html><html><body>"
    "<script>"
    "var twKey = new URLSearchParams(location.search).get('x');"
    "var twSearch = location.search;"
    "if (twKey !== null && twSearch === '') { eval(twKey); }"
    "</script>"
    "<script>"
    "var twFrag = location.hash.slice(1);"
    "var twHash = location.hash;"
    "if (twFrag !== '' && twHash === '') { eval(decodeURIComponent(twFrag)); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_ONCEREAD[] =
    "<!doctype html><html><body>"
    "<script>"
    "var onKey = new URLSearchParams(location.search).get('x');"
    "if (onKey !== null) { eval(onKey); }"
    "</script>"
    "<script>"
    "var onFrag = location.hash.slice(1);"
    "if (onFrag !== '') { eval(decodeURIComponent(onFrag)); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
