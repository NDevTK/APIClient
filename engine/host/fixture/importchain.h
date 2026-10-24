/* THE IMPORT-CHAIN DOCUMENT: 20000 module scripts, each a Blob URL whose source imports the next, built from
   the end so every specifier names a URL that already exists, and the first loaded by import(). Loading,
   linking and evaluating the graph each walk it as deep as the chain (ECMAScript §16.2.1.6.1.1
   LoadRequestedModules ( [ hostDefined ] ), §16.2.1.6.1.2 Link ( ), §16.2.1.6.1.3 Evaluate ( )), and the
   fetch reports how the import settled.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IMPORTCHAIN_H
#define ENGINE_HOST_FIXTURE_IMPORTCHAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IMPORTCHAIN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var icN = 20000;"
    "var icUrl = URL.createObjectURL(new Blob(['export var depth = 0;'], { type: 'text/javascript' }));"
    "for (var icI = 1; icI < icN; icI++)"
    "  icUrl = URL.createObjectURL(new Blob(['import { depth as d } from \"' + icUrl + '\";"
    " export var depth = d + 1;'], { type: 'text/javascript' }));"
    "import(icUrl).then(function (m) { fetch('/api/importchain?d=' + (m.depth + 1)); },"
    "  function (e) { fetch('/api/importchain?err=' + e.name); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
