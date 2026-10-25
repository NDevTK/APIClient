/* THE RANDOM-KEYS DOCUMENT: property keys built from Math.random() (ECMAScript §21.3.2.27), each call its own
   unknown under one display shape: two closure-style listener keys on one object, React's container key, and a
   jQuery-style expando written on two objects through one value. Two calls are two keys and one value is one
   key, whatever shape renders them.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MRKEYS_H
#define ENGINE_HOST_FIXTURE_MRKEYS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MRKEYS[] =
    "<!doctype html><html><body>"
    "<script>"
    "var o = {};"
    "o['closure_listenable_' + (1e6 * Math.random() | 0)] = 1;"
    "o['closure_listenable_' + (1e6 * Math.random() | 0)] = 2;"
    "var rc = {}; rc['__reactContainer$' + Math.random().toString(36).slice(2)] = 1;"
    "var ex = 'jQuery' + ('3.7.1' + Math.random()).replace(/\\D/g, ''), a = {}, b = {};"
    "a[ex] = 1; b[ex] = 2;"
    "fetch('/api/mrk-keys?n=' + Object.keys(o).length + '&ex=' + (a[ex] + b[ex]));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
