/* THE VISUAL UPDATE DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). HTML §8.1.7.3's
   Unnecessary rendering step keeps a document whose rendering changed (core/rendering/visual_update.h), and
   update the rendering's step 16 delivers ResizeObserver observations, so each change below is followed by a
   frame that reports it. Three changes, each alone after the first frame and each on its own element:
     /api/vupd   a tree change: a timer observes `vu` (initial observation of 100x10 against its 0x0 last
                 reported size is active) and appends a child to it;
     /api/vupda  an attribute change: `vb`, observed from the start, has its style attribute widen it;
     /api/vupdc  a CSSOM edit: `vc`, observed from the start, is widened by an insertRule.
   `vb` and `vc` are delivered once at the first frame, so the request comes from their second delivery. It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_VISUALUPDATE_H
#define ENGINE_HOST_FIXTURE_VISUALUPDATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_VISUALUPDATE[] =
    "<!doctype html><html><head><style id=vs></style></head>"
    "<body><div id=vu style=\"width:100px;height:10px\"></div>"
    "<div id=vb style=\"width:100px;height:10px\"></div>"
    "<div id=vc style=\"width:100px;height:10px\"></div>"
    "<script>"
    "var nb = 0, nc = 0;"
    "new ResizeObserver(function () { if (++nb === 2) fetch('/api/vupda?attr=1'); })"
    ".observe(document.getElementById('vb'));"
    "new ResizeObserver(function () { if (++nc === 2) fetch('/api/vupdc?cssom=1'); })"
    ".observe(document.getElementById('vc'));"
    "setTimeout(function () {"
    " var el = document.getElementById('vu');"
    " new ResizeObserver(function () { fetch('/api/vupd?ro=1'); }).observe(el);"
    " el.appendChild(document.createElement('span'));"
    "}, 100);"
    "setTimeout(function () {"
    " document.getElementById('vb').setAttribute('style', 'width:200px;height:10px');"
    "}, 200);"
    "setTimeout(function () {"
    " document.getElementById('vs').sheet.insertRule('#vc { width: 250px !important }', 0);"
    "}, 300);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
