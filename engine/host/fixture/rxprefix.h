/* THE REPLACE-PREFIX DOCUMENT: String.prototype.replace with a RegExp over an address whose first units the
   page wrote and whose rest is unknown input, the address then resolved against the document and its host
   compared with the document's, as a request library normalises a URL before its cross-domain test.
   - `/^\/\//` cannot match a subject that begins `/a`, so the replace returns its receiver;
   - `/#.*$/` cannot begin a match inside the page's path, so the result begins with that path;
   - `/x\/.*$/` may match from inside the page's units, so the result begins with the units before it;
   each keeps a path-absolute address, whose host is the document's: only the same-host arm runs.
   - the fragment itself, replaced the same way, leaves the host to the attacker, so both arms run.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXPREFIX_H
#define ENGINE_HOST_FIXTURE_RXPREFIX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXPREFIX[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rpQ = encodeURIComponent(location.search.slice(1));"
    "function rpSame(url) { return new URL(url, location.href).host === location.host; }"
    "fetch(rpSame(('/api/rp-path/' + rpQ).replace(/^\\/\\//, 'http://')) ? '/api/rp-same-a' : '/api/rp-cross-a');"
    "fetch(rpSame(('/api/rp-item/' + rpQ).replace(/#.*$/, '')) ? '/api/rp-same-b' : '/api/rp-cross-b');"
    "fetch(rpSame(('/api/rp-x/' + rpQ).replace(/x\\/.*$/, '')) ? '/api/rp-same-c' : '/api/rp-cross-c');"
    "fetch(rpSame(location.hash.slice(1).replace(/^\\/\\//, 'http://')) ? '/api/rp-open-same' : '/api/rp-open-cross');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
