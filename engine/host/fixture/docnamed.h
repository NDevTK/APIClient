/* THE DOCUMENT NAMED-PROPERTIES DOCUMENT: HTML §3.1.7's named elements through the Document's
   [LegacyOverrideBuiltIns] named getter, HTML §4.10.3's through a form's, and WindowProperties', each answer sent
   as a query parameter. An img names a member it shadows, a div names nothing; a shared name is an HTMLCollection;
   an object is named by its id, an img by its id only beside a name; the unforgeable `location` is never shadowed;
   a named property is not deleted and is an own key, with Web IDL §3.9.1's descriptor (the Window's unenumerable
   and writable, a form's unenumerable and unwritable). A [[Set]] on a Document ignores its named properties (Web
   IDL §3.9.2) and one on the Window defines on the Window. A form answers the controls it owns, never an Image
   Button or a div, and a renamed control by its past name. The later scripts reach named access from operations
   C walks: a computed-key object write over each link, a Document above BigInt.prototype or %IteratorPrototype%
   (each read throws a TypeError), `in` and [[GetOwnProperty]] through a trapless Proxy, Object.assign from the
   Document, and `in` over the WindowProxy.
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
    "<script>"
    "var peK = ['wnPutEl', 'dnObj', 'dnPutEl', 'dnProtoPut'];"
    "window[peK[0]] = {a: 1};"
    "var peW = window.wnPutEl.a === 1 && Object.getOwnPropertyDescriptor(window, 'wnPutEl').value.a === 1;"
    "window[peK[1]] = {c: 3};"
    "var peWn = window.dnObj.c === 3 && Object.prototype.hasOwnProperty.call(window, 'dnObj');"
    "document[peK[2]] = {b: 2};"
    "var peD = document.dnPutEl.b === 2 && Object.prototype.hasOwnProperty.call(document, 'dnPutEl');"
    "var peO = Object.create(document); peO[peK[3]] = {f: 6};"
    "var peDc = peO.dnProtoPut.f === 6 && !('dnProtoPut' in document);"
    "fetch('/api/putelnamed?w=' + peW + '&wn=' + peWn + '&d=' + peD + '&dc=' + peDc);"
    "</script>"
    "<img name='toJSON' src='/dn7.png'><img name='next' src='/dn8.png'>"
    "<script>"
    "var cwJ, cwG, cwS;"
    "Object.setPrototypeOf(BigInt.prototype, document);"
    "try { cwJ = JSON.stringify({a: 1n}); } catch (e) { cwJ = e.constructor.name; }"
    "cwG = (1n).toJSON instanceof HTMLImageElement;"
    "Object.setPrototypeOf(BigInt.prototype, Object.prototype);"
    "var cwAip = Object.getPrototypeOf([][Symbol.iterator]()), cwNext = cwAip.next;"
    "var cwIp = Object.getPrototypeOf(cwAip);"
    "delete cwAip.next; Object.setPrototypeOf(cwIp, document);"
    "try { cwS = [...[1, 2]].length; } catch (e) { cwS = e.constructor.name; }"
    "Object.setPrototypeOf(cwIp, Object.prototype);"
    "Object.defineProperty(cwAip, 'next', {value: cwNext, writable: true, configurable: true});"
    "fetch('/api/chainnamed?json=' + cwJ + '&get=' + cwG + '&spread=' + cwS);"
    "</script>"
    "<script>"
    "var pxD = new Proxy(document, {}), pxW = new Proxy(Object.getPrototypeOf(Window.prototype), {});"
    "var pxHas = 'dnObj' in pxD && !('dnNoSuch' in pxD) && 'dnObj' in pxW;"
    "var pxDd = Object.getOwnPropertyDescriptor(pxD, 'dnObj'), pxWd = Object.getOwnPropertyDescriptor(pxW, 'dnObj');"
    "var pxGopd = !!pxDd && pxDd.enumerable && pxDd.value instanceof HTMLObjectElement &&"
    " !!pxWd && !pxWd.enumerable && pxWd.writable && Object.prototype.hasOwnProperty.call(pxD, 'dnObj');"
    "var pxWin = 'dnObj' in window && 'dnObj' in self && Reflect.has(window, 'dnObj') && !('dnNoSuch' in window);"
    "var pxTo = Object.assign({}, document);"
    "var pxAssign = pxTo.dnObj instanceof HTMLObjectElement && Object.prototype.hasOwnProperty.call(pxTo, 'dnObj');"
    "fetch('/api/proxynamed?has=' + pxHas + '&gopd=' + pxGopd + '&assign=' + pxAssign + '&win=' + pxWin);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
