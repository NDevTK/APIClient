/* THE ANONYMOUS ITEM BASELINE DOCUMENT (`flexanonbase`): css-flexbox-1 §8.5 "Flex Container Baselines" taken from
   §4's anonymous item around a container's text, replit's inline-flex `<a>`. Each pair of inline-flex containers
   sits on one line: `#a1`/`#a2` hold bare text, an anonymous item, and `#e1`/`#e2` the same text in a `span`, an
   element item, so each container's baseline is its item's first line box's and the two of a pair stand at one
   height. `#a2`/`#e2` are 60px tall with `align-items: center`, so the item is placed off the content box's top.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXANONBASE_H
#define ENGINE_HOST_FIXTURE_FLEXANONBASE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define FAB_PAIR(a, e, style) \
    "<div style=\"font-size:20px;line-height:20px\">" \
    "<span id=" a " style=\"display:inline-flex;line-height:40px;padding-top:7px;" style "\">abc</span>" \
    "<span id=" e " style=\"display:inline-flex;line-height:40px;padding-top:7px;" style "\"><span>abc</span></span>" \
    "</div>"

static const char HTML_FLEXANONBASE[] =
    "<!doctype html><html><body style=\"margin:0\">"
    FAB_PAIR("a1", "e1", "")
    FAB_PAIR("a2", "e2", "height:60px;align-items:center")
    "<script>"
    "function fabD(a, e) { return Math.round(document.getElementById(a).getBoundingClientRect().top -"
    "                                         document.getElementById(e).getBoundingClientRect().top); }"
    "fetch('/api/flexanonbase?d1=' + fabD('a1', 'e1') + '&d2=' + fabD('a2', 'e2'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef FAB_PAIR

#endif
