/* THE OUTERMOST SVG AS A REPLACED ELEMENT: SVG 2 §8.12 "Intrinsic sizing properties of SVG content" makes an
   outermost `svg` laid out by CSS a replaced element whose natural sizes are its `width`/`height` where those
   are lengths and whose ratio is theirs or its `viewBox`'s, sized by CSS 2.1 §10.3.2 and §10.6.2. One of
   test_forced.c's exploring documents (its TF_DOCS table). Each svg sits in a 200px block: #r1 has nothing
   (300 x 150); #r2 only a `viewBox` (the §10.3.2 suggestion: the block equation's 200, then 100 by the
   ratio), as an inline and as a block box (#r3); #r4 a width and a `viewBox` (height by the ratio); #r5 a
   percentage width, which is no natural size, and a `viewBox`; #r6 a height and a `viewBox`; #r7 an author
   width and no ratio (150 tall); #r8 a `viewBox` with a negative size, which SVG 2 §8.6 makes invalid. Chrome
   155.0.8059.84 and Firefox 158.0.1 give every value expected. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SVGREPLACED_H
#define ENGINE_HOST_FIXTURE_SVGREPLACED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define SVGREPLACED_ROW(svg) "<div style=\"width:200px\">" svg "</div>"

static const char HTML_SVGREPLACED[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    SVGREPLACED_ROW("<svg id=r1></svg>")
    SVGREPLACED_ROW("<svg id=r2 viewBox=\"0 0 24 12\"></svg>")
    SVGREPLACED_ROW("<svg id=r3 style=\"display:block\" viewBox=\"0,0,24,12\"></svg>")
    SVGREPLACED_ROW("<svg id=r4 width=\"40\" viewBox=\"0 0 24 12\"></svg>")
    SVGREPLACED_ROW("<svg id=r5 width=\"50%\" viewBox=\"0 0 24 12\"></svg>")
    SVGREPLACED_ROW("<svg id=r6 height=\"30\" viewBox=\"0 0 24 12\"></svg>")
    SVGREPLACED_ROW("<svg id=r7 style=\"width:30px\"></svg>")
    SVGREPLACED_ROW("<svg id=r8 viewBox=\"0 0 -24 12\"></svg>")
    "<script>"
    "function Z(i){var r=document.getElementById(i).getBoundingClientRect();return r.width+'x'+r.height;}"
    "var q='',i;for(i=1;i<=8;i++)q+=(i>1?'&':'')+'r'+i+'='+Z('r'+i);"
    "fetch('/api/svgreplaced?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef SVGREPLACED_ROW
#endif
