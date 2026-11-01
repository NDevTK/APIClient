/* THE LINK IMAGE DOCUMENT. HTML §4.6.8.20 Link type "preload"'s "to preload" step 2 selects an image source from
   an `as=image` link's source set (§4.8.4.3.9 "Updating the source set" and §4.8.4.3.7). A link the parser
   inserts carries width descriptors and an `imagesizes` whose media condition fails at the 1280px viewport, so
   the source size is 300px; a link the script inserts carries density descriptors. §4.8.4.3.9 step 4's img is
   null for a link, so §4.8.4.3.11 step 3.3 reads no layout and the selection runs in the insertion steps with
   no layout job. Every candidate is an address the bundle shipped and is recorded; Chrome 153 preloads
   /lpre/p-400.png and /lpre/d-1x.png from the same markup.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LINKIMAGE_H
#define ENGINE_HOST_FIXTURE_LINKIMAGE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LINKIMAGE[] =
    "<!doctype html><html><head>"
    "<link rel=preload as=image imagesrcset=\"/lpre/p-400.png 400w, /lpre/p-800.png 800w\""
    " imagesizes=\"(min-width: 2000px) 800px, 300px\">"
    "</head><body>"
    "<script>"
    "var li = document.createElement('link'); li.rel = 'preload'; li.as = 'image';"
    "li.setAttribute('imagesrcset', '/lpre/d-1x.png 1x, /lpre/d-2x.png 2x');"
    "document.head.appendChild(li);"
    "fetch('/api/linkimage?n=' + document.querySelectorAll('link').length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
