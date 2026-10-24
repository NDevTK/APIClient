/* THE DEEP NESTING DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). Three thousand
   `div`s nest one inside the next around a 10px one, and CSSOM VIEW §6's getBoundingClientRect() of the innermost
   and of the outermost are read. Every level is laid out (CSS 2.1 §9.4.1's stack, §10.1's containing block, the
   cascade's inheritance and §2.7's box parent), and the depth is the document's choice, so each of those must
   reach the innermost box without a C frame per level: the run passes on a small C stack. The empty levels have
   no height, so the innermost box is at the top and the outermost is 10px tall. Chrome 155.0.8059.84 gives
   t=0, h=10 and l=0 for the same markup, with its parser's own depth limit flattening the levels past 512, which
   moves no box here. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DEEPNEST_H
#define ENGINE_HOST_FIXTURE_DEEPNEST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define DN_O10 "<div><div><div><div><div><div><div><div><div><div>"
#define DN_C10 "</div></div></div></div></div></div></div></div></div></div>"
#define DN_O100 DN_O10 DN_O10 DN_O10 DN_O10 DN_O10 DN_O10 DN_O10 DN_O10 DN_O10 DN_O10
#define DN_C100 DN_C10 DN_C10 DN_C10 DN_C10 DN_C10 DN_C10 DN_C10 DN_C10 DN_C10 DN_C10
#define DN_O1000 DN_O100 DN_O100 DN_O100 DN_O100 DN_O100 DN_O100 DN_O100 DN_O100 DN_O100 DN_O100
#define DN_C1000 DN_C100 DN_C100 DN_C100 DN_C100 DN_C100 DN_C100 DN_C100 DN_C100 DN_C100 DN_C100

static const char HTML_DEEPNEST[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\"><div id=dn0>"
    DN_O1000 DN_O1000 DN_O1000 "<div id=dn1 style=\"height:10px\">x</div>" DN_C1000 DN_C1000 DN_C1000
    "</div>"
    "<script>var r = document.getElementById('dn1').getBoundingClientRect();"
    "var o = document.getElementById('dn0').getBoundingClientRect();"
    "fetch('/api/deepnest?t=' + (r.top - o.top) + '&h=' + o.height + '&l=' + (r.left - o.left));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef DN_O10
#undef DN_C10
#undef DN_O100
#undef DN_C100
#undef DN_O1000
#undef DN_C1000

#endif
