/* THE FILE-PARSE (TEXT) DOCUMENT: fileparse-fetch's question asked of a text body, twice. Each statement fetches
   the file its query names, falling back to the seed (TF_SERVED's `/files/fpseed.txt`), reads it with text(),
   checks a first line `FPX1` and compiles the second line with `new Function`.
   - `?file=` is fetched wherever it points, so the attacker hosts the file: its reply is a forced reply whose
     provenance is the query, and the compiled line is attacker input rooted in it.
   - `?doc=` is fetched only when it resolves to this document's own origin, so its reply is this site's own
     bytes and no attacker input: its sink must carry no source rooted in the query.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSETEXT_H
#define ENGINE_HOST_FIXTURE_FILEPARSETEXT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_TEXT[] =
    "<!doctype html><html><body>"
    "<script>"
    "function fptParam(name) {"
    "  var q = location.search, at = q.indexOf(name + '=');"
    "  return at < 0 ? null : decodeURIComponent(q.slice(at + name.length + 1).split('&')[0]);"
    "}"
    "function fptParse(text, tag) {"
    "  var lines = text.split('\\n');"
    "  if (lines[0] !== 'FPX1') { fetch('/api/fpt-badmagic-' + tag); return null; }"
    "  fetch('/api/fpt-parsed-' + tag);"
    "  return new Function('x', lines[1]);"
    "}"
    "(async function () {"
    "  var url = fptParam('file') || '/files/fpseed.txt';"
    "  fptParse(await (await fetch(url)).text(), 'open');"
    "})();"
    "(async function () {"
    "  var url = fptParam('doc') || '/files/fpseed.txt';"
    "  if (new URL(url, location.href).origin !== location.origin) { fetch('/api/fpt-refused'); return; }"
    "  fptParse(await (await fetch(url)).text(), 'pinned');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
