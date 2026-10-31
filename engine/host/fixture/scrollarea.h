/* THE SCROLLING AREA DOCUMENT: CSSOM VIEW §6's `scrollHeight`/`scrollWidth` step 7, §2's scrolling area over a
   deep and a wide subtree, walked under a resting drive. One of test_forced.c's exploring documents (its
   TF_DOCS table). #sa1 is a 100 x 50 clipped box holding 200 nested blocks with 1px of top padding each around
   a 500 x 10 leaf, so its area is 210 tall and, by css-overflow-3 §3.3's border boxes, 500 wide (CSSOM VIEW
   §2's table says margin edges, and the over-constrained leaf's CSS 2.1 §10.3.3 right margin would put that at
   100); #sa2 holds 300 blocks 2px tall, so its area is 600 tall. Both walks visit more descendants than one
   activation of a resting drive takes, so the reads rest (`_layoutScrollAreaRests`) and resume where they
   stopped. #sa3 to #sa6 are §3.3's margin areas: a block's trailing margin, an inline-block's and a flex
   item's right margin, and a grid item inside its 200px track. Chrome 155.0.8059.84 and Firefox 158.0.1 give
   every value expected. Cross-engine notes, left out: both browsers count a zero-height block 500px wide, where
   §3.3 says "Border boxes with zero area do not affect the scrollable overflow area"; and both add §3.3's end
   padding (a 10px-padded box over a 300 x 200 child is 320 x 220), which this engine does not build yet. It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
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
    "<div id=sa3 style=\"height:50px;width:100px;overflow:hidden\"><div style=\"height:100px;margin-bottom:30px\">"
    "</div></div>"
    "<div id=sa4 style=\"height:50px;width:100px;overflow:hidden\"><div style=\"display:inline-block;width:200px;"
    "height:10px;margin-right:50px\"></div></div>"
    "<div id=sa5 style=\"height:50px;width:100px;overflow:hidden;display:flex\"><div style=\"flex:none;"
    "width:200px;height:10px;margin-right:50px\"></div></div>"
    "<div id=sa6 style=\"height:50px;width:100px;overflow:hidden;display:grid;grid-template-columns:200px\">"
    "<div style=\"margin-right:40px;height:10px\"></div></div>"
    "<script>"
    "var p=document.getElementById('sa1'),i,d;"
    "for(i=0;i<200;i++){d=document.createElement('div');d.style.paddingTop='1px';p.appendChild(d);p=d;}"
    "d=document.createElement('div');d.style.cssText='height:10px;width:500px';p.appendChild(d);"
    "p=document.getElementById('sa2');"
    "for(i=0;i<300;i++){d=document.createElement('div');d.style.height='2px';p.appendChild(d);}"
    "var a=document.getElementById('sa1'),b=document.getElementById('sa2');"
    "function E(i){return document.getElementById(i);}"
    "fetch('/api/scrollarea?sh='+a.scrollHeight+'&sw='+a.scrollWidth+'&ch='+a.clientHeight"
    "+'&m3='+E('sa3').scrollHeight+'&m4='+E('sa4').scrollWidth+'&m5='+E('sa5').scrollWidth"
    "+'&m6='+E('sa6').scrollWidth"
    "+'&lh='+b.scrollHeight+'&lw='+b.scrollWidth);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
