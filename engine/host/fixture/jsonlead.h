/* THE JSON LEAD DOCUMENT (`jsonlead`): ECMAScript §25.5.2 JSON.parse over a text derived from an attacker
   source, mdn's shape. `location.href` concatenates the query and fragment sources, and JSON.stringify of an
   object holding it is a JSON text beginning with `{`, so the parse completes and `k` is the one key, `u`. The
   source's declared prefix (`?`) is a fact about `location.search`'s own value, not about a text derived from
   it. One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_JSONLEAD_H
#define ENGINE_HOST_FIXTURE_JSONLEAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_JSONLEAD[] =
    "<!doctype html><html><body>"
    "<script>"
    "var jlS = JSON.stringify({ u: location.href });"
    "try { var jlO = JSON.parse(jlS); fetch('/api/jsonlead?k=' + Object.keys(jlO).join(',')); }"
    "catch (jle) { fetch('/api/jsonleadthrew?n=' + jle.name); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
