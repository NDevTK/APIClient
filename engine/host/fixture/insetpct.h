/* THE PERCENTAGE INSET DOCUMENT: css-position-3 §3.1's `<percentage>` inset over a containing block whose height
   is or is not definite, and an absolutely positioned table. One of test_forced.c's exploring documents (its
   TF_DOCS table). A relatively positioned box's `top: 50%` shifts it under a 200px containing block, a
   resolvable percentage height, a `calc()` length and an abspos box with both insets, and leaves it unshifted
   under an auto height, an unresolvable percentage and an unresolvable `calc()` — WPT's
   css/css-position/position-relative-015.html cases 1-4, 11, 16 and 18 — and `bottom: 50%` likewise; the
   unshifted box's resolved `top` and `bottom` are its percentage of the laid-out 20px height, `10px` and
   `-10px`, as Chrome and Firefox report them. jQuery 1.6's offset test places an
   absolutely positioned table with `top: 0; left: 0` (CSS 2.1 §17.4: the wrapper takes the table's
   `position`), and a second one is placed by `right` and `bottom`. Chrome 155.0.8059.84 and Firefox 158.0.1
   give every number the rows expect for this markup. It ends with TF_CANVAS_INK, which test_forced.c defines
   before including it. */
#ifndef ENGINE_HOST_FIXTURE_INSETPCT_H
#define ENGINE_HOST_FIXTURE_INSETPCT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INSETPCT[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=t1-cb style=\"height:200px\"><div id=t1 style=\"position:relative;width:20px;height:20px;"
    "top:50%\"></div></div>"
    "<div id=t2-cb><div id=t2 style=\"position:relative;width:20px;height:20px;top:50%\"></div></div>"
    "<div style=\"height:200px\"><div id=t3-cb style=\"height:50%\"><div id=t3 style=\"position:relative;"
    "width:20px;height:20px;top:50%\"></div></div></div>"
    "<div style=\"position:relative;height:200px\"><div id=t4-cb style=\"position:absolute;top:0;bottom:0;"
    "left:0;right:0\"><div id=t4 style=\"position:relative;width:20px;height:20px;top:50%\"></div></div>"
    "</div>"
    "<div><div id=t11-cb style=\"height:50%;min-height:200px\"><div id=t11 style=\"position:relative;"
    "width:20px;height:20px;top:50%\"></div></div></div>"
    "<div id=t16-cb style=\"height:calc(100px + 100px)\"><div id=t16 style=\"position:relative;width:20px;"
    "height:20px;top:50%\"></div></div>"
    "<div><div id=t18-cb style=\"height:calc(50% + 0px);min-height:200px\">"
    "<div id=t18 style=\"position:relative;width:20px;height:20px;top:50%\"></div></div></div>"
    "<div id=t21-cb><div style=\"height:40px\"></div><div id=t21 style=\"position:relative;width:20px;"
    "height:20px;bottom:50%\"></div></div>"
    "<div id=jq style=\"position:relative;height:40px\"><table id=jt style=\"position:absolute;top:0;left:0;"
    "margin:0;border:5px solid #000;padding:0;width:1px;height:1px\" cellpadding=0 cellspacing=0><tr>"
    "<td id=jtd></td></tr></table></div>"
    "<div id=jq2 style=\"position:relative;height:40px\"><table id=jt2 style=\"position:absolute;right:3px;"
    "bottom:2px;width:30px;height:10px;border:0\"><tr><td></td></tr></table></div>"
    "<script>"
    "function E(i){return document.getElementById(i);}"
    "function O(a,b){return E(a).getBoundingClientRect().top-E(b).getBoundingClientRect().top;}"
    "var q='t1='+O('t1','t1-cb')+'&t2='+O('t2','t2-cb')+'&t3='+O('t3','t3-cb')+'&t4='+O('t4','t4-cb')"
    "+'&t11='+O('t11','t11-cb')+'&t16='+O('t16','t16-cb')+'&t18='+O('t18','t18-cb')"
    "+'&t21='+O('t21','t21-cb')"
    "+'&t2cs='+getComputedStyle(E('t2')).top+'&t2cb='+getComputedStyle(E('t2')).bottom"
    "+'&t1cs='+getComputedStyle(E('t1')).top"
    "+'&jtx='+(E('jt').getBoundingClientRect().left-E('jq').getBoundingClientRect().left)"
    "+'&jty='+O('jt','jq')+'&jtd='+E('jtd').offsetTop+'&jtl='+E('jtd').offsetLeft"
    "+'&j2r='+(E('jq2').getBoundingClientRect().right-E('jt2').getBoundingClientRect().right)"
    "+'&j2w='+E('jt2').getBoundingClientRect().width+'&j2y='+O('jt2','jq2');"
    "fetch('/api/insetpct?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
