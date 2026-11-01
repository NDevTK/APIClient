/* THE FOCUS BOX DOCUMENT. Three focusing algorithms that ask HTML §6.6.2's "being rendered" of their candidates
   outside the focusing steps' own machine. Update the rendering's flush autofocus candidates (HTML §6.6.7)
   skips an `autofocus` button inside a `display: none` subtree and focuses the `input` after it; the script
   inserts both, so their insertion steps append them to the candidates. A `dialog`'s
   show() runs §4.11.4's dialog focusing steps, whose focus delegate (§6.6.4) skips a `display: none` button and
   takes the button after it. A `dialog popover`'s showPopover() runs §6.12's popover focusing steps, whose step 2
   hands it to the same dialog focusing steps. Each reads `document.activeElement` after it, in the first
   animation frame callback, which update the rendering runs after the autofocus flush. Chrome 153 gives
   af, db and dpb for the same markup. The popover is shown by HTML §15.3.1's `[popover]:popover-open` rule
   (rendering a `dialog:not([open])` popover), which is what lets its button be focused.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FOCUSBOX_H
#define ENGINE_HOST_FIXTURE_FOCUSBOX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FOCUSBOX[] =
    "<!doctype html><html><body>"
    "<div id=hd style=\"display:none\"></div>"
    "<dialog id=dg><button id=dh style=\"display:none\">x</button><button id=db>d</button></dialog>"
    "<dialog popover id=dp><button id=dpb>p</button></dialog>"
    "<script>"
    "var hb = document.createElement('button'); hb.id = 'hid'; hb.setAttribute('autofocus', '');"
    "document.getElementById('hd').appendChild(hb);"
    "var af = document.createElement('input'); af.id = 'af'; af.setAttribute('autofocus', '');"
    "document.body.insertBefore(af, document.getElementById('dg'));"
    "requestAnimationFrame(function () {"
    "  var a = document.activeElement.id;"
    "  document.getElementById('dg').show();"
    "  var d = document.activeElement.id;"
    "  document.getElementById('dg').close();"
    "  document.getElementById('dp').showPopover();"
    "  var p = document.activeElement.id;"
    "  fetch('/api/focusbox?a=' + a + '&d=' + d + '&p=' + p);"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
