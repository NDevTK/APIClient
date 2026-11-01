/* THE DOCUMENT NAMED-PROPERTIES DOCUMENT: HTML §3.1.7 "DOM tree accessors"' named elements, read through the
   Document's [LegacyOverrideBuiltIns] named getter, each answer sent as a concrete query parameter.
   `<img name=getElementById>` shadows the Document member of that name; `<div name=querySelector>` does not, a
   div being no named element. Two elements sharing a name answer a two-member HTMLCollection; an object is named
   by its id, an img by its id only beside a non-empty name; `location`, unforgeable, is a non-configurable own
   property of the instance and absent from the prototype, so it is never shadowed; a named property is not deleted
   and is an own key. Both named getters answer [[GetOwnProperty]] with Web IDL §3.9.1's descriptor (the Window's
   unenumerable and writable), and a bare global name reaches the Window's. A [[Set]] on the Document ignores its
   named properties (Web IDL §3.9.2), so `document.title = v` runs the setter an img named title shadows for a
   read; one on the Window takes WindowProperties' writable descriptor and defines on the Window.
   A form's named getter (HTML §4.10.3, [LegacyOverrideBuiltIns]) answers `<input name=action>` over the
   `action` member, an `img` it owns and a control whose form attribute names it, never an Image Button or a
   div, a renamed control by its old name from the past names map, with an unenumerable, unwritable descriptor.
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
    "<img name='location' src='/dn4.png'><img name='title' src='/dn5.png'><img id='wnSetMe'>"
    "<form id='fmA' action='/api/fmact'><input name='action'><input type='image' name='fmIb'><div id='fmDiv'></div>"
    "<img name='fmPic' src='/dn6.png'><input id='fmOld' name='fmOld'></form><input name='fmExt' form='fmA'>"
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
    "document.title = 'dnT';"
    "var dnSet = Object.getOwnPropertyDescriptor(Document.prototype, 'title').get.call(document) === 'dnT' &&"
    " document.title instanceof HTMLImageElement;"
    "window.wnSetMe = 5;"
    "var wnSet = wnSetMe === 5 && Object.getOwnPropertyDescriptor(window, 'wnSetMe').value === 5;"
    "fetch('/api/docnamed?ov=' + dnOv + '&qs=' + dnQs + '&coll=' + dnColl + '&obj=' + dnObj + '&bare=' + dnBare +"
    "      '&img=' + dnImg + '&loc=' + dnLoc + '&locown=' + dnLocOwn + '&del=' + dnDel + '&key=' + dnKey +"
    "      '&desc=' + dnDesc + '&wdesc=' + wnDesc + '&wglobal=' + wnGlobal + '&dset=' + dnSet + '&wset=' + wnSet);"
    "</script>"
    "<script>"
    "var fmA = document.querySelector('#fmA');"
    "var fmOv = fmA.action instanceof HTMLInputElement;"
    "var fmIb = typeof fmA.fmIb, fmDiv = typeof fmA.fmDiv, fmInv = typeof fmA.fmNoSuch;"
    "var fmPic = fmA.fmPic instanceof HTMLImageElement, fmExt = fmA.fmExt instanceof HTMLInputElement;"
    "var fmOldEl = fmA.fmOld; fmOldEl.name = 'fmNew'; fmOldEl.id = 'fmNew2';"
    "var fmPast = fmA.fmOld === fmOldEl;"
    "var fmD = Object.getOwnPropertyDescriptor(fmA, 'action');"
    "var fmDesc = !!fmD && !fmD.enumerable && fmD.configurable && !fmD.writable && fmD.value instanceof HTMLInputElement;"
    "fetch('/api/formnamed?ov=' + fmOv + '&ib=' + fmIb + '&div=' + fmDiv + '&pic=' + fmPic + '&ext=' + fmExt +"
    "      '&past=' + fmPast + '&desc=' + fmDesc + '&inv=' + fmInv);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
