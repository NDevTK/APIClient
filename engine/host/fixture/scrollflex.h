/* THE SCROLLING AREA FLEX DOCUMENT: CSSOM VIEW §2's scrolling area of a flex container holding css-flexbox-1
   §4's anonymous flex item, the box "each child text sequence is wrapped in". One of test_forced.c's exploring
   documents (its TF_DOCS table). A text advance and a line height are font metrics, so every number but two is
   a difference against a block container holding the same text, where they cancel:
   - f1 puts the text after a 300px item in a 50px `nowrap` container, so its scrolling area ends at the
     anonymous item's far edge, 300px past r1's, whose `width: 0` content box the same text overflows.
   - f2 holds the text alone in a 50px container, so the item fits and the padding box is the answer.
   - f3 stretches the item to a 5px line, and its line boxes reach as far below as r3's, a `height: 0` block.
   - f4 aligns the item to the 5px line's end, so it overflows only upward and the padding box is the answer.
   It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SCROLLFLEX_H
#define ENGINE_HOST_FIXTURE_SCROLLFLEX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCROLLFLEX[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=f1 style=\"display:flex;width:50px;height:20px;overflow:hidden;white-space:nowrap\">"
    "<div style=\"flex:none;width:300px;height:1px\"></div>x</div>"
    "<div id=r1 style=\"width:0;height:20px;overflow:hidden;white-space:nowrap\">x</div>"
    "<div id=f2 style=\"display:flex;width:50px;height:20px;overflow:hidden\">x</div>"
    "<div id=f3 style=\"display:flex;width:50px;height:5px;overflow:hidden\">x</div>"
    "<div id=r3 style=\"width:50px;height:0;overflow:hidden\">x</div>"
    "<div id=f4 style=\"display:flex;align-items:flex-end;width:50px;height:5px;overflow:hidden\">x</div>"
    "<script>"
    "function E(i){return document.getElementById(i);}"
    "var q='d1='+(E('f1').scrollWidth-E('r1').scrollWidth)+'&w2='+E('f2').scrollWidth"
    "+'&d3='+(E('f3').scrollHeight-E('r3').scrollHeight)+'&h4='+E('f4').scrollHeight;"
    "fetch('/api/scrollflex?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
