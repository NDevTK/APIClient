/* THE PERCENTAGE TRANSLATION DOCUMENT (`translatepct`): css-transforms-1 §7's percentage translations against
   §5's reference box, replit's shape. `#c` is the centring idiom, `translate(-50%, -50%)` on a 40x20 box at
   (100, 100), so its rectangle starts at (80, 90). `#b` and `#k` are 40px wide with 10px padding at
   `translateX(25%)`: the border box (60px) gives 15px, and `transform-box: content-box` 10px. `#p`'s
   `translateY(50%)` of its 40px height carries its child 20px down, through the ancestor's transform.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TRANSLATEPCT_H
#define ENGINE_HOST_FIXTURE_TRANSLATEPCT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TRANSLATEPCT[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=c style=\"position:absolute;left:100px;top:100px;width:40px;height:20px;"
    "transform:translate(-50%, -50%)\"></div>"
    "<div id=b style=\"position:absolute;left:0;top:200px;width:40px;height:20px;padding:10px;"
    "transform:translateX(25%)\"></div>"
    "<div id=k style=\"position:absolute;left:0;top:250px;width:40px;height:20px;padding:10px;"
    "transform:translateX(25%);transform-box:content-box\"></div>"
    "<div id=p style=\"position:absolute;left:0;top:300px;width:40px;height:40px;transform:translateY(50%)\">"
    "<div id=pc style=\"width:10px;height:10px\"></div></div>"
    "<script>"
    "function tpR(id) { return document.getElementById(id).getBoundingClientRect(); }"
    "fetch('/api/translatepct?cx=' + tpR('c').left + '&cy=' + tpR('c').top + '&b=' + tpR('b').left +"
    "      '&k=' + tpR('k').left + '&pc=' + tpR('pc').top);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
