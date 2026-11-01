/* THE SETPROPERTY DOCUMENT: CSSOM §6.6.1 "The CSSStyleDeclaration Interface"'s setProperty over a value that is
   an unknown with no example (`state.*`), on a block that already declares the property, so each of the three
   completions — the declaration written (step 5), removed (step 3) or left as it was (step 6) — emits its own
   token. Beside it: a name that is no supported CSS property (step 2 returns before any fork), a priority that
   is not "important" (step 4 returns unless step 3 removed first), and the concrete step order and step 2's
   lowercasing, for which the steps answer r=1 and w=5px.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CSSDSETPROP_H
#define ENGINE_HOST_FIXTURE_CSSDSETPROP_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CSSDSETPROP[] =
    "<!doctype html><html><body>"
    "<script>"
    "function csdOutcome(s) {"
    "  return s.length === 0 ? 'csdREMOVED' : s.cssText.indexOf('1px') >= 0 ? 'csdLEFT' : 'csdWROTE';"
    "}"
    "var csdA = document.createElement('div');"
    "csdA.style.setProperty('--csd', '1px');"
    "csdA.style.setProperty('--csd', state.csdA);"
    "fetch('/api/csdset?w=' + csdOutcome(csdA.style));"
    "var csdB = document.createElement('div');"
    "csdB.style.setProperty('not-a-property', state.csdB);"
    "fetch('/api/csdname?w=' + (csdB.style.length === 0 ? 'csdNONE' : 'csdDECLARED'));"
    "var csdC = document.createElement('div');"
    "csdC.style.setProperty('--csd', '1px');"
    "csdC.style.setProperty('--csd', state.csdC, 'urgent');"
    "fetch('/api/csdprio?w=' + csdOutcome(csdC.style));"
    "var csdD = document.createElement('div');"
    "csdD.style.setProperty('--csd', '1px');"
    "csdD.style.setProperty('--csd', '', 'urgent');"
    "csdD.style.setProperty('WIDTH', '5px');"
    "fetch('/api/csdconc?r=' + csdD.style.length + '&w=' + csdD.style.width);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
