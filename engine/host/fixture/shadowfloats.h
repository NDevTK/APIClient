/* The float scan over a shadow host: an `inline-block` shadow host whose light child is assigned to no slot,
   so it is in no flat tree. The host's shrink-to-fit width asks css-sizing-3 §5.1's intrinsic sizes of its
   content, which first gathers CSS 2.2 §9.5's floats of the formatting context the host establishes, by a
   pre-order walk of its flat subtree (css-shadow-1 §4.1): the shadow root's 10px block, never the light span.
   CSS 2.2 §10.3.9 then makes the host 10px wide. One of test_forced.c's exploring documents (its TF_DOCS
   table); it ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SHADOWFLOATS_H
#define ENGINE_HOST_FIXTURE_SHADOWFLOATS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SHADOWFLOATS[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=sh style=\"display:inline-block\"><span>light</span></div>"
    "<script>var h=document.getElementById('sh');"
    "h.attachShadow({mode:'open'}).innerHTML='<div style=\"width:10px;height:10px\"></div>';"
    "fetch('/api/shadowfloats?w='+h.offsetWidth);</script>"
    TF_CANVAS_INK "</body></html>";

#endif
