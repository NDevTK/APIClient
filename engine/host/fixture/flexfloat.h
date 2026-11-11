/* The floated flex item document. One of test_forced.c's exploring documents (its TF_DOCS table). css-flexbox-1
   §3 "Flex Containers: the flex and inline-flex display values": "float and clear do not create floating or
   clearance of flex item, and do not take it out-of-flow", so `ff` is the second item of its flex line and
   starts where the 30px first item ends. The container's `overflow: hidden` makes it a block formatting
   context root while its parent is the nearest block container, which is the shape a box wrongly taken for a
   float cannot place. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXFLOAT_H
#define ENGINE_HOST_FIXTURE_FLEXFLOAT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLEXFLOAT[] =
    "<!doctype html><html>"
    "<body>"
    "<div id=fc style=\"display:flex;overflow:hidden\"><div style=\"width:30px;height:10px\"></div>"
    "<div id=ff style=\"float:left;width:50px;height:10px\"></div></div>"
    "<script>"
    "fetch('/api/flexfloat?x=' + (document.getElementById('ff').getBoundingClientRect().left - "
    "document.getElementById('fc').getBoundingClientRect().left));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
