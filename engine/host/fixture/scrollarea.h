/* THE SCROLLING AREA DOCUMENT: CSSOM VIEW's scroll and offset members over a deep document, walked under a
   resting drive (`_layoutScrollAreaRests`); one of test_forced.c's TF_DOCS. #sa1 nests 200 1px-padded blocks
   around a 500 x 10 leaf in a 100 x 50 clipped box: 210 tall and, by css-overflow-3 §3.3's border boxes, 500
   wide (CSSOM VIEW §2's margin edges, with the leaf's CSS 2.1 §10.3.3 right margin, would give 100). #sa2
   holds 300 2px blocks: 600. #sa3 to #sa6 are §3.3's margin areas of a block, an inline-block, a flex item and
   a grid item in a 200px track. #sa7's 400 5px blocks make the root's `scrollHeight` (§6 step 4) 2300. §6's
   setter and scroll members measure #sa2 and land where it is; #sa7 has no scrolling box (step 10); the root's
   setter, §4's window members and the root's scrollTo() move the viewport to 100, 200, 250 and 300. HTML
   §6.6.6's focus() step 4 scrolls #fb into view, and preventScroll keeps #fa's at 0. §7's offsetTop of #oe is
   2300 and of #sa2's last block 648 (from the initial containing block, as Blink measures a non-positioned
   body offsetParent). CSSOM §9's resolved width of #sa2's first block and top of the `top: auto` #ap are
   their used 100px. #sr1 and #sr2 hold a replaced element, an 80px `svg` around a 200px `path` and a `canvas`
   around a wide fallback `span`, whose content CSS 2.2 §3.1 puts outside the CSS formatting model, so their
   scrolling areas are 80 and 50 wide; those two are read from the specs, not from a browser. Chrome
   155.0.8059.84 gives every other value expected. Cross-engine notes, left out: Chrome
   and Firefox 158.0.1 count a zero-height block 500px wide, where §3.3 says "Border boxes with zero area do not
   affect the scrollable overflow area", and add §3.3's end padding (a 10px-padded box over a 300 x 200 child
   is 320 x 220), not built here. It ends with TF_CANVAS_INK, which test_forced.c defines first. */
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
    "<div id=ap style=\"position:absolute;width:10px;height:10px\"></div>"
    "<div id=sa3 style=\"height:50px;width:100px;overflow:hidden\"><div style=\"height:100px;margin-bottom:30px\">"
    "</div></div>"
    "<div id=sa4 style=\"height:50px;width:100px;overflow:hidden\"><div style=\"display:inline-block;width:200px;"
    "height:10px;margin-right:50px\"></div></div>"
    "<div id=sa5 style=\"height:50px;width:100px;overflow:hidden;display:flex\"><div style=\"flex:none;"
    "width:200px;height:10px;margin-right:50px\"></div></div>"
    "<div id=sa6 style=\"height:50px;width:100px;overflow:hidden;display:grid;grid-template-columns:200px\">"
    "<div style=\"margin-right:40px;height:10px\"></div></div>"
    "<div id=sr1 style=\"position:absolute;top:0;left:300px;width:50px;height:20px;overflow:hidden\">"
    "<svg width=\"80\" height=\"10\"><path d=\"M0 0H200V10H0Z\"/></svg></div>"
    "<div id=sr2 style=\"position:absolute;top:0;left:400px;width:50px;height:20px;overflow:hidden\">"
    "<canvas width=\"30\" height=\"10\"><span>fallbackfallbackfallbackfallback</span></canvas></div>"
    "<div id=sa7></div><div id=oe></div>"
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
    "+'&m6='+E('sa6').scrollWidth+'&r1='+E('sr1').scrollWidth+'&r2='+E('sr2').scrollWidth"
    "+'&lh='+b.scrollHeight+'&lw='+b.scrollWidth+'&rh='+document.documentElement.scrollHeight"
    "+'&rt='+rt+'&nt='+E('sa7').scrollTop+'&w1='+w1+'&w2='+w2+'&w3='+w3"
    "+'&f1='+f1+'&f2='+f2"
    "+'&ot='+E('oe').offsetTop+'&oh='+E('oe').offsetHeight+'&lt='+b.lastChild.offsetTop"
    "+'&gw='+getComputedStyle(b.firstChild).width+'&gt='+getComputedStyle(E('ap')).top);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
