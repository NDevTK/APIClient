/* THE CLICK-GATED FRAGMENT DOCUMENT: a button whose `click` listener writes the decoded fragment into markup.
   Nothing clicks it and nothing else reads `location.hash`, so the sink exists only behind a user act.
   The fragment is decoded because URL §1.3's fragment percent-encode set holds `<`, so a raw `location.hash`
   in a data-state markup sink can never fire (fixture/hashsinks.h's negative half); decoding makes the sink a
   real XSS whose only obstacle is the click.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CLICKHASH_H
#define ENGINE_HOST_FIXTURE_CLICKHASH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CLICKHASH[] =
    "<!doctype html><html>"
    "<body>"
    "<button id='chgo'>go</button><div id='chout'></div>"
    "<script>"
    "document.getElementById('chgo').addEventListener('click', function () {"
    " fetch('/api/chclick?v=chRAN');"
    " document.getElementById('chout').innerHTML = decodeURIComponent(location.hash.slice(1));"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
