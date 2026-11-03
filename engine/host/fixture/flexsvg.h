/* THE FLEX SVG DOCUMENT (`flexsvg`): replit's header shape. A `row` flex container holds a `row` flex item that
   holds two `column` flex buttons, each holding an `svg` 20 by 24 with a viewBox, so a replaced box with a
   natural aspect ratio and a non-`auto` `height` (SVG 2 §6.6 maps the attributes to `width` and `height`).
   The outer lines measure the inner item, whose max-content size is css-sizing-3 §5.1's intrinsic size of each
   button and so of each `svg`; css-sizing-4 §4.2 transfers the svg's preferred height through its ratio there,
   rather than its post-flexing height, which is the button's own lines. `#fs1` is 32px wide; `#fs2`'s width is
   `auto`, so it is its `svg`'s 20px contribution; the inner item is their 52px sum.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXSVG_H
#define ENGINE_HOST_FIXTURE_FLEXSVG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define FS_BUTTON(id, width)                                                                                  \
    "<button id=" id " style=\"display:flex;flex-direction:column;flex-shrink:0;justify-content:center;"      \
    "align-items:center;padding:0;border:0;height:32px;" width "\">"                                          \
    "<svg width=\"20\" height=\"24\" viewBox=\"0 0 24 24\"><path d=\"M3 12H21\"></path></svg></button>"

static const char HTML_FLEXSVG[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div style=\"display:flex;align-items:center\">"
    "<div id=fsin style=\"display:flex;flex-shrink:0;align-items:center\">"
    FS_BUTTON("fs1", "width:32px") FS_BUTTON("fs2", "")
    "</div></div>"
    "<script>"
    "var fsSvg = document.querySelector('#fs2 svg').getBoundingClientRect();"
    "fetch('/api/flexsvg?b1=' + document.getElementById('fs1').offsetWidth +"
    "      '&b2=' + document.getElementById('fs2').offsetWidth +"
    "      '&in=' + document.getElementById('fsin').offsetWidth +"
    "      '&sw=' + fsSvg.width + '&sh=' + fsSvg.height);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef FS_BUTTON

#endif
