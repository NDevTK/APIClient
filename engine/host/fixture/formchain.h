/* THE FORM CHAIN DOCUMENT: forty nested blocks, each `max-width: calc(50% + 100px)` of its parent, read through
   CSSOM VIEW §7's `offsetWidth` in a loop; one of test_forced.c's TF_DOCS. Each level's width is a function of
   the initial containing block's width that css_length.h's ranges cannot order against its limit, so CSS 2.1
   §10.4's clamp and §10.3.3's floor at zero each mint an atom over the level above, and each level's form names
   the level above twice: the atoms nest forty deep and share their operands. A `long` crossing names its term
   by those atoms (core/frame/viewport.c), so the name is the size of one definition only if a nested atom is
   named by its digest rather than spelled out. At a 1280px viewport level k is 200 + 540 / 2^(k-1): 740, 470,
   335, 267.5 (crossing as 268), and 200 at the fortieth; `dw` is the page's own difference of the first and the
   last. Values read from the specs. It ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_FORMCHAIN_H
#define ENGINE_HOST_FIXTURE_FORMCHAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FORMCHAIN[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=fc></div>"
    "<script>"
    "var p=document.getElementById('fc'),i,d,w=[];"
    "for(i=0;i<40;i++){d=document.createElement('div');d.style.cssText='max-width:calc(50% + 100px);height:1px';"
    "p.appendChild(d);p=d;}"
    "for(p=document.getElementById('fc').firstChild;p;p=p.firstChild)w.push(p.offsetWidth);"
    "fetch('/api/formchain?w1='+w[0]+'&w2='+w[1]+'&w3='+w[2]+'&w4='+w[3]+'&wn='+w[39]+'&dw='+(w[0]-w[39]));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
