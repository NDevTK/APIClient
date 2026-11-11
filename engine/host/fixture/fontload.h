/* The font load document. css-fonts-4 §5.2 "Matching font styles" loads an `@font-face` face when a character
   being rendered is in its unicode-range and the face is not loaded; css-fonts-4 §4.8.1 "Font loading
   guidelines" forbids downloading a face no text uses. Each family below is one case, with its own address:
   used by two elements (one naming it in another case), never named, named only under `display: none`, named
   for text outside its unicode-range, a composite of two ranges, named after an installed family that has
   the text, a src list whose first format is unknown, a rule in a linked sheet with a relative address
   (test_forced.c's FONTLOAD_SHEET), text no script measures, and white space only.
   The families after FlSpace are css-fonts-4 §5.2's narrowing, one per case: two weights under normal
   text, two weights under weight 600, a normal and an italic face under italic text, two rules of one style,
   a weight range, a condensed and a normal width under condensed text, and a face with no weight descriptor
   beside a bold one under bold text.
   Chrome 153.0.8010.12 loading this same markup requests fl-used, fl-comp-latin, fl-fmt.ttf, fl-ext,
   fl-paint, fl-w-400, fl-b-700, fl-i-italic, fl-o-second, fl-v-range, fl-s-condensed and fl-a-700, each
   once, and none of the others.
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
    "@font-face{font-family:FlW;src:url(/fonts/fl-w-400.woff2);font-weight:400}"
    "@font-face{font-family:FlW;src:url(/fonts/fl-w-700.woff2);font-weight:700}"
    "@font-face{font-family:FlB;src:url(/fonts/fl-b-400.woff2);font-weight:400}"
    "@font-face{font-family:FlB;src:url(/fonts/fl-b-700.woff2);font-weight:bold}"
    "@font-face{font-family:FlI;src:url(/fonts/fl-i-normal.woff2);font-style:normal}"
    "@font-face{font-family:FlI;src:url(/fonts/fl-i-italic.woff2);font-style:italic}"
    "@font-face{font-family:FlO;src:url(/fonts/fl-o-first.woff2)}"
    "@font-face{font-family:FlO;src:url(/fonts/fl-o-second.woff2)}"
    "@font-face{font-family:FlV;src:url(/fonts/fl-v-range.woff2);font-weight:100 900}"
    "@font-face{font-family:FlS;src:url(/fonts/fl-s-normal.woff2);font-stretch:normal}"
    "@font-face{font-family:FlS;src:url(/fonts/fl-s-condensed.woff2);font-stretch:75%}"
    "@font-face{font-family:FlA;src:url(/fonts/fl-a-auto.woff2)}"
    "@font-face{font-family:FlA;src:url(/fonts/fl-a-700.woff2);font-weight:700}"
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
    "<p id=flw style=\"font-family:FlW\">hello</p>"
    "<p id=flb style=\"font-family:FlB;font-weight:600\">hello</p>"
    "<p id=flit style=\"font-family:FlI;font-style:italic\">hello</p>"
    "<p id=flo style=\"font-family:FlO\">hello</p>"
    "<p id=flv style=\"font-family:FlV;font-weight:700\">hello</p>"
    "<p id=flst style=\"font-family:FlS;font-stretch:condensed\">hello</p>"
    "<p id=fla style=\"font-family:FlA;font-weight:bold\">hello</p>"
    "<script>"
    "var flH = ['flu','flr','flc','fli','flf','fle','fls','flw','flb','flit','flo','flv','flst','fla'].map(function(i){"
      "return document.getElementById(i).offsetHeight > 0 ? 1 : 0; }).join('');"
    "fetch('/api/fontload?h=' + flH);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
