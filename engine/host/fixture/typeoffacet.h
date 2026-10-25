/* THE TYPE-FACET DOCUMENT: each gate below narrows an absent global member's type facet, and a later test over
   the same value has an arm no value of the remaining types takes. Each /api/tf-X marks the arm a world takes
   and /api/tf-X-impossible (or /api/tf-impossible) the world that must not exist:
   - tf-string: `typeof … === 'string'` held, so `typeof … === 'number'` cannot;
   - tf-loose-null, -other, -five: §7.2.13 IsLooselyEqual ( x, y )'s steps decide each arm's types (`== null`
     holding admits null, undefined and an [[IsHTMLDDA]] object, failing neither; `== "5"` admits no Boolean);
   - tf-ord: `> 5` held, so not Undefined (§7.2.12 IsLessThan answers undefined for NaN);
   - tf-call: `startsWith` completed, so not a Number (%Number.prototype% holds none);
   - tf-concat: a concatenation onto a String literal is a String (§13.15.3 step 1.c), never undefined;
   - tf-prefix: a key beginning "jq" names no member of a String primitive's chain, so the read is undefined.
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
    "var skKey = 'jq' + window.__tfStrCfg;"
    "if (void 0 !== skKey) fetch('/api/tf-concat'); else fetch('/api/tf-concat-impossible');"
    "if ('abc'[skKey]) fetch('/api/tf-prefix-impossible'); else fetch('/api/tf-prefix');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
