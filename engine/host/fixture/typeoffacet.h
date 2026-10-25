/* THE TYPE-FACET DOCUMENT: an absent global's member is tested `typeof … === 'string'`, and inside that arm the
   same member is tested `typeof … === 'number'`. A world that took the first arm has fixed the member's type, so
   the second test has one answer there and its number arm is a world no value produces. Each witness fetch
   carries a constant: /api/tf-string marks the reached arm and /api/tf-impossible the world that must not exist.
   A second absent global's members are compared loosely, so §7.2.13 IsLooselyEqual ( x, y )'s steps decide the
   types each arm leaves: `== null` holding admits only null, undefined and an [[IsHTMLDDA]] object, failing
   excludes exactly null and undefined, and `== "5"` holding admits no Boolean (ToNumber gives 5, not 0 or 1).
   /api/tf-loose-* mark those arms and /api/tf-loose-impossible-* the worlds they rule out. A third global's
   member ordered `> 5` and held is no Undefined (§7.2.12 IsLessThan answers undefined for NaN), and one whose
   `startsWith` call completed is no Number (%Number.prototype% holds no startsWith): /api/tf-ord and
   /api/tf-call mark the arms, /api/tf-ord-impossible and /api/tf-call-impossible the worlds they rule out.
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
    "var lfCfg = window.__tfLooseCfg;"
    "if (lfCfg && lfCfg.v == null) {"
    "  if (typeof lfCfg.v === 'string') fetch('/api/tf-loose-impossible-null');"
    "  else fetch('/api/tf-loose-null');"
    "} else if (lfCfg) {"
    "  if (typeof lfCfg.v === 'undefined') fetch('/api/tf-loose-impossible-other');"
    "  else fetch('/api/tf-loose-other');"
    "}"
    "if (lfCfg && lfCfg.n == '5') {"
    "  if (typeof lfCfg.n === 'boolean') fetch('/api/tf-loose-impossible-five');"
    "  else fetch('/api/tf-loose-five');"
    "}"
    "var ocCfg = window.__tfOrderCfg;"
    "if (ocCfg && ocCfg.n > 5) {"
    "  if (typeof ocCfg.n === 'undefined') fetch('/api/tf-ord-impossible');"
    "  else fetch('/api/tf-ord');"
    "}"
    "if (ocCfg && ocCfg.s.startsWith('a')) {"
    "  if (typeof ocCfg.s === 'number') fetch('/api/tf-call-impossible');"
    "  else fetch('/api/tf-call');"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
