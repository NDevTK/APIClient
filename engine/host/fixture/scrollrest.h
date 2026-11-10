/* The scroll-rest document (`scrollrest`): CSSOM View §6.1's scroll a target into view for a target inside an
   `overflow:auto` box below 20,000 one-pixel block boxes (test_forced.c's COLLREST_10000, twice, each `<i>` made
   a block). Both scrolling boxes' area walks and step 2.2's border box read are CSS 2.1 §9.4.1's walk over those
   boxes, which a resting drive must hold. Chrome scrolls the box to 160 and the window to 19,330, then fires
   §13.2's `scroll` and `scrollend` at the box (neither bubbles) and then at the document (both bubble), all
   before the frame's animation callbacks; a fourth box scrolled from the first box's `scroll` listener gets
   its two events in the next frame. Two more boxes, which hold no other engine state (no listener), are
   scrolled by `scrollTop`, one of them frozen first: each shows the page no own key, symbol or clone, and the
   frozen one still scrolls.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SCROLLREST_H
#define ENGINE_HOST_FIXTURE_SCROLLREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCROLLREST[] =
    "<!doctype html><html><head><style>#srPre i{display:block;height:1px}</style></head>"
    "<body style=\"margin:0\"><div id=srPre>" COLLREST_10000 COLLREST_10000 "</div>"
    "<div id=srBox style=\"overflow:auto;height:50px\"><div style=\"height:200px\"></div>"
    "<div id=srT style=\"height:10px\"></div></div>"
    "<div id=srBox2 style=\"position:absolute;top:0;overflow:auto;width:20px;height:20px\">"
    "<div style=\"height:100px\"></div></div>"
    "<div id=srBox3 style=\"position:absolute;top:0;overflow:auto;width:20px;height:20px\">"
    "<div style=\"height:100px\"></div></div>"
    "<div id=srBox4 style=\"position:absolute;top:0;overflow:auto;width:20px;height:20px\">"
    "<div style=\"height:100px\"></div></div>"
    "<script>"
    "var srBox = document.getElementById('srBox'), srBox4 = document.getElementById('srBox4'), srQ = '';"
    "function srF(e) { return (e.bubbles ? 1 : 0) + '' + (e.cancelable ? 1 : 0); }"
    "function srL(n, t) {"
    "  t.addEventListener('scroll', function (e) { srQ += (e.target === t ? n + 's' + srF(e) : n + 'x') + '.'; });"
    "  t.addEventListener('scrollend', function (e) { srQ += (e.target === t ? n + 'e' + srF(e) : n + 'x') + '.'; });"
    "}"
    "srL('b', srBox); srL('d', document); srL('4', srBox4);"
    "window.addEventListener('scroll', function () { srQ += 'w.'; });"
    "srBox.addEventListener('scroll', function () { srBox4.scrollTop = 5; });"
    "requestAnimationFrame(function () { srQ += 'f.'; });"
    "srBox4.addEventListener('scrollend', function () {"
    "  fetch('/api/srscrolled?o=' + srQ + '&t4=' + srBox4.scrollTop);"
    "});"
    "document.getElementById('srT').scrollIntoView();"
    "fetch('/api/scrollintorest?t=' + srBox.scrollTop + '&y=' + window.scrollY);"
    "var srBox2 = document.getElementById('srBox2'), srBox3 = document.getElementById('srBox3');"
    "Object.freeze(srBox2);"
    "srBox2.scrollTop = 10;"
    "srBox3.scrollTop = 30;"
    "function srLeak(el) {"
    "  var own = 0, k, c, p;"
    "  for (k in el) if (Object.prototype.hasOwnProperty.call(el, k)) own++;"
    "  try { structuredClone(el); c = 'ok'; } catch (e) { c = e.name; }"
    "  try { postMessage(el, '*'); p = 'ok'; } catch (e) { p = e.name; }"
    "  return Reflect.ownKeys(el).length + '.' + Object.getOwnPropertySymbols(el).length + '.' +"
    "         Reflect.ownKeys(new Proxy(el, {})).length + '.' +"
    "         Reflect.ownKeys(Object.getOwnPropertyDescriptors(el)).length + '.' + own + '.' + c + '.' + p;"
    "}"
    "fetch('/api/srleak?k=' + srLeak(srBox3) + '&f=' + srLeak(srBox2) + '&s=' + srBox2.scrollTop +"
    "      '&p=' + srBox3.scrollTop);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
