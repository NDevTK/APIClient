/* THE COLLECTION-VISIBILITY DOCUMENT: elements whose ids are the names of HTMLCollection.prototype's members. Web
   IDL §3.9.7's named property visibility algorithm makes a supported property name invisible when a prototype has
   an own property of that name and the interface lacks [LegacyOverrideBuiltIns], which HTMLCollection does, so
   `length` and `item` read the members and only `vsZ` reads an element. namedItem() is not the named getter and
   still answers the element named `item`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COLLVISIBLE_H
#define ENGINE_HOST_FIXTURE_COLLVISIBLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COLLVISIBLE[] =
    "<!doctype html><html><body>"
    "<div id=vsBox><p id=length></p><p id=item></p><p id=vsZ></p></div>"
    "<script>"
    "var vsC = document.getElementById('vsBox').children;"
    "fetch('/api/collvisible?l=' + typeof vsC.length + '&i=' + typeof vsC.item +"
    "      '&z=' + (vsC.vsZ === document.getElementById('vsZ') ? 1 : 0) +"
    "      '&n=' + (vsC.namedItem('item') === document.getElementById('item') ? 1 : 0) +"
    "      '&o=' + (Object.getOwnPropertyDescriptor(vsC, 'length') === undefined ? 1 : 0));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
