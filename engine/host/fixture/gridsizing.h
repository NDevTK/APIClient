/* THE GRID TRACK SIZING DOCUMENT: css-grid-1 §7.2.1 "Track Sizes"' min and max track sizing functions, sized
   by §11.4 "Initialize Track Sizes" and §11.5 "Resolve Intrinsic Track Sizes". One of test_forced.c's exploring
   documents (its TF_DOCS table). Every item's content is fixed-width inline-blocks, so the contributions are
   font-free: two side by side give a min-content contribution of the wider and a max-content one of the sum.
   It reads a `1fr max-content max-content` list beside 20px, 50px and 30px items; a `min-content` and a
   `max-content` column over the same 30px-and-40px item; `minmax()` with a fixed min above and below the
   content; `fit-content()` whose auto minimum wins (a 100px item) and whose limit clamps (60px of a 70px
   max-content); `minmax(min-content, 1fr)` taking the free space; a spanning item's max-content contribution
   shared over two `max-content` tracks; and an `inline-grid`'s min-content column. Then §7.2.3's `repeat()`: an
   integer count, and `auto-fit` beside `auto-fill` in a 500px box with 20px gaps (four 100px repetitions, of
   which `auto-fit` collapses the two empty ones), both centred; and §10.5's content alignment: `space-between`
   (with a spanning item's area widened by the spacing), `space-evenly` and `end`. Chrome 155.0.8059.84 gives
   every number the rows expect for this markup. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_GRIDSIZING_H
#define ENGINE_HOST_FIXTURE_GRIDSIZING_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GRIDSIZING[] =
    "<!doctype html><html>"
    "<body>"
    "<div id=t1 style=\"display:grid;width:300px;grid-template-columns:1fr max-content max-content\"><div>"
    "<i style=\"display:inline-block;width:20px;height:5px\"></i></div><div id=t1b>"
    "<i style=\"display:inline-block;width:50px;height:5px\"></i></div><div id=t1c>"
    "<i style=\"display:inline-block;width:30px;height:5px\"></i></div></div>"
    "<div id=t2 style=\"display:grid;width:400px;justify-content:start;"
    "grid-template-columns:min-content max-content\"><div id=t2a><i style=\"display:inline-block;"
    "width:30px;height:5px\"></i><i style=\"display:inline-block;width:40px;height:5px\"></i></div>"
    "<div id=t2b><i style=\"display:inline-block;width:30px;height:5px\"></i>"
    "<i style=\"display:inline-block;width:40px;height:5px\"></i></div></div>"
    "<div id=t3 style=\"display:grid;width:400px;justify-content:start;"
    "grid-template-columns:minmax(50px, max-content) minmax(10px,max-content)\"><div id=t3a>"
    "<i style=\"display:inline-block;width:20px;height:5px\"></i></div><div id=t3b>"
    "<i style=\"display:inline-block;width:80px;height:5px\"></i></div></div>"
    "<div id=t4 style=\"display:grid;width:400px;justify-content:start;"
    "grid-template-columns:fit-content(60px) fit-content(60px)\"><div id=t4a>"
    "<i style=\"display:inline-block;width:100px;height:5px\"></i></div><div id=t4b>"
    "<i style=\"display:inline-block;width:30px;height:5px\"></i></div></div>"
    "<div id=t5 style=\"display:grid;width:300px;grid-template-columns:100px minmax(min-content, 1fr)\">"
    "<div></div><div id=t5b><i style=\"display:inline-block;width:20px;height:5px\"></i></div></div>"
    "<div id=t6 style=\"display:grid;width:400px;justify-content:start;"
    "grid-template-columns:max-content max-content\"><div style=\"grid-column:1/3\">"
    "<i style=\"display:inline-block;width:100px;height:5px\"></i></div><div id=t6a>"
    "<i style=\"display:inline-block;width:20px;height:5px\"></i></div><div id=t6b></div></div>"
    "<div id=t8 style=\"display:grid;width:400px;justify-content:start;"
    "grid-template-columns:fit-content(60px) 10px\"><div><i style=\"display:inline-block;width:30px;"
    "height:5px\"></i><i style=\"display:inline-block;width:40px;height:5px\"></i></div><div id=t8b></div>"
    "</div>"
    "<div id=t9 style=\"display:grid;width:400px;justify-content:start;"
    "grid-template-columns:repeat(3, 40px)\"><div></div><div></div><div id=t9c></div></div>"
    "<div id=t10 style=\"display:grid;width:500px;gap:20px;justify-content:center;"
    "grid-template-columns:repeat(auto-fit,100px)\"><div id=t10a></div><div id=t10b></div></div>"
    "<div id=t11 style=\"display:grid;width:500px;gap:20px;justify-content:center;"
    "grid-template-columns:repeat(auto-fill,100px)\"><div id=t11a></div><div id=t11b></div></div>"
    "<div id=t12 style=\"display:grid;width:300px;justify-content:space-between;"
    "grid-template-columns:50px 50px 50px\"><div></div><div id=t12b></div><div id=t12c></div>"
    "<div id=t12s style=\"grid-column:1/4\"></div></div>"
    "<div id=t13 style=\"display:grid;width:250px;justify-content:space-evenly;"
    "grid-template-columns:50px 50px\"><div id=t13a></div><div id=t13b></div></div>"
    "<div id=t14 style=\"display:grid;width:300px;justify-content:end;grid-template-columns:50px 50px\">"
    "<div id=t14a></div><div></div></div>"
    "<div><span id=t7 style=\"display:inline-grid;grid-template-columns:min-content 30px\"><div>"
    "<i style=\"display:inline-block;width:25px;height:5px\"></i></div><div></div></span></div>"
    "<script>"
    "function R(id){return document.getElementById(id).getBoundingClientRect();}"
    "var q='t1b='+(R('t1b').left-R('t1').left)+'&t1c='+(R('t1c').left-R('t1').left)"
    "+'&t2a='+R('t2a').width+'&t2b='+R('t2b').width"
    "+'&t3a='+R('t3a').width+'&t3b='+R('t3b').width"
    "+'&t4x='+(R('t4b').left-R('t4').left)+'&t4b='+R('t4b').width"
    "+'&t5b='+R('t5b').width"
    "+'&t6a='+R('t6a').width+'&t6b='+R('t6b').width"
    "+'&t7='+R('t7').width+'&t8x='+(R('t8b').left-R('t8').left)+'&t9c='+(R('t9c').left-R('t9').left)"
    "+'&t10a='+(R('t10a').left-R('t10').left)+'&t10b='+(R('t10b').left-R('t10').left)"
    "+'&t11a='+(R('t11a').left-R('t11').left)+'&t11b='+(R('t11b').left-R('t11').left)"
    "+'&t12b='+(R('t12b').left-R('t12').left)+'&t12c='+(R('t12c').left-R('t12').left)"
    "+'&t12s='+R('t12s').width+'&t13a='+(R('t13a').left-R('t13').left)"
    "+'&t13b='+(R('t13b').left-R('t13').left)+'&t14a='+(R('t14a').left-R('t14').left);"
    "fetch('/api/gridsizing?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
