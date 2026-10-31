/* THE CALCULATION TREE DOCUMENT: css-values-4 §10.10 "Internal Representation"'s tree kept as the computed
   value where §10.10.1 "Simplification" stops, and simplified at used-value time (§10.11 "Computed Value")
   against each containing block. One of test_forced.c's exploring documents (its TF_DOCS table). #a and #b
   carry the `max-width` a real page's sidebar does, `min(320px,100% - 320px - calc(200px + 48px))`, in a
   1000px and a 600px parent, so each argument of the min() wins once; #d and #e do the same for
   `min(50%, 200px)` as a `width`; #c is `calc(50% * 2 - 10px)`, which §10.10.1 reduces to a Sum of two terms.
   The specified and computed serializations (§10.13 "Serialization") are compared in the page against the
   strings Chrome 155.0.8059.84 and Firefox 158.0.1 both give, and so is each specified `width` in SPEC: a
   relative unit keeps its own term, absolute units fold into px, terms sort by unit, a number distributes over
   a Sum, and a non-finite term is written as a keyword times one unit. Cross-engine notes, left out of SPEC
   and answered here as the editors' drafts read: Chrome keeps a function of one relative unit
   (`min(1em, 2em)`) where Firefox and §10.10.1's text compute `calc(1em)`; Chrome writes `calc(-0px + 1em)`
   as `calc(1em + 0px)` and Firefox as `calc(1em - 0px)`; both browsers round a number to six significant
   digits (`calc(1Q + 1MM)` is `calc(4.72441px)`) where CSSOM §6.7.2 rounds "to not produce more than 6
   decimals". It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
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
    "<div id=y></div>"
    "<script>"
    "var SPEC=[['calc(1em + 2px + 3em)','calc(4em + 2px)'],['calc(10px + 1em)','calc(1em + 10px)'],"
    "['calc(2 * 3em)','calc(6em)'],['calc(1in + 1px)','calc(97px)'],['calc(50% + 0px)','calc(50% + 0px)'],"
    "['calc(1px + 2px)','calc(3px)'],['calc(100% - 1em*2)','calc(100% - 2em)'],['calc(1vw / 2)','calc(0.5vw)'],"
    "['max(10px, 1em + 1px)','max(10px, 1em + 1px)'],['calc(50%)','calc(50%)'],"
    "['calc(-10px + 50%)','calc(50% - 10px)'],['min(10px, 20px)','calc(10px)'],"
    "['clamp(10px, 50%, 1em)','clamp(10px, 50%, 1em)'],['calc(1rem + 1REM + 1Px)','calc(1px + 2rem)'],"
    "['calc(0.5em + 1vh - 3%)','calc(-3% + 0.5em + 1vh)'],['calc(1px + (2px - 1em))','calc(-1em + 3px)'],"
    "['calc(1px * -infinity + 50%)','calc(50% - infinity * 1px)'],['calc(1px / 0)','calc(infinity * 1px)'],"
    "['calc(NaN * 1px)','calc(NaN * 1px)'],['max(10px, 1em * 2, 3em)','max(10px, 2em, 3em)'],"
    "['calc(2 * (1em + 50%))','calc(100% + 2em)'],['calc(-1 * (1em + 1px))','calc(-1em - 1px)'],"
    "['calc(1em - (50% - 1px))','calc(-50% + 1em + 1px)'],['round(10.5px, 1em)','round(10.5px, 1em)'],"
    "['calc(1PC + 3PT)','calc(20px)'],['calc(5vw - (2em * 2))','calc(-4em + 5vw)'],"
    "['min(1em, 1px)','min(1em, 1px)']];"
    "function E(i){return document.getElementById(i);}"
    "function C(i,p){return getComputedStyle(E(i))[p];}"
    "function Q(v,want){return v===want?'ok':'wrong';}"
    "var q='aw='+E('a').offsetWidth+'&bw='+E('b').offsetWidth+'&cw='+E('c').offsetWidth"
    "+'&dw='+E('d').offsetWidth+'&ew='+E('e').offsetWidth"
    "+'&as='+Q(E('a').style.maxWidth,'min(320px, 100% - 568px)')"
    "+'&ac='+Q(C('a','maxWidth'),'min(320px, 100% - 568px)')"
    "+'&cs='+Q(E('c').style.width,'calc(100% - 10px)')+'&cc='+Q(C('c','width'),'290px')"
    "+'&ds='+Q(E('d').style.width,'min(50%, 200px)')+'&dc='+Q(C('d','width'),'150px')"
    "+'&ct='+Q(E('c').style.cssText,'height: 1px; width: calc(100% - 10px);');"
    "var y=E('y'),sp=0,bad='';"
    "for(var i=0;i<SPEC.length;i++){y.style.width='';y.style.width=SPEC[i][0];"
    "if(y.style.width===SPEC[i][1])sp++;else if(!bad)bad=SPEC[i][0]+'|'+y.style.width;}"
    "E('c').style.color='red';"
    "q+='&sp='+sp+'&spn='+SPEC.length+'&ca='+Q(E('c').getAttribute('style'),"
    "'height: 1px; width: calc(100% - 10px); color: red;')+'&bad='+encodeURIComponent(bad)"
    "+'&raw='+encodeURIComponent(E('a').style.maxWidth+'|'+C('a','maxWidth')+'|'+E('c').style.width"
    "+'|'+E('d').style.width);"
    "fetch('/api/calctree?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
