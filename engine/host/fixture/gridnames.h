/* THE NAMED GRID LINES DOCUMENT: css-grid-1 §7.2.2 "Naming Grid Lines: the [<custom-ident>*] syntax" in a
   track list, placed against by §8.3 "Line-based Placement". In a 300px grid of 50px 200px 50px columns named
   full-start, content-start, content-end and full-end, `content-start / content-end` and the lone `content`
   span the middle column (left 50, width 200) and `full` the whole grid (0, 300). In `repeat(2, [a] 100px [b])
   100px` the body's trailing `b` and leading `a` share line 1, so `a 2 / b 2` is the second column (100, 100).
   The steps answer a=50,200 b=50,200 c=0,300 d=100,100.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GRIDNAMES_H
#define ENGINE_HOST_FIXTURE_GRIDNAMES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GRIDNAMES[] =
    "<!doctype html><html><body>"
    "<div id=gn1 style=\"display:grid;width:300px;"
    "grid-template-columns:[full-start] 50px [content-start] 200px [content-end] 50px [full-end]\">"
    "<div id=gnA style=\"grid-column:content-start / content-end;height:10px\"></div>"
    "<div id=gnB style=\"grid-column:content;height:10px\"></div>"
    "<div id=gnC style=\"grid-column:full;height:10px\"></div></div>"
    "<div id=gn2 style=\"display:grid;width:300px;grid-template-columns:repeat(2, [a] 100px [b]) 100px\">"
    "<div id=gnD style=\"grid-column:a 2 / b 2;height:10px\"></div></div>"
    "<script>"
    "function gnAt(g, id) {"
    "  var r = document.getElementById(id).getBoundingClientRect();"
    "  var o = document.getElementById(g).getBoundingClientRect();"
    "  return Math.round(r.left - o.left) + ',' + Math.round(r.width);"
    "}"
    "fetch('/api/gridnames?a=' + gnAt('gn1', 'gnA') + '&b=' + gnAt('gn1', 'gnB') + '&c=' + gnAt('gn1', 'gnC')"
    " + '&d=' + gnAt('gn2', 'gnD'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
