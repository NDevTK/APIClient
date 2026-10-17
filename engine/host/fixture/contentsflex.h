/* THE `display: contents` FLEX ITEM DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table),
   alone because its row is the one core/layout/used_value.c's contents-splice DFAIL refuses, and an abort
   costs only this document. css-display-3 §2.5 "Box Generation: the none and contents keywords" treats the `contents`
   element "as if it had been replaced in the element tree by its contents", so `cf` is an item of the 300px
   flex container. css-flexbox-1 §9.2 makes its flex base size its max-content size, its 120px child, and with
   `flex-grow: 0` and positive free space its main size stays 120px. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CONTENTSFLEX_H
#define ENGINE_HOST_FIXTURE_CONTENTSFLEX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CONTENTSFLEX[] =
    "<!doctype html><html>"
    "<body>"
    "<div style=\"display:flex;width:300px\"><div style=\"display:contents\">"
    "<div id=cf><div style=\"width:120px;height:10px\"></div></div>"
    "</div></div>"
    "<script>fetch('/api/cflexw?w=' + getComputedStyle(document.getElementById('cf')).width);</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
