/* THE INLINE STATIC POSITION DOCUMENT (`staticinline`): css-position-3 §3.5.3's static-position rectangle for
   an absolutely positioned box whose hypothetical box is inline-level, replit's shape. A `<span>` with
   `position: absolute` and every inset `auto` sits where its hypothetical inline box would: at the inline
   position the line had reached, on the line-over side of the line box holding it. `#a1` follows `#p1` on a
   container's first line, so `x1` (its left minus `#p1`'s right) is 0 and `y1` is that line's top, 0. `#a2`
   follows `#q2` on the second line of a container at top 100px with 20px lines, so `x2` is 0 and `y2` is 120.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STATICINLINE_H
#define ENGINE_HOST_FIXTURE_STATICINLINE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STATICINLINE[] =
    "<!doctype html><html><body style=\"margin:0;font:10px/20px monospace\">"
    "<div style=\"position:relative;width:300px\"><span id=p1>aaaa</span><span id=a1 "
    "style=\"position:absolute\">b</span></div>"
    "<div style=\"position:absolute;left:0;top:100px;width:40px\">aaaa <span id=q2>bbbb</span><span id=a2 "
    "style=\"position:absolute\">c</span></div>"
    "<script>"
    "function siR(id) { return document.getElementById(id).getBoundingClientRect(); }"
    "fetch('/api/staticinline?x1=' + (siR('a1').left - siR('p1').right) + '&y1=' + siR('a1').top +"
    "      '&x2=' + (siR('a2').left - siR('q2').right) + '&y2=' + siR('a2').top);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
