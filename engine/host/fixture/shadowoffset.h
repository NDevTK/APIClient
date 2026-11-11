/* CSSOM VIEW §7's offsetParent walk over the flat tree (css-shadow-1 §4.1), after WPT
   shadow-dom/offsetParent-across-shadow-boundaries.html. `t1` is a light child slotted into an open shadow
   tree's positioned 85px/45px-padded div, which is no shadow-including ancestor of it, so its offsetParent is
   `c` and its offsets are measured from `c`; `t2` is inside a closed shadow tree under that tree's own
   positioned `rp`; `u` is a host's light child no slot takes, so it is in no flat tree and has no box. One of
   test_forced.c's exploring documents (its TF_DOCS table); it ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SHADOWOFFSET_H
#define ENGINE_HOST_FIXTURE_SHADOWOFFSET_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SHADOWOFFSET[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=c style=\"position:relative\">"
    "<div id=h1><div id=t1 style=\"height:5px\"></div></div><div id=h2></div><div id=h3><div id=u></div></div>"
    "</div>"
    "<script>var t1=document.getElementById('t1'),u=document.getElementById('u');"
    "document.getElementById('h1').attachShadow({mode:'open'}).innerHTML="
    "'<div style=\"position:relative;padding-left:85px;padding-top:45px\"><slot></slot></div>';"
    "var s2=document.getElementById('h2').attachShadow({mode:'closed'});"
    "s2.innerHTML='<div id=rp style=\"position:relative;padding-left:100px;padding-top:70px\">"
    "<div style=\"height:5px\"></div></div>';"
    "var t2=s2.firstChild.firstChild;"
    "document.getElementById('h3').attachShadow({mode:'open'}).innerHTML='<div>x</div>';"
    "fetch('/api/shadowoffset?p1='+t1.offsetParent.id+'&l1='+t1.offsetLeft+'&t1='+t1.offsetTop+"
    "'&w1='+t1.offsetWidth+'&p2='+t2.offsetParent.id+'&l2='+t2.offsetLeft+'&t2='+t2.offsetTop+"
    "'&u='+(u.offsetParent===null)+'&ul='+u.offsetLeft);</script>"
    TF_CANVAS_INK "</body></html>";

#endif
