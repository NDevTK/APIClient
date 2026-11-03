/* THE LONG-BOOT REFERRER DOCUMENT: cutboot's setup ahead of the one read of `document.referrer`, whose decoded
   value is written to markup and fires. The referrer (HTML §3.1.4 Resource metadata management) is the address
   the victim arrived from, which the attacker's page owns, and the read is its first, so a candidate can start at
   a snapshot taken before it instead of re-running the setup from the pre-boot baseline.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUTREF_H
#define ENGINE_HOST_FIXTURE_CUTREF_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUTREF[] =
    "<!doctype html><html><body>"
    "<div id='crOut'></div>"
    "<script>"
    "var crAcc = 0;"
    "for (var crI = 0; crI < 4000000; crI++) crAcc = (crAcc + crI) % 1000003;"
    "document.getElementById('crOut').innerHTML = decodeURIComponent(document.referrer);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
