/* THE PROBE DOCUMENT: a `div` whose `onclick` content attribute holds a regular expression literal of 327681
   pattern characters followed by a stray `}`, so the body is not parsable as a FunctionBody. A script then
   reads the IDL attribute: HTML §8.1.8.1's getting the current value of the event handler performs step 3.7's
   parse, which rests inside the literal and then fails, so the getter answers null and no compile follows.
   The census counts rests while no RegExp constructor, compile or step 3.9 compile runs, so every rest is the
   probe's. The address reports whether the getter answered null.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXPROBE_H
#define ENGINE_HOST_FIXTURE_RXPROBE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define RXP_A16 "aaaaaaaaaaaaaaaa"
#define RXP_A256 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16 \
                 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16 RXP_A16
#define RXP_A4096 RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256 \
                  RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256 RXP_A256
#define RXP_A65536 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 \
                   RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096 RXP_A4096
#define RXP_PATTERN RXP_A65536 RXP_A65536 RXP_A65536 RXP_A65536 RXP_A65536

static const char HTML_RXPROBE[] =
    "<!doctype html><html><body>"
    "<div id=\"d\" onclick=\"var r = /" RXP_PATTERN "b/; }\"></div>"
    "<script>"
    "var v = document.getElementById('d').onclick;"
    "fetch('/api/rxp?n=' + (v === null));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
