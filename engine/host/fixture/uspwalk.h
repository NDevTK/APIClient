/* The URLSearchParams walk document: URL §6.2 "URLSearchParams class"' delete(name), set(name, value),
   getAll(name) and sort() over a name that is unknown input, each from its own attacker source so no
   method's world is decided by another's pin. "A tuple whose name is name" is an equality, so a list
   `a=1&b=2&a=3` holds a world per stored name and one where the name is none of them; sort() orders by
   "code unit less than", so a tuple appended under the unknown name sorts before `m` in one world and after
   it in the other. Each witness carries the list's values in order, which are concrete in every world:
   delete(k) leaves `2` where k is `a`, set(k, 'x') gives `x-2`, getAll(k) gives `1-3`, and sort() gives `2-1`
   and `1-2`. Values read from the spec. One of test_forced.c's TF_DOCS; it ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_USPWALK_H
#define ENGINE_HOST_FIXTURE_USPWALK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_USPWALK[] =
    "<!doctype html><html><body>"
    "<script>"
    "function uwV(p){var s=[];p.forEach(function(v){s.push(v);});return s.join('-');}"
    "var uwD=new URLSearchParams('a=1&b=2&a=3');uwD.delete(location.hash.slice(1));"
    "fetch('/api/uspwd?v='+uwV(uwD));"
    "</script>"
    "<script>"
    "var uwS=new URLSearchParams('a=1&b=2&a=3');uwS.set(window.name,'x');"
    "fetch('/api/uspws?v='+uwV(uwS));"
    "</script>"
    "<script>"
    "var uwG=new URLSearchParams('a=1&b=2&a=3');"
    "fetch('/api/uspwg?v='+uwG.getAll(location.search.slice(1)).join('-'));"
    "</script>"
    "<script>"
    "var uwO=new URLSearchParams('m=1');uwO.append(location.hash.slice(1),'2');uwO.sort();"
    "fetch('/api/uspwo?v='+uwV(uwO));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
