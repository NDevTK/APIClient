/* THE CALCULATION TREE DOCUMENT: css-values-4 §10.10 "Internal Representation"'s tree kept as the computed
   value where §10.10.1 "Simplification" stops, and simplified at used-value time (§10.11 "Computed Value")
   against each containing block. One of test_forced.c's exploring documents (its TF_DOCS table). #a and #b
   carry the `max-width` a real page's sidebar does, `min(320px,100% - 320px - calc(200px + 48px))`, in a
   1000px and a 600px parent, so each argument of the min() wins once; #d and #e do the same for
   `min(50%, 200px)` as a `width`; #c is `calc(50% * 2 - 10px)`, which §10.10.1 reduces to a Sum of two terms.
   The specified and computed serializations (§10.13 "Serialization") are compared in the page against the
   strings Chrome 155.0.8059.84 and Firefox 158.0.1 both give. It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CALCTREE_H
#define ENGINE_HOST_FIXTURE_CALCTREE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CALCTREE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div style=\"width:1000px\"><div id=a style=\"height:1px;"
    "max-width:min(320px,100% - 320px - calc(200px + 48px))\"></div></div>"
    "<div style=\"width:600px\"><div id=b style=\"height:1px;"
    "max-width:min(320px,100% - 320px - calc(200px + 48px))\"></div></div>"
    "<div style=\"width:300px\"><div id=c style=\"height:1px;width:calc(50% * 2 - 10px)\"></div></div>"
    "<div style=\"width:300px\"><div id=d style=\"height:1px;width:min(50%, 200px)\"></div></div>"
    "<div style=\"width:600px\"><div id=e style=\"height:1px;width:min(50%, 200px)\"></div></div>"
    "<script>"
    "function E(i){return document.getElementById(i);}"
    "function C(i,p){return getComputedStyle(E(i))[p];}"
    "function Q(v,want){return v===want?'ok':'wrong';}"
    "var q='aw='+E('a').offsetWidth+'&bw='+E('b').offsetWidth+'&cw='+E('c').offsetWidth"
    "+'&dw='+E('d').offsetWidth+'&ew='+E('e').offsetWidth"
    "+'&as='+Q(E('a').style.maxWidth,'min(320px, 100% - 568px)')"
    "+'&ac='+Q(C('a','maxWidth'),'min(320px, 100% - 568px)')"
    "+'&cs='+Q(E('c').style.width,'calc(100% - 10px)')+'&cc='+Q(C('c','width'),'290px')"
    "+'&ds='+Q(E('d').style.width,'min(50%, 200px)')+'&dc='+Q(C('d','width'),'150px')"
    "+'&raw='+encodeURIComponent(E('a').style.maxWidth+'|'+C('a','maxWidth')+'|'+E('c').style.width"
    "+'|'+E('d').style.width);"
    "fetch('/api/calctree?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
