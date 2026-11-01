/* THE INPUT BUTTON DOCUMENT. HTML §15.5.12 "The input element as a button": an `input` in the Submit Button,
   Reset Button or Button state depicts a button whose contents are "the text of the element's value attribute,
   if any, or text derived from the element's type attribute in an implementation-defined (and probably
   locale-specific) fashion, if not" (this engine takes Chrome's "Submit", "Reset" and nothing). Every button
   sets `font: 16px Arial; line-height: 20px; padding: 0 4px; border: 2px solid; margin: 0`, so the UA's own
   platform styling cannot reach the numbers. Expected values are Chrome 153's (Playwright): "Send it" 62x24,
   "Submit" 62x24, "Reset" 54x24, "Go" 33x24, and `width: 100px` 100x24, which is HTML §15.3.10's
   `box-sizing: border-box`; a text input keeps `content-box`, and with no `line-height` of its own a button's is
   §15.3.10's `initial`, `normal`, under a body whose is 40px.
   One divergence, where the spec wins: Chrome draws an empty-valued Button 12x24, a line of nothing. Its
   contents are no characters, so CSS 2.2 §9.4.2 gives it no line box and its height is its 4px of border.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INPUTBUTTON_H
#define ENGINE_HOST_FIXTURE_INPUTBUTTON_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define TF_INPUTBUTTON_STYLE "font:16px Arial;line-height:20px;padding:0 4px;border:2px solid black;margin:0"

static const char HTML_INPUTBUTTON[] =
    "<!doctype html><html><body style=\"line-height:40px\">"
    "<p><input id=b1 type=submit value=\"Send it\" style=\"" TF_INPUTBUTTON_STYLE "\"></p>"
    "<p><input id=b2 type=submit style=\"" TF_INPUTBUTTON_STYLE "\"></p>"
    "<p><input id=b3 type=reset style=\"" TF_INPUTBUTTON_STYLE "\"></p>"
    "<p><input id=b4 type=button value=\"Go\" style=\"" TF_INPUTBUTTON_STYLE "\"></p>"
    "<p><input id=b5 type=button style=\"" TF_INPUTBUTTON_STYLE "\"></p>"
    "<p><input id=b6 type=submit value=\"Send it\" style=\"" TF_INPUTBUTTON_STYLE ";width:100px\"></p>"
    "<p><input id=b7 type=submit value=\"Send it\"><input id=t1></p>"
    "<script>"
    "function ib(id) { var e = document.getElementById(id); return e.offsetWidth + 'x' + e.offsetHeight; }"
    "var cs = getComputedStyle(document.getElementById('b7')), ct = getComputedStyle(document.getElementById('t1'));"
    "fetch('/api/inputbutton?b1=' + ib('b1') + '&b2=' + ib('b2') + '&b3=' + ib('b3') + '&b4=' + ib('b4')"
    " + '&b5=' + ib('b5') + '&b6=' + ib('b6') + '&lh=' + cs.lineHeight + '&bs=' + cs.boxSizing"
    " + '&tbs=' + ct.boxSizing);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
