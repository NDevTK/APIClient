/* THE BIGINT-OVER-AN-UNKNOWN DOCUMENT: ECMAScript §21.2.1.1 BigInt ( value ) given unknown input, once an absent
   global (solver/absent.c's unknown, no example on the session's arm) and once the session clock's unknown
   (`Date.now()`, a Number example). Step 2's ToPrimitive over an unknown is the identity, so each call must answer
   with a value derived from its operand, and each witness fetch after it says the call returned.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_BIGINTUNK_H
#define ENGINE_HOST_FIXTURE_BIGINTUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_BIGINTUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var buA = 'buNONE';"
    "try { BigInt(window.__tfBigUnk); buA = 'buRAN'; } catch (e) { buA = 'buTHREW'; }"
    "fetch('/api/bigunk-absent?w=' + buA);"
    "var buC = 'buNONE';"
    "try { BigInt(Date.now()); buC = 'buRAN'; } catch (e) { buC = 'buTHREW'; }"
    "fetch('/api/bigunk-clock?w=' + buC);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
