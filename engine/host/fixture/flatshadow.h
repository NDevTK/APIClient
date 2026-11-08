/* THE FLAT SHADOW DOCUMENT (`flatshadow`): CSS Scoping's flat tree under layout. `#fsHost` (margin-left 7px)
   holds a declarative open shadow root whose tree is `#fsPad` (13px tall, padding-left 11px) and a div with
   padding-left 5px around a default `<slot>`; the host's light child `#fsSlotted` (17x19) is assigned to that
   slot, so its box is the slot's child in the flat tree and stands below `#fsPad` behind the 5px padding.
   Chrome reads `#fsPad` at x 7, y 0, 1273 wide in the 1280px viewport, and `#fsSlotted` at x 12, y 13, 17 wide;
   DOM §4.2.4's getElementById on the shadow root finds `#fsPad`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FLATSHADOW_H
#define ENGINE_HOST_FIXTURE_FLATSHADOW_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FLATSHADOW[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=fsHost style=\"margin-left:7px\"><template shadowrootmode=open>"
    "<div id=fsPad style=\"padding-left:11px;height:13px\"></div>"
    "<div style=\"padding-left:5px\"><slot></slot></div></template>"
    "<div id=fsSlotted style=\"width:17px;height:19px\"></div></div>"
    "<script>"
    "var fsRoot = document.getElementById('fsHost').shadowRoot;"
    "var fsP = fsRoot.firstElementChild.getBoundingClientRect();"
    "var fsS = document.getElementById('fsSlotted').getBoundingClientRect();"
    "fetch('/api/flatshadow?px=' + fsP.left + '&pw=' + fsP.width + '&py=' + fsP.top +"
    "      '&sx=' + fsS.left + '&sy=' + fsS.top + '&sw=' + fsS.width + '&g=' + (fsRoot.getElementById('fsPad') ==="
    "      fsRoot.firstElementChild ? 1 : 0));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
