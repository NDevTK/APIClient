/* THE FLOAT PLACEMENT DOCUMENT: CSS 2.2 §9.5.1 "Positioning the float: the float property"' placement of
   block-level floats, and §10.6.7's auto height of the context root that holds them. One of test_forced.c's
   exploring documents (its TF_DOCS table). Each `flow-root` container is a block formatting context of its own
   holding floats and block boxes but no line box: left and right floats beside and below earlier ones, a float
   between collapsing margins (placed as if in an empty anonymous block parent), floats in nested containing blocks,
   floats with margins, floats wider than the room beside earlier floats or than the container, zero-height floats,
   and a float inside a relatively positioned block. Chrome 155.0.8059.84 and Firefox 158.0.1 give every number the
   rows expect for this markup. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLOATPLACE_H
#define ENGINE_HOST_FIXTURE_FLOATPLACE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLOATPLACE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0;line-height:10px;font-size:10px\">"
    "<style>.c{width:200px;margin-bottom:50px;position:relative;"
    "display:flow-root}.L{float:left}.R{float:right}</style>"
    "<div class=c id=c1><div class=L id=f1 style=\"width:50px;height:30px\"></div>"
    "<div id=b1 style=\"height:10px\"></div></div>"
    "<div class=c id=c2><div style=\"margin-bottom:20px;height:10px\"></div>"
    "<div class=L id=f2 style=\"width:50px;height:10px\"></div><div style=\"margin-top:30px;"
    "height:10px\" id=b2></div></div>"
    "<div class=c id=c8 style=\"overflow:hidden\"><div class=L style=\"width:50px;height:30px\"></div></div>"
    "<div class=c id=c9><div class=R id=f9 style=\"width:50px;height:30px\"></div></div>"
    "<div class=c id=c10><div class=L style=\"width:50px;height:30px\"></div>"
    "<div class=L id=f10b style=\"width:60px;height:20px\"></div><div class=L id=f10c style=\"width:100px;"
    "height:10px\"></div></div>"
    "<div class=c id=c11><div class=L style=\"width:150px;height:20px\"></div>"
    "<div class=L id=f11b style=\"width:100px;height:10px\"></div><div class=L id=f11c style=\"width:20px;"
    "height:10px\"></div></div>"
    "<div class=c id=c13><div style=\"margin-top:10px\"><div class=L id=f13 style=\"width:50px;height:20px\">"
    "</div></div><div style=\"margin-top:30px;height:10px\" id=b13></div></div>"
    "<div class=c id=c14><div style=\"margin-top:10px\"><div class=L id=f14 style=\"width:50px;height:20px\">"
    "</div><div style=\"margin-top:20px;height:10px\" id=b14></div></div></div>"
    "<div class=c id=c15><div style=\"height:10px\"></div><div style=\"margin-bottom:20px\"></div>"
    "<div class=L id=f15 style=\"width:50px;height:20px\"></div><div style=\"margin-top:5px;"
    "height:10px\" id=b15></div></div>"
    "<div class=c id=c16><div style=\"margin-left:20px;padding-top:5px\">"
    "<div class=L id=f16a style=\"width:50px;height:30px\"></div></div><div style=\"margin-left:40px\">"
    "<div class=L id=f16b style=\"width:50px;height:10px\"></div></div></div>"
    "<div class=c id=c17><div class=R id=f17a style=\"width:120px;height:20px\"></div>"
    "<div class=L id=f17b style=\"width:100px;height:10px\"></div></div>"
    "<div class=c id=c18><div class=L id=f18 style=\"width:50px;height:10px;margin:5px 10px\"></div>"
    "<div class=L id=f18b style=\"width:20px;height:10px;margin-top:3px\"></div></div>"
    "<div class=c id=c22><div class=R style=\"width:150px;height:20px\"></div>"
    "<div class=R id=f22b style=\"width:100px;height:10px\"></div><div class=R id=f22c style=\"width:20px;"
    "height:10px\"></div></div>"
    "<div class=c id=c25><div class=L style=\"width:50px;height:10px\"></div>"
    "<div class=L id=f25 style=\"width:250px;height:10px\"></div></div>"
    "<div class=c id=c26 style=\"padding:4px 0 0 9px\"><div style=\"margin-top:7px;"
    "height:10px\"></div><div class=R id=f26 style=\"width:30px;height:40px;margin-right:2px\"></div></div>"
    "<div class=c id=c27><div style=\"margin-top:10px\"><div class=L id=f27 style=\"width:50px;height:20px\">"
    "</div><div style=\"margin-bottom:15px\"></div></div><div style=\"margin-top:30px;height:10px\"></div>"
    "</div>"
    "<div class=c id=c28><div style=\"margin-top:10px\"><div style=\"margin-bottom:15px\"></div>"
    "<div class=L id=f28 style=\"width:50px;height:20px\"></div></div><div style=\"margin-top:30px;"
    "height:10px\"></div></div>"
    "<div class=c id=c29><div style=\"margin-top:10px\"><div style=\"margin-bottom:-5px\"></div>"
    "<div class=L id=f29 style=\"width:50px;height:20px\"></div><div style=\"margin-top:4px;"
    "height:10px\" id=b29></div></div></div>"
    "<div class=c id=c30><div style=\"margin-top:10px\"><div class=L id=f30 style=\"width:50px;height:20px\">"
    "</div></div></div>"
    "<div class=c id=c31><div class=R id=f31 style=\"width:250px;height:10px\"></div></div>"
    "<div class=c id=c32><div class=R style=\"width:20px;height:30px\"></div>"
    "<div class=R id=f32 style=\"width:250px;height:10px\"></div></div>"
    "<div class=c id=c33><div class=L style=\"width:20px;height:30px\"></div>"
    "<div class=L id=f33 style=\"width:190px;height:10px\"></div></div>"
    "<div class=c id=c34><div class=L style=\"width:30px;height:10px\"></div>"
    "<div class=L id=f34 style=\"width:0;height:0\"></div><div class=L id=f34b style=\"width:170px;"
    "height:0\"></div></div>"
    "<div class=c id=c35><div style=\"position:relative;left:7px;top:3px;margin-left:5px\">"
    "<div class=L id=f35 style=\"width:30px;height:10px\"></div></div></div>"
    "<script>"
    "function E(i){return document.getElementById(i).getBoundingClientRect();}"
    "function P(a,c){return a+'x='+(E(a).left-E(c).left)+'&'+a+'y='+(E(a).top-E(c).top);}"
    "var q="
    "[P('f1','c1'),P('b1','c1'),P('f2','c2'),P('b2','c2'),P('f9','c9'),P('f10b','c10'),P('f10c','c10'),"
    "P('f11b','c11'),P('f11c','c11'),P('f13','c13'),P('b13','c13'),P('f14','c14'),P('b14','c14'),"
    "P('f15','c15'),P('b15','c15'),P('f16a','c16'),P('f16b','c16'),P('f17a','c17'),P('f17b','c17'),"
    "P('f18','c18'),P('f18b','c18'),P('f22b','c22'),P('f22c','c22'),P('f25','c25'),P('f26','c26'),"
    "P('f27','c27'),P('f28','c28'),P('f29','c29'),P('b29','c29'),P('f30','c30'),P('f31','c31'),"
    "P('f32','c32'),P('f33','c33'),P('f34','c34'),P('f34b','c34'),P('f35','c35')].join('&')"
    "+'&c1h='+E('c1').height+'&c8h='+E('c8').height+'&c13h='+E('c13').height+'&c14h='+E('c14').height"
    "+'&c16h='+E('c16').height+'&c18h='+E('c18').height+'&c22h='+E('c22').height+'&c25h='+E('c25').height"
    "+'&c26h='+E('c26').height+'&c27h='+E('c27').height+'&c30h='+E('c30').height+'&c33h='+E('c33').height;"
    "fetch('/api/floatplace?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
