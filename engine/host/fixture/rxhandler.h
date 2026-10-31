/* THE INLINE-HANDLER DOCUMENT: a body `onload` content attribute whose handler body holds a regular expression
   literal of 327681 pattern characters, spelled by macro repetition so the fixture's own text stays short.
   HTML §8.1.8.1 "Event handlers"' get the current value of the event handler compiles the body at the load
   event's dispatch (steps 3.8-3.12), and that compile can hand the thread back like a parser-inserted
   script's, so the literal's compile rests inside it: the census counts rests while no RegExp constructor or
   compile runs, and the program built across them still matches. The address reports the matcher's answer.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXHANDLER_H
#define ENGINE_HOST_FIXTURE_RXHANDLER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define RXH_A16 "aaaaaaaaaaaaaaaa"
#define RXH_A256 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16 \
                 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16 RXH_A16
#define RXH_A4096 RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256 \
                  RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256 RXH_A256
#define RXH_A65536 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 \
                   RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096 RXH_A4096
#define RXH_PATTERN RXH_A65536 RXH_A65536 RXH_A65536 RXH_A65536 RXH_A65536

static const char HTML_RXHANDLER[] =
    "<!doctype html><html><body onload=\""
    "var r = /" RXH_PATTERN "b/, n = 327680;"
    "fetch('/api/rxh?m=' + (r.test('a'.repeat(n) + 'b') && !r.test('a'.repeat(n)) && r.source.length === n + 1));"
    "\">"
    TF_CANVAS_INK
    "</body></html>";

#endif
