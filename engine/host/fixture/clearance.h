/* THE CLEARANCE DOCUMENT: CSS 2.2 §9.5.2 "Controlling flow next to floats: the clear property". One of
   test_forced.c's exploring documents (its TF_DOCS table). Each `flow-root` container is a block formatting
   context holding floats and a box with `clear` set: below a left float, both floats and the right one, after a
   float its container holds while that container's top margin still escapes (where the clearance resolves the
   container's top edge before the cleared box's margin), after margins that collapse to a position already below
   the float (no clearance), with a large top margin that clearance absorbs, and for an empty box that would
   otherwise collapse through. Chrome 155.0.8059.84 and Firefox 158.0.1 give every number the rows expect for this
   markup. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CLEARANCE_H
#define ENGINE_HOST_FIXTURE_CLEARANCE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CLEARANCE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0;line-height:10px;font-size:10px\">"
    "<style>.c{width:200px;margin-bottom:50px;position:relative;"
    "display:flow-root}.L{float:left}.R{float:right}</style>"
    "<div class=c id=k1><div class=L style=\"width:50px;height:30px\"></div><div id=kb1 style=\"clear:left;"
    "height:10px\"></div></div>"
    "<div class=c id=k2><div><div class=L style=\"width:50px;height:30px\"></div>"
    "<div class=R style=\"width:50px;height:40px\"></div><div id=kb2 style=\"clear:both\"></div></div>"
    "<div id=kn2 style=\"height:10px\"></div></div>"
    "<div class=c id=k3><div style=\"margin-top:10px\"><div class=L style=\"width:50px;height:30px\"></div>"
    "<div id=kb3 style=\"clear:left;margin-top:100px;height:10px\"></div></div></div>"
    "<div class=c id=k4><div style=\"margin-top:10px\"><div class=L style=\"width:50px;height:30px\"></div>"
    "<div id=kb4 style=\"clear:left;margin-top:15px;height:10px\"></div></div></div>"
    "<div class=c id=k5><div style=\"height:10px;margin-bottom:20px\"></div><div class=L style=\"width:50px;"
    "height:30px\"></div><div id=kb5 style=\"clear:left;margin-top:5px;height:10px\"></div>"
    "<div id=kn5 style=\"margin-top:7px;height:10px\"></div></div>"
    "<div class=c id=k6><div style=\"height:10px;margin-bottom:20px\"></div><div class=L style=\"width:50px;"
    "height:5px\"></div><div id=kb6 style=\"clear:left;margin-top:5px;height:10px\"></div></div>"
    "<div class=c id=k7><div class=R style=\"width:50px;height:30px\"></div><div id=kb7 style=\"clear:left;"
    "height:10px\"></div><div id=kc7 style=\"clear:right;height:10px\"></div></div>"
    "<div class=c id=k8><div style=\"margin-top:10px\"><div class=L style=\"width:50px;height:30px\"></div>"
    "<div id=kb8 style=\"clear:left\"></div></div><div id=kn8 style=\"margin-top:20px;height:10px\"></div>"
    "</div>"
    "<div class=c id=k9><div style=\"height:10px;margin-bottom:20px\"></div><div class=L style=\"width:50px;"
    "height:5px\"></div><div id=kb9 style=\"clear:left;margin-top:50px;height:10px\"></div></div>"
    "<div class=c id=k10><div style=\"height:10px;margin-bottom:20px\"></div>"
    "<div class=L style=\"width:50px;height:50px\"></div><div id=kb10 style=\"clear:both;margin-top:40px;"
    "height:10px;margin-bottom:5px\"></div><div id=kn10 style=\"margin-top:3px;height:10px\"></div></div>"
    "<div class=c id=k11><div class=L style=\"width:50px;height:20px\"></div><div>"
    "<div class=R style=\"width:50px;height:35px\"></div><div id=kb11 style=\"clear:right;height:10px\">"
    "</div></div></div>"
    "<script>"
    "function E(i){return document.getElementById(i).getBoundingClientRect();}"
    "function Y(a,c){return a+'='+(E(a).top-E(c).top);}"
    "var q=[Y('kb1','k1'),Y('kb2','k2'),Y('kn2','k2'),Y('kb3','k3'),Y('kb4','k4'),Y('kb5','k5'),Y('kn5','k5'),"
    "Y('kb6','k6'),Y('kb7','k7'),Y('kc7','k7'),Y('kb8','k8'),Y('kn8','k8'),Y('kb9','k9'),Y('kb10','k10'),"
    "Y('kn10','k10'),Y('kb11','k11')].join('&')+['k1','k2','k3','k4','k5','k6','k7','k8','k9','k10','k11']"
    ".map(function(k){return '&'+k+'h='+E(k).height;}).join('');"
    "fetch('/api/clearance?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
