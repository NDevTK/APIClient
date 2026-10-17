/* THE `@custom-media` DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). Media Queries 5
   §10 "Custom Media Queries" defines the rule and §11 "CSSOM" its CSSCustomMediaRule; the reference browser
   (Chrome 155) does not ship either, so CSS Syntax 3 §8 "CSS stylesheets" discards the rule as not recognized.
   The sheet then holds only its style rule and its `@media`, whose `(--n)` is Media Queries 4's
   `<general-enclosed>` and unknown, so `#a` keeps its green, `matchMedia` answers false and the interface is
   absent. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUSTOMMEDIA_H
#define ENGINE_HOST_FIXTURE_CUSTOMMEDIA_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUSTOMMEDIA[] =
    "<!doctype html><html><head>"
    "<style>@custom-media --n (min-width: 1px); #a { color: rgb(0, 128, 0) } "
    "@media (--n) { #a { color: rgb(255, 0, 0) } }</style>"
    "</head><body><div id=a>x</div>"
    "<script>fetch('/api/cmedia?n=' + document.styleSheets[0].cssRules.length"
    " + '&m=' + matchMedia('(--n)').matches"
    " + '&c=' + (getComputedStyle(document.getElementById('a')).color === 'rgb(0, 128, 0)')"
    " + '&i=' + typeof CSSCustomMediaRule);</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
