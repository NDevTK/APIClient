/* THE FILL WIDTH DOCUMENT (`fillwidth`): CSS 2.2 §9.4.2's line box width for a run that breaks, astexplorer's
   shape. `#a` is absolutely positioned with `width: 50%`, so its line boxes' width resolves against §10.1's
   containing block, a positioned block whose `auto` width is that of a `row` flex item's css-flexbox-1 §9.7
   used main size (400px), so the run of two 16-letter words wraps onto two lines 200px wide. The span's
   `offsetHeight` reaches from its first fragment's top to its second's bottom, one 30px line pitch more than
   the one-line `#r` in the same font: `d` is 30.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILLWIDTH_H
#define ENGINE_HOST_FIXTURE_FILLWIDTH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILLWIDTH[] =
    "<!doctype html><html><body style=\"margin:0;font-size:10px;line-height:30px\">"
    "<div style=\"display:flex;width:400px\"><div style=\"flex:1 1 0\"><div>"
    "<div style=\"position:relative\">"
    "<div id=a style=\"position:absolute;left:0;top:0;width:50%\">"
    "<span id=s>mmmmmmmmmmmmmmmm mmmmmmmmmmmmmmmm</span></div></div></div></div></div>"
    "<div style=\"margin-top:100px\"><span id=r>mmmmmmmmmmmmmmmm</span></div>"
    "<script>"
    "var fwS = document.getElementById('s').offsetHeight, fwR = document.getElementById('r').offsetHeight;"
    "fetch('/api/fillwidth?d=' + (fwS - fwR));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
