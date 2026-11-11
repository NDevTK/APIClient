/* The environment-type document. One of test_forced.c's exploring documents (its TF_DOCS table). Four tests
   compare an environment value against a type its declaration excludes: Web IDL makes Navigator's userAgent and
   language a DOMString and Screen's width a long, and ECMAScript §21.3.2.25 Math.max ( ...args ) returns a
   Number, so `== undefined`, `== null` and `typeof … === "undefined"` over them have one answer in every world
   and take no fork. Two tests the types leave open (an ordering over screen.width, a search of the user agent)
   fork. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ENVTYPES_H
#define ENGINE_HOST_FIXTURE_ENVTYPES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ENVTYPES[] =
    "<!doctype html><html><body>"
    "<script>"
    "var etUa = navigator.userAgent == undefined ? 'etUaAbsent' : 'etUaString';"
    "var etSw = screen.width == null ? 'etSwNull' : 'etSwNumber';"
    "var etLang = typeof navigator.language === 'undefined' ? 'etLangAbsent' : 'etLangString';"
    "var etMax = Math.max(document.documentElement.clientWidth, window.innerWidth) == undefined"
    " ? 'etMaxAbsent' : 'etMaxNumber';"
    "fetch('/api/envtype?ua=' + etUa + '&sw=' + etSw + '&lang=' + etLang + '&mx=' + etMax);"
    "fetch('/api/envtypewide?w=' + (screen.width > 1000 ? 'etWide' : 'etNarrow'));"
    "fetch('/api/envtypemob?w=' + (navigator.userAgent.indexOf('Mobile') >= 0 ? 'etMobile' : 'etDesktop'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
