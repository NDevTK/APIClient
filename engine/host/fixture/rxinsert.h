/* THE INSERTED-SCRIPT DOCUMENT: a classic script the page inserts with appendChild, whose source holds a regular
   expression literal several work quanta long, built at run time so the fixture's own source stays short. HTML
   §4.12.1.1's "execute the script element" compiles it, and that compile can hand the thread back like a
   parser-inserted script's, so the literal's compile rests inside it: the census counts rests while no RegExp
   constructor or compile runs, and the program built across them still matches. The address reports the
   matcher's answer and the literal's source length.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXINSERT_H
#define ENGINE_HOST_FIXTURE_RXINSERT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXINSERT[] =
    "<!doctype html><html><body>"
    "<script>"
    "var riN = 300000, riS = document.createElement('script');"
    "riS.textContent = 'var riR = /' + 'a'.repeat(riN) + 'b/;'"
    "  + 'var riM = riR.test(\"' + 'a'.repeat(riN) + 'b\") && !riR.test(\"' + 'a'.repeat(riN) + '\")'"
    "  + ' && riR.source.length === ' + (riN + 1) + ';'"
    "  + 'fetch(\"/api/rxi?m=\" + riM);';"
    "document.body.appendChild(riS);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
