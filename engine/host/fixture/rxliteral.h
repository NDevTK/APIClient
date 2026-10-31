/* THE REGEXP-LITERAL DOCUMENT: a classic script whose source holds a regular expression literal several work
   quanta long, inserted at run time so the fixture's own source stays short. The script parse compiles the
   literal's pattern (the steps of ECMAScript §22.2.3.3 RegExpInitialize ( obj, pattern, flags ) that parse
   it) inside the parse, resting between Terms where the parse can hand the thread back, so the census counts
   rests while no RegExp constructor or compile runs; the program built across them still matches, and the address reports
   the matcher's answer and the literal's source length.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXLITERAL_H
#define ENGINE_HOST_FIXTURE_RXLITERAL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXLITERAL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rlN = 300000, rlS = document.createElement('script');"
    "rlS.textContent = 'var rlR = /' + 'a'.repeat(rlN) + 'b/;'"
    "  + 'var rlM = rlR.test(\"' + 'a'.repeat(rlN) + 'b\") && !rlR.test(\"' + 'a'.repeat(rlN) + '\")'"
    "  + ' && rlR.source.length === ' + (rlN + 1) + ';'"
    "  + 'fetch(\"/api/rxl?m=\" + rlM);';"
    "document.body.appendChild(rlS);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
