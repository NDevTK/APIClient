/* THE FLEX `display: none` DOCUMENT (`flexnone`): replit's below-the-fold column, reduced. A `column` flex
   container nested in another is a block-level box with an `auto` height, so its height is css-flexbox-1 §9.2's
   "max-content size" (§9.9.1); its first child is `display: none` (Tailwind's `empty:hidden`), which
   css-display-3 §2.5 says generates no boxes, so §4 makes it no flex item and its 50px declared height counts
   for nothing. Each container is the 10px item's height; `#fnout` is that plus 5px of padding top and bottom.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXNONE_H
#define ENGINE_HOST_FIXTURE_FLEXNONE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLEXNONE[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=fnout style=\"display:flex;flex-direction:column;padding:5px 0\">"
    "<div id=fnin style=\"display:flex;flex-direction:column\">"
    "<div style=\"display:none;height:50px\"></div><div style=\"height:10px\"></div>"
    "</div></div>"
    "<script>"
    "fetch('/api/flexnone?o=' + document.getElementById('fnout').offsetHeight +"
    "      '&i=' + document.getElementById('fnin').offsetHeight);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
