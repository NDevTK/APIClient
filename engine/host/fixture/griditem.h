/* THE GRID ITEM DOCUMENT: css-grid-1 §6.2 "Grid Item Sizing" for a `width: auto` item. One of test_forced.c's
   exploring documents (its TF_DOCS table), alone because its row is the one core/layout/used_value.c's
   grid-item DFAIL refuses, and an abort costs only this document. The container is 300px wide with no explicit
   columns, so its one implicit `auto` column takes the free space (§11.8 "Stretch auto Tracks") and the item,
   stretch-fit for `normal` alignment, is 300px. The container's definite height keeps its own row sizing out
   of the question. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_GRIDITEM_H
#define ENGINE_HOST_FIXTURE_GRIDITEM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GRIDITEM[] =
    "<!doctype html><html>"
    "<body>"
    "<div style=\"display:grid;width:300px;height:50px\"><div id=gi>x</div></div>"
    "<script>fetch('/api/gridiw?w=' + getComputedStyle(document.getElementById('gi')).width);</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
