/* The container units document: CSS Conditional 5 §5.1 "Creating Query Containers: the container-type property",
   §5.2's `container-name` through §5.3's `container` shorthand, and §7 "Container Relative Lengths: the cqw,
   cqh, cqi, cqb, cqmin, cqmax units". One of test_forced.c's exploring documents (its TF_DOCS table). It reads
   the six units under a `size` container, under no container (§7's small viewport fallback, 1280 x 720 here),
   and under an `inline-size` container read on the block axis, which that container does not accept; the
   nearest-container walk past an `inline-size` container to a `size` one and past an ineligible `display:
   inline` one (the expectations of WPT css/css-conditional/container-queries/container-units-selection.html and
   container-units-ineligible-container.html); the size containment `container-type: size` applies; the
   computed values; and §5.4's `@container` rules: a true and a false size query, a named one over `inline-size`,
   one on an axis its container does not accept, one whose `em` is the container's own 20px font size, and one
   over an element with no container. It ends with TF_CANVAS_INK, which test_forced.c defines before including
   it. */
#ifndef ENGINE_HOST_FIXTURE_CQUNITS_H
#define ENGINE_HOST_FIXTURE_CQUNITS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CQUNITS[] =
    "<!doctype html><html><head><style>"
    "#qa,#qb,#qn,#qh,#qe,#qo{width:5px}"
    "@container (width > 200px){#qa{width:11px}}"
    "@container (width > 400px){#qb{width:12px}}"
    "@container card (inline-size >= 300px){#qn{width:13px}}"
    "@container (height > 0px){#qh{width:14px}}"
    "@container (width > 16em){#qe{width:16px}}"
    "@container (width > 0px){#qo{width:15px}}"
    "</style></head>"
    "<body>"
    "<div id=qc style=\"container:card / inline-size;width:300px;font-size:20px\">"
    "<div id=qa></div><div id=qb></div><div id=qn></div><div id=qh></div><div id=qe></div></div>"
    "<div id=qo></div>"
    "<div id=sc style=\"container-type:size;width:300px;height:200px\">"
    "<div id=st style=\"width:10cqw;height:10cqh;margin-left:10cqmin;margin-top:10cqmax\"></div></div>"
    "<div id=nc style=\"width:10cqw;height:10cqh\"></div>"
    "<div id=ic style=\"container-type:inline-size;width:400px\">"
    "<div id=it style=\"width:10cqi;height:10cqb\"></div></div>"
    "<div style=\"container-type:size;width:500px;height:400px\">"
    "<div style=\"container-type:inline-size;width:100px\"><div id=nt style=\"width:10cqw;height:10cqh\"></div>"
    "</div></div>"
    "<div style=\"container-type:size;width:300px;height:250px\">"
    "<span style=\"container-type:size\"><span id=ie style=\"display:inline-block;width:10cqw;height:10cqh\"></span>"
    "</span></div>"
    "<div id=sz style=\"container-type:size\"><div style=\"height:50px\"></div></div>"
    "<div id=cn style=\"container:card main / inline-size\"></div>"
    "<div id=cs style=\"container-type:scroll-state size\"></div>"
    "<script>"
    "function R(id){return document.getElementById(id).getBoundingClientRect();}"
    "function S(id,p){return getComputedStyle(document.getElementById(id)).getPropertyValue(p).replace(/ /g,'_');}"
    "var q='stw='+R('st').width+'&sth='+R('st').height+'&stml='+S('st','margin-left')+'&stmt='+S('st','margin-top')"
    "+'&ncw='+R('nc').width+'&nch='+R('nc').height"
    "+'&itw='+R('it').width+'&ith='+R('it').height+'&ich='+R('ic').height"
    "+'&ntw='+R('nt').width+'&nth='+R('nt').height+'&iew='+R('ie').width+'&ieh='+R('ie').height"
    "+'&szh='+R('sz').height+'&cnn='+S('cn','container-name')+'&cnt='+S('cn','container-type')"
    "+'&cst='+S('cs','container-type')+'&sct='+S('sc','container-type')+'&scn='+S('sc','container-name')"
    "+'&qa='+R('qa').width+'&qb='+R('qb').width+'&qn='+R('qn').width+'&qh='+R('qh').width+'&qe='+R('qe').width"
    "+'&qo='+R('qo').width;"
    "fetch('/api/cqunits?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
