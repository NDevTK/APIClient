/* THE INTRINSIC SIZE KEYWORD DOCUMENT (`sizekw`): css-sizing-3 §3.2's `min-content`, `max-content` and
   `fit-content` as `width`. Every box holds two inline-blocks 30px and 40px wide at `font-size: 0`, so its §5.1
   min-content size is 40px and its max-content size 70px. `#kwo` is an `inline-flex` column, so its width is its
   §5.2 max-content contribution, which reads each item's keyword width (replit's header shape); the items are
   70px, 40px and, at `fit-content` with 70px available, 70px. In a 50px block, `fit-content` is
   min(70, max(40, 50)) = 50px and `max-content` 70px.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SIZEKW_H
#define ENGINE_HOST_FIXTURE_SIZEKW_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define KW_TWO "<span style=\"display:inline-block;width:30px;height:5px\"></span> " \
               "<span style=\"display:inline-block;width:40px;height:5px\"></span>"

static const char HTML_SIZEKW[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=kwo style=\"display:inline-flex;flex-direction:column;align-items:flex-start;font-size:0\">"
    "<div id=kw1 style=\"width:max-content\">" KW_TWO "</div>"
    "<div id=kw2 style=\"width:min-content\">" KW_TWO "</div>"
    "<div id=kw3 style=\"width:fit-content\">" KW_TWO "</div>"
    "</div>"
    "<div style=\"width:50px;font-size:0\">"
    "<div id=kw4 style=\"width:fit-content\">" KW_TWO "</div>"
    "<div id=kw5 style=\"width:max-content\">" KW_TWO "</div>"
    "</div>"
    "<script>"
    "function kwW(id) { return document.getElementById(id).offsetWidth; }"
    "fetch('/api/sizekw?o=' + kwW('kwo') + '&a=' + kwW('kw1') + '&b=' + kwW('kw2') + '&c=' + kwW('kw3') +"
    "      '&d=' + kwW('kw4') + '&e=' + kwW('kw5'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef KW_TWO

#endif
