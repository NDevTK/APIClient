/* The wide flex document: CSSOM VIEW §6 "Extensions to the Element Interface"' `scrollWidth` of a 100px
   scroller over a `display: flex` container holding two hundred `flex: none` items 10px wide; one of
   test_forced.c's TF_DOCS. css-overflow-3 §3.3 "Calculating the Scrollable Overflow Area" folds every item's
   border box into the scroller's area, so the walk places each item through css-flexbox-1 §9.5 "Main-Axis
   Alignment" over one line: one placement, read two hundred times. The items overflow a `nowrap` line from its
   main-start edge, so the area is 2000px wide, and the last item's `offsetLeft` (CSSOM VIEW §7 "Extensions to
   the HTMLElement Interface", from the body's padding edge) is 1990. Values read from the specs. It ends with
   TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_FLEXWIDE_H
#define ENGINE_HOST_FIXTURE_FLEXWIDE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLEXWIDE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=sc style=\"width:100px;height:50px;overflow:auto\"><div id=fx style=\"display:flex\"></div></div>"
    "<script>"
    "var f=document.getElementById('fx'),i,d;"
    "for(i=0;i<200;i++){d=document.createElement('div');d.style.cssText='flex:none;width:10px;height:1px';"
    "f.appendChild(d);}"
    "fetch('/api/flexwide?sw='+document.getElementById('sc').scrollWidth+'&l='+f.lastChild.offsetLeft);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
