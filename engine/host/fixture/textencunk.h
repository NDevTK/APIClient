/* THE TEXT-ENCODER DOCUMENT: Encoding §7.4 TextEncoder's encode(input) inside a function nothing calls, so an
   orphan drive reaches it with its parameter an unknown with no example (an axios-shaped body-length helper).
   The output is the encoder's image of that unknown, so its length and its first byte are unknowns with no
   example and each gate on them runs both arms. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TEXTENCUNK_H
#define ENGINE_HOST_FIXTURE_TEXTENCUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TEXTENCUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "function teuBodyLength(teuIn) {"
    "  var teuOut = new TextEncoder().encode(teuIn);"
    "  fetch('/api/teu-len?w=' + (teuOut.length > 4 ? 'teuLONG' : 'teuSHORT'));"
    "  fetch('/api/teu-byte?w=' + (teuOut[0] === 0x3c ? 'teuLT' : 'teuOTHER'));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
