/* THE CYCLE-CHURN DOCUMENT: a page that allocates garbage cycles without pause until it sees one collected. It
   holds a live set of 400000 objects, so every collection's trial visits far more than one step's work, and it
   creates one two-object cycle that only a WeakRef names. Then it allocates 20000 more cycles per task, a task
   at a time, and asks the WeakRef after each. The fetch is reached only if a collection finishes while the
   page keeps allocating: a scheduler that let the page run between a trial's steps would restart every trial.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GCCHURN_H
#define ENGINE_HOST_FIXTURE_GCCHURN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GCCHURN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var gcKeep = [];"
    "for (var i = 0; i < 400000; i++) gcKeep.push({ i: i });"
    "var gcRef = (function () { var a = {}, b = { a: a }; a.b = b; return new WeakRef(a); })();"
    "function gcChurn() {"
    "  for (var i = 0; i < 20000; i++) { var a = {}, b = { a: a }; a.b = b; }"
    "  if (gcRef.deref() === undefined) fetch('/api/gcchurn?kept=' + gcKeep.length);"
    "  else setTimeout(gcChurn, 0);"
    "}"
    "setTimeout(gcChurn, 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
