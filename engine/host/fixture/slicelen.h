/* THE SLICE-LENGTH DOCUMENT: String.prototype.slice over an unknown String from its own length, as a request
   library splits an address into its part before the fragment and the rest (`url.slice(head.length)`, where
   the head is the address itself when it holds no fragment).
   - from the String's own length, and from the length of the replace that returned it unchanged, the result is
     the empty String in every world: only that arm runs;
   - from one less than the length of a String that may be empty, the result may or may not be empty: both arms.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SLICELEN_H
#define ENGINE_HOST_FIXTURE_SLICELEN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SLICELEN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slQ = location.search.slice(1);"
    "var slU = '/api/sl-path/' + encodeURIComponent(slQ);"
    "var slF = slU.replace(/#.*$/, '');"
    "fetch(slU.slice(slU.length) === '' ? '/api/sl-own-empty' : '/api/sl-own-nonempty');"
    "fetch(slU.slice(slF.length) === '' ? '/api/sl-head-empty' : '/api/sl-head-nonempty');"
    "fetch(slQ.slice(slQ.length - 1) === '' ? '/api/sl-less-empty' : '/api/sl-less-nonempty');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
