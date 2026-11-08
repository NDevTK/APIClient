/* The scroll-rest document (`scrollrest`): CSSOM View §6.1's scroll a target into view for a target inside an
   `overflow:auto` box below 20,000 one-pixel block boxes (test_forced.c's COLLREST_10000, twice, each `<i>` made
   a block). Both scrolling boxes' area walks and step 2.2's border box read are CSS 2.1 §9.4.1's walk over those
   boxes, which a resting drive must hold. Chrome scrolls the box to 160 and the window to 19,330, then fires
   §13.2's `scroll` and `scrollend` at the box (neither bubbles) and at the document (both bubble). A scrolled
   box, and a frozen one, show the page no own key, symbol or clone, and the frozen one still scrolls.
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
    "<script>"
    "var srBox = document.getElementById('srBox'), srB = '', srD = '';"
    "function srF(e) { return (e.bubbles ? 1 : 0) + '' + (e.cancelable ? 1 : 0); }"
    "srBox.addEventListener('scroll', function (e) { srB += 's' + srF(e); });"
    "srBox.addEventListener('scrollend', function (e) { srB += 'e' + srF(e); });"
    "document.addEventListener('scroll', function (e) { srD += e.target === document ? 's' + srF(e) : 'x'; });"
    "window.addEventListener('scroll', function () { srD += 'w'; });"
    "document.addEventListener('scrollend', function (e) {"
    "  if (e.target !== document) { srD += 'x'; return; }"
    "  fetch('/api/srscrolled?b=' + srB + '&d=' + srD + 'e' + srF(e));"
    "});"
    "document.getElementById('srT').scrollIntoView();"
    "fetch('/api/scrollintorest?t=' + srBox.scrollTop + '&y=' + window.scrollY);"
    "var srBox2 = document.getElementById('srBox2');"
    "Object.freeze(srBox2);"
    "srBox2.scrollTop = 10;"
    "function srLeak(el) {"
    "  var own = 0, k, c, p;"
    "  for (k in el) if (Object.prototype.hasOwnProperty.call(el, k)) own++;"
    "  try { structuredClone(el); c = 'ok'; } catch (e) { c = e.name; }"
    "  try { postMessage(el, '*'); p = 'ok'; } catch (e) { p = e.name; }"
    "  return Reflect.ownKeys(el).length + '.' + Object.getOwnPropertySymbols(el).length + '.' +"
    "         Reflect.ownKeys(new Proxy(el, {})).length + '.' +"
    "         Reflect.ownKeys(Object.getOwnPropertyDescriptors(el)).length + '.' + own + '.' + c + '.' + p;"
    "}"
    "fetch('/api/srleak?k=' + srLeak(srBox) + '&f=' + srLeak(srBox2) + '&s=' + srBox2.scrollTop);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
