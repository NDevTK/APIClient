/* THE INDEX-GATE DOCUMENTS: a sink behind a comparison of an attacker component's occurrence index, which the
   candidate's delivered value must satisfy. `indexgate` gates the query on `indexOf('q=') >= 0` and the fragment
   on the false arm of `indexOf('k=') < 0`; each composed delivery places the token first. `indexgatefar` gates
   the fragment on `indexOf('k=') > 5`, a set that excludes -1 and the head, so its delivery places the token at
   index 6 and not at the start.
   Two of test_forced.c's exploring documents (its TF_DOCS table). Each ends with TF_CANVAS_INK, which
   test_forced.c defines before including this. */
#ifndef ENGINE_HOST_FIXTURE_INDEXGATE_H
#define ENGINE_HOST_FIXTURE_INDEXGATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INDEXGATE[] =
    "<!doctype html><html><body>"
    "<div id='igQuery'></div><div id='igFrag'></div>"
    "<script>"
    "var igS = location.search;"
    "if (igS.indexOf('q=') >= 0)"
    "  document.getElementById('igQuery').innerHTML = decodeURIComponent(igS.slice(igS.indexOf('q=') + 2)"
    "                                                                      .split('&')[0]);"
    "var igH = location.hash;"
    "if (igH.indexOf('k=') < 0) {} else"
    "  document.getElementById('igFrag').innerHTML = decodeURIComponent(igH.slice(igH.indexOf('k=') + 2));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_INDEXGATEFAR[] =
    "<!doctype html><html><body>"
    "<div id='igFar'></div>"
    "<script>"
    "var igF = location.hash;"
    "if (igF.indexOf('k=') > 5)"
    "  document.getElementById('igFar').innerHTML = decodeURIComponent(igF.slice(igF.indexOf('k=') + 2));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
