/* THE SCROLLING AREA ICB DOCUMENT: CSSOM VIEW §2's scrolling area of an element against a descendant laid out in
   the initial containing block or the viewport. One of test_forced.c's exploring documents (its TF_DOCS
   table). Each 100x50 scroll container holds a `position: absolute` box with no positioned ancestor, so CSS
   2.1 §10.1 makes the initial containing block its containing block: placed by insets past the element's
   padding edge, nested one level deeper and placed below it, at its static position pushed right by a margin,
   and a visually hidden `h1` inside `overflow: hidden`, the shape a real page's title takes. Two more hold a
   `position: fixed` box, whose containing block is the viewport. None moves when the element scrolls, so none
   is in its scrolling area, and `scrollWidth` and `scrollHeight` stay the padding box's. Chrome 155.0.8059.84
   gives every number the rows expect for this markup, and Firefox 158.0.1 agrees on every `overflow: hidden`
   one. Cross-engine note: on the `overflow: scroll` ones Firefox excludes the boxes too and answers 88 and 38,
   its client sizes inside the 12px scrollbars it renders, where Chrome and this engine render none. It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SCROLLICB_H
#define ENGINE_HOST_FIXTURE_SCROLLICB_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCROLLICB[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=s1 style=\"overflow:scroll;width:100px;height:50px\"><div style=\"position:absolute;left:300px;"
    "top:0;width:40px;height:10px\"></div></div>"
    "<div id=s2 style=\"overflow:scroll;width:100px;height:50px;margin-top:20px\"><div><div style=\""
    "position:absolute;left:300px;top:100px;width:40px;height:10px\"></div></div></div>"
    "<div id=s3 style=\"overflow:scroll;width:100px;height:50px;margin-top:20px\"><div style=\""
    "position:absolute;width:40px;height:10px;margin-left:200px\"></div></div>"
    "<div id=s4 style=\"overflow:hidden;width:100px;height:50px;margin-top:20px\"><h1 style=\"position:absolute;"
    "height:1px;width:1px;overflow:hidden;clip:rect(1px,1px,1px,1px);white-space:nowrap;margin:0;left:300px;"
    "top:200px\">Title</h1></div>"
    "<div id=s5 style=\"overflow:hidden;width:100px;height:50px;margin-top:20px\"><div style=\"position:fixed;"
    "left:300px;top:200px;width:40px;height:10px\"></div></div>"
    "<div id=s6 style=\"overflow:scroll;width:100px;height:50px;margin-top:20px\"><div style=\"position:fixed;"
    "width:40px;height:10px;margin-left:200px\"></div></div>"
    "<script>"
    "function E(i){return document.getElementById(i);}"
    "var q='s1w='+E('s1').scrollWidth+'&s1h='+E('s1').scrollHeight+'&s2w='+E('s2').scrollWidth"
    "+'&s2h='+E('s2').scrollHeight+'&s3w='+E('s3').scrollWidth+'&s3h='+E('s3').scrollHeight"
    "+'&s4w='+E('s4').scrollWidth+'&s4h='+E('s4').scrollHeight+'&s5w='+E('s5').scrollWidth"
    "+'&s5h='+E('s5').scrollHeight+'&s6w='+E('s6').scrollWidth+'&s6h='+E('s6').scrollHeight;"
    "fetch('/api/scrollicb?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
