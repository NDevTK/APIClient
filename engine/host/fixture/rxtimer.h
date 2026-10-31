/* THE TIMER-STRING DOCUMENT: a string handler passed to setTimeout whose source holds a regular expression literal
   several work quanta long, built at run time so the fixture's own source stays short. HTML §8.7 Timers' task
   compiles it as a classic script (substep 9.8.7), and that compile can hand the thread back like a
   parser-inserted script's, so the literal's compile rests inside it: the census counts rests while no RegExp
   constructor or compile runs, and the program built across them still matches. The address reports the
   matcher's answer.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXTIMER_H
#define ENGINE_HOST_FIXTURE_RXTIMER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXTIMER[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rtN = 300000;"
    "setTimeout('var rtR = /' + 'a'.repeat(rtN) + 'b/;'"
    "  + 'var rtM = rtR.test(\\'' + 'a'.repeat(rtN) + 'b\\') && !rtR.test(\\'' + 'a'.repeat(rtN) + '\\')'"
    "  + ' && rtR.source.length === ' + (rtN + 1) + ';'"
    "  + 'fetch(\\'/api/rxt?m=\\' + rtM);', 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
