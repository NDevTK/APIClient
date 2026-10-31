/* THE DOCUMENT NAMED-PROPERTIES DOCUMENT: HTML §3.1.7 "DOM tree accessors"' named elements, read through the
   Document's [LegacyOverrideBuiltIns] named getter, each answer sent as a concrete query parameter.
   `<img name=getElementById>` shadows the Document member of that name; `<div name=querySelector>` does not, a
   div being no named element. Two elements sharing a name answer a two-member HTMLCollection; an object is named
   by its id, an img by its id only beside a non-empty name; `location`, unforgeable, is a non-configurable own
   property of the instance and absent from the prototype, so it is never shadowed; a named property is not deleted
   and is an own key. Both named getters answer [[GetOwnProperty]] with Web IDL §3.9.1's descriptor (the Window's
   unenumerable and writable), and a bare global name reaches the Window's.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DOCNAMED_H
#define ENGINE_HOST_FIXTURE_DOCNAMED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DOCNAMED[] =
    "<!doctype html><html><body>"
    "<img name='getElementById' src='/dn.png'><div name='querySelector'></div>"
    "<form name='dnDup'></form><img name='dnDup' src='/dn2.png'>"
    "<object id='dnObj'></object><img id='dnBare'><img id='dnImg' name='dnImgName' src='/dn3.png'>"
    "<img name='location' src='/dn4.png'>"
    "<script>"
    "var dnOv = document.getElementById instanceof HTMLImageElement;"
    "var dnQs = typeof document.querySelector;"
    "var dnDup = document.dnDup;"
    "var dnColl = dnDup instanceof HTMLCollection ? dnDup.length : -1;"
    "var dnObj = document.dnObj instanceof HTMLObjectElement;"
    "var dnBare = typeof document.dnBare;"
    "var dnImg = document.dnImg instanceof HTMLImageElement;"
    "var dnLoc = typeof document.location.href;"
    "var dnLocD = Object.getOwnPropertyDescriptor(document, 'location');"
    "var dnLocOwn = !!dnLocD && !dnLocD.configurable && !('location' in Document.prototype);"
    "var dnDel = delete document.dnDup;"
    "var dnKey = Object.keys(document).indexOf('dnDup') >= 0;"
    "var dnD = Object.getOwnPropertyDescriptor(document, 'dnObj');"
    "var dnDesc = !!dnD && dnD.enumerable && dnD.configurable && !dnD.writable && dnD.value instanceof HTMLObjectElement;"
    "var wnD = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Window.prototype), 'dnObj');"
    "var wnDesc = !!wnD && !wnD.enumerable && wnD.configurable && wnD.writable && wnD.value instanceof HTMLObjectElement;"
    "var wnGlobal = typeof getElementById === 'object' && typeof dnNoSuchName === 'undefined';"
    "fetch('/api/docnamed?ov=' + dnOv + '&qs=' + dnQs + '&coll=' + dnColl + '&obj=' + dnObj + '&bare=' + dnBare +"
    "      '&img=' + dnImg + '&loc=' + dnLoc + '&locown=' + dnLocOwn + '&del=' + dnDel + '&key=' + dnKey +"
    "      '&desc=' + dnDesc + '&wdesc=' + wnDesc + '&wglobal=' + wnGlobal);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
