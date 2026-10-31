/* THE REGEXP-LITERAL DOCUMENT: a parser-inserted classic script whose source holds a regular expression literal
   of 327681 pattern characters, spelled by macro repetition so the fixture's own text stays short. The script
   parse compiles the literal's pattern (the steps of ECMAScript §22.2.3.3 RegExpInitialize ( obj, pattern,
   flags ) that parse it) inside the parse, resting between Terms where the parse can hand the thread back, so
   the census counts rests while no RegExp constructor or compile runs; the program built across them still
   matches, and the address reports the matcher's answer and the literal's source length.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXLITERAL_H
#define ENGINE_HOST_FIXTURE_RXLITERAL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define RXL_A16 "aaaaaaaaaaaaaaaa"
#define RXL_A256 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16 \
                 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16 RXL_A16
#define RXL_A4096 RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256 \
                  RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256 RXL_A256
#define RXL_A65536 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 \
                   RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096 RXL_A4096
#define RXL_PATTERN RXL_A65536 RXL_A65536 RXL_A65536 RXL_A65536 RXL_A65536

static const char HTML_RXLITERAL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rlR = /" RXL_PATTERN "b/, rlN = 327680;"
    "var rlM = rlR.test('a'.repeat(rlN) + 'b') && !rlR.test('a'.repeat(rlN)) && rlR.source.length === rlN + 1;"
    "fetch('/api/rxl?m=' + rlM);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
