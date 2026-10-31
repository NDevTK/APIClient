/* THE PIECE-SLICE DOCUMENT: a string concatenated from an unknown the page's own state supplies (the cookie
   string) and an attacker source (the fragment), then sliced back apart. A slice inside the fragment's piece
   is the fragment's own derivation, so its sink write is an attacker PoC delivered through `#`; a slice across
   both pieces depends on both and stays labelled with the cookie's precondition. The fragment is set first so
   the source's example places its piece.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PIECESLICE_H
#define ENGINE_HOST_FIXTURE_PIECESLICE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PIECESLICE[] =
    "<!doctype html><html><body>"
    "<div id='psView'></div><div id='psCross'></div>"
    "<script>"
    "history.replaceState(null, '', '#seed');"
    "var psMix = document.cookie + '|' + location.hash.slice(1);"
    "document.getElementById('psView').innerHTML = psMix.slice(psMix.indexOf('|') + 1);"
    "document.getElementById('psCross').innerHTML = psMix.slice(0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
