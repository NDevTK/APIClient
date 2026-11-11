/* The colour mixing document (`colormix`): CSS Color 5 §3 "Mixing Colors: the color-mix() Function" read back
   through getComputedStyle. `#a` two colours declared directly; `#v` openlibrary's shape, a `var()` at 8% with
   white; `#t` a mix with `transparent`, which §13.5's premultiplication keeps blue; `#l` two 30%s, whose
   leftover becomes alpha; `#c` a `currentcolor` argument; `#n` a nested mix; `#s` one item; `#r` srgb-linear;
   `#x` a 120% argument, which is invalid. Each colour is reported as rounded 0-255 components and an alpha,
   whichever of §16.2's `rgb()` and §16.5's `color(srgb ...)` forms it serializes in. One of test_forced.c's
   exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_COLORMIX_H
#define ENGINE_HOST_FIXTURE_COLORMIX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COLORMIX[] =
    "<!doctype html><html><body>"
    "<div id=a style=\"background-color:color-mix(in srgb, red, blue)\"></div>"
    "<div id=v style=\"--accent:#0376b8;background-color:color-mix(in srgb, var(--accent) 8%, #fff)\"></div>"
    "<div id=t style=\"background-color:color-mix(in srgb, #00f 40%, transparent)\"></div>"
    "<div id=l style=\"background-color:color-mix(in srgb, red 30%, blue 30%)\"></div>"
    "<div id=c style=\"color:#00ff00;background-color:color-mix(in srgb, currentcolor 75%, red)\"></div>"
    "<div id=n style=\"background-color:color-mix(in srgb, color-mix(in srgb, red, blue), white)\"></div>"
    "<div id=s style=\"background-color:color-mix(in srgb, red 50%)\"></div>"
    "<div id=r style=\"background-color:color-mix(in srgb-linear, red, blue)\"></div>"
    "<div id=x style=\"background-color:color-mix(in srgb, red 120%, blue)\"></div>"
    "<script>"
    "function cm(id) {"
    "  var s = getComputedStyle(document.getElementById(id)).backgroundColor, n = s.match(/[0-9.]+/g);"
    "  var k = /^color\\(srgb /.test(s) ? 255 : /^rgba?\\(/.test(s) ? 1 : 0;"
    "  if (!k || !n || n.length < 3) return 'raw:' + s.replace(/ /g, '_');"
    "  return [Math.round(n[0] * k), Math.round(n[1] * k), Math.round(n[2] * k),"
    "          n.length > 3 ? Math.round(n[3] * 100) / 100 : 1].join('_');"
    "}"
    "fetch('/api/colormix?a=' + cm('a') + '&v=' + cm('v') + '&t=' + cm('t') + '&l=' + cm('l') + '&c=' + cm('c') +"
    "      '&n=' + cm('n') + '&s=' + cm('s') + '&r=' + cm('r') + '&x=' + cm('x'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
