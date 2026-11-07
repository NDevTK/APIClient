/* THE RANGE TEXT RECTANGLE DOCUMENT (`rangetext`): CSSOM View §9's getClientRects() and getBoundingClientRect()
   over Text nodes, CodeMirror's measuring shape (astexplorer's). `#d1` holds "abcd" as one Text node and `#d2`
   the same characters in a span (one shaping run each, so the same kerning), so a range over all of `#d1`'s text
   answers the rectangle of `#all`'s content area: the advance wide, the font's ascent and descent tall. A range
   over [1, 3) starts where [0, 1) ends and ends where [2, 3) does, and its bounding box is its one rectangle. A
   collapsed range at 2 is a zero-width caret where [2, 3) starts. `#d3`'s text wraps onto two lines, so a range
   over all of it answers two rectangles.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RANGETEXT_H
#define ENGINE_HOST_FIXTURE_RANGETEXT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RANGETEXT[] =
    "<!doctype html><html><body style=\"margin:0;font-size:20px;line-height:30px\">"
    "<div id=d1>abcd</div>"
    "<div id=d2><span id=all>abcd</span></div>"
    "<div id=d3 style=\"width:30px\">aa bb</div>"
    "<script>"
    "function rtR(node, a, b) { var r = document.createRange(); r.setStart(node, a); r.setEnd(node, b);"
    "                           return r; }"
    "function rtD(x) { return Math.round(x * 100); }"
    "var t1 = document.getElementById('d1').firstChild;"
    "var o1 = document.getElementById('d1').getBoundingClientRect(), o2 = document.getElementById('d2')"
    "         .getBoundingClientRect();"
    "var sel = rtR(t1, 0, 4).getClientRects(), bc = document.getElementById('all').getBoundingClientRect();"
    "var mid = rtR(t1, 1, 3).getClientRects(), box = rtR(t1, 1, 3).getBoundingClientRect();"
    "var a0 = rtR(t1, 0, 1).getClientRects()[0];"
    "var caret = rtR(t1, 2, 2).getClientRects(), after = rtR(t1, 2, 3).getClientRects();"
    "var t3 = document.getElementById('d3').firstChild;"
    "fetch('/api/rangetext?n=' + sel.length + '&x=' + rtD((sel[0].left - o1.left) - (bc.left - o2.left)) +"
    "      '&y=' + rtD((sel[0].top - o1.top) - (bc.top - o2.top)) + '&w=' + rtD(sel[0].width - bc.width) +"
    "      '&h=' + rtD(sel[0].height - bc.height) + '&bw=' + rtD(box.width - mid[0].width) + '&mn=' + mid.length +"
    "      '&ml=' + rtD(mid[0].left - a0.right) +"
    "      '&cn=' + caret.length + '&cw=' + rtD(caret[0].width) + '&cx=' + rtD(caret[0].left - after[0].left) +"
    "      '&mr=' + rtD(mid[0].right - after[0].right) +"
    "      '&wn=' + rtR(t3, 0, 5).getClientRects().length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
