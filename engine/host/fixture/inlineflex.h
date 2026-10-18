/* THE INLINE FLEX CONTAINER DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table).
   css-flexbox-1 §9.2 sizes an `inline-flex` "using the rules of the formatting context in which it
   participates", CSS 2.2 §10.3.9's shrink-to-fit over §9.9's intrinsic sizes, and §8.5 "Flex Container
   Baselines" puts it on its line by its first baseline. The first container's first item has 5px of padding
   above its text, so the text beside it sits 5px down. The second's item in baseline alignment gives the
   group's shared baseline, 9px down. The third has no item and sits on its margin box's bottom. The fourth's
   first item is empty: Chrome 155 synthesizes from it, 23px down, where the section's third step would take the
   second item's text. An inline-block holds a `column` container whose items are 40px and 60px wide, so its
   §9.9.2 width is 60px. Chrome 155 gives these numbers for the same markup. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INLINEFLEX_H
#define ENGINE_HOST_FIXTURE_INLINEFLEX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INLINEFLEX[] =
    "<!doctype html><html>"
    "<body>"
    "<div id=l1><span id=t1>text</span> <span style=\"display:inline-flex\"><div style=\"padding-top:5px\">ab</div>"
    "<div>cd ef</div></span> tail</div>"
    "<div id=l2><span id=t2>text</span> <span style=\"display:inline-flex\"><div style=\"height:30px;width:20px\">"
    "</div><div style=\"align-self:baseline;padding-top:9px\">xy</div></span> tail</div>"
    "<div id=l3><span id=t3>text</span> <span style=\"display:inline-flex;height:30px;width:20px;"
    "margin-bottom:4px\"></span> tail</div>"
    "<div id=l4><span id=t4>text</span> <span style=\"display:inline-flex\"><div style=\"height:20px;width:20px;"
    "padding-bottom:3px\"></div><div>xy</div></span> tail</div>"
    "<div><div id=c5 style=\"display:inline-block\"><div style=\"display:flex;flex-direction:column\">"
    "<div style=\"width:40px;height:10px\"></div><div style=\"width:60px;height:10px\"></div></div></div></div>"
    "<script>function R(id) { return document.getElementById(id).getBoundingClientRect(); }"
    "fetch('/api/iflex?t1=' + (R('t1').top - R('l1').top) + '&t2=' + (R('t2').top - R('l2').top)"
    " + '&t3=' + (R('t3').top - R('l3').top) + '&t4=' + (R('t4').top - R('l4').top)"
    " + '&l3=' + R('l3').height + '&c5=' + R('c5').width);</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
