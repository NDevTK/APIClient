/* THE CONTAINMENT DOCUMENT: css-contain-2 §2 "Strong Containment: the contain property" and its §3 types. One of
   test_forced.c's exploring documents (its TF_DOCS table). It reads CSSOM VIEW §7's offsetParent over the
   positioning containing blocks §3.3 and §3.5 establish (a `fixed` box's is the paint containment ancestor, past
   a relative one); an absolutely positioned and a fixed box placed and sized against a containment box's padding
   box; §3.1's sizing as if empty for a block, an `inline-block`, an absolutely positioned box and a `canvas`,
   and §3.2's for an `inline-block`, whose height still follows its content; an empty size containment box whose
   margins still collapse through and one holding text whose margins do not; a paint containment box's
   independent formatting context (a child's margin stays inside it); §3.3's missing baseline, which puts a
   contained `inline-block` on the line like an empty one; scrollWidth over a contained box's overflow, against
   an uncontained control; and the computed value's serialization, `strict` and `content` included. Chrome
   155.0.8059.84 gives every number the rows expect for this markup. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CONTAIN_H
#define ENGINE_HOST_FIXTURE_CONTAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CONTAIN[] =
    "<!doctype html><html>"
    "<body>"
    "<div id=cp style=\"contain:paint\"><div id=rel style=\"position:relative\">"
    "<div id=f1 style=\"position:fixed\"></div><div id=s1></div></div></div>"
    "<div id=cl2 style=\"contain:layout\"><div><div id=s2></div></div></div>"
    "<div><div id=f3 style=\"position:fixed\"></div></div>"
    "<div id=cl style=\"contain:layout;margin-left:50px;width:200px;padding:5px;height:20px\">"
    "<div id=ab style=\"position:absolute;left:0;top:0;width:50%;height:3px\"></div></div>"
    "<div id=cpf style=\"contain:paint;margin-left:30px;width:100px;height:40px\">"
    "<div id=fx style=\"position:fixed;left:3px;top:4px;width:50%;height:50%\"></div></div>"
    "<div id=cs style=\"contain:size\"><div style=\"height:50px\"></div></div>"
    "<div><span id=csi style=\"display:inline-block;contain:size;padding:2px\">hello world</span></div>"
    "<div style=\"position:relative;height:30px\"><div style=\"position:absolute;contain:size\" id=csa>"
    "<span style=\"display:inline-block;width:40px;height:5px\"></span></div></div>"
    "<div><canvas id=cc style=\"contain:size\"></canvas></div>"
    "<div><span style=\"display:inline-block;contain:inline-size\" id=cis><div style=\"height:20px;width:80px\">"
    "</div></span></div>"
    "<div id=m0 style=\"height:1px\"></div><div style=\"margin:10px 0;contain:size\"></div>"
    "<div id=m1 style=\"height:1px\"></div>"
    "<div style=\"margin:10px 0;contain:size\"><span>x</span></div><div id=m2 style=\"height:1px\"></div>"
    "<div id=bp style=\"contain:paint\"><div style=\"margin-top:20px;height:5px\" id=bpc></div></div>"
    "<div><span id=e1 style=\"display:inline-block;height:30px;width:5px\"></span>"
    "<span id=e2 style=\"display:inline-block;contain:layout;height:30px\">"
    "<span style=\"display:inline-block;width:5px;height:10px\"></span></span>"
    "<span id=e3 style=\"display:inline-block;height:30px\">"
    "<span style=\"display:inline-block;width:5px;height:10px\"></span></span></div>"
    "<div id=sw style=\"overflow:hidden;width:100px;height:50px\">"
    "<div style=\"contain:paint;width:50px;height:10px\">"
    "<span style=\"display:inline-block;width:300px;height:5px\"></span></div></div>"
    "<div id=sw2 style=\"overflow:hidden;width:100px;height:50px\">"
    "<div style=\"contain:layout;width:50px;height:10px\">"
    "<span style=\"display:inline-block;width:300px;height:5px\"></span></div></div>"
    "<div id=sw3 style=\"overflow:hidden;width:100px;height:50px\"><div style=\"width:50px;height:10px\">"
    "<span style=\"display:inline-block;width:300px;height:5px\"></span></div></div>"
    "<div id=k1 style=\"contain:strict\"></div><div id=k2 style=\"contain:content\"></div>"
    "<div id=k3 style=\"contain:paint layout\"></div>"
    "<div id=k4 style=\"contain:size layout style paint\"></div><div id=k5 style=\"contain:inline-size\"></div>"
    "<div id=k6 style=\"contain:layout size\"></div>"
    "<div id=k7 style=\"contain:size inline-size\"></div><div id=k8 style=\"contain:style\"></div>"
    "<script>"
    "function R(id){return document.getElementById(id).getBoundingClientRect();}"
    "function E(id){return document.getElementById(id);}"
    "function P(id){var p=E(id).offsetParent;return p?(p.id||p.tagName):'null';}"
    "function C(id){return getComputedStyle(E(id)).contain.replace(/ /g,'_');}"
    "var q='f1p='+P('f1')+'&s1p='+P('s1')+'&s2p='+P('s2')+'&f3p='+P('f3')"
    "+'&abx='+(R('ab').left-R('cl').left)+'&aby='+(R('ab').top-R('cl').top)+'&abw='+R('ab').width"
    "+'&fxx='+(R('fx').left-R('cpf').left)+'&fxy='+(R('fx').top-R('cpf').top)+'&fxw='+R('fx').width"
    "+'&fxh='+R('fx').height"
    "+'&csh='+R('cs').height+'&csiw='+R('csi').width+'&csih='+R('csi').height+'&csaw='+R('csa').width"
    "+'&ccw='+R('cc').width+'&cch='+R('cc').height+'&cisw='+R('cis').width+'&cish='+R('cis').height"
    "+'&m1='+(R('m1').top-R('m0').bottom)+'&m2='+(R('m2').top-R('m1').bottom)"
    "+'&bpc='+(R('bpc').top-R('bp').top)"
    "+'&e2='+(R('e2').bottom-R('e1').bottom)+'&e3='+(R('e3').bottom-R('e1').bottom)"
    "+'&sw='+E('sw').scrollWidth+'&sw2='+E('sw2').scrollWidth+'&sw3='+E('sw3').scrollWidth"
    "+'&k1='+C('k1')+'&k2='+C('k2')+'&k3='+C('k3')+'&k4='+C('k4')+'&k5='+C('k5')+'&k6='+C('k6')+'&k7='+C('k7')"
    "+'&k8='+C('k8');"
    "fetch('/api/contain?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
