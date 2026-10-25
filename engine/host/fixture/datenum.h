/* THE DATE-NUMBER DOCUMENT: the session clock's unknown (`Date.now()`) after an addition and a subtraction,
   coerced back with ECMAScript §13.5.4 Unary + Operator and §21.1.1.1 Number ( value ), each gating a branch.
   §7.1.4 ToNumber of a Number is the Number itself, so each coercion answers the unknown it was given and
   both arms of each gate run. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DATENUM_H
#define ENGINE_HOST_FIXTURE_DATENUM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DATENUM[] =
    "<!doctype html><html><body>"
    "<script>"
    "var dnT0 = Date.now();"
    "var dnLater = +(Date.now() + 1310);"
    "fetch('/api/dn-plus?w=' + (dnLater > 1800000000000 ? 'dnFUTURE' : 'dnPRESENT'));"
    "var dnSince = Number(Date.now() - dnT0) + Number(dnT0);"
    "fetch('/api/dn-number?w=' + (dnSince > 1800000000000 ? 'dnFUTURE' : 'dnPRESENT'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
