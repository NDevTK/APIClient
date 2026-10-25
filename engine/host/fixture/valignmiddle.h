/* THE VERTICAL-ALIGN MIDDLE DOCUMENT: CSS 2.2 §10.8.1's `middle` for an atomic inline-level box, "Align the
   vertical midpoint of the box with the baseline of the parent box plus half the x-height of the parent". One
   of test_forced.c's exploring documents (its TF_DOCS table). An `inline-block` 100px tall beside text, without
   and with a 6px top margin, makes its line box exactly its margin box, whatever the x-height, because both of
   its halves outreach the strut's; a 4px one leaves the strut's 19px. Chrome 155.0.8059.84 and Firefox 158.0.1
   give every number the rows expect for this markup. Cross-engine note: the text's offset in those lines, and
   the small box's, follow the x-height, which each engine reads from its installed face (Chrome 39.375px and
   8.625px, Firefox 39.5px and 8.5px for the first and third) where this engine assumes css-values-4 §6.1.1's
   0.5em; no row reads them. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_VALIGNMIDDLE_H
#define ENGINE_HOST_FIXTURE_VALIGNMIDDLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_VALIGNMIDDLE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=m1 style=\"font:16px monospace\"><span>x</span><span id=b1 style=\"display:inline-block;"
    "vertical-align:middle;width:10px;height:100px\"></span></div>"
    "<div id=m2 style=\"font:16px monospace\"><span>x</span><span id=b2 style=\"display:inline-block;"
    "vertical-align:middle;width:10px;height:100px;margin-top:6px\"></span></div>"
    "<div id=m3 style=\"font:16px monospace\"><span>x</span><span style=\"display:inline-block;"
    "vertical-align:middle;width:10px;height:4px\"></span></div>"
    "<script>"
    "function R(i){return document.getElementById(i).getBoundingClientRect();}"
    "var q='h1='+R('m1').height+'&b1='+(R('b1').top-R('m1').top)+'&h2='+R('m2').height"
    "+'&b2='+(R('b2').top-R('m2').top)+'&h3='+R('m3').height;"
    "fetch('/api/valignmiddle?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
