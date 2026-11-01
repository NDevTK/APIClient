/* THE UNKNOWN FORMDATA NAME DOCUMENT: a stored record's own keys appended to a FormData as entry names, as
   axios's toFormData does with an object's keys, then iterated and sent. With no stored value each key is an
   unknown member's name, so XHR §4's append(name, value) creates an entry whose name is input. One of
   test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FDUNKNAME_H
#define ENGINE_HOST_FIXTURE_FDUNKNAME_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FDUNKNAME[] =
    "<!doctype html><html><body>"
    "<script>"
    "var s = JSON.parse(localStorage.getItem('fd') || '{}'), fd = new FormData(), n = 0, e;"
    "Object.keys(s).forEach(function (k) { fd.append(k, s[k]); });"
    "for (e of fd) n++;"
    "fetch('/api/fdunkname?n=' + n, { method: 'POST', body: fd });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
