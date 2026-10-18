/* THE GRID ITEM DOCUMENT: css-grid-1 §6.2 "Grid Item Sizing" for `auto`-sized items. One of test_forced.c's
   exploring documents (its TF_DOCS table). The host's paint of the presented document asks each item's
   containing block (css-grid-1 §3.3 "Grid Areas") and border-box origin, which §11's track sizing produces.
   The first container is 300px by 50px with no explicit tracks, so its one implicit `auto` column and its
   one `auto` row take the free space (§11.8 "Stretch auto Tracks") and its item, stretch-fit for `normal`
   alignment, is 300px by 50px. The second has an `auto` height, which §5.2 "Sizing Grid Containers" makes the
   sum of its two rows: 5px of margin plus 20px, then 30px, 55px in all, with the second item's border box
   25px below the container's top. It ends with TF_CANVAS_INK, which test_forced.c defines before including
   it. */
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
    "<script>var g2 = document.getElementById('g2');"
    "fetch('/api/gridiw?w=' + getComputedStyle(document.getElementById('gi')).width"
    " + '&h=' + getComputedStyle(document.getElementById('gi')).height"
    " + '&c=' + getComputedStyle(g2).height"
    " + '&t=' + (document.getElementById('r2').getBoundingClientRect().top - g2.getBoundingClientRect().top));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
