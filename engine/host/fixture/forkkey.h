/* THE SELF-DERIVING LEXER DOCUMENT: marked's lexer loop over an unknown source, `src = src.substring(cap[0].length)`
   with `cap` the match of a pattern against `src`, so each iteration's value is derived from the previous one
   twice. The loop runs at most 40 times and forks at each test of the match, so the deepest arm has derived its
   source 40 times. The fetch carries how many times the loop consumed. The document's result stays small only if
   an identity is bounded rather than spelling its whole derivation, which would double with each iteration.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FORKKEY_H
#define ENGINE_HOST_FIXTURE_FORKKEY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FORKKEY[] =
    "<!doctype html><html><body>"
    "<script>"
    "var fkSrc = location.hash.slice(1), fkN = 0;"
    "for (; fkN < 40; fkN++) {"
    "  var fkCap = /^a/.exec(fkSrc);"
    "  if (!fkCap) break;"
    "  fkSrc = fkSrc.substring(fkCap[0].length);"
    "}"
    "fetch('/api/forkkey?n=' + fkN);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
