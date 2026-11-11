/* THE NESTED GRID DOCUMENTS: css-grid-1 §11.3 "Track Sizing Algorithm" of grid containers nested through
   shrink-to-fit atomic inlines, two levels (`gridnest2`) and four (`gridnest4`). Two of test_forced.c's
   exploring documents (its TF_DOCS table). Each level is a grid with one `auto` column holding one item, which
   holds an `inline-block` holding the next level; the innermost `inline-block` holds a 10px square. The
   outermost grid is 200px wide, so §11.8 "Stretch auto Tracks" makes its column 200px, and the `inline-block`
   in it is CSS 2.2 §10.3.9's shrink-to-fit over that width: its max-content width is the inner grid's, whose
   column is its item's max-content contribution, so every level below is 10px wide and the square starts at
   the left edge. CSSOM VIEW §7's `offsetWidth` and `offsetLeft` and §6's `scrollWidth` are read under a
   resting drive; the shrink-to-fit width reads its containing block's width, a grid area's, which the drive
   asks as the columns' layout job question before the line box reads it. The numbers are derived from those
   sections, not read off a browser. They end with TF_CANVAS_INK, which test_forced.c defines before including
   it.
   `gridrun` nests a grid in a grid's item over one text run of 1024 words, longer than one activation's line
   items, so its css-sizing-3 §5.1 measurement rests. The outer grid's columns ask it, and its row sizing lays
   the inner grid out in a grid row context whose columns ask it again; it reads nothing a context decides, so
   one job measures it once. Both grids' one `auto` column stretches to the 200px (§11.8), so #gt is 200px. */
#ifndef ENGINE_HOST_FIXTURE_GRIDNEST_H
#define ENGINE_HOST_FIXTURE_GRIDNEST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define GN_OPEN "<div style=\"display:grid\"><div><span style=\"display:inline-block\">"
#define GN_LEAF "<div id=gl style=\"width:10px;height:10px\"></div>"
#define GN_CLOSE "</span></div></div>"
#define GN_DOC(name, levels, closes)                                                                          \
    "<!doctype html><html><body style=\"margin:0\">"                                                          \
    "<div id=go style=\"width:200px;overflow:hidden\">"                                                       \
    "<div style=\"display:grid\"><div><span id=gs style=\"display:inline-block\">" levels GN_LEAF closes      \
    GN_CLOSE "</div>"                                                                                         \
    "<script>var o=document.getElementById('go'),s=document.getElementById('gs'),"                            \
    "l=document.getElementById('gl');"                                                                        \
    "fetch('/api/" name "?w='+s.offsetWidth+'&l='+l.offsetLeft+'&sw='+o.scrollWidth);</script>"               \
    TF_CANVAS_INK "</body></html>"

static const char HTML_GRIDNEST2[] = GN_DOC("gridnest2", GN_OPEN, GN_CLOSE);
static const char HTML_GRIDNEST4[] = GN_DOC("gridnest4", GN_OPEN GN_OPEN GN_OPEN, GN_CLOSE GN_CLOSE GN_CLOSE);

#define GN_W4 "aa bb cc dd "
#define GN_W16 GN_W4 GN_W4 GN_W4 GN_W4
#define GN_W64 GN_W16 GN_W16 GN_W16 GN_W16
#define GN_W256 GN_W64 GN_W64 GN_W64 GN_W64
static const char HTML_GRIDRUN[] =
    "<!doctype html><html><body style=\"margin:0\"><div style=\"width:200px\">"
    "<div style=\"display:grid\"><div><div style=\"display:grid\"><div id=gt>" GN_W256 "</div></div></div></div>"
    "</div><script>fetch('/api/gridrun?w='+document.getElementById('gt').offsetWidth);</script>"
    TF_CANVAS_INK "</body></html>";
#undef GN_W4
#undef GN_W16
#undef GN_W64
#undef GN_W256

#undef GN_OPEN
#undef GN_LEAF
#undef GN_CLOSE
#undef GN_DOC

#endif
