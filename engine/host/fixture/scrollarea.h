/* THE SCROLLING AREA DOCUMENT: CSSOM VIEW §6's scroll members over §2's scrolling area, walked under a resting
   drive (`_layoutScrollAreaRests`). One of test_forced.c's exploring documents (its TF_DOCS table). #sa1 nests
   200 blocks with 1px of top padding around a 500 x 10 leaf in a 100 x 50 clipped box: 210 tall and, by
   css-overflow-3 §3.3's border boxes, 500 wide (CSSOM VIEW §2's margin edges, with the leaf's CSS 2.1 §10.3.3
   right margin, would give 100). #sa2 holds 300 blocks 2px tall: 600. #sa3 to #sa6 are §3.3's margin areas of
   a block, an inline-block, a flex item and a grid item in a 200px track. #sa7's 400 blocks of 5px make the
   root's `scrollHeight` (step 4, the viewport's area) 2300. The setter and scroll members measure #sa2 for steps
   10 and 11, landing where it is; #sa7 has no scrolling box, so step 10 terminates; the root's setter, §4's
   window members and the root's scrollTo() move the viewport to 100, 200, 250 and 300. HTML §6.6.6's focus()
   step 4 scrolls #fb, 2000px down, into view, and preventScroll keeps #fa's focus() at 0. Chrome 155.0.8059.84
   and Firefox 158.0.1 give every value expected. Cross-engine notes, left out: both count a zero-height block
   500px wide, where §3.3 says "Border boxes with zero area do not affect the scrollable overflow area"; both add
   §3.3's end padding (a 10px-padded box over a 300 x 200 child is 320 x 220), not built here. It ends with
   TF_CANVAS_INK, which test_forced.c defines first. */
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
    "<div id=sa7></div>"
    "<input id=fa style=\"position:absolute;top:1900px;left:0;width:50px;height:10px;border:0;padding:0\">"
    "<input id=fb style=\"position:absolute;top:2000px;left:0;width:50px;height:10px;border:0;padding:0\">"
    "<script>"
    "var p=document.getElementById('sa1'),i,d;"
    "for(i=0;i<200;i++){d=document.createElement('div');d.style.paddingTop='1px';p.appendChild(d);p=d;}"
    "d=document.createElement('div');d.style.cssText='height:10px;width:500px';p.appendChild(d);"
    "p=document.getElementById('sa2');"
    "for(i=0;i<300;i++){d=document.createElement('div');d.style.height='2px';p.appendChild(d);}"
    "p=document.getElementById('sa7');"
    "for(i=0;i<400;i++){d=document.createElement('div');d.style.height='5px';p.appendChild(d);}"
    "var a=document.getElementById('sa1'),b=document.getElementById('sa2');"
    "function E(i){return document.getElementById(i);}"
    "b.scrollTop=0;b.scrollTo({top:0});b.scrollBy(0,0);E('sa7').scrollTop=5;"
    "document.documentElement.scrollTop=100;var rt=document.documentElement.scrollTop;"
    "scrollTo(0,200);var w1=scrollY;scrollBy({top:50});var w2=scrollY;"
    "document.documentElement.scrollTo(0,300);var w3=scrollY;"
    "scrollTo(0,0);E('fa').focus({preventScroll:true});var f1=scrollY;"
    "E('fb').focus();var f2=scrollY>1500?'moved':'still';"
    "fetch('/api/scrollarea?sh='+a.scrollHeight+'&sw='+a.scrollWidth+'&ch='+a.clientHeight"
    "+'&m3='+E('sa3').scrollHeight+'&m4='+E('sa4').scrollWidth+'&m5='+E('sa5').scrollWidth"
    "+'&m6='+E('sa6').scrollWidth"
    "+'&lh='+b.scrollHeight+'&lw='+b.scrollWidth+'&rh='+document.documentElement.scrollHeight"
    "+'&rt='+rt+'&nt='+E('sa7').scrollTop+'&w1='+w1+'&w2='+w2+'&w3='+w3"
    "+'&f1='+f1+'&f2='+f2);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
