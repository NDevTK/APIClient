/* THE STAND-SLICE DOCUMENT: one microtask checkpoint long enough that slices of different work end between
   different jobs of it. One script queues twenty-four microtasks, each a distinct function looping about fifteen
   thousand units, and a last one that throws. It fetches nothing and arms no timer, so no reply or timer arrival
   reads where a step ended, and every member's path is the same however the slices fall; engine/build.mjs's
   slicing parity stage runs it at two slice budgets and compares the stand series. One of test_forced.c's
   exploring documents (TF_DOCS); it ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STANDSLICE_H
#define ENGINE_HOST_FIXTURE_STANDSLICE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

/* One job of the checkpoint: a body of its own at its own position in the script. */
#define STANDSLICE_FN(n) "function ssF" #n "(){var s=" #n ";for(var i=0;i<1500;i++)s=(s*31+i)|0;ssT=(ssT+s)|0;}"
#define STANDSLICE_FN8(a) STANDSLICE_FN(a##0) STANDSLICE_FN(a##1) STANDSLICE_FN(a##2) STANDSLICE_FN(a##3) \
                          STANDSLICE_FN(a##4) STANDSLICE_FN(a##5) STANDSLICE_FN(a##6) STANDSLICE_FN(a##7)
#define STANDSLICE_REF8(a) "ssF" #a "0,ssF" #a "1,ssF" #a "2,ssF" #a "3,ssF" #a "4,ssF" #a "5,ssF" #a "6,ssF" #a "7,"

static const char HTML_STANDSLICE[] =
    "<!doctype html><html><body>"
    "<script>"
    STANDSLICE_FN8(1) STANDSLICE_FN8(2) STANDSLICE_FN8(3)
    "var ssT = 0;"
    "[" STANDSLICE_REF8(1) STANDSLICE_REF8(2) STANDSLICE_REF8(3) "].forEach(function (f) { queueMicrotask(f); });"
    "queueMicrotask(function () { throw new Error('ssBOOMssEND' + (ssT === 0 ? '0' : '')); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef STANDSLICE_REF8
#undef STANDSLICE_FN8
#undef STANDSLICE_FN

#endif
