/* THE WORKER DOCUMENT: HTML §10.2.6.3 dedicated workers over blob: URLs, in the two shapes real libraries use.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WORKER_H
#define ENGINE_HOST_FIXTURE_WORKER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WORKER[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* /api/wkpapa: PapaParse's `worker: true` shape. The worker's program is the library's own factory, taken
       with Function.prototype.toString into a Blob, and it detects that it is a worker by having no `document`
       and a `postMessage`. It parses what it is posted and posts the rows back; the page reads them in the
       Worker's `onmessage`. */
    "function wkFactory() {"
    " var g = typeof self !== 'undefined' ? self : this;"
    " if (typeof document === 'undefined' && typeof g.postMessage === 'function') {"
    "  g.onmessage = function(e) {"
    "   var rows = e.data.input.split('\\n').map(function(l) { return l.split(','); });"
    "   g.postMessage({ id: e.data.id, rows: rows });"
    "  };"
    " }"
    "}"
    "var wkPapa = new Worker(URL.createObjectURL(new Blob(['(' + wkFactory.toString() + ')();'],"
    " { type: 'text/javascript' })));"
    "wkPapa.onmessage = function(e) {"
    " fetch('/api/wkpapa?n=' + e.data.rows.length + '&c=' + e.data.rows[1][1] + '&id=' + e.data.id);"
    "};"
    "wkPapa.postMessage({ input: 'a,b\\n1,2', id: 7 });"
    /* /api/wkpdf: pdf.js's shape. The page transfers the document's bytes to its parser worker, which reads
       them and posts its result back; the transfer detaches the page's buffer (HTML §2.7.6 transfer steps). */
    "var wkPdf = new Worker(URL.createObjectURL(new Blob(["
    " 'self.addEventListener(\"message\", function(e) {'"
    " + ' var b = new Uint8Array(e.data), s = 0;'"
    " + ' for (var i = 0; i < b.length; i++) s += b[i];'"
    " + ' self.postMessage({ len: b.length, sum: s });'"
    " + '});'"
    "])));"
    "var wkBuf = new Uint8Array([37, 80, 68, 70]).buffer;"
    "wkPdf.addEventListener('message', function(e) {"
    " fetch('/api/wkpdf?len=' + e.data.len + '&sum=' + e.data.sum + '&det=' + (wkBuf.byteLength === 0));"
    "});"
    "wkPdf.postMessage(wkBuf, [wkBuf]);"
    /* /api/wkmod: a module worker over a blob: script (run a worker step 13's module worker script graph). Its
       root statically imports one chunk (`s`) and, on a message, dynamically imports the same chunk (`a`); both
       answers come back through the ports. The dynamic import's address is part of the program rather than the
       message, so the handler's import names a module whatever event it is called with. */
    "var wkChunk = URL.createObjectURL(new Blob(['export var answer = 42;'], { type: 'text/javascript' }));"
    "var wkMod = new Worker(URL.createObjectURL(new Blob(["
    " 'import { answer } from \"' + wkChunk + '\";'"
    " + 'self.postMessage(\"s\" + answer);'"
    " + 'self.onmessage = function() { import(\"' + wkChunk + '\").then(function(m) { self.postMessage(\"a\" + m.answer); }); };'"
    "], { type: 'text/javascript' })), { type: 'module' });"
    "wkMod.onmessage = function(e) { fetch('/api/wkmod?v=' + e.data); };"
    "wkMod.postMessage('go');"
    /* /api/wkerr: a worker whose script does not parse fires `error` at the Worker (run a worker's onComplete
       given a script whose error to rethrow is non-null). */
    "var wkBad = new Worker(URL.createObjectURL(new Blob(['this is not ( a program'])));"
    "wkBad.onerror = function() { fetch('/api/wkerr?v=fired'); };"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
