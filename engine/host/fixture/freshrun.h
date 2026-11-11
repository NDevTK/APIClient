/* THE FRESH-RUN DOCUMENT: the example's world runs into code no member has served while a world resting on an
   assumption waits at the decision both were minted at. One script branches over a localStorage key the area
   lacks: the arm taking it as present is priced (solver/flow.c's FLOW_PRICED_WORK) and throws from a microtask.
   The example's world then calls forty distinct functions, each a loop of about two hundred thousand units, with
   no fork among them, and throws from a microtask after the last. Its work runs at forty bodies no member has
   stood at, so its stand follows it there and it keeps the thread over the priced world until it throws. The
   loop's length is a global only that world writes, so a drive of a function the priced world never called runs
   no iteration. One of test_forced.c's exploring documents (TF_DOCS); it ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRESHRUN_H
#define ENGINE_HOST_FIXTURE_FRESHRUN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

/* One function of the run: a body of its own at its own position in the script. */
#define FRESHRUN_FN(n) "function frF" #n "(){var s=" #n ";for(var i=0;i<frN;i++)s=(s*31+i)|0;return s;}"
#define FRESHRUN_FN8(a) FRESHRUN_FN(a##0) FRESHRUN_FN(a##1) FRESHRUN_FN(a##2) FRESHRUN_FN(a##3) \
                        FRESHRUN_FN(a##4) FRESHRUN_FN(a##5) FRESHRUN_FN(a##6) FRESHRUN_FN(a##7)
#define FRESHRUN_REF8(a) "frF" #a "0,frF" #a "1,frF" #a "2,frF" #a "3,frF" #a "4,frF" #a "5,frF" #a "6,frF" #a "7,"

static const char HTML_FRESHRUN[] =
    "<!doctype html><html><body>"
    "<script>"
    FRESHRUN_FN8(1) FRESHRUN_FN8(2) FRESHRUN_FN8(3) FRESHRUN_FN8(4) FRESHRUN_FN8(5)
    "var frN = 0;"
    "var frW = localStorage.getItem('frTok') !== null ? 'frLS' : 'frOBS';"
    "fetch('/api/frarm?w=' + frW);"
    "if (frW === 'frLS') queueMicrotask(function () { throw new Error('frBOOMfrLS'); });"
    "else {"
    " frN = 20000;"
    " var frL = [" FRESHRUN_REF8(1) FRESHRUN_REF8(2) FRESHRUN_REF8(3) FRESHRUN_REF8(4) FRESHRUN_REF8(5) "], frS = 0;"
    " for (var k = 0; k < frL.length; k++) frS = (frS + frL[k]()) | 0;"
    " queueMicrotask(function () { throw new Error('frBOOMfrOBS'); });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef FRESHRUN_REF8
#undef FRESHRUN_FN8
#undef FRESHRUN_FN

#endif
