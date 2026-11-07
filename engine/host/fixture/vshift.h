/* THE BASELINE SHIFT DOCUMENT (`vshift`): css-inline-3 §4.2.3's baseline-relative shifts, the `vertical-align`
   values `<length>`, `<percentage>`, `sub` and `super`. Every box is a 10px `inline-block` on one line of a 30px
   font, so each one's top edge sits its shift above `#r`'s, which is baseline-aligned. `#s` is `-50%` of its own
   20px `line-height` (Font Awesome's `.fa-lg` shape, astexplorer's), `#p` is `4px`, `#b` is `sub` (a fifth of the
   parent's 30px) and `#u` is `super` (a third of it); `#nn` takes the `5px` of the inline box around it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_VSHIFT_H
#define ENGINE_HOST_FIXTURE_VSHIFT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define VS_BOX(id, style) "<span id=" id " style=\"display:inline-block;width:10px;height:10px;" style "\"></span>"

static const char HTML_VSHIFT[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div style=\"font-size:30px;line-height:30px\">"
    VS_BOX("r", "") VS_BOX("s", "line-height:20px;vertical-align:-50%") VS_BOX("p", "vertical-align:4px")
    VS_BOX("b", "vertical-align:sub") VS_BOX("u", "vertical-align:super")
    "<span style=\"vertical-align:5px\">" VS_BOX("nn", "") "</span>"
    "</div>"
    "<script>"
    "function vsD(id) { return document.getElementById(id).getBoundingClientRect().top -"
    "                          document.getElementById('r').getBoundingClientRect().top; }"
    "fetch('/api/vshift?s=' + vsD('s') + '&p=' + vsD('p') + '&b=' + vsD('b') + '&u=' + vsD('u') +"
    "      '&n=' + vsD('nn'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef VS_BOX

#endif
