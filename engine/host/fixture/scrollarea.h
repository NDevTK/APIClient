/* THE SCROLLING AREA DOCUMENT: CSSOM VIEW §6's `scrollHeight`/`scrollWidth` step 7, §2's scrolling area over a
   deep and a wide subtree, walked under a resting drive. One of test_forced.c's exploring documents (its
   TF_DOCS table). #sa1 is a 100 x 50 clipped box holding 200 nested blocks with 1px of top padding each around
   a 500 x 10 leaf, so its area is 210 tall and 500 wide; #sa2 holds 300 blocks 2px tall, so its area is 600
   tall. Both walks visit more descendants than one activation of a resting drive takes, so the reads rest
   (`_layoutScrollAreaRests`) and resume where they stopped. Chrome 155.0.8059.84 and Firefox 158.0.1 give every
   value expected. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SCROLLAREA_H
#define ENGINE_HOST_FIXTURE_SCROLLAREA_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCROLLAREA[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=sa1 style=\"height:50px;width:100px;overflow:hidden\"></div>"
    "<div id=sa2 style=\"height:50px;width:100px;overflow:hidden\"></div>"
    "<script>"
    "var p=document.getElementById('sa1'),i,d;"
    "for(i=0;i<200;i++){d=document.createElement('div');d.style.paddingTop='1px';p.appendChild(d);p=d;}"
    "d=document.createElement('div');d.style.cssText='height:10px;width:500px';p.appendChild(d);"
    "p=document.getElementById('sa2');"
    "for(i=0;i<300;i++){d=document.createElement('div');d.style.height='2px';p.appendChild(d);}"
    "var a=document.getElementById('sa1'),b=document.getElementById('sa2');"
    "fetch('/api/scrollarea?sh='+a.scrollHeight+'&sw='+a.scrollWidth+'&ch='+a.clientHeight"
    "+'&lh='+b.scrollHeight+'&lw='+b.scrollWidth);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
