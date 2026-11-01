/* THE ALT BUTTON DOCUMENT. HTML §15.4.2 "Images"' fifth rule: an `input` in the Image Button state with an
   `alt` and no `src` does not represent an image, and nothing is fetched that could change that, so it is "a
   replaced element consisting of a button whose content is the element's alternative text", whose natural
   dimensions are "about one line in height and whatever width is necessary to render the text on one line".
   With `font: 16px Arial; line-height: 20px`, "Go now" is 55.140625px wide in Chrome 153 (measured on a `span`
   in the same font), so `offsetWidth` is 55 and `offsetHeight` 20; with `padding: 0 4px` they are 63 and 20.
   Chrome's own Image Button is 16px wider than either, the broken-image icon its fallback draws beside the
   text, which §15.4.2 offers for its second rule ("optionally alongside an icon") and not for the fifth.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ALTBUTTON_H
#define ENGINE_HOST_FIXTURE_ALTBUTTON_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ALTBUTTON[] =
    "<!doctype html><html><body>"
    "<p><input id=ib1 type=image alt=\"Go now\" style=\"font:16px Arial;line-height:20px\"></p>"
    "<p><input id=ib2 type=image alt=\"Go now\" style=\"font:16px Arial;line-height:20px;padding:0 4px\"></p>"
    "<script>"
    "var ib1 = document.getElementById('ib1'), ib2 = document.getElementById('ib2');"
    "fetch('/api/altbutton?w1=' + ib1.offsetWidth + '&h1=' + ib1.offsetHeight"
    " + '&w2=' + ib2.offsetWidth + '&h2=' + ib2.offsetHeight);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
