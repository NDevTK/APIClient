/* THE DATE-SLOT DOCUMENT: ECMAScript §21.4.2.1 Date ( ...values ) over unknown input builds a Date object whose
   [[DateValue]] holds the unknown time value, so §21.4.4 Properties of the Date Prototype Object's methods run on
   it. `navigator.hardwareConcurrency` is an environment unknown with an example; `window.__tfDateUnk` is an absent
   global, unknown with no example on the session's arm, so §21.4.4.36 Date.prototype.toISOString's step 4 has a
   RangeError arm and a String arm; `new Date()` holds the session clock's unknown, which a field setter and
   §21.4.4.37 Date.prototype.toJSON then read. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DATESLOT_H
#define ENGINE_HOST_FIXTURE_DATESLOT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DATESLOT[] =
    "<!doctype html><html><body>"
    "<script>"
    "var dsD = new Date(navigator.hardwareConcurrency * 86400000);"
    "fetch('/api/ds-inst?w=' + (dsD instanceof Date ? 'dsDATE' : 'dsNOTDATE'));"
    "var dsI = 'dsNONE';"
    "try { dsD.toISOString(); dsI = 'dsISO'; } catch (e) { dsI = 'dsTHREW'; }"
    "fetch('/api/ds-iso?w=' + dsI);"
    "fetch('/api/ds-time?w=' + (typeof dsD.getTime() === 'number' ? 'dsNUM' : 'dsNOTNUM'));"
    "var dsN = new Date(window.__tfDateUnk);"
    "var dsR = 'dsNONE';"
    "try { dsN.toISOString(); dsR = 'dsSTRING'; } catch (e) { dsR = e instanceof RangeError ? 'dsRANGE' : 'dsOTHER'; }"
    "fetch('/api/ds-nan?w=' + dsR);"
    "var dsC = new Date();"
    "var dsS = 'dsNONE';"
    "try { dsC.setHours(0, 0, 0, 0); dsS = dsC instanceof Date ? 'dsSET' : 'dsNOTDATE'; } catch (e) { dsS = 'dsTHREW'; }"
    "fetch('/api/ds-set?w=' + dsS);"
    "var dsJ = 'dsNONE';"
    "try { JSON.stringify({ at: dsC }); dsJ = 'dsJSON'; } catch (e) { dsJ = 'dsTHREW'; }"
    "fetch('/api/ds-json?w=' + dsJ);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
