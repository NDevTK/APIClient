/* The component-record read document: two DOMPoints made before any fork, so their records are shared state, then
   a branch on an unknown (an injected global the document does not spell, which solver/absent.c mints). One arm
   writes `q.x`; both read `p.y` and the other reads `q.x`, so each arm's head holds a record it only read. A second
   branch freezes those heads, and each world reports `q.x`: 9 in the writing arm and 5 in the other, which reads
   the baseline its sibling's write must not reach. One of test_forced.c's exploring documents (its TF_DOCS table).
   It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RECREAD_H
#define ENGINE_HOST_FIXTURE_RECREAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RECREAD[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rrP = new DOMPoint(3, 4), rrQ = new DOMPoint(5, 6), rrArm, rrY;"
    "if (window.__tfRecReadRole === 'rrw') { rrArm = 'w'; rrQ.x = 9; } else { rrArm = 'r'; rrY = rrQ.x; }"
    "rrY = rrP.y;"
    "if (window.__tfRecReadTwo === 'rr2') rrY = rrY + 0;"
    "fetch('/api/rr/' + rrArm + '?x=' + rrQ.x + '&y=' + rrP.y);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
