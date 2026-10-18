/* THE `@custom-media` DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). Media Queries 5
   §10 "Custom Media Queries" defines the rule and §11 "CSSOM" its CSSCustomMediaRule; the reference browser
   (Chrome 155) does not ship either, so CSS Syntax 3 §8 "CSS stylesheets" discards the rule as not recognized.
   The sheet then holds only its style rule and its `@media`, whose `(--n)` is Media Queries 4's
   `<general-enclosed>` and unknown, so `#a` keeps its green, `matchMedia` answers false and the interface is
   absent. The second sheet's `@font-feature-values` holds a `@historical-forms` block, which CSS Fonts 4
   §6.9.1 defines and Chrome 155 does not recognize, so it is an unknown at-rule there, invalid and ignored:
   the rule has no `historicalForms` map and its `@swash` block is still read. It ends with TF_CANVAS_INK,
   which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUSTOMMEDIA_H
#define ENGINE_HOST_FIXTURE_CUSTOMMEDIA_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUSTOMMEDIA[] =
    "<!doctype html><html><head>"
    "<style>@custom-media --n (min-width: 1px); #a { color: rgb(0, 128, 0) } "
    "@media (--n) { #a { color: rgb(255, 0, 0) } }</style>"
    "<style>@font-feature-values Foo { @historical-forms { a: 1; } @swash { b: 2; } }</style>"
    "</head><body><div id=a>x</div>"
    "<script>fetch('/api/cmedia?n=' + document.styleSheets[0].cssRules.length"
    " + '&m=' + matchMedia('(--n)').matches"
    " + '&c=' + (getComputedStyle(document.getElementById('a')).color === 'rgb(0, 128, 0)')"
    " + '&i=' + typeof CSSCustomMediaRule"
    " + '&h=' + ('historicalForms' in document.styleSheets[1].cssRules[0])"
    " + '&s=' + document.styleSheets[1].cssRules[0].swash.size);</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
