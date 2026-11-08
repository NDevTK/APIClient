/* THE ATOMIC-AGENT DOCUMENT: a dedicated worker forks its flow while the page parses a page-sized fragment. The
   page posts its URL's fragment, an attacker source, to the worker, and the worker compares it against one
   literal per job, with a stretch of work between, so each comparison forks the flow, the page's agent included
   (flow_agents_fork). The page meanwhile sets 100000 paragraphs as a fragment's innerHTML, HTML §8.5.4's
   fragment parse, which rests over many quanta and which a fork may not clone mid-parse
   (fragment_parse_unforkable). The page's agent keeps the thread until the parse ends (flow_agent_atomic), so
   every fork lands outside it, and each arm reports the parsed count beside the literal its worker matched.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_AGENTATOMIC_H
#define ENGINE_HOST_FIXTURE_AGENTATOMIC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_AGENTATOMIC[] =
    "<!doctype html><html><body>"
    "<script>"
    "var aaW = new Worker(URL.createObjectURL(new Blob(["
    "  'self.onmessage = function (e) {'"
    "  + ' var h = e.data, t = 0, hit = \"none\";'"
    "  + ' function tick() {'"
    "  + '  if (t >= 24) { self.postMessage(hit); return; }'"
    "  + '  if (h === \"#t\" + t) hit = \"m\" + t;'"
    "  + '  for (var k = 0, s = 0; k < 20000; k++) s += k % 7;'"
    "  + '  t++; Promise.resolve().then(tick);'"
    "  + ' }'"
    "  + ' tick();'"
    "  + '};'"
    "], { type: 'text/javascript' })));"
    "var aaDone = 0, aaN = -1, aaHit = '';"
    "function aaReport() { if (++aaDone === 2) fetch('/api/aadone?n=' + aaN + '&w=' + aaHit); }"
    "aaW.onmessage = function (e) { aaHit = e.data; aaReport(); };"
    "aaW.postMessage(location.hash);"
    "var aaHtml = '';"
    "for (var aaI = 0; aaI < 100000; aaI++) aaHtml += '<p>x</p>';"
    "setTimeout(function () {"
    "  var d = document.createElement('div');"
    "  d.innerHTML = aaHtml;"
    "  aaN = d.childNodes.length;"
    "  aaReport();"
    "}, 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
