/* THE LINE SHORTENING DOCUMENT: CSS 2.2 §9.5's "the current and subsequent line boxes created next to the float
   are shortened as necessary to make room for the margin box of the float". One of test_forced.c's exploring
   documents (its TF_DOCS table). Each `flow-root` container holds floats and then a block whose line boxes, of
   40px inline-blocks, sit beside them: lines shortened beside a left and a right float and full-width below,
   a float in an earlier block's subtree reaching the next block's lines, a band that narrows again at a lower
   float with `clear: left`, a padding edge inside the float's band, and `text-align: right` against a shortened
   line. Chrome 155.0.8059.84, the oracle, gives every number the rows expect for this markup. Cross-engine note:
   Firefox 158.0.1 agrees on every inline position and height and puts each inline-block's baseline half a pixel
   lower. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SHORTENING_H
#define ENGINE_HOST_FIXTURE_SHORTENING_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SHORTENING[] =
    "<!doctype html><html>"
    "<body style=\"margin:0;line-height:10px;font-size:10px\">"
    "<style>.c{width:200px;margin-bottom:50px;position:relative;"
    "display:flow-root}.L{float:left}.R{float:right}i{display:inline-block;height:5px;width:40px}</style>"
    "<div class=c id=s1><div class=L style=\"width:50px;height:30px\"></div><div id=d1><i id=s1a></i><i>"
    "</i><i id=s1c></i><i id=s1d></i><i></i><i></i><i></i><i></i><i></i><i></i><i id=s1k></i></div></div>"
    "<div class=c id=s2><div class=R style=\"width:50px;height:15px\"></div><div id=d2><i id=s2a></i><i>"
    "</i><i></i><i id=s2d></i><i></i><i id=s2f></i></div></div>"
    "<div class=c id=s3><div style=\"height:5px\"><div class=L style=\"width:120px;height:20px\"></div></div>"
    "<div id=d3 style=\"padding-left:10px\"><i id=s3a></i><i id=s3b></i><i id=s3c></i></div></div>"
    "<div class=c id=s4><div class=L style=\"width:50px;height:10px\"></div>"
    "<div class=L style=\"width:100px;height:25px;clear:left\"></div><div id=d4><i id=s4a></i><i id=s4b>"
    "</i><i id=s4c></i><i id=s4d></i><i id=s4e></i></div></div>"
    "<div class=c id=s5><div style=\"height:10px\"><div class=L style=\"width:50px;height:30px\"></div></div>"
    "<div id=d5><i id=s5a style=\"width:30px\"></i></div></div>"
    "<div class=c id=s6><div class=R style=\"width:70px;height:12px\"></div>"
    "<div id=d6 style=\"text-align:right\"><i id=s6a></i><i id=s6b></i><i id=s6c></i><i id=s6d></i></div>"
    "</div>"
    "<script>"
    "function E(i){return document.getElementById(i).getBoundingClientRect();}"
    "function X(a,c){return E(a).left-E(c).left;}"
    "function Y(a,c){return E(a).top-E(c).top;}"
    "var q='s1ax='+X('s1a','s1')+'&s1ay='+Y('s1a','s1')+'&s1cx='+X('s1c','s1')+'&s1cy='+Y('s1c','s1')"
    "+'&s1dx='+X('s1d','s1')+'&s1dy='+Y('s1d','s1')+'&s1kx='+X('s1k','s1')+'&s1ky='+Y('s1k','s1')"
    "+'&s2ax='+X('s2a','s2')+'&s2ay='+Y('s2a','s2')+'&s2dx='+X('s2d','s2')+'&s2dy='+Y('s2d','s2')"
    "+'&s2fx='+X('s2f','s2')+'&s2fy='+Y('s2f','s2')+'&s3ax='+X('s3a','s3')+'&s3ay='+Y('s3a','s3')"
    "+'&s3bx='+X('s3b','s3')+'&s3by='+Y('s3b','s3')+'&s3cx='+X('s3c','s3')+'&s3cy='+Y('s3c','s3')"
    "+'&s4ax='+X('s4a','s4')+'&s4ay='+Y('s4a','s4')+'&s4bx='+X('s4b','s4')+'&s4by='+Y('s4b','s4')"
    "+'&s4cx='+X('s4c','s4')+'&s4cy='+Y('s4c','s4')+'&s4dx='+X('s4d','s4')+'&s4dy='+Y('s4d','s4')"
    "+'&s4ex='+X('s4e','s4')+'&s4ey='+Y('s4e','s4')+'&s5ax='+X('s5a','s5')+'&s5ay='+Y('s5a','s5')"
    "+'&s6ax='+X('s6a','s6')+'&s6ay='+Y('s6a','s6')+'&s6cx='+X('s6c','s6')+'&s6cy='+Y('s6c','s6')"
    "+'&s6dx='+X('s6d','s6')+'&s6dy='+Y('s6d','s6')+'&s1h='+E('s1').height+'&s2h='+E('s2').height"
    "+'&s3h='+E('s3').height+'&s4h='+E('s4').height+'&d1h='+E('d1').height+'&d3h='+E('d3').height"
    "+'&d4h='+E('d4').height+'&s5h='+E('s5').height+'&s6h='+E('s6').height;"
    "fetch('/api/shortening?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
