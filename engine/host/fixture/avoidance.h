/* THE FLOAT AVOIDANCE DOCUMENT: CSS 2.2 §9.5's rule that the border box of an element establishing a block
   formatting context "must not overlap the margin box of any floats in the same block formatting context", and
   the same for a flex container (css-flexbox-1 §3) and a grid container (css-grid-1 §5.1). One of test_forced.c's
   exploring documents (its TF_DOCS table). Each `flow-root` container holds floats and then such a box: an `auto`
   width narrowed to the band beside left and right floats (whatever its content's width), with margins that
   overlap the floats and one that reaches past them, a definite width that does not fit and moves below the float,
   a top margin that already puts the box beside or below the float, a band that opens lower at a float's bottom,
   and a box taller than the float beside it. Chrome 155.0.8059.84 and Firefox 158.0.1 give every number the rows
   expect for this markup. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_AVOIDANCE_H
#define ENGINE_HOST_FIXTURE_AVOIDANCE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_AVOIDANCE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0;line-height:10px;font-size:10px\">"
    "<style>.c{width:200px;margin-bottom:50px;position:relative;"
    "display:flow-root}.L{float:left}.R{float:right}.o{overflow:hidden}</style>"
    "<div class=c id=a1><div class=L style=\"width:50px;height:30px\"></div>"
    "<div id=ab1 class=o style=\"height:10px\"></div></div>"
    "<div class=c id=a2><div class=L style=\"width:50px;height:30px\"></div>"
    "<div id=ab2 class=o style=\"height:50px\"></div></div>"
    "<div class=c id=a3><div class=L style=\"width:50px;height:30px\"></div><div class=R style=\"width:30px;"
    "height:20px\"></div><div id=ab3 class=o style=\"height:10px;margin-left:5px;margin-right:7px\"></div>"
    "</div>"
    "<div class=c id=a4><div class=L style=\"width:150px;height:30px\"></div>"
    "<div id=ab4 class=o style=\"height:10px;width:80px\"></div></div>"
    "<div class=c id=a5><div class=L style=\"width:150px;height:30px\"></div>"
    "<div id=ab5 class=o style=\"height:10px;margin-top:8px\"></div></div>"
    "<div class=c id=a6><div class=L style=\"width:150px;height:30px\"></div>"
    "<div id=ab6 class=o style=\"height:10px;margin-top:40px\"></div></div>"
    "<div class=c id=a7><div class=L style=\"width:150px;height:30px\"></div><div id=ab7 class=o>"
    "<div style=\"width:80px;height:10px\"></div></div></div>"
    "<div class=c id=a8><div class=L style=\"width:50px;height:30px\"></div><div id=ab8 class=o>"
    "<div style=\"height:45px\"></div></div></div>"
    "<div class=c id=a9><div style=\"height:10px;margin-bottom:20px\"></div>"
    "<div class=L style=\"width:150px;height:30px\"></div><div id=ab9 class=o style=\"height:10px;"
    "margin-top:5px;width:80px\"></div><div id=an9 style=\"height:10px;margin-top:3px\"></div></div>"
    "<div class=c id=a10><div class=L style=\"width:50px;height:30px\"></div>"
    "<div class=L style=\"width:120px;height:10px;margin-top:0\"></div>"
    "<div id=ab10 class=o style=\"height:20px;width:40px\"></div></div>"
    "<div class=c id=a11><div class=L style=\"width:50px;height:30px\"></div>"
    "<div id=ab11 style=\"display:flex;height:10px\"></div></div>"
    "<div class=c id=a12><div class=R style=\"width:60px;height:30px\"></div>"
    "<div id=ab12 style=\"display:grid;height:10px;margin-top:4px\"></div></div>"
    "<div class=c id=a13><div class=L style=\"width:150px;height:30px\"></div>"
    "<div id=ab13 style=\"display:flex;width:80px;height:10px\"></div></div>"
    "<div class=c id=a14><div class=L style=\"width:50px;height:30px\"></div>"
    "<div id=ab14 style=\"display:flow-root;height:10px;margin-left:60px\"></div></div>"
    "<script>"
    "function E(i){return document.getElementById(i).getBoundingClientRect();}"
    "function P(a,c){return a+'x='+(E(a).left-E(c).left)+'&'+a+'y='+(E(a).top-E(c).top)+'&'+a"
    "+'w='+E(a).width;}"
    "var q="
    "[P('ab1','a1'),P('ab2','a2'),P('ab3','a3'),P('ab4','a4'),P('ab5','a5'),P('ab6','a6'),P('ab7','a7'),"
    "P('ab8','a8'),P('ab9','a9'),P('an9','a9'),P('ab10','a10'),P('ab11','a11'),P('ab12','a12'),"
    "P('ab13','a13'),P('ab14','a14')].join('&')"
    "+['a1','a2','a5','a6','a8','a9','a10','a13'].map(function(k){return '&'+k"
    "+'h='+E(k).height;}).join('');"
    "fetch('/api/avoidance?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
