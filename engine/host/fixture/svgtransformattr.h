/* THE SVG TRANSFORM ATTRIBUTE DOCUMENT (`svgtransformattr`): css-transforms-1 §6 "The SVG transform Attribute"
   as the presentation attribute of the `transform` property, read through §3.2's resolved value. `#a` is a
   translation by two numbers; `#b` the three-value rotate(a cx cy), which §6.3 maps to translate(cx, cy)
   rotate(a) translate(-cx, -cy); `#c` two functions separated by a comma; `#d` a matrix; `#j` two numbers with
   no separator but the sign. `#e` and `#f` do not match §6.2's grammar and compute to `none`. `#g`'s style
   declaration outranks its attribute (§6.1), `#h` is a gradient's `gradientTransform`, and `#i` a gradient's
   `transform`, which is not that element's presentation attribute. `#k` is an HTML box's §3.2 matrix.
   Spaces and commas are written as `_`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SVGTRANSFORMATTR_H
#define ENGINE_HOST_FIXTURE_SVGTRANSFORMATTR_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SVGTRANSFORMATTR[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<svg width=\"100\" height=\"100\">"
    "<g id=a transform=\"translate(10 20)\"></g>"
    "<g id=b transform=\"rotate(90 10 10)\"></g>"
    "<g id=c transform=\" scale(2) ,skewX(0) \"></g>"
    "<g id=d transform=\"matrix(1,2,3,4,5,6)\"></g>"
    "<g id=j transform=\"translate(1e1-5)\"></g>"
    "<g id=e transform=\"translate(10,\"></g>"
    "<g id=f transform=\"translateX(10)\"></g>"
    "<g id=g style=\"transform:translate(5px)\" transform=\"translate(100)\"></g>"
    "<linearGradient id=h gradientTransform=\"scale(3)\"></linearGradient>"
    "<linearGradient id=i transform=\"scale(3)\"></linearGradient>"
    "</svg>"
    "<div id=k style=\"width:10px;height:10px;transform:translate(10px) rotate(90deg)\"></div>"
    "<script>"
    "function sa(id) { return getComputedStyle(document.getElementById(id)).transform.replace(/[ ,]+/g, '_'); }"
    "fetch('/api/svgtransformattr?a=' + sa('a') + '&b=' + sa('b') + '&c=' + sa('c') + '&d=' + sa('d') +"
    "      '&j=' + sa('j') + '&e=' + sa('e') + '&f=' + sa('f') + '&g=' + sa('g') + '&h=' + sa('h') +"
    "      '&i=' + sa('i') + '&k=' + sa('k'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
