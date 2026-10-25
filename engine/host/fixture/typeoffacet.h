/* THE TYPE-FACET DOCUMENT: an absent global's member is tested `typeof … === 'string'`, and inside that arm the
   same member is tested `typeof … === 'number'`. A world that took the first arm has fixed the member's type, so
   the second test has one answer there and its number arm is a world no value produces. Each witness fetch
   carries a constant: /api/tf-string marks the reached arm and /api/tf-impossible the world that must not exist.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TYPEOFFACET_H
#define ENGINE_HOST_FIXTURE_TYPEOFFACET_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TYPEOFFACET[] =
    "<!doctype html><html><body>"
    "<script>"
    "var tfCfg = window.__tfTypeCfg;"
    "if (tfCfg && typeof tfCfg.mode === 'string') {"
    "  if (typeof tfCfg.mode === 'number') fetch('/api/tf-impossible');"
    "  else fetch('/api/tf-string');"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
