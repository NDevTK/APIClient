/* THE CLASSIC-WORKER DOCUMENT: a dedicated worker whose classic script, /chunk/rxworker.js (test_forced.c's
   TF_SERVED), holds a regular expression literal of 327681 pattern characters and posts the matcher's answer.
   HTML §10.2.4 run a worker creates the classic script on the worker agent's own turn, and that parse can hand
   the thread back like a parser-inserted script's, so the literal's compile rests inside it: the census counts
   rests while no RegExp constructor or compile runs, and the posted answer is what the address reports.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXWORKER_H
#define ENGINE_HOST_FIXTURE_RXWORKER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXWORKER[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rwW = new Worker('/chunk/rxworker.js');"
    "rwW.onmessage = function (e) { fetch('/api/rxw?m=' + e.data); };"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
