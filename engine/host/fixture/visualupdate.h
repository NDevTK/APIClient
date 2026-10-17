/* THE VISUAL UPDATE DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). After its first
   frame has revealed it, a timer observes an element with a ResizeObserver and appends a child to it. The
   append is a visible effect (HTML §8.1.7.3's Unnecessary rendering step keeps the document), so a rendering
   opportunity follows, and update the rendering's step 16 delivers the observation: Resize Observer's initial
   observation of a 100x10 element against its 0x0 last reported size is active. It ends with TF_CANVAS_INK,
   which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_VISUALUPDATE_H
#define ENGINE_HOST_FIXTURE_VISUALUPDATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_VISUALUPDATE[] =
    "<!doctype html><html>"
    "<body><div id=vu style=\"width:100px;height:10px\"></div>"
    "<script>setTimeout(function () {"
    " var el = document.getElementById('vu');"
    " new ResizeObserver(function () { fetch('/api/vupd?ro=1'); }).observe(el);"
    " el.appendChild(document.createElement('span'));"
    "}, 100);</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
