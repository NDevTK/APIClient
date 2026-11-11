/* The font load document. css-fonts-4 §5.2 "Matching font styles" loads an `@font-face` face when a character
   being rendered is in its unicode-range and the face is not loaded; css-fonts-4 §4.8.1 "Font loading
   guidelines" forbids downloading a face no text uses. Each family below is one case, with its own address:
   used by two elements (one naming it in another case), never named, named only under `display: none`, named
   for text outside its unicode-range, a composite of two ranges, named after an installed family that has
   the text, a src list whose first format is unknown, a rule in a linked sheet with a relative address
   (test_forced.c's FONTLOAD_SHEET), text no script measures, and white space only.
   Chrome 153.0.8010.12 loading this same markup requests fl-used, fl-comp-latin, fl-fmt.ttf, fl-ext and
   fl-paint, each once, and none of the others.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FONTLOAD_H
#define ENGINE_HOST_FIXTURE_FONTLOAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FONTLOAD[] =
    "<!doctype html><html><head><style>"
    "@font-face{font-family:FlUsed;src:url(/fonts/fl-used.woff2) format(\"woff2\")}"
    "@font-face{font-family:FlUnused;src:url(/fonts/fl-unused.woff2)}"
    "@font-face{font-family:FlNone;src:url(/fonts/fl-none.woff2)}"
    "@font-face{font-family:FlRange;src:url(/fonts/fl-range.woff2);unicode-range:U+4E00-9FFF}"
    "@font-face{font-family:FlComp;src:url(/fonts/fl-comp-latin.woff2);unicode-range:U+0-7F}"
    "@font-face{font-family:FlComp;src:url(/fonts/fl-comp-cjk.woff2);unicode-range:U+4E00-9FFF}"
    "@font-face{font-family:FlInst;src:url(/fonts/fl-inst.woff2)}"
    "@font-face{font-family:FlFmt;src:url(/fonts/fl-zebra.zeb) format(\"zebra\"),"
    "url(/fonts/fl-fmt.ttf) format(\"truetype\")}"
    "@font-face{font-family:FlPaint;src:url(/fonts/fl-paint.woff2)}"
    "@font-face{font-family:FlSpace;src:url(/fonts/fl-space.woff2)}"
    "</style><link rel=stylesheet href='/sheet/fontload.css'></head><body>"
    "<p id=flu style=\"font-family:FlUsed,sans-serif\">hello</p><p style=\"font-family:flused\">again</p>"
    "<p id=fln style=\"font-family:FlNone;display:none\">hello</p>"
    "<p id=flr style=\"font-family:FlRange\">hello</p>"
    "<p id=flc style=\"font-family:FlComp\">hello</p>"
    "<p id=fli style=\"font-family:'Liberation Sans',FlInst\">hello</p>"
    "<p id=flf style=\"font-family:FlFmt\">hello</p>"
    "<p id=fle style=\"font-family:FlExt\">hello</p>"
    "<p id=flp style=\"font-family:FlPaint\">painted</p>"
    "<p id=fls style=\"font-family:FlSpace\"> </p>"
    "<script>"
    "var flH = ['flu','flr','flc','fli','flf','fle','fls'].map(function(i){"
      "return document.getElementById(i).offsetHeight > 0 ? 1 : 0; }).join('');"
    "fetch('/api/fontload?h=' + flH);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
