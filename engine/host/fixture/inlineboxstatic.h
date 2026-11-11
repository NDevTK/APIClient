/* The static position inside an inline box: an absolutely positioned `span` whose hypothetical box is
   inline-level (its specified `display` is `inline`), inside a `label` inline box that does not break, after a
   30px atomic inline and below a 20px block. CSS 2.2 §9.2.1.1's content order of #sc steps over the label, so
   the span's run is the label's: the one after the 20px block. css-position-3 §3.5.3 "Calculating the Static
   Position and the Static-Position Rectangle" puts the static position on that run's line, at its line-over
   side (20px down #sc) and at the inline position the line reached (30px in). #sd's hypothetical box is
   block-level, so §9.2.1.1 breaks the `b` around it: it sits below the anonymous block box holding the
   first fragment's one 20px line, 20px down #sb and at its left edge. The numbers are derived from those
   sections, not read off a browser. One of test_forced.c's exploring documents (its TF_DOCS table); it
   ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INLINEBOXSTATIC_H
#define ENGINE_HOST_FIXTURE_INLINEBOXSTATIC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INLINEBOXSTATIC[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=sc style=\"position:relative\"><div style=\"height:20px\"></div>"
    "<label><i style=\"display:inline-block;width:30px;height:10px\"></i>"
    "<span id=sv style=\"position:absolute;width:1px;height:1px;overflow:hidden\">x</span></label></div>"
    "<div id=sb style=\"position:relative;font:10px/20px monospace\"><b>aaaa"
    "<div id=sd style=\"position:absolute;width:5px;height:5px\"></div></b></div>"
    "<script>var v=document.getElementById('sv'),d=document.getElementById('sd');"
    "fetch('/api/inlineboxstatic?t='+v.offsetTop+'&l='+v.offsetLeft+'&bt='+d.offsetTop+'&bl='+d.offsetLeft);"
    "</script>"
    TF_CANVAS_INK "</body></html>";

#endif
