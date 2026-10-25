/* THE LINE FLOAT DOCUMENT: CSS 2.2 §9.5's float inside a run of inline content, placed at the line box it falls
   on: "if there is a line box, the outer top of the floated box is aligned with the top of the current line box",
   when it fits beside that line's content, which then starts beside it, and otherwise at the next line box. One of
   test_forced.c's exploring documents (its TF_DOCS table). Each `flow-root` container holds 40px-scale
   inline-blocks around floats: a float after content that moves that content beside it, one that does not fit and
   starts on the next line, one before all content, a right float mid-line, two floats side by side ahead of a long
   line, and a float after a forced break. Chrome 155.0.8059.84, the oracle, gives every number the rows expect for
   this markup. Cross-engine note: Firefox 158.0.1 agrees on every float position, inline position and height and
   puts each inline-block's baseline half a pixel lower. The script's query is one plain string concatenation on
   purpose: an `Array.join` over the same pieces is opaque as a whole once any piece carries the font metrics'
   environment, which the baselines here do; keep it concatenated until a join composes like concatenation. It
   ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LINEFLOAT_H
#define ENGINE_HOST_FIXTURE_LINEFLOAT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LINEFLOAT[] =
    "<!doctype html><html>"
    "<body style=\"margin:0;line-height:10px;font-size:10px\">"
    "<style>.c{width:200px;margin-bottom:50px;position:relative;display:flow-root}i{display:inline-block;"
    "height:5px}.L{float:left;height:20px}.R{float:right;height:20px}</style>"
    "<div class=c id=c3><i style=\"width:30px\"></i><span class=L id=f3 style=\"width:50px\"></span>"
    "<i style=\"width:30px\" id=i3></i></div>"
    "<div class=c id=c4><i style=\"width:160px\"></i><span class=L id=f4 style=\"width:50px\"></span>"
    "<i style=\"width:10px\" id=i4></i></div>"
    "<div class=c id=c5 style=\"width:100px\"><span class=L id=f5 style=\"width:60px;height:25px\"></span>"
    "<i style=\"width:30px\" id=i5a></i><i style=\"width:30px\" id=i5b></i><i style=\"width:30px\" id=i5c></i>"
    "<i style=\"width:30px\" id=i5d></i></div>"
    "<div class=c id=c6><i style=\"width:40px\" id=i6a></i><span class=R id=f6 style=\"width:70px\"></span>"
    "<i style=\"width:40px\" id=i6b></i><i style=\"width:40px\" id=i6c></i><i style=\"width:40px\" id=i6d></i>"
    "</div>"
    "<div class=c id=c7><span class=L id=f7a style=\"width:30px\"></span>"
    "<span class=L id=f7b style=\"width:40px;height:10px\"></span><i style=\"width:100px\" id=i7a></i>"
    "<i style=\"width:60px\" id=i7b></i></div>"
    "<div class=c id=c8><i style=\"width:50px\" id=i8a></i><br><span class=L id=f8 style=\"width:30px\">"
    "</span><i style=\"width:20px\" id=i8b></i></div>"
    "<script>"
    "function E(i){return document.getElementById(i).getBoundingClientRect();}"
    "function X(a,c){return E(a).left-E(c).left;}"
    "function Y(a,c){return E(a).top-E(c).top;}"
    "var q='f3x='+X('f3','c3')+'&f3y='+Y('f3','c3')+'&i3x='+X('i3','c3')+'&i3y='+Y('i3','c3')"
    "+'&f4x='+X('f4','c4')+'&f4y='+Y('f4','c4')+'&i4x='+X('i4','c4')+'&i4y='+Y('i4','c4')"
    "+'&f5x='+X('f5','c5')+'&f5y='+Y('f5','c5')+'&i5ax='+X('i5a','c5')+'&i5ay='+Y('i5a','c5')"
    "+'&i5cx='+X('i5c','c5')+'&i5cy='+Y('i5c','c5')+'&i5dx='+X('i5d','c5')+'&i5dy='+Y('i5d','c5')"
    "+'&f6x='+X('f6','c6')+'&f6y='+Y('f6','c6')+'&i6bx='+X('i6b','c6')+'&i6by='+Y('i6b','c6')"
    "+'&i6cx='+X('i6c','c6')+'&i6cy='+Y('i6c','c6')+'&i6dx='+X('i6d','c6')+'&i6dy='+Y('i6d','c6')"
    "+'&f7ax='+X('f7a','c7')+'&f7ay='+Y('f7a','c7')+'&f7bx='+X('f7b','c7')+'&f7by='+Y('f7b','c7')"
    "+'&i7ax='+X('i7a','c7')+'&i7ay='+Y('i7a','c7')+'&i7bx='+X('i7b','c7')+'&i7by='+Y('i7b','c7')"
    "+'&f8x='+X('f8','c8')+'&f8y='+Y('f8','c8')+'&i8bx='+X('i8b','c8')+'&i8by='+Y('i8b','c8')"
    "+'&c3h='+E('c3').height+'&c4h='+E('c4').height+'&c5h='+E('c5').height+'&c6h='+E('c6').height"
    "+'&c7h='+E('c7').height+'&c8h='+E('c8').height;"
    "fetch('/api/linefloat?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
