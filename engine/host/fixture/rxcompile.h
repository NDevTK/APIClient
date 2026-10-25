/* THE REGEXP-COMPILE DOCUMENT: `new RegExp` and RegExp.prototype.compile over patterns several work quanta
   long, built at run time so the script source stays short. Each compile is charged its pattern bytes as host
   work and rests between Terms (ECMAScript §22.2.3.3 RegExpInitialize steps 12-22), so the census counts rests
   and both compiled programs still match: the address reports each matcher's answer and its source length.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXCOMPILE_H
#define ENGINE_HOST_FIXTURE_RXCOMPILE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXCOMPILE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rxN = 300000, rxP = 'a'.repeat(rxN) + 'b';"
    "var rxA = new RegExp(rxP);"
    "var rxCtor = rxA.test('a'.repeat(rxN) + 'b') && !rxA.test('a'.repeat(rxN)) && rxA.source.length === rxN + 1;"
    "var rxB = /x/;"
    "rxB.compile('c'.repeat(rxN), 'g');"
    "var rxCmp = rxB.global && rxB.test('c'.repeat(rxN)) && rxB.source.length === rxN;"
    "fetch('/api/rxc?ctor=' + rxCtor + '&cmp=' + rxCmp);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
