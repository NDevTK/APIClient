/* THE SOURCE-REACH DOCUMENT: the derivations real bundles put between an address source and a markup sink, one
   per element. Each statement writes its own element `srN` from one attacker source through one transform
   family that shipping bundles use (URL and URLSearchParams parses of the address, fragment slicing and
   decoding, a regular-expression router, JSON.parse, Array join, String replace with a function, Map and Set
   round trips, a message listener), so a sink reached with the source's taint gone names the transform that
   dropped it. Every value reaches its sink through the real operation; nothing here is spelled for the solver.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SRCREACH_H
#define ENGINE_HOST_FIXTURE_SRCREACH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SRCREACH[] =
    "<!doctype html><html><body>"
    "<div id='sr1'></div><div id='sr2'></div><div id='sr3'></div><div id='sr4'></div><div id='sr5'></div>"
    "<div id='sr6'></div><div id='sr7'></div><div id='sr8'></div><div id='sr9'></div><div id='sr10'></div>"
    "<div id='sr11'></div><div id='sr12'></div><div id='sr13'></div><div id='sr14'></div><div id='sr15'></div>"
    "<div id='sr16'></div><div id='sr17'></div><div id='sr18'></div><div id='sr19'></div><div id='sr20'></div>"
    "<script>"
    "function srEl(id) { return document.getElementById(id); }"
    "function srSplit(q, k) { var ps = q.split('&');"
    "  for (var i = 0; i < ps.length; i++) { var kv = ps[i].split('=');"
    "    if (kv[0] === k) return decodeURIComponent(kv[1]); } return null; }"
    /* Each row: the arm that holds a value writes the sink, the arm that holds none and a throw each report. */
    "function srRow(n, f) { try { var v = f(); if (v) { fetch('/api/' + n + '-arm'); srEl(n).innerHTML = v; }"
    "  else fetch('/api/' + n + '-none'); } catch (e) { fetch('/api/' + n + '-threw?n=' + e.name); } }"
    "srRow('sr1', function () { return new URLSearchParams(location.search).get('sra'); });"
    "srRow('sr2', function () { return new URL(location.href).searchParams.get('srb'); });"
    "srRow('sr3', function () { return new URLSearchParams(location.hash.slice(1)).get('src'); });"
    "srRow('sr4', function () { return decodeURIComponent(location.hash.slice(1)); });"
    "srRow('sr5', function () { return (location.hash.match(/^#srd=(.*)$/) || [])[1]; });"
    "srRow('sr6', function () { return (/^#\\/sre\\/([^/]+)$/.exec(location.hash) || [])[1]; });"
    "srRow('sr7', function () { return (JSON.parse(decodeURIComponent(location.hash.slice(1))) || {}).srf; });"
    "srRow('sr8', function () { return ['<i>', new URLSearchParams(location.search).get('srg'), '</i>'].join(''); });"
    "srRow('sr9', function () { return '<p>{name}</p>'.replace(/\\{(\\w+)\\}/g, function (m, k) {"
    "  return { name: new URLSearchParams(location.search).get('srh') }[k]; }); });"
    "srRow('sr10', function () { return new Map([['k', new URLSearchParams(location.search).get('sri')]]).get('k'); });"
    "srRow('sr11', function () { return Array.from(new Set([new URLSearchParams(location.search).get('srj')]))[0]; });"
    "srRow('sr13', function () { return srSplit(location.search.substring(1), 'srl'); });"
    "srRow('sr14', function () { return "
    "  Object.assign({}, { d: { __html: new URLSearchParams(location.search).get('srm') } }).d.__html; });"
    "srRow('sr15', function () { return new URL(document.URL).searchParams.get('srn'); });"
    "srRow('sr16', function () { return "
    "  new URL(location.origin + location.pathname + location.search).searchParams.get('sro'); });"
    "srRow('sr17', function () { return `<b>${new URLSearchParams(location.search).get('srp')}</b>`; });"
    "srRow('sr18', function () { return window.name; });"
    "srRow('sr19', function () { return new URL(document.referrer).searchParams.get('srq'); });"
    "srRow('sr20', function () { return String(location).split('#')[1]; });"
    "window.addEventListener('message', function srMsg(e) {"
    "  if (e.data && e.data.srk) {"
    "    fetch('/api/sr12-arm'); srEl('sr12').insertAdjacentHTML('beforeend', e.data.srk); } });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

/* THE NEGATIVE HALF: the same sources are read and the markup sink is reached, but only with a constant. It
   must report the sink reached and untainted. */
static const char HTML_SRCREACHNEG[] =
    "<!doctype html><html><body>"
    "<div id='srnOut'></div>"
    "<script>"
    "var srnQ = new URLSearchParams(location.search).get('srz');"
    "var srnH = location.hash.slice(1);"
    "document.getElementById('srnOut').innerHTML = '<b>srn-constant</b>';"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
