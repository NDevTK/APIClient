/* The shuffle document. One of test_forced.c's exploring documents (its TF_DOCS table). A Fisher-Yates shuffle
   swaps four elements of an Array of DOM nodes at indices computed from ECMAScript §21.3.2.28 Math.random ( ),
   an environment unknown that holds an example, and appends each to their parent. The element read at such an
   index is the element at the example's index, a Node, so appendChild takes it and the rail still holds the
   four tiles. A second statement picks a String from an Array the same way and compares it, which keeps both
   arms. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SHUFFLE_H
#define ENGINE_HOST_FIXTURE_SHUFFLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SHUFFLE[] =
    "<!doctype html><html><body>"
    "<div id='shRail'><span class='shTile' data-g='a'></span><span class='shTile' data-g='b'></span>"
    "<span class='shTile' data-g='c'></span><span class='shTile' data-g='d'></span></div>"
    "<script>"
    "var shRail = document.getElementById('shRail');"
    "var shRest = Array.prototype.slice.call(shRail.querySelectorAll('.shTile'));"
    "for (var shI = shRest.length - 1; shI > 0; shI--) {"
    "  var shJ = Math.floor(Math.random() * (shI + 1));"
    "  var shSwap = shRest[shI]; shRest[shI] = shRest[shJ]; shRest[shJ] = shSwap;"
    "}"
    "shRest.forEach(function (tile) { shRail.appendChild(tile); });"
    "var shSeen = [];"
    "for (var shK = 0; shK < shRail.children.length; shK++) shSeen.push(shRail.children[shK].getAttribute('data-g'));"
    "fetch('/api/shuffled?n=' + shRail.children.length + '&set=' + shSeen.sort().join(''));"
    "var shPick = ['shFirst', 'shSecond'][Math.floor(Math.random() * 2)];"
    "fetch('/api/shpick?v=' + (shPick === 'shSecond' ? 'shTookSecond' : 'shTookFirst'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
