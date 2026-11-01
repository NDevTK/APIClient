/* THE UNKNOWN PROPERTY KEY DOCUMENT: a stored value used as a computed key in an object literal and as the key
   of a compound assignment, the two places QuickJS emits OP_to_propkey and OP_to_propkey2 for ECMAScript
   §7.1.21 ToPropertyKey ( arg ). With no stored value the key is an unknown, which §7.1.1 ToPrimitive answers
   as itself. One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PROPKEYUNK_H
#define ENGINE_HOST_FIXTURE_PROPKEYUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PROPKEYUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var k = localStorage.getItem('pk'), p = {[k]: 1}, o = {};"
    "o[k] = 0; o[k] += 2;"
    "fetch('/api/propkeyunk?v=' + p[k] + '&w=' + o[k]);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
