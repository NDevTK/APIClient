/* THE LINK-NEST DOCUMENT: a `<link rel=stylesheet>` whose sheet (test_forced.c's LINKNEST_SHEET, served as
   text/css) is one `@layer` block of 10,002 rules, about 160 KiB, followed by one rule of 1,001 declarations.
   The block's first rule is `a:not(.lnA)`, an ident and a colon where CSS Syntax 3 §5.5.5 Consume a block's
   contents tries a declaration first, so its parse and its build are each more than a work quantum inside one
   top-level rule. The page reads the sheet back through CSSOM and the cascade once `load` has fired, or at once
   if the sheet is already there: a parse and a build that rested between quanta answer what ones that did not
   do.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LINKNEST_H
#define ENGINE_HOST_FIXTURE_LINKNEST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LINKNEST[] =
    "<!doctype html><html><head><link id=lnLink rel=stylesheet href='/sheet/linknest.css'></head><body>"
    "<div id=lnEl></div><div id=lnBig></div>"
    "<script>"
    "var lnL = document.getElementById('lnLink');"
    "function lnSheet() {"
    "  for (var i = 0; i < document.styleSheets.length; i++)"
    "    if (document.styleSheets[i].ownerNode === lnL) return document.styleSheets[i];"
    "  return null;"
    "}"
    "function lnReport() {"
    "  var s = lnSheet(), r = s.cssRules[0].cssRules;"
    "  fetch('/api/linknest?n=' + r.length + '&f=' + encodeURIComponent(r[0].selectorText) +"
    "        '&w=' + getComputedStyle(document.getElementById('lnEl')).width +"
    "        '&h=' + getComputedStyle(document.getElementById('lnBig')).height +"
    "        '&d=' + s.cssRules[1].style.getPropertyValue('--lnd999').trim());"
    "}"
    "if (lnSheet()) lnReport(); else lnL.addEventListener('load', lnReport);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
