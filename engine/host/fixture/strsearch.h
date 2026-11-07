/* THE STRING-SEARCH DOCUMENT: String.prototype.replace with a String searchValue over an address whose first
   units the page wrote and whose rest is unknown input, the address then resolved against the document and its
   host compared with the document's.
   - `#` lies neither in the page's path nor in encodeURIComponent's result, so the replace returns its receiver;
   - `#` may lie in the raw query after the page's path, so the result begins with that path;
   each keeps a path-absolute address, whose host is the document's: only the same-host arm runs.
   - the fragment itself, replaced the same way, leaves the host to the attacker, so both arms run.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STRSEARCH_H
#define ENGINE_HOST_FIXTURE_STRSEARCH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STRSEARCH[] =
    "<!doctype html><html><body>"
    "<script>"
    "var ssS = location.search.slice(1);"
    "function ssSame(url) { return new URL(url, location.href).host === location.host; }"
    "fetch(ssSame(('/api/ss-a/' + encodeURIComponent(ssS)).replace('#', '')) ? '/api/ss-none-same' : '/api/ss-none-cross');"
    "fetch(ssSame(('/api/ss-b/' + ssS).replace('#', '')) ? '/api/ss-tail-same' : '/api/ss-tail-cross');"
    "fetch(ssSame(location.hash.slice(1).replace('#', '')) ? '/api/ss-open-same' : '/api/ss-open-cross');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
