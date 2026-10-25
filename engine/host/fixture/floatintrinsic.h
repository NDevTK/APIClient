/* THE FLOAT INTRINSIC SIZE DOCUMENT: css-sizing-3 §5.1's max-content size of a block container
   holding block-level floats (CSS 2.2 §9.5). One of test_forced.c's exploring documents (its TF_DOCS table). Each
   shrink-to-fit `inline-block` holds floats and block-level boxes: floats side by side and on both sides, cleared
   by a later float or block, followed by a box that must not overlap them (`overflow: hidden`, a flex container),
   which adds the floats' widths, and by an ordinary block, which does not;
   a second box after the first, which starts a new set; margins; and a float nested in a child block. Chrome 155.0.8059.84 and Firefox 158.0.1 give every number the rows expect for this markup. It
   ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLOATINTRINSIC_H
#define ENGINE_HOST_FIXTURE_FLOATINTRINSIC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLOATINTRINSIC[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<style>body{font:16px monospace}.s{display:inline-block}b{display:block;"
    "height:5px}.L{float:left;height:5px}.R{float:right;height:5px}</style>"
    "<div><span class=s id=a><div class=L style=\"width:50px\"></div><b style=\"width:30px\"></b>"
    "<b style=\"width:40px\"></b></span></div>"
    "<div><span class=s id=b><div class=L style=\"width:50px\"></div><b style=\"width:30px\"></b>"
    "<div class=L style=\"width:20px\"></div><b style=\"width:40px\"></b></span></div>"
    "<div><span class=s id=c><div class=L style=\"width:50px\"></div><div class=R style=\"width:20px\"></div>"
    "<b style=\"width:30px\"></b></span></div>"
    "<div><span class=s id=d><div class=L style=\"width:50px\"></div><b style=\"width:30px;clear:right\"></b>"
    "</span></div>"
    "<div><span class=s id=e><div class=L style=\"width:50px\"></div><div class=R style=\"width:20px;"
    "clear:left\"></div><b style=\"width:30px\"></b></span></div>"
    "<div><span class=s id=f><div class=L style=\"width:50px\"></div><div class=L style=\"width:20px;"
    "clear:left\"></div><b style=\"width:30px\"></b></span></div>"
    "<div><span class=s id=g><div class=L style=\"width:50px;margin-right:5px\"></div><b style=\"width:30px;"
    "margin-left:7px\"></b></span></div>"
    "<div><span class=s id=i><div class=L style=\"width:50px\"></div><div><div class=L style=\"width:20px\">"
    "</div><b style=\"width:30px\"></b></div></span></div>"
    "<div><span class=s id=j><b style=\"width:30px\"></b><div class=L style=\"width:50px\"></div></span>"
    "</div>"
    "<div><span class=s id=k><div class=L style=\"width:50px\"></div><b style=\"width:30px\"></b></span>"
    "</div>"
    "<div><span class=s id=l><div class=L style=\"width:50px\"></div><b style=\"width:30px\"></b>"
    "<b style=\"width:10px\"></b></span></div>"
    "<div><span class=s id=n><div class=L style=\"width:50px\"></div><b style=\"width:30px;overflow:hidden\">"
    "</b><b style=\"width:40px;overflow:hidden\"></b></span></div>"
    "<div><span class=s id=o><div class=L style=\"width:50px\"></div><div style=\"height:0\"></div>"
    "<b style=\"width:30px;overflow:hidden\"></b></span></div>"
    "<div><span class=s id=q><div class=L style=\"width:50px\"></div><div style=\"display:flex;width:30px;"
    "height:5px\"></div></span></div>"
    "<script>"
    "function W(i){return document.getElementById(i).getBoundingClientRect().width;}"
    "var q='a='+W('a')+'&b='+W('b')+'&c='+W('c')+'&d='+W('d')+'&e='+W('e')+'&f='+W('f')+'&g='+W('g')"
    "+'&i='+W('i')+'&j='+W('j')+'&k='+W('k')+'&l='+W('l')+'&n='+W('n')"
    "+'&o='+W('o')+'&q='+W('q');"
    "fetch('/api/floatintrinsic?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
