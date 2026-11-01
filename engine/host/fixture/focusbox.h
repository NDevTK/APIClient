/* THE FOCUS BOX DOCUMENT. Three focusing algorithms that ask HTML §6.6.2's "being rendered" of their candidates
   outside the focusing steps' own machine. Update the rendering's flush autofocus candidates (HTML §6.6.7)
   skips an `autofocus` button inside a `display: none` subtree and focuses the `input` after it; the parser
   inserts both, so §6.6.7's insertion steps reach them through core/html/autofocus.h's parsed-tree walk. A
   `dialog`'s
   show() runs §4.11.4's dialog focusing steps, whose focus delegate (§6.6.4) skips a `display: none` button and
   takes the button after it, and a parsed `<button autofocus>` in a second dialog is its autofocus delegate
   ahead of the button before it. A `dialog popover`'s showPopover() runs §6.12's popover focusing steps, whose step 2
   hands it to the same dialog focusing steps. Each reads `document.activeElement` after it, in the first
   animation frame callback, which update the rendering runs after the autofocus flush. Chrome 153 gives
   af, db, q2 and dpb for the same markup. The popover is shown by HTML §15.3.3's `dialog:popover-open` rule
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
    "<div id=hd style=\"display:none\"><button id=hid autofocus>h</button></div><input id=af autofocus>"
    "<dialog id=dg><button id=dh style=\"display:none\">x</button><button id=db>d</button></dialog>"
    "<dialog id=dq><button id=q1>1</button><button id=q2 autofocus>2</button></dialog>"
    "<dialog popover id=dp><button id=dpb>p</button></dialog>"
    "<script>"
    "requestAnimationFrame(function () {"
    "  var a = document.activeElement.id;"
    "  document.getElementById('dg').show();"
    "  var d = document.activeElement.id;"
    "  document.getElementById('dg').close();"
    "  document.getElementById('dq').show();"
    "  var q = document.activeElement.id;"
    "  document.getElementById('dq').close();"
    "  document.getElementById('dp').showPopover();"
    "  var p = document.activeElement.id;"
    "  fetch('/api/focusbox?a=' + a + '&d=' + d + '&q=' + q + '&p=' + p);"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
