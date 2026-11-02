/* THE REPLACED CONTENT DOCUMENT: CSSOM VIEW §6 and §7 asked of elements inside replaced elements; one of
   test_forced.c's TF_DOCS. CSS 2 §3.1 "Definitions" puts a replaced element's content outside the formatting
   model, so the `span` and the 40px block in a scripted `canvas`'s fallback (HTML §15.4.1 "Embedded content")
   and the `span` inside a `video` have no associated box: no client rectangles, a zero bounding rectangle and
   a zero `offsetWidth`. An `object` with no `data` represents its children and is an ordinary element (§15.4.1
   again), so the 40px block in its fallback has its box: one rectangle, 40 wide. Values read from the specs.
   It ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_REPLACEDCONTENT_H
#define ENGINE_HOST_FIXTURE_REPLACEDCONTENT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REPLACEDCONTENT[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<canvas width=\"30\" height=\"10\"><span id=cs>fallback</span>"
    "<div id=cd style=\"width:40px;height:10px\"></div></canvas>"
    "<video><span id=vs>fallback</span></video>"
    "<object style=\"display:block\"><div id=od style=\"width:40px;height:10px\"></div></object>"
    "<script>"
    "function E(i){return document.getElementById(i);}"
    "fetch('/api/replacedcontent?cr='+E('cs').getClientRects().length+'&cw='+E('cs').getBoundingClientRect().width"
    "+'&dw='+E('cd').offsetWidth+'&vw='+E('vs').getBoundingClientRect().width"
    "+'&or='+E('od').getClientRects().length+'&ow='+E('od').getBoundingClientRect().width);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
