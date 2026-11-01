/* THE ISARRAY-TYPE DOCUMENT: ECMAScript §7.2.2 IsArray ( argument ) over unknowns. URLSearchParams get()'s found
   arm is a String in every world (URL §6.2), so IsArray's step 1 answers false and there is no arm where it is an
   Array (ia-a1). A JSON.parse result over an unknown text may be an Array, so IsArray over it keeps both arms
   (ia-a2 and ia-b2). A reply member whose example is an Object, on the arm where `typeof` proved it a Boolean,
   holds no example there, so IsArray answers false from its type set (ia-b3, never ia-a3), and its own member
   holds none either (ia-m3 never carries the Object's `1`). One of test_forced.c's exploring documents (TF_DOCS); it ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ISARRAYTYPE_H
#define ENGINE_HOST_FIXTURE_ISARRAYTYPE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ISARRAYTYPE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var iaV = new URLSearchParams(location.search).get('v');"
    "if (iaV) { if (Array.isArray(iaV)) fetch('/api/ia-a1'); else fetch('/api/ia-b1'); }"
    "try {"
    " var iaJ = JSON.parse(location.hash.slice(1));"
    " if (Array.isArray(iaJ)) fetch('/api/ia-a2'); else fetch('/api/ia-b2');"
    "} catch (e) { fetch('/api/ia-threw'); }"
    "(async function () {"
    "  var iaRec = await (await fetch('/api/iarec')).json();"
    "  if (typeof iaRec.f === 'boolean') { if (Array.isArray(iaRec.f)) fetch('/api/ia-a3'); else fetch('/api/ia-b3');"
    "    fetch('/api/ia-m3?x=' + iaRec.f.a); }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
