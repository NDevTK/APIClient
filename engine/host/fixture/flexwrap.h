/* THE FLEX WRAP DOCUMENT: css-flexbox-1 §9.6's placement of a multi-line flex container's lines and items. One of
   test_forced.c's exploring documents (its TF_DOCS table). Each 100px-wide `wrap` container breaks two 60px
   items into two lines of cross sizes 10px and 20px: in a 60px-high container under `align-content: normal`
   (§9.4's step 9 stretches the lines to 25px and 35px), `center`, `space-between`, and with `align-items:
   center` inside the stretched lines; and with an automatic height, where a third item shares the second line.
   Two more containers each hold an empty 40px item stretched over a line its 40px sibling sizes, a 50px block
   (f6) or a baseline-aligned item 9px of padding over one 20px line box (f7); the empty item's clientHeight is
   read first, so §9.4 step 8 measures the sibling, and step 8.1 its baseline-sharing group, under that
   member's resting job with nothing else asked.
   Chrome 155.0.8059.84 and Firefox 158.0.1 give every number the rows expect for this markup. It ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXWRAP_H
#define ENGINE_HOST_FIXTURE_FLEXWRAP_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLEXWRAP[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=f1 style=\"display:flex;flex-wrap:wrap;width:100px;height:60px\"><div id=a1 style=\"width:60px;"
    "height:10px\"></div><div id=b1 style=\"width:60px;height:20px\"></div></div>"
    "<div id=f2 style=\"display:flex;flex-wrap:wrap;width:100px;height:60px;align-content:center\"><div id=a2 "
    "style=\"width:60px;height:10px\"></div><div id=b2 style=\"width:60px;height:20px\"></div></div>"
    "<div id=f3 style=\"display:flex;flex-wrap:wrap;width:100px;height:60px;align-content:space-between\">"
    "<div id=a3 style=\"width:60px;height:10px\"></div><div id=b3 style=\"width:60px;height:20px\"></div></div>"
    "<div id=f4 style=\"display:flex;flex-wrap:wrap;width:100px;height:60px;align-items:center\"><div id=a4 "
    "style=\"width:60px;height:10px\"></div><div id=b4 style=\"width:60px;height:20px\"></div></div>"
    "<div id=f5 style=\"display:flex;flex-wrap:wrap;width:100px\"><div id=a5 style=\"width:60px;height:10px\">"
    "</div><div id=b5 style=\"width:60px;height:20px\"></div><div id=c5 style=\"width:30px;height:5px\"></div>"
    "</div>"
    "<div id=f6 style=\"display:flex;flex-wrap:wrap;width:100px\"><div id=a6 style=\"width:40px\"></div>"
    "<div style=\"width:40px\"><div style=\"height:50px\"></div></div></div>"
    "<div id=f7 style=\"display:flex;flex-wrap:wrap;width:100px\"><div id=a7 style=\"width:40px\"></div>"
    "<div style=\"width:40px;align-self:baseline;padding-top:9px;line-height:20px\">xy</div></div>"
    "<script>"
    "var h6=document.getElementById('a6').clientHeight,h7=document.getElementById('a7').clientHeight;"
    "function R(i){return document.getElementById(i).getBoundingClientRect();}"
    "function T(i,f){return R(i).top-R(f).top;}"
    "var q='a1='+T('a1','f1')+'&b1='+T('b1','f1')+'&a2='+T('a2','f2')+'&b2='+T('b2','f2')"
    "+'&a3='+T('a3','f3')+'&b3='+T('b3','f3')+'&a4='+T('a4','f4')+'&b4='+T('b4','f4')"
    "+'&a5='+T('a5','f5')+'&b5='+T('b5','f5')+'&c5='+T('c5','f5')+'&c5l='+(R('c5').left-R('f5').left)"
    "+'&f5h='+R('f5').height+'&h6='+h6+'&h7='+h7;"
    "fetch('/api/flexwrap?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
