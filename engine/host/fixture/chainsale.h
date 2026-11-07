/* THE CHAIN-SALE DOCUMENT: the boot forks on the URL's fragment, an attacker source, so neither arm is priced as
   an assumption and the two take turns. Each arm calls an async function that calls a generator's `next()`,
   whose body calls chGrow(), which grows the heap by strings over many quanta; the document is served with a
   small share (its TF_DOCS row), so RAM pressure sells an arm parked inside chGrow() with the two bodies
   standing mid-call below it on its stashed heap-frame chain, each holding 50 objects of its own. chGrow()'s
   strings are no objects, so the sale census's object count for that arm is the two bodies' 100 and the few
   the frames above hold, and the bodies' are counted only if each coroutine record marks its own mid-call
   frame. The surviving arm finishes and reports the three sizes.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_CHAINSALE_H
#define ENGINE_HOST_FIXTURE_CHAINSALE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CHAINSALE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var chMode = location.hash === '#ch' ? 'held' : 'free';"
    "function chGrow() {"
    "  var b = [];"
    "  for (var i = 0; i < 1000000; i++) b.push('v' + i);"
    "  return b.length / 10000;"
    "}"
    "function* chGen() {"
    "  var kg = [];"
    "  for (var i = 0; i < 50; i++) kg.push({ g: i });"
    "  yield chGrow() + kg.length;"
    "}"
    "async function chRun() {"
    "  var ka = [];"
    "  for (var i = 0; i < 50; i++) ka.push({ a: i });"
    "  var it = chGen();"
    "  var r = it.next();"
    "  return r.value + ka.length;"
    "}"
    "chRun().then(function (n) { fetch('/api/chsell?m=' + chMode + '&n=' + n); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
