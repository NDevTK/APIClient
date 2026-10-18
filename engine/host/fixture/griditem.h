/* THE GRID ITEM DOCUMENT: css-grid-1 §6.2 "Grid Item Sizing" for `auto`-sized items. One of test_forced.c's
   exploring documents (its TF_DOCS table). The host's paint of the presented document asks each item's
   containing block (css-grid-1 §3.3 "Grid Areas") and border-box origin, which §11's track sizing produces.
   The first container is 300px by 50px with no explicit tracks, so its one implicit `auto` column and its
   one `auto` row take the free space (§11.8 "Stretch auto Tracks") and its item, stretch-fit for `normal`
   alignment, is 300px by 50px. The second has an `auto` height, which §5.2 "Sizing Grid Containers" makes the
   sum of its two rows: 5px of margin plus 20px, then 30px, 55px in all, with the second item's border box
   25px below the container's top. The third is 400px wide over `100px 1fr 2fr` (§7.2 "Explicit Track
   Sizing"): the fixed column is 100px and §11.7 "Expand Flexible Tracks" gives the two flexible ones 100px
   and 200px of the 300px left. The fourth is 300px by 200px over rows `50px 1fr`, so its second row is
   150px tall and starts 50px down. Two `inline-grid`s sit on lines beside text, aligned by css-grid-1 §10.6
   "Grid Container Baselines": the first by its first item's first line, the second by its first item's
   synthesized baseline (an empty 30px box, the first in grid order in a row whose other item has text).
   Chrome 155 gives these numbers for the same markup. It ends with TF_CANVAS_INK, which test_forced.c defines
   before including it. */
#ifndef ENGINE_HOST_FIXTURE_GRIDITEM_H
#define ENGINE_HOST_FIXTURE_GRIDITEM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GRIDITEM[] =
    "<!doctype html><html>"
    "<body>"
    "<div style=\"display:grid;width:300px;height:50px\"><div id=gi>x</div></div>"
    "<div id=g2 style=\"display:grid;width:200px\"><div style=\"height:20px;margin-top:5px\">a</div>"
    "<div id=r2 style=\"height:30px\">b</div></div>"
    "<div id=g3 style=\"display:grid;width:400px;grid-template-columns:100px 1fr 2fr\">"
    "<div style=\"height:10px\">a</div><div id=c2 style=\"height:10px\">b</div>"
    "<div id=c3 style=\"height:10px\">c</div></div>"
    "<div id=g4 style=\"display:grid;width:300px;height:200px;grid-template-rows:50px 1fr\">"
    "<div>a</div><div id=r4>b</div></div>"
    "<div id=l5><span id=t5>text</span> <span id=g5 style=\"display:inline-grid;width:100px;padding-top:7px\">"
    "<div>ab</div><div>cd</div></span> tail</div>"
    "<div id=l6><span id=t6>text</span> <span style=\"display:inline-grid;width:100px;"
    "grid-template-columns:50px 50px\"><div style=\"height:30px\"></div><div>xy</div></span> tail</div>"
    /* Items spanning several tracks (§11.5 "Resolve Intrinsic Track Sizes"). A 170px item over two `auto`
       columns of 40px and 50px items adds 40px to each, so the second starts at 80 and is 90 wide. An
       `auto`-width item over `100px 1fr` has an automatic minimum of zero (§6.6), so the `1fr 1fr` columns
       split the 200px left and the third starts at 200. A 100px item spanning two `auto` rows of 20px items
       makes each 50px. Two `span 2` items and a single one over three 50px columns place the second at the
       next row's start and the third after it, 100px across and 10px down. */
    "<div id=s1 style=\"display:grid;width:300px;grid-template-columns:auto auto;justify-content:start\">"
    "<div style=\"grid-column:1/3\"><div style=\"width:170px;height:10px\"></div></div>"
    "<div><div style=\"width:40px;height:10px\"></div></div>"
    "<div id=s1c><div style=\"width:50px;height:10px\"></div></div></div>"
    "<div id=s2 style=\"display:grid;width:300px;grid-template-columns:100px 1fr 1fr\">"
    "<div style=\"grid-column:1/3\"><div style=\"width:250px;height:10px\"></div></div>"
    "<div id=s2b><div style=\"width:10px;height:10px\"></div></div></div>"
    "<div id=s3 style=\"display:grid;width:200px;grid-template-columns:100px 100px\">"
    "<div style=\"grid-row:span 2\"><div style=\"height:100px\"></div></div>"
    "<div id=s3b><div style=\"height:20px\"></div></div><div id=s3c><div style=\"height:20px\"></div></div></div>"
    "<div id=s4 style=\"display:grid;width:150px;grid-template-columns:50px 50px 50px\">"
    "<div style=\"grid-column:span 2;height:10px\"></div><div style=\"grid-column:span 2;height:10px\"></div>"
    "<div id=s4c style=\"height:10px\"></div></div>"
    "<script>var g2 = document.getElementById('g2');"
    "function R(id) { return document.getElementById(id).getBoundingClientRect(); }"
    "fetch('/api/gridiw?w=' + getComputedStyle(document.getElementById('gi')).width"
    " + '&h=' + getComputedStyle(document.getElementById('gi')).height"
    " + '&c=' + getComputedStyle(g2).height"
    " + '&t=' + (document.getElementById('r2').getBoundingClientRect().top - g2.getBoundingClientRect().top)"
    " + '&x2=' + (R('c2').left - R('g3').left) + '&w2=' + R('c2').width + '&w3=' + R('c3').width"
    " + '&y4=' + (R('r4').top - R('g4').top) + '&h4=' + R('r4').height"
    " + '&t5=' + (R('t5').top - R('l5').top) + '&g5=' + (R('g5').top - R('l5').top) + '&l5=' + R('l5').height"
    " + '&t6=' + (R('t6').top - R('l6').top) + '&l6=' + R('l6').height"
    " + '&s1x=' + (R('s1c').left - R('s1').left) + '&s1w=' + R('s1c').width"
    " + '&s2x=' + (R('s2b').left - R('s2').left)"
    " + '&s3y=' + (R('s3c').top - R('s3').top) + '&s3h=' + R('s3b').height"
    " + '&s4x=' + (R('s4c').left - R('s4').left) + '&s4y=' + (R('s4c').top - R('s4').top));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
