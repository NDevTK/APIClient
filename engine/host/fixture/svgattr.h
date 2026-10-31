/* THE SVG PRESENTATION ATTRIBUTE DOCUMENT: SVG 2 §6.6 "Presentation attributes"' `width` and `height` on an
   `svg`, parsed by §4.2 "Attribute syntax" into the author presentational hint origin. One of test_forced.c's
   exploring documents (its TF_DOCS table). Every `svg` here is block-level with both sizes given, so its used
   size is the declared one whatever its replaced classification. #g1 is the shape a real page's icon has — a
   grid item under `align-items: center` with `width="1em" height="1em"` and a `viewBox` — and #g6 is centred
   in an 80px row. The rows cover a unitless number (px), a length, a percentage, a comment and whitespace
   around a token, a math function, an exponent, and an author `style` winning over the attribute. Chrome
   155.0.8059.84 and Firefox 158.0.1 give every value expected. Cross-engine note, left out: Firefox sizes an
   attribute that does not parse (`width="foo"`, `width="-5"`) as `auto`, which is what SVG 2 says such an
   attribute is taken as, and Chrome does not. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_SVGATTR_H
#define ENGINE_HOST_FIXTURE_SVGATTR_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SVGATTR[] =
    "<!doctype html><html>"
    "<body style=\"margin:0;font-size:15px\">"
    "<div style=\"width:200px\">"
    "<svg id=s1 style=\"display:block\" width=\"100\" height=\"50\"></svg>"
    "<svg id=s2 style=\"display:block\" width=\"1em\" height=\"2em\"></svg>"
    "<svg id=s3 style=\"display:block\" width=\"50%\" height=\"10\"></svg>"
    "<svg id=s4 style=\"display:block\" width=\"10px/**/\" height=\" 7 \"></svg>"
    "<svg id=s5 style=\"display:block\" width=\"calc(10px + 5px)\" height=\"1e1\"></svg>"
    "<svg id=s6 style=\"display:block;width:30px\" width=\"40\" height=\"10\"></svg>"
    "</div>"
    "<div style=\"display:grid;align-items:center;grid-template-columns:100px;width:100px\">"
    "<svg id=g1 width=\"1em\" height=\"1em\" viewBox=\"0 0 24 24\"></svg></div>"
    "<div id=p6 style=\"display:grid;grid-template-columns:100px;grid-template-rows:80px;align-items:center;"
    "width:100px\"><svg id=g6 width=\"30\" height=\"20\"></svg></div>"
    "<script>"
    "function E(i){return document.getElementById(i);}"
    "function Z(i){var r=E(i).getBoundingClientRect();return r.width+'x'+r.height;}"
    "var q='s1='+Z('s1')+'&s2='+Z('s2')+'&s3='+Z('s3')+'&s4='+Z('s4')+'&s5='+Z('s5')+'&s6='+Z('s6')"
    "+'&g1='+Z('g1')+'&g6='+Z('g6')"
    "+'&g6t='+(E('g6').getBoundingClientRect().top-E('p6').getBoundingClientRect().top)"
    "+'&c1='+getComputedStyle(E('s1')).width+'&st='+(E('s1').style.width===''?'ok':'wrong');"
    "fetch('/api/svgattr?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
