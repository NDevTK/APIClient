/* THE REGEXP-SEARCH DOCUMENT: String.prototype.search and RegExp.prototype[@@search] over an address whose first
   units the page wrote and whose rest is unknown input.
   - `#` lies neither in the page's path nor in encodeURIComponent's result, so the search is -1 in every world
     and only that arm runs;
   - `?` may lie in the raw query after the page's path, so the search is -1 in some worlds and an index in
     others: both arms run, through String.prototype.search and through @@search called directly.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXSEARCH_H
#define ENGINE_HOST_FIXTURE_RXSEARCH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXSEARCH[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rsS = location.search.slice(1);"
    "fetch(('/api/rs-path/' + encodeURIComponent(rsS)).search(/#/) === -1 ? '/api/rs-none' : '/api/rs-some');"
    "fetch(('/api/rs-q/' + rsS).search(/\\?/) === -1 ? '/api/rs-q-none' : '/api/rs-q-some');"
    "fetch(/\\?/[Symbol.search]('/api/rs-s/' + rsS) === -1 ? '/api/rs-s-none' : '/api/rs-s-some');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
