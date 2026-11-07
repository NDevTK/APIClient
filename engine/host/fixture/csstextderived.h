/* THE DERIVED cssText DOCUMENT (`csstextderived`): CSSOM §6.6.1's cssText setter given a value computed from two
   environment unknowns, `devicePixelRatio` and a font metric (a span's rendered height), astexplorer's shape.
   The setter parses the value's example, so `position` reads back as `relative`, and each declaration keeps a
   derivation over the value, so `style.left` and `getPropertyValue('left')` read back as unknowns rather than as
   plain strings. A concrete cssText of the same form reads back concrete.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CSSTEXTDERIVED_H
#define ENGINE_HOST_FIXTURE_CSSTEXTDERIVED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CSSTEXTDERIVED[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<span id=ctm>x</span><div id=ctd></div><div id=ctc></div>"
    "<script>"
    "var ctH = document.getElementById('ctm').getBoundingClientRect().height;"
    "var ctV = Math.round(devicePixelRatio * 10 + ctH);"
    "var ctD = document.getElementById('ctd'), ctC = document.getElementById('ctc');"
    "ctD.style.cssText = 'left: ' + ctV + 'px; position: relative';"
    "ctC.style.cssText = 'left: 7px; position: relative';"
    "fetch('/api/csstext?l=' + ctD.style.left + '&g=' + ctD.style.getPropertyValue('left') +"
    "      '&p=' + ctD.style.position + '&c=' + ctC.style.left);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
