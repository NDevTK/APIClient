/* The cycle-garbage document: a page that holds 300000 live objects, drops a 300000-node doubly linked ring
   that only a WeakRef names, and keeps running. It allocates 20000 small cycles per task and asks the WeakRef
   after each, so the fetch is reached once a collection has freed the ring. The live set sizes the scan and
   the ring sizes the freeing, each far above one collector step. The live set is 300 arrays of 1000, so no
   one object's edges reach a step's budget and a step's work is the phase's and not one array's.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GCCYCLE_H
#define ENGINE_HOST_FIXTURE_GCCYCLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GCCYCLE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var gyKeep = [], gyKept = 0;"
    "for (var c = 0; c < 300; c++) {"
    "  var gyRow = [];"
    "  for (var i = 0; i < 1000; i++) { gyRow.push({ i: i }); gyKept++; }"
    "  gyKeep.push(gyRow);"
    "}"
    "var gyRef = (function () {"
    "  var head = { n: 0 }, p = head;"
    "  for (var i = 1; i < 300000; i++) { var q = { n: i, prev: p }; p.next = q; p = q; }"
    "  p.next = head; head.prev = p;"
    "  return new WeakRef(head);"
    "})();"
    "function gyTick() {"
    "  for (var i = 0; i < 20000; i++) { var a = {}, b = { a: a }; a.b = b; }"
    "  if (gyRef.deref() === undefined) fetch('/api/gccycle?kept=' + gyKept);"
    "  else setTimeout(gyTick, 0);"
    "}"
    "setTimeout(gyTick, 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
