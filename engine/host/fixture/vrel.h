/* THE LINE-RELATIVE ALIGNMENT DOCUMENT (`vrel`): css-inline-3 §4.2.3's `top`, `center` and `bottom` on atomic
   inlines, the `vertical-align` values astexplorer's inline-blocks carry. Each block holds one line of a 30px font
   with a baseline-aligned 10px box (`#r1`-`#r3`). In `#d1` a 50px `top` box and a 50px `bottom` box make the line
   50px tall and both stand at its top edge, and a 20px `center` box sits 15px down. `#d2`'s 60px `bottom` box and
   `#d3`'s 60px `top` box each make a 60px line; the first moves the baseline down so the line's whole height
   fits above the strut's depth, the second leaves it where the strut put it, so `#r2` stands 30px (60px less the
   30px strut) below `#r3`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_VREL_H
#define ENGINE_HOST_FIXTURE_VREL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define VR_BOX(id, h, va) "<span id=" id " style=\"display:inline-block;width:10px;height:" h ";vertical-align:" \
                          va "\"></span>"

static const char HTML_VREL[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=d1 style=\"font-size:30px;line-height:30px\">"
    VR_BOX("r1", "10px", "baseline") VR_BOX("t", "50px", "top") VR_BOX("b", "50px", "bottom")
    VR_BOX("c", "20px", "center") "</div>"
    "<div id=d2 style=\"font-size:30px;line-height:30px\">"
    VR_BOX("r2", "10px", "baseline") VR_BOX("b2", "60px", "bottom") "</div>"
    "<div id=d3 style=\"font-size:30px;line-height:30px\">"
    VR_BOX("r3", "10px", "baseline") VR_BOX("t3", "60px", "top") "</div>"
    "<script>"
    "function vrT(id, d) { return Math.round(document.getElementById(id).getBoundingClientRect().top -"
    "                                        document.getElementById(d).getBoundingClientRect().top); }"
    "function vrH(d) { return document.getElementById(d).offsetHeight; }"
    "fetch('/api/vrel?t=' + vrT('t', 'd1') + '&b=' + vrT('b', 'd1') + '&c=' + vrT('c', 'd1') + '&h1=' + vrH('d1') +"
    "      '&h2=' + vrH('d2') + '&b2=' + vrT('b2', 'd2') + '&h3=' + vrH('d3') + '&t3=' + vrT('t3', 'd3') +"
    "      '&dr=' + (vrT('r2', 'd2') - vrT('r3', 'd3')));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef VR_BOX

#endif
