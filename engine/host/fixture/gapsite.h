/* The gap-site document: a page whose one function crosses four DFAIL constructs the fixture owns
   (test_forced.c's `gapSite`) and then fetches, so a release run both records the sites and shows it went on.
   The first construct is crossed twice with the second between, so the rows read a count and a first-reach
   order; the third is a DFAILF and the fourth hands DFAIL a text it composed. A release document only: a dev
   build aborts at the first crossing, so TF_DOCS lists it where APICLIENT_DEV is 0.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GAPSITE_H
#define ENGINE_HOST_FIXTURE_GAPSITE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GAPSITE[] =
    "<!doctype html><html><body>"
    "<script>"
    "function gapArm() {"
    "  gapSite(0); gapSite(1); gapSite(0);"
    "  gapSite(2); gapSite(3);"
    "  fetch('/api/gap-continued');"
    "}"
    "gapArm();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
