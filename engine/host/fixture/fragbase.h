/* THE FRAGMENT BASE DOCUMENT: a `<link rel=preload>` placed by innerHTML in front of a `<base href>` in the same
   fragment. DOM §4.2.3 insert runs each node's insertion steps inside its per-node loop, so the link becomes
   browsing-context connected, and HTML §4.2.4.3 "Fetching and processing a resource from a link element"'s create
   link options from element reads "document's document base URL", before the base element is in the tree: the
   preload resolves against the document's own address, and only requests made after the setter see `/fbsub/`.
   The same order inside ONE body call, which inserts several nodes before it returns: `head.append(link, base)`
   resolves its link against `/fbsub/`, and a DocumentFragment holding a link and a base, prepended to the head,
   resolves its link against `/fbsub2/`, the base the append put first in tree order.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGBASE_H
#define ENGINE_HOST_FIXTURE_FRAGBASE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGBASE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var fbd = document.createElement('div');"
    "document.body.appendChild(fbd);"
    "fbd.innerHTML = '<link rel=\"preload\" as=\"script\" href=\"fragbase.js\"><base href=\"/fbsub/\">';"
    "function fbLink(h) { var l = document.createElement('link'); l.setAttribute('rel', 'preload');"
    " l.setAttribute('as', 'script'); l.setAttribute('href', h); return l; }"
    "function fbBase(h) { var b = document.createElement('base'); b.setAttribute('href', h); return b; }"
    "document.head.append(fbLink('fragbase2.js'), fbBase('/fbsub2/'));"
    "var fbf = document.createDocumentFragment();"
    "fbf.append(fbLink('fragbase3.js'), fbBase('/fbsub3/'));"
    "document.head.prepend(fbf);"
    "fetch('/api/fragbase?b=' + encodeURIComponent(document.baseURI));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
