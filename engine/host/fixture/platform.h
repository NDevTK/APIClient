/* THE PLATFORM DOCUMENT: DOM, CSSOM, geometry and event statements whose findings are the engine's own asserts,
   read by its `platform-algo` row, each inside its own endpoint's record.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PLATFORM_H
#define ENGINE_HOST_FIXTURE_PLATFORM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PLATFORM[] =
    "<!doctype html><html><head>"
    /* Author CSS whose SPECIFICITY and DOCUMENT ORDER disagree: the id rule is written FIRST and must still
       win. Order-only cascading answers `none` here, and every page puts its general rules last. */
    "<style>#cs1 { display: block; color: rgb(1, 2, 3) } div { display: none }</style>"
    "</head><body><div id=cs1 style=\"margin-top: 4px\"></div><h1 id=dh>doch</h1>"
    "<script>"
    /* WEB IDL §3.8's OWN-PROPERTY BAND, OBSERVED FROM THE PAGE — the half core/realm.c's walk structurally
       cannot ask. That walk enumerates the own properties a global HAS and judges each; this asks what the
       global must NOT have, which is a question about an absence and has no row to stand on. Both are needed
       and neither is the other: browser/idl_exposure.h's IDL_PROTOTYPE_ONLY makes the wrong-target install
       abort in a dev build, and this row is what scores the engine when it does not abort, because an absent
       crash is the weakest evidence a fix can produce.
       THE FOUR ARMS ARE THE FOUR THINGS A BROWSER ANSWERS, and WPT's window-properties.https.html asserts
       three of them for these same names: `id in window` true, the descriptor `undefined`, and the value
       IDENTICAL to the one on EventTarget.prototype. The fourth is window-prototype-chain.html's, and it is
       why the third can hold — `window` reaches §2.7's prototype in exactly three links, through
       Window.prototype and the WindowProperties object core/frame/window.c builds between them.
       `o` IS THE DISCRIMINATING ARM and it is asked of all three operations, because a placement that put one
       of them back would otherwise be scored by a row that only looked at the first. THE PAYLOAD IS CONSTANTS:
       every character is a literal, so nothing here can become unknown and turn a run that took the path into
       a request that never went out. */
    "var geo = '';"
    "geo += ('addEventListener' in window) ? 'i' : 'I';"
    "geo += (Object.getOwnPropertyDescriptor(window, 'addEventListener') === undefined"
    " && Object.getOwnPropertyDescriptor(window, 'removeEventListener') === undefined"
    " && Object.getOwnPropertyDescriptor(window, 'dispatchEvent') === undefined) ? 'o' : 'O';"
    "geo += (window.addEventListener === EventTarget.prototype.addEventListener) ? 's' : 'S';"
    "geo += (Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf(window)))"
    " === EventTarget.prototype) ? 'c' : 'C';"
    "fetch('/api/globalown?v=' + (geo === 'iosc' ? 'protoonly' : 'arm' + geo));"
    "fetch('/api/docnode?v=' + (document.nodeType === 9 && document.contains(document.body) ? 'isnode' : 'wrong'));"
    /* getRootNode's `composed` is a page GETTER, so the option read is a request the machine parks on and the
       loop inside it forces a suspend — the answer must still be the document. */
    "fetch('/api/rootnode?v=' + (document.body.getRootNode({ get composed(){ var n=0; for (var i=0;i<400;i++) n+=i; return true; } }) === document ? 'isroot' : 'wrong'));"
    /* DOM §4.9's VALUE SPACE, which a `const char *` cannot carry. `setAttribute(DOMString qualifiedName,
       (TrustedType or DOMString) value)` names no exclusion and a DOMString is UTF-16, so U+0000 is an
       ORDINARY attribute value — and every write converted it with JS_ToCString and then re-derived the length
       with strlen, which stops at the first 0x00. Three bytes went in and ONE came out, silently, at all three
       of §4.9's write sites: the element's own setAttribute, §4.9.2's `Attr.value` setter, and "set an
       attribute" step 5, whose policy call had just read the value back out of Lexbor WITH its length.
       THE LENGTH IS THE WHOLE OF IT — Lexbor's setters have always taken (value, value_len) and both reads
       hand that length straight to JS_NewStringLen, so nothing else in the chain ever lost a byte. The middle
       code unit is asserted as well as the length, because a length alone is satisfied by a replacement
       character too. A lone surrogate is NOT tested here and needs no fix: its WTF-8 holds no 0x00, so strlen
       never truncated it, and cutils.h's decoder accepts it back by name. */
    "var nz = document.createElement('div'); nz.setAttribute('x', 'a\\u0000b');"
    "var nzr = nz.getAttribute('x');"
    "var nza = document.createAttribute('y'); nza.value = 'a\\u0000b';"
    "var nzav = nza.value;"
    "nz.setAttributeNode(nza);"
    "var nzn = nz.getAttribute('y');"
    /* THE FOURTH WRITE SITE, which no grep over core/dom/ could see: HTML §3.2.2's DOMStringMap setter is
       `setAttribute`'s value decision reached through `dataset`, and it held its own copy of it. The
       population was derived from the WRITE DOOR's callers rather than from a directory. */
    "nz.dataset.k = 'a\\u0000b';"
    "var nzd = nz.dataset.k;"
    /* §4.5.1's OTHER value space. createDocumentType validates only its FIRST argument — `publicId` and
       `systemId` are plain DOMStrings the doctype stores verbatim — so they are the attribute-value space
       reached through a different consumer, and a population derived from the attribute WRITE DOOR could not
       see them either. The qualified name is NOT tested with a U+0000 in it: that one must THROW, which is the
       name axis and a different diff. */
    "var nzt = document.implementation.createDocumentType('n', 'a\\u0000b', 'c\\u0000d');"
    "fetch('/api/domnul?set=' + (nzr.length === 3 && nzr.charCodeAt(1) === 0 ? 'roundtrip' : 'truncated')"
    " + '&attrval=' + (nzav.length === 3 && nzav.charCodeAt(1) === 0 ? 'roundtrip' : 'truncated')"
    " + '&setnode=' + (nzn.length === 3 && nzn.charCodeAt(1) === 0 ? 'roundtrip' : 'truncated')"
    " + '&dataset=' + (nzd.length === 3 && nzd.charCodeAt(1) === 0 ? 'roundtrip' : 'truncated')"
    " + '&doctype=' + (nzt.publicId.length === 3 && nzt.publicId.charCodeAt(1) === 0"
    "                  && nzt.systemId.length === 3 && nzt.systemId.charCodeAt(1) === 0 ? 'roundtrip' : 'truncated'));"
    /* THE WALK IS THE PAGE'S SIZE, so it is a MACHINE that yields at every pair. 300 nested nodes is 300
       suspension points inside one member — the answer being right afterwards is the whole claim, because a
       resume that lost its cursors would compare the wrong pair or re-walk from the top and never finish. */
    "var dp = document.createElement('div'), dq = dp;"
    "for (var di = 0; di < 300; di++) { var dn = document.createElement('span'); dn.setAttribute('k', 'v' + di);"
    " dq.appendChild(dn); dq = dn; }"
    "var dp2 = dp.cloneNode(true);"
    "var deep1 = dp.isEqualNode(dp2);"
    "dq.setAttribute('k', 'changed');"
    "var deep2 = dp.isEqualNode(dp2);"
    "fetch('/api/deepequal?v=' + (deep1 && !deep2 ? 'isdeep' : 'wrong'));"
    /* §4.5's importNode — the member `document.importNode(t.content, true)` is written with, and the one this
       file could not exercise at all because it was ABSENT: a page reaching for it got a TypeError and stopped.
       A SECOND DOCUMENT IS WHAT MAKES IT OBSERVABLE, and that is not fixture convenience — a same-document
       import answers the byte-identical tree a clone answers, so a row over one document cannot tell the two
       members apart and would pass with §4.4's `document` default still hardcoded. What each row asserts is a
       value the engine COMPUTES: whose document the copy belongs to, what the copy's subtree serializes to, and
       that the ORIGINAL is still standing where it was — which is the whole difference from adoptNode beside
       it, since an adopt MOVES and an import COPIES. */
    "var idoc = document.implementation.createHTMLDocument('imp');"
    "var isrc = idoc.createElement('div'); isrc.setAttribute('id', 'isrcid');"
    "var ipp = idoc.createElement('p'); ipp.setAttribute('class', 'a');"
    "ipp.appendChild(idoc.createTextNode('x'));"
    "var ibb = idoc.createElement('b'); ibb.appendChild(idoc.createTextNode('y')); ipp.appendChild(ibb);"
    "isrc.appendChild(ipp);"
    "var iii = idoc.createElement('i'); iii.appendChild(idoc.createTextNode('z')); isrc.appendChild(iii);"
    "idoc.body.appendChild(isrc);"
    "var ideep = document.importNode(isrc, true);"
    "fetch('/api/importdeep?own=' + (ideep.ownerDocument === document ? 'impthisdoc' : 'wrong')"
    " + '&kid=' + (ideep.firstChild && ideep.firstChild.ownerDocument === document ? 'impkidthis' : 'wrong')"
    " + '&ser=' + encodeURIComponent(ideep.innerHTML)"
    " + '&det=' + (ideep.parentNode === null && !ideep.isConnected ? 'impdetached' : 'wrong')"
    " + '&src=' + (isrc.ownerDocument === idoc && isrc.parentNode === idoc.body ? 'impstayed' : 'impmoved'));"
    /* THE THREE ARMS OF `optional (boolean or ImportNodeOptions) options = false`, WHICH DISAGREE, and the
       reason the declaration states its default explicitly. An OMITTED argument is §3.6's declared default and
       that default is the BOOLEAN `false` — a SHALLOW import; an explicit EMPTY DICTIONARY is the other arm,
       whose `selfOnly` defaults to false and which step 5.1 NEGATES — a DEEP one; and `{selfOnly:true}` is
       shallow again. Every other union-with-dictionary position in the platform's IDL defaults to `{}`, so an
       engine that expressed this one the same way would answer DEEP for `document.importNode(n)`, which is the
       commonest call there is. `no` and `dict` differing is the whole assertion. */
    "var ish = document.importNode(isrc);"
    "var idd = document.importNode(isrc, {});"
    "var iso = document.importNode(isrc, { selfOnly: true });"
    "var itt = document.importNode(isrc, true);"
    "fetch('/api/importarm?no=' + ish.childNodes.length + '&dict=' + idd.childNodes.length"
    " + '&self=' + iso.childNodes.length + '&bool=' + itt.childNodes.length"
    " + '&attr=' + (ish.getAttribute('id') === 'isrcid' ? 'impattr' : 'wrong'));"
    /* STEP 1's TWO REFUSALS, WHICH ARE ONE EXCEPTION OVER TWO NODE KINDS — a DOCUMENT and a SHADOW ROOT. They
       are asserted together because landing one of a two-kind refusal is a WRONG answer and not a partial one:
       a page's `catch (e) { e.name }` cannot tell a kind that is refused from a kind that is copied, so the
       only way to state that both are refused is to read both names. The third probe is the declaration's own
       §3.2.15 refusal of a `customElementRegistry` that is not one, which is a TypeError and not step 5.3's
       NotSupportedError — a different exception at a different layer, and the pair says the ORDER is right. */
    "var ierr = [];"
    "try { document.importNode(document); ierr.push('nothrow'); } catch (e) { ierr.push(e.name); }"
    "var ihost = document.createElement('div');"
    "try { var isr = ihost.attachShadow({ mode: 'open' }); document.importNode(isr); ierr.push('nothrow'); }"
    " catch (e) { ierr.push(e.name); }"
    "try { document.importNode(isrc, { customElementRegistry: 5 }); ierr.push('nothrow'); }"
    " catch (e) { ierr.push(e.name); }"
    "fetch('/api/importthrow?v=' + ierr.join(','));"
    /* STEP 5.3's ACCEPTING ARMS, WHICH ARE THE TWO REGISTRIES AN IMPORT MAY NAME: a SCOPED one — §4.13.4's
       constructor sets `is scoped` to true — and THIS DOCUMENT'S OWN, which is what `window.customElements` is.
       Both must pass and the import must still answer a copy.
       WHAT THIS FIXTURE CANNOT REACH is the REFUSING arm, and saying so is part of the row rather than a gap in
       it: a registry that is neither scoped nor this document's is another WINDOW's `customElements`, and a
       `createHTMLDocument` document has no Window at all (its `defaultView` is null), so one realm cannot
       produce one. The refusal is asserted by the code path it shares with §4.9's attachShadow and §4.5's
       createElement, whose own rows reach the same predicate. */
    "var iregs = [];"
    "try { iregs.push(document.importNode(isrc, { customElementRegistry: new CustomElementRegistry() })"
    "        .ownerDocument === document ? 'impscoped' : 'wrong'); } catch (e) { iregs.push(e.name); }"
    "try { iregs.push(document.importNode(isrc, { customElementRegistry: window.customElements })"
    "        .ownerDocument === document ? 'impown' : 'wrong'); } catch (e) { iregs.push(e.name); }"
    "fetch('/api/importreg?v=' + iregs.join(','));"
    /* §4.10 a template has TWO child lists and both are real: the parser fills its content fragment, and
       `t.appendChild(x)` appends to the ELEMENT — only the parser and `t.content` reach the fragment. So a walk
       over a template must do the content AND then the ordinary children, which is why coming back from the
       content has to resume between the two. Nested, because one level proves nothing about the stack. */
    "var tt = document.createElement('div');"
    "tt.innerHTML = '<template><i>c1</i><template><s>c2</s></template></template>';"
    "var tte = tt.firstChild;"
    "tte.appendChild(document.createElement('u'));"
    "var ttc = tt.cloneNode(true);"
    "fetch('/api/tplboth?ser=' + encodeURIComponent(tt.innerHTML)"
    " + '&cl=' + encodeURIComponent(ttc.innerHTML)"
    " + '&kids=' + tte.childNodes.length + ':' + ttc.firstChild.childNodes.length"
    " + '&kn=' + (ttc.firstChild.firstChild ? ttc.firstChild.firstChild.nodeName : 'none'));"
    "var host = document.createElement('div');"
    "host.appendChild(document.createTextNode('A')); host.appendChild(document.createTextNode('B'));"
    "host.normalize();"
    "fetch('/api/normalize?v=' + (host.childNodes.length === 1 ? host.firstChild.data : 'wrong'));"
    "var ib = document.createElement('i'); host.insertBefore(ib, host.firstChild);"
    "fetch('/api/insertbefore?v=' + (host.firstChild === ib && ib.parentElement === host ? 'isfirst' : 'wrong'));"
    "fetch('/api/connected?v=' + (document.body.isConnected && !host.isConnected ? 'isconn' : 'wrong'));"
    /* §4.4 compareDocumentPosition is FIVE walks behind one answer — two to the roots, two up the ancestor
       chains, and a pre-order walk of the whole shared tree when neither contains the other. Only the
       disconnected arm was asserted, so four of the five could have been anything. Each is a machine stage now,
       so a wrong resume in any of them is a wrong bit here. */
    "var pw = document.createElement('div'); document.body.appendChild(pw);"
    "var pa = document.createElement('p'); var pb = document.createElement('q');"
    "pw.appendChild(pa); pw.appendChild(pb);"
    "var pd = document.createElement('b'); pa.appendChild(pd);"
    "var P = Node;"
    "fetch('/api/posbits?anc=' + (pw.compareDocumentPosition(pd) === (P.DOCUMENT_POSITION_CONTAINED_BY | P.DOCUMENT_POSITION_FOLLOWING) ? 'contains' : 'wrong')"
    " + '&desc=' + (pd.compareDocumentPosition(pw) === (P.DOCUMENT_POSITION_CONTAINS | P.DOCUMENT_POSITION_PRECEDING) ? 'containedby' : 'wrong')"
    " + '&ord=' + (pa.compareDocumentPosition(pb) === P.DOCUMENT_POSITION_FOLLOWING"
    " && pb.compareDocumentPosition(pa) === P.DOCUMENT_POSITION_PRECEDING"
    " && pd.compareDocumentPosition(pb) === P.DOCUMENT_POSITION_FOLLOWING ? 'order' : 'wrong')"
    " + '&self=' + (pa.compareDocumentPosition(pa) === 0 ? 'zero' : 'wrong'));"
    /* §4.4 normalize's second loop: a RUN of adjacent Text siblings absorbed into the first, which is as long
       as the number of chunks a page appended. 40 of them, with empty ones interleaved so the drop arm runs
       too, and one non-Text node in the middle so the run has to STOP rather than swallow the tree. */
    "var nz = document.createElement('div');"
    "for (var nzi = 0; nzi < 40; nzi++) {"
      "nz.appendChild(document.createTextNode('' + (nzi % 10)));"
      "nz.appendChild(document.createTextNode('')); }"
    "nz.appendChild(document.createElement('hr'));"
    "for (var nzj = 0; nzj < 5; nzj++) nz.appendChild(document.createTextNode('z'));"
    "nz.normalize();"
    "fetch('/api/normrun?n=' + nz.childNodes.length + '&a=' + nz.firstChild.data + '&b=' + nz.lastChild.data);"
    /* §2.2 THE EVENT INTERFACE. EventInit is a DICTIONARY, so its members are property READS — the getter here
       is the page's code with a loop in it, and the conversion parks on that member and resumes. */
    "var ev = new Event('custom', { bubbles: true,"
    " get cancelable(){ var n = 0; for (var i = 0; i < 300; i++) { n += i; } return true; } });"
    "fetch('/api/event?v=' + (ev instanceof Event && ev.type === 'custom' && ev.bubbles && ev.cancelable &&"
    " !ev.isTrusted && ev.eventPhase === Event.NONE ? 'isevent' : 'wrong'));"
    /* preventDefault writes the CANCELED SLOT, which defaultPrevented and returnValue both read — they were a
       shared no-op over a public property the page could simply assign. */
    "ev.preventDefault();"
    "fetch('/api/evcancel?v=' + (ev.defaultPrevented && ev.returnValue === false ? 'iscancel' : 'wrong'));"
    /* WEB IDL §3.2.17 Dictionary types' CONVERSION IS INSIDE THE MEMBER LOOP, WHICH IS OBSERVABLE. Step
       4.1.4.1 converts a member "to an IDL value whose type is the type member is declared to be of", and it
       runs before the NEXT member's step 4.1.3.1 `Get(jsDict, key)` — so a member whose type refuses the value
       throws while every member after it is still unread. `screenX` sorts after `relatedTarget` among
       MouseEventInit's own members and `which` sorts after `view` among UIEventInit's, so each getter below is
       one a browser never calls. It is a fixture rather than an assert because the thing under test is
       ORDER: the engine cannot assert that a getter it never ran did not run, and only the page can see it.
       These two members were IDL_ANY and brand-checked by their event bodies, which runs after the whole walk
       — `ran` came back true and the error was the body's, one full dictionary late. */
    "var mo = { ran: false, threw: 'none' };"
    "try { new MouseEvent('mo', { relatedTarget: 42, get screenX(){ mo.ran = true; return 0; } }); }"
    " catch (e) { mo.threw = (e instanceof TypeError) ? 'TypeError' : 'other'; }"
    "var vo = { ran: false, threw: 'none' };"
    "try { new UIEvent('vo', { view: 42, get which(){ vo.ran = true; return 0; } }); }"
    " catch (e) { vo.threw = (e instanceof TypeError) ? 'TypeError' : 'other'; }"
    "fetch('/api/dictorder?v=' + (mo.threw === 'TypeError' && mo.ran === false &&"
    " vo.threw === 'TypeError' && vo.ran === false ? 'isorder'"
    " : mo.threw + mo.ran + '/' + vo.threw + vo.ran));"
    /* AND THE OTHER SIDE OF THE SAME DECLARATION, because a type that refuses everything passes the test above.
       `Window? view = null` takes this realm's own Window and `EventTarget? relatedTarget = null` takes a node
       — two interfaces no JSClassID can name, branded per member by their owning component's realm-taking
       predicate — and each `= null` is what an omitted member IS, not a hole a body filled. */
    "var okev = new MouseEvent('ok', { view: window, relatedTarget: document.body });"
    "var bare = new MouseEvent('bare');"
    "fetch('/api/dictiface?v=' + (okev.view === window && okev.relatedTarget === document.body &&"
    " bare.view === null && bare.relatedTarget === null ? 'isbrand' : 'wrong'));"
    /* §2.9 DISPATCH — SYNCHRONOUS, and its answer depends on what the listeners did. The first listener holds a
       LOOP, so the walk suspends inside it and resumes at the listener it was on; the second calls
       preventDefault, which is what dispatchEvent's false return reports. A job-enqueued dispatch would answer
       true here because nothing would have run yet. */
    "var dt = document.createElement('button'); var dhit = 0;"
    "dt.addEventListener('go', function(e){ var n = 0; for (var i = 0; i < 400; i++) { n += i; } dhit = n; });"
    "dt.addEventListener('go', function(e){ e.preventDefault(); });"
    "var dev = new Event('go', { cancelable: true });"
    "var dres = dt.dispatchEvent(dev);"
    "fetch('/api/dispatch?v=' + (dres === false && dhit === 79800 && dev.defaultPrevented &&"
    " dev.target === dt && dev.currentTarget === null && dev.eventPhase === 0 ? 'isdispatch' : 'wrong'));"
    /* stopImmediatePropagation ends the walk BETWEEN listeners, which is the only place it is observable. */
    "var st2 = document.createElement('b'); var sthit = 0;"
    "st2.addEventListener('s', function(e){ sthit += 1; e.stopImmediatePropagation(); });"
    "st2.addEventListener('s', function(e){ sthit += 10; });"
    "st2.dispatchEvent(new Event('s'));"
    "fetch('/api/stopimmediate?v=' + (sthit === 1 ? 'isstop' : 'wrong'));"
    /* HTML §8.1.7.2 EVENT HANDLER IDL ATTRIBUTES. The handler is NOT the listener: the slot keeps its POSITION
       while its handler changes underneath it, so `onh = a; addEventListener(h, b); onh = c` runs c then b. A
       run that registered the handler function itself would append it and get that backwards. */
    "var hh = document.createElement('u'); var order = '';"
    "hh.onclick = function(){ order += 'A'; };"
    "hh.addEventListener('click', function(){ order += 'B'; });"
    "hh.onclick = function(){ order += 'C'; };"
    "hh.dispatchEvent(new Event('click'));"
    "fetch('/api/handler?v=' + (order === 'CB' && typeof hh.onclick === 'function' ? 'ishandler' : 'wrong'));"
    /* Assigning null is §8.1.7's "deactivate": the slot goes, and the addEventListener listener stays. */
    "hh.onclick = null; order = ''; hh.dispatchEvent(new Event('click'));"
    "fetch('/api/handlernull?v=' + (order === 'B' && hh.onclick === null ? 'isnull' : 'wrong'));"
    /* The GLOBAL's set: window's IDL mixes in GlobalEventHandlers, and `onload` is how real code starts. */
    "globalThis.onerror = function(){ }; document.onreadystatechange = function(){ };"
    "fetch('/api/onglobal?v=' + (typeof globalThis.onerror === 'function' &&"
    " typeof document.onreadystatechange === 'function' && globalThis.onload === null ? 'isglobal' : 'wrong'));"
    /* HTML §4's ELEMENT-INTERFACE TABLE. The tag decides the interface, and the reflections belong to the
       interface that DECLARES them — `src` used to be on every element and `form.action` did not exist. */
    "var ia = document.createElement('a'); var isc = document.createElement('script');"
    "var ifo = document.createElement('form'); var iin = document.createElement('input');"
    "var idv = document.createElement('div'); var ixx = document.createElement('blink');"
    "fetch('/api/iface2?v=' + (ia instanceof HTMLAnchorElement && ia instanceof HTMLElement &&"
    " ia instanceof Element && ia instanceof Node && idv instanceof HTMLDivElement &&"
    " !(idv instanceof HTMLAnchorElement) && ixx instanceof HTMLUnknownElement ? 'isiface2' : 'wrong'));"
    /* HTML §3.2.2 Elements in the DOM's SEVEN STEPS, each reported by the WEB IDL §3.7.3 class string of the
       prototype the element actually got — the same question WPT html/semantics/interfaces.html asks with
       assert_class_string, and the reason this endpoint carries a NAME rather than a boolean: a `?v=wrong`
       says the mapping is broken and says nothing about how, while this one distinguishes an element mapped
       CORRECTLY from one mapped to the WRONG interface from one that fell through to HTMLUnknownElement,
       per name, in the value itself. `section` and `tt` are HTMLElement by steps 4 and 2, `progress` is its
       own interface by step 4, `listing` is HTMLPreElement by step 3 although it is not `pre`, `applet` is
       HTMLUnknownElement by step 1 although the name is one the standard knows, and `xxx` is step 7's. */
    "var ifn = function(n){ return Object.prototype.toString.call(document.createElement(n)).slice(8, -1); };"
    "fetch('/api/ifacestep?v=' + ifn('section') + '-' + ifn('tt') + '-' + ifn('progress') + '-' +"
    " ifn('listing') + '-' + ifn('applet') + '-' + ifn('xxx'));"
    /* §3.2.2 step 6 is a VALID CUSTOM ELEMENT NAME and not a hyphen: `foo-BAR` carries an ASCII upper alpha
       and `annotation-xml` is one of the eight reserved names, so both are step 7's HTMLUnknownElement. The
       case-preserving createElementNS is deliberate — DOM §4.5's createElement lowercases in an HTML document,
       which would hide the uppercase clause entirely. */
    "var ifns = function(ns, n){ return Object.prototype.toString.call("
    " document.createElementNS(ns, n)).slice(8, -1); };"
    "var XHTMLNS = 'http://www.w3.org/1999/xhtml';"
    "fetch('/api/ifacecename?v=' + ifns(XHTMLNS, 'foo-bar') + '-' + ifns(XHTMLNS, 'foo-BAR') + '-' +"
    " ifns(XHTMLNS, 'annotation-xml'));"
    /* §3.2.2 is scoped to the HTML NAMESPACE, and an SVG `<a>` used to be handed HTMLAnchorElement.prototype
       because this resolver never asked. It is now DOM §4.5's default, `Element` — narrower than the platform,
       where SVG names SVGAElement, and this endpoint is what says which of the two this engine gives. */
    "fetch('/api/ifacesvgns?v=' + ifns('http://www.w3.org/2000/svg', 'a'));"
    /* The reflections are on their own interfaces now: a link has href, a div does not, and a div carrying an
       `src` property would be the old flat table answering for an attribute its IDL never declared. */
    "ia.href = '/api/reflected?v=isreflect'; ifo.action = '/api/formaction'; iin.name = 'q';"
    "isc.src = '/chunk/iface.js';"
    "fetch(ia.href);"
    "fetch('/api/reflect2?v=' + (ifo.getAttribute('action') === '/api/formaction' && iin.name === 'q' &&"
    " isc.getAttribute('src') === '/chunk/iface.js' && idv.href === undefined && idv.src === undefined"
    " ? 'isreflect2' : 'wrong'));"
    /* A BOOLEAN reflection is the attribute's PRESENCE, and it must be able to UNSET — which needs
       removeAttribute, which had no implementation at all. */
    "isc.async = true; iin.disabled = true; iin.disabled = false;"
    "fetch('/api/reflectbool?v=' + (isc.async === true && isc.getAttribute('async') === '' &&"
    " iin.disabled === false && iin.hasAttribute('disabled') === false ? 'isbool' : 'wrong'));"
    /* §2.6.1's URL REFLECTION: the getter encoding-parses and serializes against the element's NODE DOCUMENT
       and answers the ABSOLUTE URL, while the attribute keeps exactly what was written. A REFLECT_STRING row
       answers `/a.mp4` here and is INSTALLED, so the gap auditor scores it complete either way — this endpoint
       is the only thing in the tree that can tell the two apart. */
    "var iv = document.createElement('video');"
    "fetch('/api/refurlabsent?v=' + (iv.src === '' ? 'isempty' : 'wrong'));"
    "iv.src = '/a.mp4';"
    "fetch('/api/refurl?v=' + (iv.src === 'https://x.test/a.mp4' &&"
    " iv.getAttribute('src') === '/a.mp4' ? 'isabs' : 'wrong'));"
    /* A path-relative value resolves against the document's PATH, not against the origin alone. */
    "iv.setAttribute('src', 'b.mp4');"
    "fetch('/api/refurlrel?v=' + (iv.src === 'https://x.test/b.mp4' ? 'isrel' : 'wrong'));"
    /* Step 3: a value that does not parse reads back as itself — a page that wrote garbage reads garbage. */
    "iv.src = 'https://[!';"
    "fetch('/api/refurlbad?v=' + (iv.src === 'https://[!' ? 'israw' : 'wrong'));"
    /* `src` is HTMLMediaElement's ONE member and not a copy per subclass — the reflection put it on the
       prototype the IDL names, which the two deleted hand-written accessors also had to get right. */
    "fetch('/api/refurlproto?v=' + ("
    " Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype, 'src') === undefined &&"
    " typeof Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, 'src').get === 'function'"
    " ? 'isone' : 'wrong'));"
    /* §2.6.1's `unsigned long` reflection, whose two fallbacks are DIFFERENT STEPS. `<td>` declares a range
       starting at 0 for rowspan and 1 for colspan, and a default of 1 for both — so an in-range 0 and an
       unparseable value give different answers on the same element, which one "fallback" number cannot. */
    "var itd = document.createElement('td');"
    "fetch('/api/ulabsent?v=' + (itd.colSpan === 1 && itd.rowSpan === 1 ? 'isdflt' : 'wrong'));"
    "itd.setAttribute('rowspan', '0'); itd.setAttribute('colspan', '0');"
    /* rowspan 0 is IN RANGE; colspan 0 is below its minimum and CLAMPS up, not to the default (they agree at
       1 here, so the discriminating pair is rowspan's 0-vs-1). */
    "fetch('/api/ulrange?v=' + (itd.rowSpan === 0 && itd.colSpan === 1 ? 'isrange' : 'wrong'));"
    "itd.setAttribute('rowspan', 'x');"
    "fetch('/api/uldflt?v=' + (itd.rowSpan === 1 ? 'isdflt2' : 'wrong'));"
    /* An overflowing digit run is ABOVE the maximum, not a parse error — the clamp, never the default. */
    "itd.setAttribute('rowspan', '99999999999999999999999');"
    "fetch('/api/uloverflow?v=' + (itd.rowSpan === 65534 ? 'isclamp' : 'wrong'));"
    /* THE SETTER DOES NOT CLAMP: "clamped to the range has no effect on the setter steps", so the attribute
       holds 5000 and only the read pulls it to 1000. A setter that clamped would store 1000 and the two
       assertions below would agree with each other and with a browser on neither. */
    "itd.colSpan = 5000;"
    "fetch('/api/ulset?v=' + (itd.getAttribute('colspan') === '5000' && itd.colSpan === 1000"
    " ? 'isnoclamp' : 'wrong'));"
    /* A value the IDL's own unsigned-long conversion leaves above 2147483647 is out of the setter's window, so
       the DEFAULT is written — not a wrap and not a clamp. */
    "itd.colSpan = 3000000000;"
    "fetch('/api/ulwindow?v=' + (itd.getAttribute('colspan') === '1' ? 'isdefaultwritten' : 'wrong'));"
    /* headingOffset has a range and NO default, so its fallback is the range's minimum. */
    "var ihd = document.createElement('div'); ihd.setAttribute('headingoffset', 'x');"
    "fetch('/api/ulmin?v=' + (ihd.headingOffset === 0 && ihd.headingReset === false ? 'ismin' : 'wrong'));"
    /* HTML §4.10.13 "The progress element" and §4.10.14 "The meter element" — the two numbers in each section
       that ONLY its own order produces, over plain attributes so `===` is legal here.
       §4.10.13: "the current value is the maximum value, if value is greater than the maximum value, and
       value otherwise", so `<progress value=5 max=2>` reads `value` 2 and `position` 1. An implementation
       answering the VALUE OF THE PROGRESS BAR instead reads 5 and 2.5, and one that clamped only `position`
       reads 5 and 1 — three distinct answers, and only one of them is here.
       §4.10.14: "If the candidate high boundary is less than the low boundary, then the high boundary is the
       low boundary", which is the one place in the six where the lower bound is NOT the minimum value. With
       low 8 and high 2 the high boundary is 8; clamping it into [minimum, maximum] like the other three gives
       2, and neither number can be reached by luck from the other's rule. */
    "var gp = document.createElement('progress');"
    "gp.setAttribute('value', '5'); gp.setAttribute('max', '2');"
    "var gm = document.createElement('meter');"
    "gm.setAttribute('min', '0'); gm.setAttribute('max', '10');"
    "gm.setAttribute('low', '8'); gm.setAttribute('high', '2'); gm.setAttribute('value', '5');"
    "fetch('/api/gaugeorder?v=' + gp.value + '-' + gp.position + '-' + gm.high + '-' + gm.optimum);"
    /* THE SAME TWO ELEMENTS OVER UNKNOWN EXTERNAL INPUT, which is what has no `===` to make: each read is a
       DERIVATION and what this asserts is the SHAPE it carries. `position` is computed from the maximum value
       AND the `value` attribute, and the optimum point's default is "the midpoint between the minimum value
       and the maximum value" — so `/api/gaugemid`'s emitted shape must name {state} AND {num}, and
       `/api/gaugepos`'s must name both of its own. A derivation naming ONE of them renders one hole and is
       the defect these two rows exist to make visible: two gauges differing only in the operand it did not
       name would share one constraint entry.
       `'' + num` AND NOT `num`, because §2.3.4.3's rules for parsing floating-point number values consume
       BYTES: `num`'s own example is the Number 1920, and an attribute whose example is not a string reaches
       the parse-error arm, which is a true answer to a different question than the one these rows ask. */
    "var gpu = document.createElement('progress');"
    "gpu.setAttribute('max', '' + num); gpu.setAttribute('value', '' + state);"
    "fetch('/api/gaugepos?v=' + gpu.position);"
    "var gmu = document.createElement('meter');"
    "gmu.setAttribute('min', '' + state); gmu.setAttribute('max', '' + num);"
    "fetch('/api/gaugemid?v=' + gmu.optimum);"
    /* HTML §6.5's click() is "Fire a synthetic pointer event named click at this element, with the not
       trusted flag set." — the SAME dispatch, so a handler wired with onclick sees an untrusted, cancelable,
       bubbling event and preventDefault reaches the caller. */
    "var ck = 0; var ckt = null; idv.onclick = function(e){ ck++; ckt = e; e.preventDefault(); };"
    "idv.click();"
    "fetch('/api/click?v=' + (ck === 1 && ckt.type === 'click' && ckt.isTrusted === false &&"
    " ckt.bubbles && ckt.cancelable && ckt.defaultPrevented && ckt.target === idv ? 'isclick' : 'wrong'));"
    /* CSSOM. getComputedStyle layers inline over the author cascade over the UA default over the property's
       initial value, all resolved from THIS flow's tree. */
    "var cs = document.getElementById('cs1'); var csc = getComputedStyle(cs);"
    "fetch('/api/cascade?v=' + (csc.display === 'block' ? 'isspec' : 'wrong'));"
    "fetch('/api/cssinline?v=' + (csc.marginTop === '4px' && cs.style.marginTop === '4px' &&"
    " cs.style.display === '' ? 'isinline' : 'wrong'));"
    /* The UA default reaches an element no rule names, and an undeclared property reads its INITIAL value. */
    "var csp = document.createElement('p'); document.body.appendChild(csp);"
    "fetch('/api/cssua?v=' + (getComputedStyle(csp).display === 'block' &&"
    " getComputedStyle(document.createElement('span')).display === 'inline' ? 'isua' : 'wrong'));"
    /* AND SO DOES AN UNDECLARED BACKGROUND LONGHAND, which is the same sentence one property family over and
       was the one place it was not true. Seven of css-backgrounds-3 §2.10 "Backgrounds Shorthand: the
       background property"'s eight longhands are in NO property registry, so css-cascade-5 §7.1 "Initial
       Values" had nothing to fall to and `cssom_initial_value` answered NOTHING for them — which
       core/css/css_computed_value.c's resolved-value entry reads as CSSOM's answer for a property that is not
       set, the EMPTY STRING. `getComputedStyle(csp).backgroundImage` was `""` where every user agent says
       `none`, so a page asking the ordinary question `does this element have a background image` took the arm
       for one that HAS one. `csp` is the element this block already made that no rule names, which is exactly
       the population the defect was invisible on.
       THE LAST TWO ARE THE DISCRIMINATING PAIR and are why this row names four properties rather than one:
       css-backgrounds-3 §2.8 "Positioning Area: the background-origin property" initializes to `padding-box`
       and css-backgrounds-3 §2.7 "Painting Area: the background-clip property" to `border-box`, so they are
       the only two of the eight whose initial
       values differ from each other — an off-by-one in the index these values are read by would answer both
       with one word and every other assertion here would still pass. */
    "fetch('/api/cssbginitial?v=' + (getComputedStyle(csp).backgroundImage === 'none' &&"
    " getComputedStyle(csp).backgroundRepeat === 'repeat' &&"
    " getComputedStyle(csp).backgroundOrigin === 'padding-box' &&"
    " getComputedStyle(csp).backgroundClip === 'border-box' ? 'isbginitial' : 'wrong'));"
    /* A USED VALUE THAT IS A JOINT FUNCTION OF TWO ENVIRONMENT FACTS — the shape a box with a border has, and
       the one that used to crash. CSS 2.1 §10.3.3 solves this `width: auto` against the INITIAL CONTAINING
       BLOCK and subtracts a border css-values §6 SNAPPED to a whole number of DEVICE PIXELS, so the number
       moves with both and its domain is ONE identity over the pair (solver/concolic.h) rather than either
       fact alone. There is no token to compare against: the value is a concolic, so what this asserts is the
       arithmetic's own asserts staying silent, and what it emits is the joint shape. */
    "var jw = document.createElement('p'); document.body.appendChild(jw);"
    "jw.style.setProperty('border-left-style', 'solid');"
    "jw.style.setProperty('border-left-width', '1px');"
    "fetch('/api/cssjoint?w=' + getComputedStyle(jw).width);"
    /* ---- CSS 2.1 §10.3.2 "Inline, replaced elements" over HTML §15.4.2 "Images" ---------------------------
       EVERY NUMBER HERE IS A PLAIN INTEGER AND NOT A CONCOLIC, which is what makes `===` legal in this block
       where the probe above had to emit instead. A replaced element with no natural dimensions is CSS 2.1
       §10.3.2's and §10.6.2's 300 x 150 rectangle, and core/layout/used_value.c asserts that the one clause
       that could make it environment-dependent — "if 300px is too wide to fit the device" — does not bind
       against the modelled display. So the domain is a single point, and a value that started forking here
       would mean that assert had been softened rather than that the model changed.
       AN `img` WITH NO `src` AND AN EMPTY `alt` REPRESENTS NOTHING (§4.8.3 "The img element": "if the src
       attribute is not set and either the alt attribute is set to the empty string or the alt attribute is not
       set at all: the element represents nothing"), which is §15.4.2's FOURTH rule — "a replaced element whose
       NATURAL DIMENSIONS ARE 0". §10.3.2's first arm then takes that intrinsic width whole, so the box is 0 by
       0 and NOT the default rectangle: the two answers differ by 300 pixels and only one of them comes from
       reading the rules in order. */
    "var rz = document.createElement('img'); rz.setAttribute('alt', '');"
    "document.body.appendChild(rz);"
    "fetch('/api/replacedzero?v=' + (rz.width === 0 && rz.height === 0 &&"
    " getComputedStyle(rz).width === '0px' && getComputedStyle(rz).height === '0px' ? 'iszero' : 'wrong'));"
    /* AN `img` WHOSE REQUEST IS STILL OUTSTANDING is §15.4.2's SECOND rule — the user agent "has reason to
       believe the image will become available", which is a fact about this moment and not about whether this
       agent owns a decoder. That rule gives the replaced element no natural dimensions at all, so §10.3.2's
       LAST arm and §10.6.2's last arm size it: 300 by 150.
       THE ELEMENT IS `display: inline`, which is the whole point of the probe. CSS 2.1 §10.2's "Applies to:"
       line is "all elements but NON-REPLACED inline elements", so answering this at all is CSSOM §9's first
       conjunct being decided by HTML §15.4 rather than by a tag list — an inline `<span>` one line below still
       resolves `width` to the computed `auto`. */
    "var rd = document.createElement('img'); rd.src = '/api/replaced-pending.png';"
    "document.body.appendChild(rd);"
    "fetch('/api/replaceddefault?v=' + (rd.width === 300 && rd.height === 150 &&"
    " getComputedStyle(rd).width === '300px' && getComputedStyle(rd).height === '150px' ? 'is300' : 'wrong'));"
    "var rs = document.createElement('span'); document.body.appendChild(rs);"
    "fetch('/api/replacedspan?v=' + (getComputedStyle(rs).width === 'auto' ? 'isauto' : 'wrong'));"
    /* ---- CSSOM VIEW §2 "Terminology"'s SCROLLING AREA over CSS 2.2 §9.4.2 "Inline formatting contexts" ----
       The probe above asserts that a non-replaced inline box resolves `width` to the computed `auto` (CSS 2.2
       §10.3.1 "Inline, non-replaced elements": "the 'width' property does not apply"); §2's scrolling area
       still has to report "the right margin edge of all of the element's descendants' boxes" for it, so the
       edge comes from §9.4.2's fragments.
       The assertion is a difference: where a fragment ends is a font advance (core/css/font_metrics.h's
       modelled face), so neither `scrollWidth` alone is a number this file may write. The two containers
       differ only by 300px of `margin-right` on the span, whose used value is its declaration (CSS 2.2 §8.3
       "Margin properties"), so the advance cancels and §6's `long` rounding cancels with it.
       The block axis is the other half: §8.3 says "vertical margins will not have any effect on non-replaced
       inline elements", so 300px of `margin-bottom` moves `scrollHeight` by zero, and a fold that reached for
       a margin box on both axes passes the first claim and fails this one. */
    /* Each container declares `display: block` over this document's `div { display: none }`: CSSOM VIEW §6
       answers zero for scrollWidth of an element with no associated box, and both differences would be 0. */
    "var saInline = function(p, v){"
    "  var d = document.createElement('div'); d.style.setProperty('display', 'block');"
    "  d.style.setProperty('width', '50px');"
    "  var s = document.createElement('span'); s.style.setProperty(p, v);"
    "  s.appendChild(document.createTextNode('x'));"
    "  d.appendChild(s); document.body.appendChild(d); return d;"
    "};"
    "var saNarrow = saInline('margin-right', '400px'), saWide = saInline('margin-right', '700px');"
    "var saTall = saInline('margin-bottom', '400px'), saTaller = saInline('margin-bottom', '700px');"
    "fetch('/api/sainlineinline?v=' + (saWide.scrollWidth - saNarrow.scrollWidth === 300 ? 'is300' : 'wrong'));"
    "fetch('/api/sainlineblock?v=' + (saTaller.scrollHeight - saTall.scrollHeight === 0 ? 'iszero' : 'wrong'));"
    /* THE TWO DELTAS THEMSELVES, because a `wrong` above names no number and the whole point of the pair is
       WHICH of 300 and 0 the fold produced. */
    "fetch('/api/sainlinedelta?i=' + (saWide.scrollWidth - saNarrow.scrollWidth) +"
    " '&b=' + (saTaller.scrollHeight - saTall.scrollHeight));"
    /* §4.8.3's `[CEReactions, ReflectSetter] attribute unsigned long width`: the setter is HTML §2.6.1's
       reflection and the getter is determine-the-dimensions, two different questions this pair holds apart.
       core/css/css_presentational_hints.c carries HTML §15.4.3 "Attributes for embedded content and images",
       so writing 64 to the content attribute is what the rendered width follows.
       The asymmetry that remains is the cascade origin: css-cascade-5 §6.5 puts the hint in a
       "special-purpose author presentational hint origin between the regular user origin and the author
       origin", so a `style` width of 10px leaves the content attribute at 64 and the rendered width at 10. A
       hint written into the author origin or the UA sheet reads 64 here.
       §2.6.1's setter range test is the second half: `unsigned long` converts -1 to 4294967295, outside "the
       range minimum to 2147483647, inclusive", so the attribute becomes "0", which §15.4.3's row maps to
       `height: 0px`. */
    "rd.width = 64;"
    "rd.height = -1;"
    "fetch('/api/replacedsetter?v=' + (rd.getAttribute('width') === '64' &&"
    " rd.getAttribute('height') === '0' && rd.width === 64 ? 'isreflect' : 'wrong'));"
    "rd.style.setProperty('width', '10px');"
    "fetch('/api/preshintorigin?v=' + (rd.getAttribute('width') === '64' && rd.width === 10"
    " ? 'isauthor' : 'wrong'));"
    /* ---- HTML §15.4.3's MAPPING OF THE ATTRIBUTES HTML §4.8.17 "Dimension attributes" DEFINES --------------
       THE ROW THAT MAKES A REAL PAGE RENDER. Hacker News ships `<img src="y18.svg" width="18" height="18">`
       and every engine without this mapping lays it out at CSS 2.1 §10.3.2's 300px — a visible, whole-page
       divergence from one missing row. EVERY NUMBER BELOW IS A LITERAL THIS FIXTURE WROTE: 18 comes from the
       attribute and 300 is the default these same elements take when the attribute does not parse, so a run
       that answers these lines has read the attribute rather than measured anything.
       ALL FOUR IMAGES ARE PENDING FOR THE WHOLE OF THIS BLOCK — §4.8.4.3.5 writes a current URL after a
       MICROTASK, so nothing here can be `complete` in the task that set it — which is HTML §15.4.2 "Images"'
       SECOND rule, a replaced element with no natural dimensions. That is what makes 300 the control: the
       failing arms fall to a number this file already asserts one probe above, and not to a number some
       decoder produced. */
    "var dimA = document.createElement('img'); dimA.src = '/api/dim-a.png';"
    "dimA.setAttribute('width', '18'); dimA.setAttribute('height', '18');"
    "document.body.appendChild(dimA);"
    "fetch('/api/preshintdim?v=' + (getComputedStyle(dimA).width === '18px' &&"
    " getComputedStyle(dimA).height === '18px' ? 'is18' : 'wrong'));"
    /* HTML §2.3.4.4 "Percentages and lengths"' rules for parsing dimension values stop at the first code
       point that is not a digit and then ask ONE question of it — is it U+0025 — so TRAILING CONTENT IS NOT
       AN ERROR and `18px` is eighteen. A parser that required the whole attribute to be a number would
       answer 300 here, which is the arm this separates. */
    "var dimB = document.createElement('img'); dimB.src = '/api/dim-b.png';"
    "dimB.setAttribute('width', '18px');"
    "document.body.appendChild(dimB);"
    "fetch('/api/preshintdimtrail?v=' + (getComputedStyle(dimB).width === '18px' ? 'is18' : 'wrong'));"
    /* A FAILING PARSE IS THE ABSENCE OF THE HINT AND NEVER A DEFAULT. §2.3.4.4 takes no sign — a `-` is not
       an ASCII digit, so its step 4 returns failure — and HTML §15.2 conditions the whole mapping on the
       parse, so the property is simply undeclared by this origin and §10.3.2's last arm answers. §15.3.2's
       8px fallback is that section's own sentence and reaching for it here would be a number invented out of
       a different row. */
    "var dimC = document.createElement('img'); dimC.src = '/api/dim-c.png';"
    "dimC.setAttribute('width', '-5');"
    "document.body.appendChild(dimC);"
    "fetch('/api/preshintdimfail?v=' + (getComputedStyle(dimC).width === '300px' ? 'isdefault' : 'wrong'));"
    /* §2.3.4.4's OTHER CATEGORY — "If the code point at position within input is U+0025 (%), then return
       value as a percentage." This EMITS rather than comparing, because a percentage width resolves against
       the containing block and pinning a number here would assert this fixture's viewport rather than the
       categorisation. What it does hold is that the path ANSWERS: a run in which this row is absent is a run
       that never reached the block. */
    "var dimD = document.createElement('img'); dimD.src = '/api/dim-d.png';"
    "dimD.setAttribute('width', '50%');"
    "document.body.appendChild(dimD);"
    "fetch('/api/preshintdimpct?w=' + encodeURIComponent(getComputedStyle(dimD).width));"
    /* AN `img` THAT IS NOT BEING RENDERED takes determine-the-dimensions' THIRD step — "return a width of 0
       and a height of 0" — which is the algorithm's own answer and not a fallback. `display: none` is the
       cheapest way to have a connected element with no box. */
    "var rh = document.createElement('img'); rh.src = '/api/replaced-hidden.png';"
    "rh.style.setProperty('display', 'none'); document.body.appendChild(rh);"
    "fetch('/api/replacedunrendered?v=' + (rh.width === 0 && rh.height === 0 ? 'isnobox' : 'wrong'));"
    /* THE WORDPRESS SHAPE, WHICH IS WHY THIS WHOLE BLOCK EXISTS. §4.8.4.3.11 "Parsing a sizes attribute" step
       3.3 substitutes the CONCRETE OBJECT SIZE WIDTH for a `sizes="auto"` on an img that is being rendered and
       ALLOWS AUTO-SIZES (§4.8.3: `loading` in the Lazy state and `sizes` starting `auto,`), and css-images-3
       §4.5 "Sizing Objects: the object-fit property" makes that the element's used width under the initial
       `fill`. That is 300 here, so the 400w candidate is the narrowest that still covers it and the selected
       source is the endpoint this emits. Before §10.3.2 existed this call reached used_value.c and aborted. */
    "var sa = document.createElement('img');"
    "sa.setAttribute('loading', 'lazy');"
    "sa.setAttribute('sizes', 'auto, (max-width: 600px) 100vw, 800px');"
    "sa.setAttribute('srcset', '/api/sizes-200.png 200w, /api/sizes-400.png 400w, /api/sizes-900.png 900w');"
    "document.body.appendChild(sa);"
    "fetch('/api/sizesauto?src=' + encodeURIComponent(sa.currentSrc));"
    /* A MATH FUNCTION AS A `<source-size-value>`, which §4.8.4.2.2 "Sizes attributes" admits by name ("must not
       use CSS functions other than the math functions") and which css-values-4 §10 makes a `<length>` of
       whichever type its operands give it. This is the END-TO-END path — §4.8.4.3.11's tokenizer into
       core/css/css_math.c's §10.8 grammar into core/css/media_query.c's unit table — and it EMITS rather than
       comparing, because which candidate wins is a joint function of the modelled viewport and the modelled
       device pixel ratio and pinning it here would assert this fixture's environment rather than §10's
       arithmetic. What it does hold is that the path ANSWERS: every DCHECK on it fires as an abort, and before
       §10 existed this attribute reached a DFAIL naming the missing grammar. */
    "var sc = document.createElement('img');"
    "sc.setAttribute('sizes', 'calc(100vw - 40px)');"
    "sc.setAttribute('srcset', '/api/calc-320.png 320w, /api/calc-640.png 640w, /api/calc-1280.png 1280w');"
    "document.body.appendChild(sc);"
    "fetch('/api/sizescalc?src=' + encodeURIComponent(sc.currentSrc));"
    /* A COMMA INSIDE A FUNCTION, WHICH IS THE COMPONENT-VALUE BOUNDARY ITSELF and is a different assertion from
       the row above it. §4.8.4.3.11 step 1 cuts the attribute into a COMMA-SEPARATED LIST OF COMPONENT VALUES,
       and CSS Syntax Level 3 §5.5.8 "Consume a component value" makes a `<function-token>` ONE of them —
       §5.5.10 "Consume a function" runs it to its `)` — so the comma in `min(40px, 40px)` separates nothing.
       THE TWO CUTS ARE DISTINGUISHABLE IN WHAT THIS EMITS, which is why the trailing `99999px` is here rather
       than being tidied away: cut correctly, the list is ONE entry, the source size is 40 and the narrow
       candidate is selected; cut on every comma it is THREE (`calc(min(40px`, `40px))`, `99999px`), the first
       two fail to parse, and §4.8.4.3.11 falls through to a source size of 99999 and the wide one. So the URL
       in this call names which walk ran.
       IT EMITS RATHER THAN COMPARING for the reason the row above states — which candidate wins is a joint
       function of the modelled viewport and device pixel ratio, and pinning it would assert this fixture's
       environment rather than the cut. What it holds unconditionally is that the path ANSWERS: this shape is
       what fired iss_source_size_value's own two-sided assert the hour `calc()` started parsing, because the
       span check consumed ONE TOKEN where the cut had consumed one COMPONENT VALUE. */
    "var sn = document.createElement('img');"
    "sn.setAttribute('sizes', 'calc(min(40px, 40px)), 99999px');"
    "sn.setAttribute('srcset', '/api/nest-40.png 40w, /api/nest-4000.png 4000w');"
    "document.body.appendChild(sn);"
    "fetch('/api/sizesnested?src=' + encodeURIComponent(sn.currentSrc));"
    /* §10.7's PERCENTAGE ESCAPE, which is a RULE and not a missing basis: "if the height of the containing
       block is not specified explicitly (i.e., it depends on content height), and this element is not
       absolutely positioned, the percentage value is treated as '0' (for 'min-height') or 'none' (for
       'max-height')." So `max-height: 50%` inside an `auto`-height parent does NOTHING — the box keeps its
       200px — while the same declaration inside a 400px parent clamps a 200px box up to its 300px floor. The
       pair is one test because a basis invented for the first case would silently change the first number. */
    /* Every div here declares `display: block` over this document's `div { display: none }`: CSSOM §9
       "Resolved Values" answers height's computed value, not its used value, where display is none. */
    "var hblk = function(el){ el.style.setProperty('display', 'block'); return el; };"
    "var ha = hblk(document.createElement('div')); document.body.appendChild(ha);"
    "var hax = hblk(document.createElement('div')); ha.appendChild(hax);"
    "hax.style.setProperty('height', '200px'); hax.style.setProperty('max-height', '50%');"
    "var hd = hblk(document.createElement('div')); document.body.appendChild(hd);"
    "hd.style.setProperty('height', '400px');"
    "var hdx = hblk(document.createElement('div')); hd.appendChild(hdx);"
    "hdx.style.setProperty('height', '200px'); hdx.style.setProperty('min-height', '75%');"
    "fetch('/api/clamppcth?v=' + (getComputedStyle(hax).height === '200px' &&"
    " getComputedStyle(hdx).height === '300px' ? 'isindefinite' : 'wrong'));"
    /* §10.7 OVER §10.6.3's CONTENT-BASED HEIGHT, which is the one path where the tentative value comes from a
       walk rather than from a declaration — and the CHILD's clamp is what this asserts. The inner box has no
       content and a 120px floor, so its border box is 120; the outer box's `auto` height is that stack. A
       child that reported its walk's 0 unclamped would make the OUTER height wrong too, with nothing
       downstream to say so. */
    "var hw = hblk(document.createElement('div')); document.body.appendChild(hw);"
    "var hwi = hblk(document.createElement('div')); hw.appendChild(hwi);"
    "hwi.style.setProperty('min-height', '120px');"
    "fetch('/api/clampwalk?v=' + (getComputedStyle(hwi).height === '120px' &&"
    " getComputedStyle(hw).height === '120px' ? 'iswalk' : 'wrong'));"
    /* AND THE ONE THAT MUST NOT BE COMPARED. `min(50vw, 400px)` picks 400 at the modelled viewport and 50vw at
       a narrower one, so css_length.h's rule is that the clamped used width carries the ICB's fact even though
       400 won here — "the operand that lost at this viewport is the one that wins at another". Emitting it is
       what states that: a `===` here would fork the world instead of deciding, and a number that had gone
       CONCRETE would mean `css_px_min`'s union had been dropped somewhere in §10.4's substitution. */
    "var cv = document.createElement('div'); document.body.appendChild(cv);"
    "cv.style.setProperty('max-width', 'min(50vw, 400px)');"
    "fetch('/api/clampviewport?w=' + getComputedStyle(cv).width);"
    /* ---- css-fonts-4 §2.5's computed `font-size` and css-values-4 §6.1.1's `em`/`rem` --------------------
       EVERY VALUE COMPARED HERE IS ROOTED IN A DECLARED ABSOLUTE FONT SIZE, deliberately: an undeclared one is
       §2.5's `medium`, which IS the CSS_ENV_DEFAULT_FONT_SIZE fact, so its computed value is a CONCOLIC and a
       `===` against it would fork rather than decide — the same reason /api/cssjoint one probe up emits
       instead of comparing. Every arithmetic below is exact in binary64 (20 x 1.2 is 24.0, not 23.999…), so a
       serialization that round-trips is the only thing the equality depends on.
       §2.5's `Percentages:` line is "refer to parent element's font size" and §2.5's `<relative-size>` is
       "relative to the computed font-size of the parent element", so `150%` and `larger` are both a ratio of
       the PARENT's 20px — and so is a `1.5em` INSIDE `font-size`, which is the case §6.1.1's font-affecting
       rule decides and the one an implementation gets wrong by resolving against the element's own size. The
       same `2em` in a `margin-top` is NOT font-affecting and resolves against the element's OWN 20px, which is
       what makes the pair a test rather than two spellings of one number. */
    "var fsp = document.createElement('div'); document.body.appendChild(fsp);"
    "fsp.style.setProperty('font-size', '20px'); fsp.style.setProperty('margin-top', '2em');"
    "var fse = document.createElement('div'); fsp.appendChild(fse);"
    "fse.style.setProperty('font-size', '1.5em');"
    "var fsq = document.createElement('div'); fsp.appendChild(fsq);"
    "fsq.style.setProperty('font-size', '150%');"
    "var fsl = document.createElement('div'); fsp.appendChild(fsl);"
    "fsl.style.setProperty('font-size', 'larger');"
    "fetch('/api/fontsize?v=' + (getComputedStyle(fsp).fontSize === '20px' &&"
    " getComputedStyle(fse).fontSize === '30px' && getComputedStyle(fsq).fontSize === '30px' &&"
    " getComputedStyle(fsl).fontSize === '24px' && getComputedStyle(fsp).marginTop === '40px'"
    " ? 'isfs' : 'wrong'));"
    /* §6.1.1's `rem` is "the computed value of the em unit on the ROOT element", so the root's own font size is
       declared here for the same reason the parent's is above — an undeclared root font size is the default
       fact and the answer would carry its domain. */
    "document.documentElement.style.setProperty('font-size', '10px');"
    "var fsr = document.createElement('div'); document.body.appendChild(fsr);"
    "fsr.style.setProperty('margin-top', '2rem');"
    "fetch('/api/fontrem?v=' + (getComputedStyle(fsr).marginTop === '20px' ? 'isrem' : 'wrong'));"
    /* AND THE SELF-REFERENTIAL PAIR, LAST because it changes the root's font size out from under the probe
       above. §6.1.1: inside a font-affecting property on the element the unit REFERS TO, the base is the
       parent's metrics "or … the computed metrics corresponding to the initial values of the font and
       line-height properties, if the element has no parent" — so `html { font-size: 2rem }` is twice the
       INITIAL font size and not twice itself, and the root has no parent element. There is no token to compare
       against (the base is the default-font-size fact), and what this asserts is that the walk TERMINATED at
       all: resolving either unit against the element's own computed font-size here is not a wrong number, it
       is a definition of itself. */
    "document.documentElement.style.setProperty('font-size', '2rem');"
    "fetch('/api/fontroot?s=' + getComputedStyle(document.documentElement).fontSize);"
    /* element.style WRITES go through setAttribute's chokepoint, so an inline style time-travels like every
       other DOM write — and [SameObject] means the page gets the same declaration back each read. */
    "cs.style.color = 'red'; cs.style.setProperty('padding-left', '2px');"
    "fetch('/api/cssset?v=' + (cs.style.color === 'red' && cs.getAttribute('style').indexOf('padding-left') >= 0"
    " && cs.style === cs.style ? 'isset' : 'wrong'));"
    "cs.style.removeProperty('color');"
    "fetch('/api/cssdel?v=' + (cs.style.color === '' && cs.style.length >= 1 ? 'isdel' : 'wrong'));"
    /* CSSOM §6.6.1 The CSSStyleDeclaration Interface's indexed property getter, `getter CSSOMString
       item(unsigned long index)`, told apart from a wired-but-wrong one and not only from an absent one. A
       fresh element gets three longhands (none a shorthand setProperty would expand) in a known order, so the
       supported property indices are exactly 0, 1, 2:
         · position — [0]/[1]/[2] are the names in insertion order, which a set-equality check would pass;
         · the boundary is the getter's — indices run to "one less than the number of CSS declarations", so
           `[3]` is undefined and `3 in style` false while `item(3)` is the empty string, and a getter
           forwarding to `item` fails exactly here;
         · the index parse — `'01'` and `'-1'` are not array index property names;
         · enumeration — Object.keys is the indices in order, and Web IDL §3.7.9 Iterable declarations' step 1
           gives an indexed getter %Array.prototype.values% as @@iterator, so spread and for..of agree;
         · live — after removeProperty, [0] shifts and index 2 stops being supported, which a snapshot taken
           at mint fails. */
    "var ix = document.createElement('div'); document.body.appendChild(ix);"
    "ix.style.setProperty('color', 'red');"
    "ix.style.setProperty('padding-left', '2px');"
    "ix.style.setProperty('margin-top', '3px');"
    "var ixspread = [].concat.apply([], [[...ix.style]]).join(',');"
    "var ixfor = []; for (var ixt of ix.style) ixfor.push(ixt);"
    "fetch('/api/cssidx?v=' + ("
    "   ix.style.length === 3"
    " && ix.style[0] === 'color' && ix.style[1] === 'padding-left' && ix.style[2] === 'margin-top'"
    " && ix.style[0] === ix.style.item(0) && ix.style[2] === ix.style.item(2)"
    " && ix.style[3] === undefined && ix.style.item(3) === ''"
    " && (2 in ix.style) && !(3 in ix.style)"
    " && ix.style['01'] === undefined && ix.style['-1'] === undefined"
    " && Object.keys(ix.style).join(',') === '0,1,2'"
    " && ixspread === 'color,padding-left,margin-top'"
    " && ixfor.join(',') === 'color,padding-left,margin-top'"
    " ? 'isidx' : 'wrong'));"
    "ix.style.removeProperty('color');"
    "fetch('/api/cssidxlive?v=' + ("
    "   ix.style.length === 2 && ix.style[0] === 'padding-left' && ix.style[1] === 'margin-top'"
    " && ix.style[2] === undefined && !(2 in ix.style)"
    " ? 'islive' : 'wrong'));"
    /* A computed declaration is READ-ONLY, which the spec makes an error rather than a silent no-op. */
    "var csro = 'nothrow'; try { csc.setProperty('color', 'blue'); } catch (e) { csro = 'isro'; }"
    "fetch('/api/cssro?v=' + csro);"
    /* §8.4 THE FRAGMENT SERIALISER. innerHTML was WRITE-ONLY, so every read answered undefined — and undefined
       does not throw, it PROPAGATES: `wrap.innerHTML = head.innerHTML + row` builds the string "undefined…"
       and the page carries on, so the engine reported a surface assembled out of a value the page never had.
       outerHTML is the same serialiser over the element itself. */
    "var sz = document.createElement('section');"
    "sz.setAttribute('data-k', 'v');"
    "sz.innerHTML = '<p class=\\'q\\'>hi<br></p>';"
    "fetch('/api/serialize?in=' + encodeURIComponent(sz.innerHTML)"
    " + '&out=' + encodeURIComponent(sz.outerHTML));"
    /* §4.9 THE OTHER HTML-CONTEXT SINKS. insertAdjacentHTML and outerHTML= parse markup exactly as innerHTML
       does and were absent entirely — so a bundle using them had its DOM unbuilt AND its XSS invisible, which
       is the pair this engine exists to report. The four positions are one algorithm; what differs is where
       the fragment lands and, for the outside two, that it is parsed in the PARENT's context. */
    "var ia = document.createElement('div'); document.body.appendChild(ia);"
    "var iac = document.createElement('p'); ia.appendChild(iac);"
    "iac.insertAdjacentHTML('beforebegin', '<i>bb</i>');"
    "iac.insertAdjacentHTML('afterbegin', '<b>ab</b>');"
    "iac.insertAdjacentHTML('beforeend', '<u>be</u>');"
    "iac.insertAdjacentHTML('afterend', '<s>ae</s>');"
    "iac.insertAdjacentText('afterbegin', 'T');"
    "iac.insertAdjacentElement('beforeend', document.createElement('q'));"
    "fetch('/api/adjacent?v=' + encodeURIComponent(ia.innerHTML));"
    /* §13.4: the CONTEXT decides what survives. A `<td>` parsed against a `<tr>` is kept; the same markup
       parsed against a `<div>` is dropped by the tree builder, which is why the context is not a caller's
       choice. Only the row's own innerHTML can show it. */
    "var tb = document.createElement('table'); var tr = document.createElement('tr');"
    "tb.appendChild(tr); document.body.appendChild(tb);"
    "tr.insertAdjacentHTML('beforeend', '<td>cell</td>');"
    "fetch('/api/fragctx?v=' + encodeURIComponent(tr.innerHTML));"
    /* outerHTML= REPLACES the element in its parent, so the old one is gone and the new markup is in its place. */
    "var oh = document.createElement('div'); document.body.appendChild(oh);"
    "var ohk = document.createElement('span'); ohk.setAttribute('id','goneid'); oh.appendChild(ohk);"
    "ohk.outerHTML = '<h1>replaced</h1>';"
    "fetch('/api/outerset?v=' + encodeURIComponent(oh.innerHTML));"
    /* §13.2 A HEADING END TAG, which lexbor v2.4.0 mis-tokenised in FRAGMENT parsing only: `</h1>` lost its
       digit to a bogus comment and never closed the heading, so everything after it was nested INSIDE it. The
       document parse was clean, which is why nothing caught it until innerHTML could be READ. Both halves are
       asserted, because the version pin is what makes them true. */
    "var pz = document.createElement('div');"
    "pz.innerHTML = '<h1>a</h1>tail';"
    "fetch('/api/heading?v=' + encodeURIComponent(pz.innerHTML)"
    " + '&d=' + encodeURIComponent(document.querySelector('#dh').innerHTML));"
    /* §4.10 `<template>`'s CHILDREN ARE NOT UNDER IT. They live on its content fragment, whose node has no
       parent, so the serialiser's walk has to leave the tree it is on and come back — lexbor recurses there,
       and a machine whose C stack is gone at every suspension has to push instead. Nested, because one level
       of that push proves nothing about the stack. */
    "var tp = document.createElement('div');"
    "tp.innerHTML = '<template><i>x</i><template><b>y</b></template>z</template>after';"
    "fetch('/api/template?v=' + encodeURIComponent(tp.innerHTML));"
    /* A WALK THAT SUSPENDS MANY TIMES. One node per step is only correct if the resume comes back to the node
       it was on: a cursor that restarts duplicates the prefix, one that over-advances drops a node, and either
       shows up as the wrong bytes. 64 nested elements is 128 steps and the same number of suspensions, and the
       nesting is what makes an ascent bug visible — a flat list ascends once. */
    "var dp = document.createElement('div'); var dpc = dp;"
    "for (var dpi = 0; dpi < 64; dpi++) { var dpn = document.createElement('b'); dpc.appendChild(dpn); dpc = dpn; }"
    "dpc.appendChild(document.createTextNode('deep'));"
    "var dps = dp.innerHTML;"
    "fetch('/api/serdeep?o=' + (dps.split('<b>').length - 1) + '&c=' + (dps.split('</b>').length - 1)"
    " + '&t=' + (dps.indexOf('deep') > 0 ? 'once' : 'lost'));"
    /* §4.9: a position that is not one of the four is a SyntaxError, and an outside position with no element
       parent is a NoModificationAllowedError. Neither is a quiet no-op. */
    "var iaerr = 'wrong'; try { iac.insertAdjacentHTML('nowhere', 'x'); } catch (e) { iaerr = 'issyn'; }"
    "fetch('/api/adjbad?v=' + iaerr);"
    /* THE SINK HALF. Attacker input reaching insertAdjacentHTML is the same HTML-context breakout innerHTML
       reports — a sink that was invisible because the member did not exist. */
    "document.createElement('div').insertAdjacentHTML('beforeend', '<div>' + state.html2 + '</div>');"
    /* §4.2.7/§4.2.8 the ChildNode and ParentNode mixins. A bundle that builds its UI with append() and tears it
       down with remove() had NONE of it — the page's own call threw and every fetch behind that render never
       happened. A STRING argument becomes a Text node, which is the whole reason they take (Node or DOMString). */
    "var mx = document.createElement('ul'); document.body.appendChild(mx);"
    "var li1 = document.createElement('li'); var li2 = document.createElement('li');"
    "mx.append(li1, 'tail'); mx.prepend(li2);"
    /* §4.2.4's union, through the DECLARATION. `(Node or DOMString)...` is variadic, so EVERY argument is
       brand-checked and the non-Nodes are coerced before the body runs — and the coercion is the page's code
       with a loop in it, which used to be a JS_ToCStringLen from C with no flow base to park into. Nine
       arguments, because a fixed argument array is a cap and `ul.append(...items)` is how a list renders. */
    "var mv = document.createElement('ol'); document.body.appendChild(mv);"
    "var mvt = { toString: function(){ var n = 0; for (var i = 0; i < 60; i++) n += i; return 'n' + n; } };"
    "mv.append(document.createElement('li'), mvt, 'a', 'b', 'c', 'd', 'e', 'f', 'g');"
    "fetch('/api/variadic?v=' + encodeURIComponent(mv.innerHTML));"
    "li1.before(document.createElement('hr'));"
    "var mid = document.createElement('em'); li1.after(mid);"
    "fetch('/api/mixin?v=' + encodeURIComponent(mx.innerHTML));"
    "mid.replaceWith(document.createElement('b'));"
    "li2.remove();"
    "fetch('/api/mixin2?v=' + encodeURIComponent(mx.innerHTML));"
    "mx.replaceChildren(document.createElement('span'));"
    "fetch('/api/mixin3?v=' + encodeURIComponent(mx.innerHTML));"
    /* THE INDEX CACHE, and the two ways it can be wrong. A collection has nothing to cache the tree in, so
       `list[i]` is a walk of i and the loop every page writes over one is quadratic in the page's own markup;
       the cache is keyed on the tree version, exactly as Blink's CollectionIndexCache is on the document's.
       (1) SEQUENTIAL: 200 children read forwards then backwards — 400 reads that were 40,000 link steps, and
       a cursor that steps the wrong way or fails to move gives the wrong join. (2) INVALIDATION: insert at the
       FRONT between two reads. Every index shifts by one, the cached member is still a child of the same owner
       so no cheap check catches it, and only the version bump makes the second read right. */
    "var ic = document.createElement('div'); document.body.appendChild(ic);"
    "for (var ici = 0; ici < 200; ici++) {"
      "var icn = document.createElement('b'); icn.setAttribute('id', 'n' + ici); ic.appendChild(icn); }"
    "var icl = ic.children, icf = '', icb = '';"
    "for (var icj = 0; icj < icl.length; icj++) icf += icl[icj].getAttribute('id');"
    "for (var ick = icl.length - 1; ick >= 0; ick--) icb += icl[ick].getAttribute('id');"
    "var icv = icl[7].getAttribute('id');"
    "ic.insertBefore(document.createElement('b'), ic.firstChild);"
    "fetch('/api/idxcache?f=' + (icf.indexOf('n0n1n2') === 0 && icf.lastIndexOf('n199') === icf.length - 4"
    " ? 'fwd' : 'wrong') + '&b=' + (icb.indexOf('n199n198') === 0 ? 'back' : 'wrong')"
    " + '&i=' + (icv === 'n7' && icl[7].getAttribute('id') === 'n6' && icl.length === 201 ? 'shifted' : 'stale'));"
    /* §4.2.6 THE MIXIN IS ONE THING. Document had querySelector and querySelectorAll but NOT children,
       firstElementChild, lastElementChild or childElementCount — the IDL puts all of them on ParentNode, so a
       page reading `document.children` got undefined and took the branch behind it. And Document's
       querySelector ignored its receiver entirely, scoping every lookup to the global document's root, so a
       SCOPED lookup off an element that happened to reach document.c's copy searched the whole page. */
    "var pn = document.createElement('div'); document.body.appendChild(pn);"
    "pn.innerHTML = '<p id=\"pna\"></p>txt<span id=\"pnb\"></span>';"
    "fetch('/api/parentmixin?dc=' + (document.children.length === 1"
    " && document.firstElementChild === document.documentElement"
    " && document.childElementCount === 1 ? 'doc' : 'wrong')"
    " + '&el=' + (pn.children.length === 2 && pn.childNodes.length === 3"
    " && pn.firstElementChild.getAttribute('id') === 'pna'"
    " && pn.lastElementChild.getAttribute('id') === 'pnb' ? 'el' : 'wrong')"
    /* the scope is the RECEIVER: `pn` has no <body> under it, the document does. */
    " + '&sc=' + (pn.querySelector('body') === null && document.querySelector('body') === document.body"
    " && pn.querySelectorAll('p').length === 1 ? 'scoped' : 'wrong'));"
    /* ---- §4.2.6 moveBefore / §4.2.3 move — ONE CONTIGUOUS BLOCK, /api/movebefore* ------------------------
       THE POINT OF THE MEMBER IS THE STATE IT DOES NOT DESTROY, so the assertion that matters is the one a
       remove-then-insert `moveBefore` would fail while getting the tree shape right: a custom element that is
       MOVED gets `connectedMoveCallback` and does NOT get the disconnected/connected pair. HTML §4.13.2.1
       "Preserving custom element state when moved" is that behaviour, and a page that loses it silently loses
       whatever the callbacks reset — an observer, a tab index, an iframe's document.
       The three validity assertions are the ones where move's six steps DIVERGE from pre-insert's eleven:
       step 1 (one shadow-including root, which is what makes every disconnected↔connected move throw) has no
       pre-insert counterpart at all, and step 4 refuses a DocumentFragment and a DocumentType that pre-insert
       validity step 4 admits. */
    "var mbCe = [];"
    "customElements.define('x-moved', class extends HTMLElement {"
      "connectedCallback(){ mbCe.push('c'); }"
      "disconnectedCallback(){ mbCe.push('d'); }"
      "connectedMoveCallback(){ mbCe.push('m'); } });"
    "var mbA = document.createElement('div'); document.body.appendChild(mbA);"
    "var mbB = document.createElement('div'); var mbC = document.createElement('div');"
    "mbA.append(mbB, mbC);"
    "var mbR = mbA.moveBefore(mbC, mbB);"
    "var mbOrder = mbA.firstChild === mbC && mbA.lastChild === mbB && mbR === undefined;"
    /* §4.2.6 step 2: a reference child that IS the node becomes its next sibling, so this is a no-op. */
    "mbA.moveBefore(mbC, mbC); mbA.moveBefore(mbB, mbB);"
    "var mbSelf = mbA.firstChild === mbC && mbA.lastChild === mbB;"
    "fetch('/api/movebefore?v=' + (mbOrder && mbSelf ? 'ismoved' : 'wrong'));"
    "var mbThrew = '';"
    /* STEP 1, the step pre-insert does not have: a node with no parent is its own shadow-including root, so
       every disconnected→connected move is a HierarchyRequestError where the same appendChild succeeds. */
    "try { document.body.moveBefore(document.createElement('p'), null); mbThrew += 'none'; }"
      "catch (e) { mbThrew += e.name; }"
    /* …and a DocumentFragment is always its own root, so step 1 answers before step 4 ever can. That is not a
       redundancy: it is why step 4's fragment arm is unreachable and its DOCTYPE arm is not. */
    "try { document.body.moveBefore(new DocumentFragment(), null); mbThrew += ':none'; }"
      "catch (e) { mbThrew += ':' + e.name; }"
    /* STEP 3's NotFoundError, over a reference child of a different parent. */
    "try { mbA.moveBefore(mbB, document.body.firstChild); mbThrew += ':none'; }"
      "catch (e) { mbThrew += ':' + e.name; }"
    /* STEP 4 IS ONLY REACHABLE FOR A DOCTYPE, and only for one already in this tree — `document.doctype` is
       the only such node there is. Its presence is reported beside the throw rather than assumed, because a
       null one would make this a TypeError from the IDL and read as a pass by name. */
    "var mbDt = document.doctype;"
    "try { document.body.moveBefore(mbDt, null); mbThrew += ':none'; } catch (e) { mbThrew += ':' + e.name; }"
    "fetch('/api/movebeforethrow?v=' + encodeURIComponent(mbThrew)"
    " + '&doctype=' + (mbDt ? 'has' : 'null'));"
    /* DOM §4.6's three DocumentType attributes asked of a receiver that is not one — Web IDL §3.7.6
       "Attributes"' attribute getter, whose foreign-receiver arm is "Otherwise, throw a TypeError." They
       install through core/idl_args.h's idl_install_accessor_this, which states document_type_is at the
       declaration and performs Web IDL §3.7 Interfaces' implementation check before the body.
       Three receivers, three refusals: the interface prototype carries no node class (a page's
       `DocumentType.prototype.name`); a bare object is a corpus's `.call({})`; `document.body` is a node
       wrapper of the right class with the wrong node type, the one a class comparison alone would admit.
       It lives in this program so reachability is witnessed: `mbDt` is in hand and `/api/movebeforethrow`
       proves the program ran, so a silent `/api/doctypebrand` is the request being absent.
       `r` and `ok` are literals: a payload composed from a value the solver can make unknown is a request
       that never goes out. `v` carries the names for diagnosis. The positive half is on the same line,
       because a getter that threw for everything would satisfy the three refusals. */
    "var dtB = '';"
    "var dtG = Object.getOwnPropertyDescriptor(DocumentType.prototype, 'name').get;"
    "try { dtG.call(DocumentType.prototype); } catch (e) { dtB += e.name; }"
    "try { dtG.call({}); } catch (e) { dtB += ':' + e.name; }"
    "try { dtG.call(document.body); } catch (e) { dtB += ':' + e.name; }"
    "fetch('/api/doctypebrand?v=' + encodeURIComponent(dtB)"
    " + '&r=' + (dtB === 'TypeError:TypeError:TypeError' ? 'threw3' : 'not3')"
    " + '&ok=' + (mbDt && mbDt.name === 'html' ? 'named' : 'wrong'));"
    /* THE STATE-PRESERVING HALF. The element is appended (one `c`), then moved between two connected parents
       (one `m` and nothing else). A move written as remove-then-insert reads 'cdc' here and the tree looks
       identical either way, which is the whole reason this is the assertion. */
    "var mbP1 = document.createElement('div'); var mbP2 = document.createElement('div');"
    "document.body.append(mbP1, mbP2);"
    "var mbEl = document.createElement('x-moved'); mbP1.appendChild(mbEl);"
    "mbP2.moveBefore(mbEl, null);"
    "fetch('/api/movebeforece?v=' + (mbCe.join('') === 'cm' && mbEl.parentNode === mbP2"
    " ? 'ispreserved' : mbCe.join('') || 'none'));"
    /* THE OTHER SIDE OF THE SAME MOVE — §4.13.6 "enqueue a custom element callback reaction" STEP 3, which is
       what a class with NO `connectedMoveCallback` gets. Step 3.4 synthesizes a callback that calls
       disconnectedCallback and then connectedCallback, so the move that reads 'm' above reads 'dc' here, and
       the three arms below are step 3's three outcomes. It is the same tree edit either way, which is why the
       callback trace is the only thing that can tell them apart.
       ONE: the synthesis fires, in order. Appended is 'c', moved is 'd' then 'c' — 'cdc'.
       TWO: step 3.3's "If connectedCallback and disconnectedCallback are null, then return" — a class with
       none of the three is a move that enqueues NOTHING, and the counter proves it by staying 0 rather than
       by nothing being observed.
       THREE: step 3.4.1's call has no exception behavior stated, so Web IDL § 3.12 supplies rethrow — the
       throw leaves the synthesized steps, step 1.3.1's own "report" reports it once, and 3.4.2 NEVER RUNS.
       That is the assertion that separates ONE reaction holding two calls from TWO queued reactions, which
       would run connectedCallback anyway and read 'cdc'. This one reads 'cd'. */
    "var mbSyn = [];"
    "customElements.define('x-synmoved', class extends HTMLElement {"
      "connectedCallback(){ mbSyn.push('c'); }"
      "disconnectedCallback(){ mbSyn.push('d'); } });"
    "var mbS = document.createElement('x-synmoved'); mbP1.appendChild(mbS);"
    "mbP2.moveBefore(mbS, null);"
    "var mbNone = 0;"
    "customElements.define('x-nonemoved', class extends HTMLElement {"
      "adoptedCallback(){ mbNone++; } });"
    "var mbN = document.createElement('x-nonemoved'); mbP1.appendChild(mbN);"
    "mbP2.moveBefore(mbN, null);"
    "var mbThrow = [];"
    "customElements.define('x-throwmoved', class extends HTMLElement {"
      "connectedCallback(){ mbThrow.push('c'); }"
      "disconnectedCallback(){ mbThrow.push('d'); throw new Error('synmove'); } });"
    "var mbT = document.createElement('x-throwmoved'); mbP1.appendChild(mbT);"
    "mbP2.moveBefore(mbT, null);"
    /* ONE VALUE AND NOT FOUR PARAMS, because the row that reads this is `emitted_record_has` — a substring
       test inside the ONE record — so four params are four needles a row cannot ask for together. Joined with
       `-`, which survives the query unencoded, the way /api/ifacestep's chain does. */
    "fetch('/api/movebeforesyn?v=' + (mbSyn.join('') || 'none') + '-' + mbNone"
    " + '-' + (mbThrow.join('') || 'none')"
    " + '-' + (mbS.parentNode === mbP2 && mbT.parentNode === mbP2 ? 'moved' : 'wrong'));"
    /* §13.4 A PARSE OF THE PAGE'S SIZE. The tokeniser is fed ONE BYTE per step, so this assignment suspends
       about two thousand times — and a resume that loses the tokeniser's position gives a DIFFERENT TREE, not
       a slower one, which is what the counts below catch. The markup is built by doubling rather than by a
       60-iteration loop because the loop is what forced execution explores, not the parse. */
    "var bigOne = '<li class=\"r\"><b>c</b>t</li>';"
    "for (var bpI = 0; bpI < 4; bpI++) bigOne += bigOne;"
    "var bigHost = document.createElement('ul'); document.body.appendChild(bigHost);"
    "bigHost.innerHTML = bigOne + '<li class=\"last\"><b>z</b></li>';"
    "fetch('/api/bigparse?n=' + bigHost.children.length + '&b=' + bigHost.querySelectorAll('b').length"
    " + '&last=' + bigHost.lastElementChild.getAttribute('class')"
    " + '&len=' + bigOne.length);"
    /* IDENTITY AT SCALE. One JS object per Lexbor node is what makes `n === n` true, and the map behind it is
       keyed by the node's address — so a hash that mishandles a collision hands out a SECOND object for a node
       and every comparison against the first is silently false. A page hits this constantly (a Set of visited
       nodes, a WeakMap keyed by node, `el.parentNode === container`), and nothing throws when it breaks.
       Two independent walks over the same 34 nodes, compared both ways. */
    "var idSeen = new Set(), idAll = [], idOk = 1;"
    "var idW = function(el) { idSeen.add(el); idAll.push(el);"
      "for (var c = el.firstElementChild; c; c = c.nextElementSibling) idW(c); };"
    "for (var idI = 0; idI < bigHost.children.length; idI++) idW(bigHost.children[idI]);"
    "for (var idJ = 0; idJ < idAll.length; idJ++) if (!idSeen.has(idAll[idJ])) idOk = 0;"
    "var idB = bigHost.querySelectorAll('b');"
    "for (var idK = 0; idK < idB.length; idK++) if (!idSeen.has(idB[idK])) idOk = 0;"
    "fetch('/api/nodeident?n=' + idSeen.size + '&all=' + idAll.length"
    " + '&ok=' + (idOk && bigHost.firstElementChild === bigHost.children[0] ? 'same' : 'split'));"
    /* §4.2.3 THE INSERTION STEPS, now drained by a machine instead of walked inside the chokepoint.
       (1) ORDERING IS UNCHANGED, which is the whole constraint: the steps run synchronously as part of the
       insertion, so an element is UPGRADED by the time appendChild returns and a method its constructor
       installed is callable on the very next statement. A deferred job would fail this.
       (2) THE WALK IS OF THE PAGE'S SIZE: 120 custom elements nested 120 deep, inserted by ONE appendChild, is
       one call whose steps visit every one of them — and a resume that restarts or skips gives a wrong count. */
    "var upN = 0, upSync = 'notyet';"
    /* The count is reported by the LAST callback rather than read after the inserts: a connected reaction is
       ENQUEUED, so nothing has run yet when appendChild returns. If the walk skips or repeats a node the count
       never reaches 121 and this endpoint is simply never fetched, which is what the oracle checks. */
    "customElements.define('up-x', class extends HTMLElement {"
      "connectedCallback() { upN++; if (upN === 121) fetch('/api/insertsteps?sync=' + upSync + '&deep=' + upN); }"
      "mark() { return 'up'; } });"
    "var upHost = document.createElement('div'); document.body.appendChild(upHost);"
    "var upOne = document.createElement('up-x'); upHost.appendChild(upOne);"
    "upSync = (typeof upOne.mark === 'function') ? upOne.mark() : 'notyet';"
    "var upDeep = document.createElement('div'), upCur = upDeep;"
    "for (var upI = 0; upI < 120; upI++) {"
      "var upE = document.createElement('up-x'); upCur.appendChild(upE); upCur = upE; }"
    "document.body.appendChild(upDeep);"
    /* §4.7 DocumentFragment. It was ABSENT in the worst shape a gap takes: a fragment node HAD a wrapper and
       answered nodeType 11, while every member that makes one useful was undefined — nothing threw, the page
       took the branch behind the undefined, and the engine reported that branch's surface. §4.12.3's
       `template.content` was the same: `t.content.querySelector(...)`, the ordinary way to use a template,
       threw about a property of undefined instead of naming the missing member.
       The mixin is why this is short: DocumentFragment includes ParentNode, so consolidating that first is what
       makes these members exist rather than a third copy of them. */
    "var df = new DocumentFragment();"
    "var dfa = document.createElement('p'); dfa.setAttribute('id', 'dfid');"
    "df.appendChild(dfa); df.appendChild(document.createTextNode('t'));"
    "df.append(document.createElement('b'));"
    "var dupHost = document.createElement('div'); document.body.appendChild(dupHost);"
    "dupHost.innerHTML = '<i id=\"dup\">1</i><u id=\"dup\">2</u>';"
    "var tq = document.createElement('div');"
    "tq.innerHTML = '<template><p class=\"tc\">in</p><i></i></template>';"
    "var tqe = tq.firstChild;"
    "fetch('/api/fragment?i=' + (df instanceof DocumentFragment && df instanceof Node"
    " && df.nodeType === 11 ? 'iface' : 'wrong')"
    " + '&pn=' + (df.children.length === 2 && df.childNodes.length === 3"
    " && df.querySelector('p') === dfa && df.querySelectorAll('p').length === 1"
    " && df.getElementById('dfid') === dfa && df.getElementById('nope') === null ? 'mixin' : 'wrong')"
    /* §4.2.4 is ONE member over its RECEIVER. Document's copy searched a global instead, so a scope that is
       not the document had no way to be wrong — and it took the FIRST in tree order by collecting every match
       and then indexing 0, which is a different algorithm wearing the same name. Both are asserted: the
       fragment's id is invisible to the document, and a DUPLICATE id answers with the first. */
    " + '&scope=' + (document.getElementById('dfid') === null ? 'receiver' : 'global')"
    /* [SameObject]: the fragment's wrapper is ONE object, so a page stashing t.content still has it. */
    " + '&first=' + (document.getElementById('dup').textContent === '1' ? 'tree-order' : 'wrong')"
    " + '&tc=' + (tqe.content === tqe.content && tqe.content instanceof DocumentFragment"
    " && tqe.content.querySelector('.tc').textContent === 'in'"
    " && tqe.content.childElementCount === 2 && tqe.childNodes.length === 0 ? 'content' : 'wrong'));"
    /* §4.5/§4.9 getElementsByTagName / getElementsByClassName — LIVE and over the RECEIVER, both of which the
       old Document-only copy got wrong: it searched a global root and answered with a static Array, which its
       own comment named as a gap, and Element did not have them at all. Liveness is the half a static snapshot
       cannot express: read the length, insert a matching element, read it again. */
    "var gbn = document.createElement('div'); document.body.appendChild(gbn);"
    "gbn.innerHTML = '<i class=\"a b\">1</i><p><i class=\"a\">2</i></p><i class=\"b\">3</i>';"
    "var gbnI = gbn.getElementsByTagName('i'), gbnA = gbn.getElementsByClassName('a');"
    "var gbnAB = gbn.getElementsByClassName('b a'), gbnStar = gbn.getElementsByTagName('*');"
    "var gbn0 = gbnI.length + ':' + gbnA.length + ':' + gbnAB.length + ':' + gbnStar.length;"
    "var gbnNew = document.createElement('i'); gbnNew.setAttribute('class', 'a b'); gbn.appendChild(gbnNew);"
    "var gbn1 = gbnI.length + ':' + gbnA.length + ':' + gbnAB.length + ':' + gbnStar.length;"
    "fetch('/api/byname?before=' + gbn0 + '&after=' + gbn1"
    /* descendant, not children: the <i> inside the <p> counts, and the <p> itself does not match 'i' */
    /* descendant, not children: the <i> inside the <p> counts, and the <p> itself does not match 'i' */
    " + '&deep=' + (gbnI[1].textContent === '2' && gbnI[1].parentNode.tagName === 'P' ? 'descend' : 'wrong')"
    /* §4.5 matches the query case-insensitively in an HTML document — `DIV` is how a lot of older code spells it */
    " + '&ci=' + (gbn.getElementsByTagName('I').length === gbnI.length ? 'nocase' : 'wrong')"
    /* §4.9 tagName is the HTML-UPPERCASED qualified name, and must agree with nodeName, which already was */
    " + '&tag=' + (gbnNew.tagName === 'I' && gbnNew.tagName === gbnNew.nodeName"
    " && gbnNew.localName === 'i' ? 'upper' : 'wrong')"
    /* the RECEIVER scopes it: an <i> outside `gt` is not in `gt`'s collection but is in the document's */
    " + '&scope=' + (document.getElementsByTagName('i').length > gbnI.length ? 'receiver' : 'global')"
    " + '&live=' + (gbnI instanceof HTMLCollection && typeof gbnI.map === 'undefined' ? 'iface' : 'wrong'));"
    "fetch('/api/locsrc?o=' + location.origin + '&pn=' + location.pathname);"
    /* §3.1.7's element shortcuts and §4.5's createDocumentFragment. All five shortcuts are LIVE
       HTMLCollections over the document — a bundle scanner reaches for document.scripts and document.forms in
       particular, and with them absent the loop over them never ran and nothing said why.
       `links` is the one that is a PREDICATE rather than a tag: `a`/`area` WITH an href, so an anchor used as a
       scroll target is not a link. */
    "var scHost = document.createElement('div'); document.body.appendChild(scHost);"
    "scHost.innerHTML = '<form></form><img><a href=\"/l1\">l</a><a name=\"nolink\">n</a>'"
      "+ '<area href=\"/l2\"><embed>';"
    "var scF = document.forms, scL = document.links;"
    "var scBefore = scF.length + ':' + document.images.length + ':' + scL.length"
      "+ ':' + document.embeds.length;"
    "var scMore = document.createElement('form'); scHost.appendChild(scMore);"
    "var scFrag = document.createDocumentFragment();"
    "scFrag.appendChild(document.createElement('b')); scFrag.appendChild(document.createElement('i'));"
    "var scFragN = scFrag.childNodes.length; scHost.appendChild(scFrag);"
    "fetch('/api/docshort?before=' + scBefore + '&after=' + scF.length"
    /* §4.2.3: appending a fragment inserts its CHILDREN, so the last child is the <i> element and not the
       fragment itself — nodeType 1, which was 11 when the fragment node went into the tree instead. */
    " + '&fragnode=' + scHost.lastChild.nodeType"
    /* the anchor with no href is NOT a link, and the <area> with one IS */
    " + '&links=' + scL[0].getAttribute('href') + ',' + scL[1].getAttribute('href')"
    " + '&iface=' + (scF instanceof HTMLCollection && scFrag instanceof DocumentFragment ? 'live' : 'wrong')"
    /* §4.2.3: appending a fragment moves its CHILDREN, leaving the fragment empty */
    " + '&frag=' + scFragN + ':' + scFrag.childNodes.length"
    " + ':' + scHost.getElementsByTagName('b').length);"
    /* §4.9/§4.9.1/§4.9.2 attributes, NamedNodeMap and Attr. `el.attributes` was absent and so was every object
       behind it, which is a gap with a particular shape: the loops a page writes over an element's attributes
       — copying them onto a clone, deciding which to forward — are `for (const a of el.attributes)`, and with
       no attributes there is no iteration and no error either. The loop body simply never ran.
       An Attr IS a node, so it gets identity from the one wrapper table for free; what was missing was a
       PROTOTYPE for node type 2, so an attribute answered nodeType 2 with `name` and `value` undefined. */
    "var atE = document.createElement('div'); document.body.appendChild(atE);"
    "atE.setAttribute('id', 'atid'); atE.setAttribute('data-k', 'v');"
    "var atM = atE.attributes, atSeen = [];"
    "for (var atI = 0; atI < atM.length; atI++) atSeen.push(atM[atI].name + '=' + atM[atI].value);"
    "var atIter = []; for (const a of atE.attributes) atIter.push(a.name);"
    "atM.getNamedItem('data-k').value = 'w';"
    "var atRm = 'none';"
    "try { atM.removeNamedItem('nope'); } catch (e) { atRm = e.name; }"
    "atM.removeNamedItem('id');"
    "fetch('/api/attrs?list=' + atSeen.join(',')"
    " + '&iter=' + atIter.join(',')"
    /* the value setter is setAttribute's change steps, so the element really changed */
    " + '&set=' + atE.getAttribute('data-k')"
    /* §4.9.1: a name that is not there is a NotFoundError, not a quiet no-op */
    " + '&miss=' + atRm + '&removed=' + (atE.getAttribute('id') === null ? 'gone' : 'stayed')"
    /* an Attr is a Node with identity, and [SameObject] holds for the map */
    " + '&iface=' + (atE.attributes === atM && atM instanceof NamedNodeMap"
    " && atM[0] instanceof Attr && atM[0] instanceof Node && atM[0].nodeType === 2"
    " && atM[0] === atM[0] && atM[0].ownerElement === atE ? 'node' : 'wrong')"
    " + '&names=' + atE.getAttributeNames().join(','));"
    /* §3.2.2 dataset. It was ABSENT, so `el.dataset.userId` read undefined — and undefined does not throw, so a
       page storing its routing on data-* took the branch behind it and the engine reported THAT branch's
       surface. The mapping is the spec and it is asymmetric on purpose: `data-user-id` is `userId`, an
       property name with a dash before a lowercase letter is a SyntaxError rather than a new attribute.
       `data-X` is NOT a counter-example to the uppercase rule: §4.9's setAttribute lowercases the qualified
       name in an HTML document, so it is stored as `data-x` and is exposed as `x`, which is what a browser does
       too. The rule about an uppercase surviving `data-` is about a name that could only arrive through a
       namespace-aware setter, and it is implemented for when one lands. */
    "var dsE = document.createElement('div'); document.body.appendChild(dsE);"
    "dsE.setAttribute('data-user-id', 'u7');"
    "dsE.setAttribute('data-X', 'hidden');"
    "dsE.setAttribute('title', 'notdata');"
    "dsE.dataset.roleName = 'admin';"
    "var dsBad = 'none';"
    "try { dsE.dataset['a-b'] = 'x'; } catch (e) { dsBad = e.name; }"
    "var dsKeys = Object.keys(dsE.dataset).sort().join(',');"
    "fetch('/api/dataset?read=' + dsE.dataset.userId"
    /* the write went through setAttribute's chokepoint, so it is a real attribute with the mangled name */
    " + '&wrote=' + dsE.getAttribute('data-role-name')"
    " + '&keys=' + dsKeys"
    /* an uppercase after data- is not a supported property name, and a non-data attribute is not one either */
    /* the case §4.9 lowercased, and an attribute that is not data-* at all */
    " + '&skip=' + (dsE.dataset.X === undefined && dsE.dataset.x === 'hidden'"
    " && dsE.dataset.title === undefined ? 'unexposed' : 'leaked')"
    " + '&bad=' + dsBad"
    " + '&same=' + (dsE.dataset === dsE.dataset ? 'sameobject' : 'fresh')"
    " + '&del=' + ((delete dsE.dataset.userId), dsE.getAttribute('data-user-id') === null ? 'gone' : 'stayed'));"
    /* PROBE §4.2.6 scoped matching: `el.querySelectorAll('div p')` must match a <p> inside el whose <div>
       ancestor is OUTSIDE el — the selector is evaluated against the whole document and only the RESULTS are
       filtered to el's subtree. An implementation that walks el's subtree in isolation misses it. */
    "var scOuter = document.createElement('div'); document.body.appendChild(scOuter);"
    "var scMid = document.createElement('section'); scOuter.appendChild(scMid);"
    "scMid.innerHTML = '<p id=\"scp\">x</p>';"
    "var qsThrew = 'none';"
    "try { scMid.querySelector('###'); } catch (e) { qsThrew = e.name; }"
    "var qaThrew = 'none';"
    "try { document.querySelectorAll(':::'); } catch (e) { qaThrew = e.name; }"
    "fetch('/api/scopesel?anc=' + scMid.querySelectorAll('div p').length"
    " + '&self=' + scMid.querySelectorAll('section p').length"
    " + '&plain=' + scMid.querySelectorAll('p').length"
    /* §4.2.6: an unparseable selector is a SyntaxError from ALL FOUR members. matches and closest already
       threw; the two queries answered null and an empty list, so a page with a typo in a selector was told
       "no such element" and ran the branch behind that answer. */
    " + '&bad=' + qsThrew + ':' + qaThrew"
    /* closest walks INCLUSIVE ancestors and compiles the selector ONCE now, not once per level */
    " + '&closest=' + (scMid.querySelector('p').closest('div') === scOuter"
    " && scMid.querySelector('p').closest('p').tagName === 'P' ? 'up' : 'wrong'));"
    /* §4.4 textContent's READ is a walk of the SUBTREE, so it is a machine now. What it must reproduce exactly
       is WHICH nodes count: Text nodes only, over child links — so a comment contributes nothing, and a
       `<template>`'s content is not part of its element's text because it is not under it. */
    "var tcx = document.createElement('div');"
    "tcx.innerHTML = 'a<b>B</b><!--no--><template>HID</template>c';"
    "fetch('/api/textwalk?v=' + tcx.textContent"
    " + '&deep=' + gbn.textContent"
    " + '&big=' + bigHost.textContent.length);"
    /* DOM §4.9 "Interface Element"'s ID is not the `id` attribute: "If localName is id, namespace is null, and
       value is null or the empty string, then unset element's ID. Otherwise, if localName is id, namespace is
       null, then set element's ID to value." So an element whose only `id`-named attribute is in a non-null
       namespace has no ID, while lexbor's `lxb_dom_element_attr_by_name` compares names and tests no
       namespace. Both doors, `getElementById` and §4.2.10.2's named lookup, are asserted because both go
       through one read, core/dom/element.h's element_id_bytes, and a statement over one would pass with the
       other wrong.
       `has` is the reachability witness and is true either way. `getAttributeNS` arms `ns`: without it a
       pair of `=== null` readings is also what a `setAttributeNS` that stored nothing gives. §4.9's
       "validate and extract" throws for neither, since the qualified name has no prefix. */
    "var idnsH = document.createElement('div'); document.body.appendChild(idnsH);"
    "var idnsY = document.createElement('p'); idnsY.setAttribute('id', 'idnsyes'); idnsH.appendChild(idnsY);"
    "var idnsN = document.createElement('p');"
    "idnsN.setAttributeNS('http://x.test/idns', 'id', 'idnsonly'); idnsH.appendChild(idnsN);"
    "fetch('/api/idns?has=' + (document.getElementById('idnsyes') === idnsY"
    " && idnsH.children.namedItem('idnsyes') === idnsY ? 'isidnshas' : 'wrong')"
    " + '&ns=' + (idnsN.getAttributeNS('http://x.test/idns', 'id') === 'idnsonly'"
    " && document.getElementById('idnsonly') === null"
    " && idnsH.children.namedItem('idnsonly') === null ? 'isidnsns' : 'wrong'));"
    /* §4.2.10.2 Interface HTMLCollection's OTHER TWO STEPS, from the same fetched sentence: "The
       namedItem(key) method steps are: If key is the empty string, return null. Return the first element in
       the collection for which at least one of the following is true: it has an ID which is key; it is in the
       HTML namespace and has a name attribute whose value is key; or null if there is no such element."
       STEP 1 IS NOT REDUNDANT WITH §4.9'S UNSET STEP, and this is the statement that says so: no element's ID
       is the empty string, so the ID clause can never answer an empty key — and the `name` clause can, because
       lexbor hands a `name=""` attribute back as non-NULL data of length 0, so `vlen == nlen` held at zero.
       THE NAMESPACE IS THE ELEMENT'S AND NOT THE ATTRIBUTE'S: an `<svg>` child with `name="nsgname"` is not a
       match however its attribute reads.
       `ctl` IS THE REACHABILITY WITNESS AND THE PROOF THAT THE `name` CLAUSE STILL WORKS, which the two
       null-asserting arms cannot be: an engine that simply stopped matching on `name` would satisfy both of
       them. Each null arm carries its own attribute read-back for the same reason — a `=== null` is otherwise
       confirmed identically by the repair and by a write that stored nothing. */
    "var nskH = document.createElement('div'); document.body.appendChild(nskH);"
    "var nskE = document.createElement('p'); nskE.setAttribute('name', ''); nskH.appendChild(nskE);"
    "var nskP = document.createElement('p'); nskP.setAttribute('name', 'nshname'); nskH.appendChild(nskP);"
    "var nskG = document.createElementNS('http://www.w3.org/2000/svg', 'g');"
    "nskG.setAttribute('name', 'nsgname'); nskH.appendChild(nskG);"
    "fetch('/api/namedsteps?ctl=' + (nskH.children.namedItem('nshname') === nskP"
    " && nskH.children.nshname === nskP ? 'isnsctl' : 'wrong')"
    " + '&key=' + (nskE.getAttribute('name') === ''"
    " && nskH.children.namedItem('') === null ? 'isnskey' : 'wrong')"
    " + '&ns=' + (nskG.getAttribute('name') === 'nsgname'"
    " && nskH.children.namedItem('nsgname') === null"
    " && nskH.children.nsgname === undefined ? 'isnsns' : 'wrong'));"
    /* THE ENGINE'S OWN FIRE goes through the SAME walk. DOMContentLoaded is fired AT THE DOCUMENT and bubbles
       to the window — which used to need the window passed in by hand as a second fire_at, and is now just the
       document's ancestor. A trusted event, which is what tells it from one the page dispatched. */
    "addEventListener('DOMContentLoaded', function(e){"
    " fetch('/api/dclbubble?v=' + (e.target === document && e.currentTarget === globalThis && e.isTrusted"
    " && e.eventPhase === 3 ? 'isdcl' : 'wrong')); });"
    /* §3.2 abort() is SYNCHRONOUS: the listener has run by the time abort() returns, so a flag it set is
       already readable on the next line. A queued fire answers that question after the caller returned, which
       is what this used to do. The listener holds a LOOP, so the dispatch suspends inside it and abort()
       resumes at the listener it parked on — synchronous does not mean uninterruptible. */
    "var sc = new AbortController(); var sflag = 0;"
    "sc.signal.addEventListener('abort', function(){ var n = 0; for (var i = 0; i < 400; i++) { n += i; }"
    " sflag = n; });"
    "sc.abort();"
    "fetch('/api/abortsync?v=' + (sflag === 79800 && sc.signal.aborted ? 'issync' : 'wrong'));"
    /* §3.2: a second abort() fires nothing — the state half answers whether there is anything to fire. */
    "var sonce = sflag; sc.abort();"
    "fetch('/api/abortonce?v=' + (sflag === sonce ? 'isonce' : 'wrong'));"
    /* WEB IDL §3.2.25 Union types step 4 — "If V is null or undefined, then:" "If types includes a dictionary
       type, then return the result of converting V to that dictionary type." It sits above step 11's Object
       test and step 12/18's boolean, and §3.6 hands a declared dictionary position `undefined` when the page
       stops short of it, so the omitted call of every member declaring `(boolean or D)` reaches it.
       HTML §6.12 The popover attribute's togglePopover: step 1 "Let force be null", step 5 hides when showing
       "and force is null or false", step 6 shows "if force is null or true" — so `togglePopover()` on a
       hidden popover shows it, and a `false` from the boolean arm reaches step 7 and does nothing.
       The `{}` rows test the member type: `dictionary TogglePopoverOptions : ShowPopoverOptions { boolean
       force; }` has no default and step 3 asks whether options["force"] exists, so the member is
       IDL_BOOLEAN_NO_DEFAULT; IDL_BOOLEAN's `= false` would place an absent `force` as false.
       Every row is a transition. An engine ignoring `force` reads `1010111110`; either defect reads `0` where
       this reads `1`. Rows 6-7 are step 6's condition failing, 8-9 step 5's; row 9 shows an already-showing
       popover, which check popover validity step 2 answers by returning false, not by throwing. `manual`:
       show popover step 15 is gated on "If originalType is Auto or Hint", so no close watcher is made. */
    "var uapop = document.createElement('div'); uapop.setAttribute('popover','manual');"
    "document.body.appendChild(uapop); var uaseq = '';"
    /* THE SEQUENCE IS WRAPPED SO A THROW IS A RESULT AND NOT A CASUALTY. None of these ten rows may throw —
       every check popover validity below reaches its step 2's plain `false` rather than a DOMException, which
       is why row 9 (show on an ALREADY-showing popover) is a legal row at all. But an uncaught one here would
       take the three unrelated claims below it down with the whole script, and a row that can silence its
       neighbours is a row that reports the wrong defect. `E` ends the sequence at the call that threw. */
    "try {"
    "uaseq += uapop.togglePopover() ? '1' : '0';"                    /* hidden, force null   -> show   */
    "uaseq += uapop.togglePopover() ? '1' : '0';"                    /* showing, force null  -> hide   */
    "uaseq += uapop.togglePopover({}) ? '1' : '0';"                  /* member absent        -> show   */
    "uaseq += uapop.togglePopover({}) ? '1' : '0';"                  /* member absent        -> hide   */
    "uaseq += uapop.togglePopover(null) ? '1' : '0';"                /* step 4's other value -> show   */
    "uaseq += uapop.togglePopover(false) ? '1' : '0';"               /* showing, force false -> hide   */
    "uaseq += uapop.togglePopover(false) ? '1' : '0';"               /* hidden,  force false -> step 7 */
    "uaseq += uapop.togglePopover(true) ? '1' : '0';"                /* hidden,  force true  -> show   */
    "uaseq += uapop.togglePopover(true) ? '1' : '0';"                /* showing, force true  -> step 6 */
    "uaseq += uapop.togglePopover({ force: false }) ? '1' : '0';"    /* present false        -> hide   */
    "} catch (uaerr) { uaseq += 'E'; }"
    "fetch('/api/toggleforce?v=' + (uaseq === '1010100110' ? 'istoggle' : 'seq' + uaseq));"
    "document.body.dispatchEvent(new Event('wheel', { bubbles: true, cancelable: true }));"
    /* HTML §8.11.1 "The ImageData interface". Web IDL §3.6's distinguishing argument index for its two
       constructors is 0 (`unsigned long sw` against `ImageDataArray data`), and the entry chosen there decides
       position 2: `optional ImageDataSettings settings = {}` on the shorter, `optional unsigned long sh` on
       the longer — the first position whose type is step 15.2's "the type at index i in the type list of the
       remaining entry".
       Rows one to three are the two entries and the settings dictionary: `a` allocates transparent black at 4
       bytes per pixel; `b` takes the page's own array ("This step does not set this's data to a copy of
       data") and derives the height; `c` reaches step 6's `colorSpace` arm, which declares no default so
       steps 7 and 8 stay reachable.
       Row four separates the design from a union: `new ImageData(u8, 2, {})` selects the longer entry, so `{}`
       converts through ToNumber to 0 and step 7 ("If sh was given and its value is not equal to height, then
       throw an IndexSizeError") throws, the height being 2. A `(unsigned long or ImageDataSettings)` union
       sends the object down §3.2.25 step 11's dictionary arm and succeeds: `d` for the standard, `D` for it. */
    "var uimg = '';"
    "try { var ui1 = new ImageData(2, 2);"
    " uimg += (ui1.width === 2 && ui1.height === 2 && ui1.data.length === 16) ? 'a' : 'A'; }"
    "catch (e) { uimg += 'E'; }"
    "try { var ui2 = new ImageData(new Uint8ClampedArray(16), 2);"
    " uimg += (ui2.width === 2 && ui2.height === 2 && ui2.data.length === 16) ? 'b' : 'B'; }"
    "catch (e) { uimg += 'F'; }"
    "try { var ui3 = new ImageData(2, 2, { colorSpace: 'display-p3' });"
    " uimg += (ui3.colorSpace === 'display-p3' && ui3.data.length === 16) ? 'c' : 'C'; }"
    "catch (e) { uimg += 'G'; }"
    "try { new ImageData(new Uint8ClampedArray(16), 2, {}); uimg += 'D'; } catch (e) { uimg += 'd'; }"
    "fetch('/api/imagedata?v=' + (uimg === 'abcd' ? 'isimagedata' : 'arm' + uimg));"
    /* AND THE ROUND TRIP, WHICH IS WHAT §NO-STUBS MEANS BY A `data` THAT IS NOT A SHAPE. A byte written
       through the attribute is a byte a later read gives back, and the array is the page's own object on the
       longer entry — so the identity is observable as well as the value, and an implementation that handed
       back a copy would answer `wrote16` here and fail the identity. The payload is CONSTANTS: a witness
       composed from a value this engine computed can itself become unknown, at which point the request never
       goes out and its absence reads as the path not being taken. */
    "var uimr = 'na';"
    "try { var ui4 = new Uint8ClampedArray(16); var ui5 = new ImageData(ui4, 2);"
    " ui5.data[0] = 200; ui5.data[15] = 7;"
    " uimr = (ui5.data === ui4 && ui4[0] === 200 && ui5.data[15] === 7) ? 'roundtrip' : 'lost'; }"
    "catch (e) { uimr = 'threw'; }"
    "fetch('/api/imagedataio?v=' + uimr);"

    /* THE REACHABILITY WITNESS FOR solver's OWED-NAME CENSUS, not a probe row. Every `typeof` guard above names
       an interface this build installs, so none reaches the arm that records an unanswered name. `WebSocket`
       is carried by browser/platform_names.h and installed by no realm (core/platform.c's interface column has
       no row for it), so a read of it misses on a name this engine's vocabulary owns. The witness lasts until
       `WebSocket` reaches a realm; the diff that installs it owes this block a subject that is still absent.
       Two spellings, one row: `window.WebSocket` is a [[Get]] missing the global's whole prototype chain and
       reaches JSConcolicHooks.absent; `typeof WebSocket` is an unresolvable Reference that ECMAScript
       §13.5.3.1 "Runtime Semantics: Evaluation" step 2.a answers before step 3's GetValue, recorded by
       JSConcolicHooks.absent_unresolved. Neither throws or forks.
       What to observe in the run's `_absent`: the `"WebSocket"` row puts 1 in `read` and 1 in `typeof`, and
       the typeof member reads at least 1; every bucket is emitted including zero. No probe row: a census
       member is neither an endpoint nor a param, and engine/probegate.mjs cannot resolve a bare needle over a
       census key to a document token. */
    "var absTy = typeof WebSocket;"
    "var absRd = window.WebSocket;"
    /* THE THIRD SPELLING. `'WebSocket' in window` is ECMAScript §13.10.1 "Runtime Semantics: Evaluation",
       whose last step is "Return ? HasProperty(rightValue, ? ToPropertyKey(leftValue))"; §7.3.11
       "HasProperty ( obj, propertyKey )" performs no [[Get]], so it reaches neither hook above and is recorded
       at the `in` operator's own placement. It answers `false` and nothing throws.
       What to observe: the `"WebSocket"` row reads `read 1, typeof 1, in 1`, three buckets summing to three
       reads, and the `in` member reads at least 1 beside the typeof member. This document is the corpus's
       only whole-run witness for the per-name split: only a row whose three buckets each stand at 1 says the
       spellings reached the same name. A regressed split reads `read 3, typeof 0, in 0`. No probe row, for
       the reason above. */
    "var absIn = 'WebSocket' in window;"
    /* §4.13 CUSTOM ELEMENTS — the reason this component exists: connectedCallback's body is code NOTHING ELSE IN
       THE PROGRAM CALLS. The endpoints below are reachable only through the lifecycle, so their presence is the
       whole claim. `extends HTMLElement` is what a real bundle writes, and after the upgrade `this.setAttribute`
       must still resolve — that is the prototype CHAIN, not just the class's own prototype. */
    "class XPanel extends HTMLElement { connectedCallback(){ this.setAttribute('data-up','ceUP');"
    " fetch('/api/celife?v=' + (this.getAttribute('data-up') === 'ceUP' ? 'isce' : 'wrong')); } }"
    "customElements.define('x-panel', XPanel);"
    "document.body.appendChild(document.createElement('x-panel'));"
    /* §4.13.3 the RETROACTIVE upgrade: this element was in the tree as a plain HTMLElement before the name was
       defined. The expando set before the definition is still there afterwards, which is the identity half — the
       upgrade re-points the SAME wrapper, it does not build a new one. */
    "var ce0 = document.createElement('x-early'); ce0.marker = 'keptID'; document.body.appendChild(ce0);"
    "class XEarly extends HTMLElement { connectedCallback(){"
    " fetch('/api/ceearly?v=' + (this.marker === 'keptID' ? 'isearly' : 'wrong')); } }"
    "customElements.define('x-early', XEarly);"
    /* A reaction is a FLOW, not a C call: this one holds a loop AND an await, so it must park at both and resume.
       s = 0+..+299 = 44850 proves the loop ran to its end across the preempts. */
    "class XAsync extends HTMLElement { async connectedCallback(){ var n = 0; for (var i = 0; i < 300; i++) n += i;"
    " var w = await Promise.resolve('ceAWAIT'); fetch('/api/ceasync?n=' + n + '&w=' + w); } }"
    "customElements.define('x-async', XAsync);"
    "document.body.appendChild(document.createElement('x-async'));"
    /* §4.13.3 THE SUBTREE, and the removal reaction. A page that builds its UI off-tree and appends the ROOT
       once is the ordinary case, and the insertion steps used to run for the appended node alone — so a custom
       element inside a built fragment was never upgraded and its lifecycle code never ran. insertBefore did not
       run them at all. Both are the same one hook now, at the mutation chokepoint. */
    "class XDeep extends HTMLElement { connectedCallback(){ fetch('/api/cedeep?v=isdeep'); }"
    " disconnectedCallback(){ fetch('/api/cegone?v=isgone'); } }"
    "customElements.define('x-deep', XDeep);"
    "var dwrap = document.createElement('div'); var dinner = document.createElement('span');"
    "var ddeep = document.createElement('x-deep');"
    "dwrap.appendChild(dinner); dinner.appendChild(ddeep);"   /* all OFF-TREE: nothing has entered a document */
    "document.body.appendChild(dwrap);"                       /* one append, and the element two levels down runs */
    "document.body.removeChild(dwrap);"
    /* insertBefore is the OTHER tree write, and it never ran the steps at all. */
    "var dref = document.createElement('i'); document.body.appendChild(dref);"
    "document.body.insertBefore(document.createElement('x-deep'), dref);"
    /* §4.13.3 attributeChangedCallback. It runs only for a name the class declared as OBSERVED, which is why
       observedAttributes is read at DEFINE time — and reading it is the page's code (a static getter whose
       entries are themselves coerced), so the read and every entry's ToString are requests the declaration's
       step body parks on. The unwatched attribute proves the filter: only an ABSENCE can. */
    "class XAttr extends HTMLElement {"
    " static get observedAttributes(){ var n = 0; for (var i = 0; i < 30; i++) n += i;"
    "  return [{ toString: function(){ return n === 435 ? 'data-w' : 'bad'; } }]; }"
    " attributeChangedCallback(nm, ov, nv){ fetch('/api/ceattr?n=' + nm + '&o=' + ov + '&v=' + nv); } }"
    "customElements.define('x-attr', XAttr);"
    "var xa = document.createElement('x-attr'); document.body.appendChild(xa);"
    "xa.setAttribute('data-w', 'first');"
    "xa.setAttribute('data-w', 'second');"
    "xa.setAttribute('data-ignored', 'nope');"   /* not observed: no reaction, and only an absence proves it */
    "xa.removeAttribute('data-w');"
    /* §4.13.4 get(), and §4.13.3's name rule — a name with no hyphen is a SyntaxError, not a quiet registration. */
    "var cename = 'wrong'; try { customElements.define('nohyphen', XPanel); } catch (e) { cename = 'issyntax'; }"
    "fetch('/api/cename?v=' + cename);"
    /* §4.13.3's OTHER THREE REFUSALS, AND THE REASON EACH ONE NAMES. "A valid custom element name" is five
       requirements and §4.13.4 step 2 answers all five with one "SyntaxError", so the row above passes with
       four of the clauses DELETED — the reserved list, the upper-alpha clause and DOM §1.4's own predicate
       have never once been exercised by this fixture, and `catch (e) { ok = true }` is why. That is the
       "right exception for the wrong reason" shape exactly, so each row asserts the REASON the exception
       carries and reads `SyntaxError-wrongreason` when the clause that fired is not the clause under test. */
    "var ceres = 'wrong'; try { customElements.define('annotation-xml', class extends HTMLElement {}); }"
    " catch (e) { ceres = e.name + (e.message.indexOf('reserves') >= 0 ? '-isreserved' : '-wrongreason'); }"
    "fetch('/api/cereserved?v=' + ceres);"
    "var ceup = 'wrong'; try { customElements.define('x-A', class extends HTMLElement {}); }"
    " catch (e) { ceup = e.name + (e.message.indexOf('upper') >= 0 ? '-isupper' : '-wrongreason'); }"
    "fetch('/api/ceupper?v=' + ceup);"
    "var celoc = 'wrong'; try { customElements.define('x-a b', class extends HTMLElement {}); }"
    " catch (e) { celoc = e.name + (e.message.indexOf('local name') >= 0 ? '-islocal' : '-wrongreason'); }"
    "fetch('/api/celocal?v=' + celoc);"
    /* …AND THE ACCEPTANCE NO REFUSAL CAN PROVE. §4.13.3 says it in its own words — "a large variety of names
       is allowed, to give maximum flexibility for use cases like <math-alpha> or <emotion-emoji>" — and that
       is true only because its first bullet is DOM §1.4's "valid element local name" rather than a character
       class: §1.4's ASCII-alpha arm excludes U+0000, ASCII whitespace, U+002F and U+003E and NOTHING ELSE.
       The `PotentialCustomElementName` grammar that used to be cited here is gone from the standard, and an
       engine still checking a PCENChar-shaped `[-.0-9a-z]` set would refuse this name while every refusal row
       above stayed green — which is why the acceptance is a row and not a remark. */
    "var ceuni = 'wrong'; try { customElements.define('emotion-\\u{1F60D}', class extends HTMLElement {});"
    " ceuni = 'isuni'; } catch (e) { ceuni = 'threw-' + e.message; } fetch('/api/ceuni?v=' + ceuni);"
    /* …and the same throw UNCAUGHT, at a chunk's top level, where a page error is what it becomes. */
    "loadScript('/chunk/cethrow.js');"
    /* HTML §8.1.4.7 Unhandled promise rejections — the two claims that make the two lists necessary, and
       neither can be proved by the other.
       (1) A rejection nobody ever handles IS a page error: an async bundle delivers most of its errors this way
       and every one of them used to be silent. (2) A handler attached in a LATER microtask is ordinary correct
       code, so that rejection must never be reported — which only an ABSENCE can prove. */
    /* §8.1.4.7 Unhandled promise rejections fires `unhandledrejection` at the global, CANCELABLE, before
       anything is reported. A page that
       ships its own error reporter cancels it — and that reporter is code with a fetch in it, which is the
       surface this engine exists to reach, so the event is worth more than the report. Cancelling must
       SUPPRESS the report, which only the absence of the reason from the output can prove. */
    "addEventListener('unhandledrejection', function(e){ if (e.reason === 'rejCANCEL') { e.preventDefault();"
    " fetch('/api/rejevent?v=' + (e.promise && e instanceof PromiseRejectionEvent ? 'isrej' : 'wrong')); } });"
    "Promise.reject('rejCANCEL');"
    /* The interface a page can construct, and its REQUIRED member: PromiseRejectionEventInit.promise has no
       default, so an init without it is a TypeError from the declaration rather than a check in a body. */
    "var prereq = 'wrong'; try { new PromiseRejectionEvent('x', {}); } catch (e) { prereq = 'isreq'; }"
    "fetch('/api/prereq?v=' + prereq);"
    "var pe = new PromiseRejectionEvent('t', { promise: Promise.resolve(), reason: 'peREASON', cancelable: true });"
    "fetch('/api/prector?v=' + (pe.reason === 'peREASON' && pe instanceof Event && pe.cancelable && !pe.isTrusted"
    " ? 'isctor' : 'wrong'));"
    "var prs = Promise.reject('rejSYNC'); prs.catch(function(){});"
    "var prh = Promise.reject('rejHANDLED');"
    "Promise.resolve().then(function(){ prh.catch(function(){}); });"
    /* HTML §8.1.4.6 "Runtime script errors"' extract error information sets `message` to one of the
       "implementation-defined values derived from exception". A derivation reading own properties gives four
       shapes one anonymous description: a framework error class puts `message` behind a prototype accessor,
       an ECMAScript error kind puts `name` on its prototype, a hand-built reason puts either behind its own
       getter, and a Proxy has no data slot at all.
       The report path reads a value the page supplied, so neither `JS_GetOwnSlot` (which refuses an
       accessor) nor `JS_GetOwnSlotDesc` (which refuses a Proxy) may reach a dev abort from here.
       They reject rather than throw, because §8.1.4.7's route reaches the console in one call with the
       page's value unchanged, and nothing handles them, so each is reported. The order is the witness: the
       Proxy's description carries no token, so `nonameKind`, rejected after it in this script, being
       reported is what proves it ran. */
    "class NonameProto { get message(){ return 'nonameProtoGetterRan'; } }"
    "Promise.reject(new NonameProto());"
    "Promise.reject(Object.defineProperty({ name: 'nonameOwnAcc' }, 'message',"
    " { get: function(){ return 'nonameGetterRan'; } }));"
    "Promise.reject(new Proxy({}, { get: function(){ return 'nonameTrapRan'; },"
    " getOwnPropertyDescriptor: function(){ return { value: 'nonameTrapRan', configurable: true }; } }));"
    "Promise.reject(new TypeError('nonameKind'));"
    /* A FUNCTION THROWN AS A VALUE carries no `message` and no frames, so it is described by where it was
       WRITTEN — MEASURED as the largest page-error row of a real application (x12), reading `[object Function]`
       and naming nothing. The comma keeps the second one ANONYMOUS: a bare function expression as an argument
       gets no inferred name, and its empty `name` used to compose an empty description the report drops. */
    "Promise.reject(function nonameFnNamed(){});"
    "Promise.reject((0, function(){}));"
    "fetch('/api/ceget?v=' + (customElements.get('x-panel') === XPanel"
    " && customElements.get('x-none') === undefined ? 'isget' : 'wrong'));"
    /* A TYPED DICTIONARY MEMBER, converted through the page's own code. `extends` is a DOMString member of
       ElementDefinitionOptions, so Web IDL READS it (an accessor here, which a Proxy would make of any object)
       and then runs ToString on what it got — two suspension points inside one member, and the page's toString
       holds a loop, so the resume must come back to the CONVERSION and not re-run the read. HTML §4.13.4 The
       CustomElementRegistry interface's define steps accept `button` (not a valid custom element name, and its
       element interface is not HTMLUnknownElement) and refuse `bad` with a "NotSupportedError", so the
       definition succeeding is what proves the converted value arrived. */
    "var ceext = 'wrong'; var ceX = class {};"
    "try { customElements.define('x-ext', ceX, { get extends(){ return { toString: function(){"
    " var n = 0; for (var i = 0; i < 120; i++) n += i; return n === 7140 ? 'button' : 'bad'; } }; } });"
    " ceext = customElements.get('x-ext') === ceX ? 'isext' : 'wrong'; }"
    " catch (e) { ceext = 'threw-' + e.name; }"
    "fetch('/api/ceext?v=' + ceext);"
    "var acc = 0; for (var i = 0; i < 15; i++) { acc = acc + i; }"   /* a real LOOP (back-edges) so the quantum fires: a flow yields MID-LOOP and the scheduler interleaves parked sibling flows, exercising the COW+decide+pins swap */
    "function* gen(n){ var s=0; for(var i=0;i<n;i++){ s=s+i; } yield s; yield s*2; }"   /* GENERATOR body with a LOOP: .next() runs the body on the tramp chain, the loop preempts the base flow */
    "var git = gen(2000); fetch('/api/gen?a=' + git.next().value + '&b=' + git.next().value);"
    "function* gg(){ for(var i=0;i<10;i++) yield i*i; } var gsum=0; for(const x of gg()){ gsum = gsum + x; } fetch('/api/genforof?s=' + gsum);"
    "function* gt(){ try { yield 1; } catch(e){ yield 'c:'+e; } } var gi = gt(); gi.next(); fetch('/api/genthrow?v=' + gi.throw('X').value);"   /* direct .next()/.next() -> 1999000, 3998000 */
    /* THIS DOCUMENT IS ONE PROGRAM AND A FUNCTION DECLARATION IS INITIALIZED BEFORE ANY OF IT RUNS, so a
       `var` ANYWHERE ABOVE IT that happens to spell the same name overwrites it before its own call site is
       reached. That inversion is the one collision shape a var/var pair does NOT have — a var assigns where
       it is written, a declaration assigns at the top — and it is silent in the corpus: the statement cannot
       answer, and a statement that cannot answer reads exactly like an engine gap. A hoisted name in this
       literal is therefore unique against THE WHOLE LITERAL, never against its neighbours. */
    "function* gpar([a,b,c]){ yield a+b+c; } fetch('/api/genparam?v=' + gpar([10,20,30]).next().value);"   /* PARAM-DESTRUCTURING generator: the array destructuring iterates at CREATION on the tramp chain (do_generator_create_tramp) */
    "var _fe=0; [10,20,30].forEach(function(x){ var t=0; for(var i=0;i<50;i++) t++; _fe += x + t; }); fetch('/api/foreach?s=' + _fe);"   /* forEach callback with a LOOP: the callback runs on the tramp chain (do_array_iter_tramp), so its back-edges PARK the base flow and resume — never drive-to-completion -> 60+150=210 */
    "fetch('/api/arrmap?s=' + [1,2,3,4,5].map(function(x){ return x*2; }).filter(function(x){ return x>4; }).join(','));"   /* map + filter through the step coroutine -> 6,8,10 */
    "var _fc=0; Array.prototype.forEach.call([5,6], function(x){ var t=0; for(var i=0;i<20;i++) t++; _fc += x + t; }); fetch('/api/fecall?s=' + _fc);"   /* .call is CALL-SITE-RESOLVED: unwrapped at the operator site (do_forward_call) and re-dispatched into the array-iter coroutine, so the looping callback still PARKS the base -> 11+40=51 */
    "fetch('/api/reduce?s=' + [1,2,3,4].reduce(function(a,x){ var t=0; for(var i=0;i<10;i++) t++; return a + x*t; }, 0));"   /* reduce callback with a LOOP: same coroutine shape (do_array_reduce_tramp), the accumulator rides the state -> 100 */
    "(async function(){ await Promise.resolve(1); var s=0; for(var i=0;i<30;i++) s+=i; fetch('/api/postawait?s=' + s); })();"   /* loop AFTER an await: the post-await body is a reaction-driven CONTINUATION, resumed as its OWN flow (js_async_function_resume_as_flow) so its loop PARKS and re-enters the job pump -> 435 */
    "async function* agp(x = (function(){ throw 'pthrow'; })()) { yield 1; }"
    "try { agp(); } catch (e) { fetch('/api/agthrow?e=' + e); }"   /* PARAM-BINDING THROW during async-generator creation: the object does not exist yet and the frame's buffers belong to its STATE, so this must take the agen-create exception path (not the generic pop) -> pthrow */
    "var _om = { async m(x){ var s=0; for(var i=0;i<15;i++) s+=i; return x+s; } };"
    "_om.m(5).then(function(v){ fetch('/api/asyncmethod?v=' + v); });"   /* ASYNC METHOD (obj.m(), tramp_first==-2): its operands are [this,func,args] — do_async_tramp_call must pop by tramp_first, not a hardcoded -1, or `this` leaks and the caller's sp lands a slot high (heap corruption) -> 110 */
    "function Ctr(n){ var s=0; for(var i=0;i<n;i++) s+=i; this.v=s; } fetch('/api/ctor?v=' + new Ctr(20).v);"   /* do_construct_tramp: a plain-function constructor body loop PARKS the base flow instead of C-recursing through JS_CallConstructorInternal -> 190 */
    "var _go = { get big(){ var s=0; for(var i=0;i<25;i++) s+=i; return s; } }; fetch('/api/getter?v=' + _go.big);"   /* BYTECODE GETTER via OP_get_field on the tramp chain: the getter body loop PARKS the base flow (do_tramp_call via tramp_bytecode_getter) instead of C-recursing through JS_GetPropertyInternal -> 300 */
    "var _pf = ({poisoned}) => { return 1; }; var _pc = 0; try { _pf({ get poisoned(){ throw 7; } }); } catch(e){ _pc = e; } fetch('/api/dstrthrow?v=' + _pc);"   /* getter throws during PARAM DESTRUCTURING -> nested tramp unwind */
    "var _ga = { get bracket(){ var t=0; for(var i=0;i<30;i++) t+=i; return t; } }; fetch('/api/getarr?v=' + _ga['bracket']);"   /* GETTER via OP_get_array_el (computed key): tramp_bytecode_getter after ToPropertyKey routes the getter-body loop onto the chain -> 435 */
    "var _so = { _x:0, set load(v){ var s=0; for(var i=0;i<v;i++) s+=i; this._x = s; } }; _so.load = 20; fetch('/api/setter?v=' + _so._x);"   /* BYTECODE SETTER on the tramp chain: the setter-body loop PARKS the base flow (do_tramp_call via tramp_bytecode_setter, CONT_SETTER discards the result) -> 190 */
    "var _sa = { _y:0, set slot(v){ var s=0; for(var i=0;i<v;i++) s+=i; this._y = s; } }; _sa['slot'] = 25; fetch('/api/setarr?v=' + _sa._y);"   /* SETTER via OP_put_array_el (computed key): tramp_bytecode_setter routes the setter-body loop onto the chain -> 300 */
    "function _ap(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } fetch('/api/apply?v=' + _ap.apply(null, [20, 5, 3]));"   /* f.apply(this, arr) with a looping body -> do_apply_tramp: args spread from arr into f's frame -> 190+5+3=198 */
    "function _at(){ throw 9; } var _atc=0; try { _at.apply(null, [1,2]); } catch(e){ _atc=e; } fetch('/api/applythrow?v=' + _atc);"   /* apply target THROWS -> nested tramp unwind -> 9 */
    "function _bf(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } var _bb = _bf.bind(null, 20, 5); fetch('/api/bind?v=' + _bb(3));"   /* f.bind(this, boundArgs)(callArgs) with a looping body -> do_bound_tramp assembles bound++call args -> 190+5+3=198 */
    "function _bt(){ throw 4; } var _btb = _bt.bind(null); var _btc=0; try { _btb(); } catch(e){ _btc=e; } fetch('/api/bindthrow?v=' + _btc);"   /* bound target THROWS -> nested tramp unwind -> 4 */
    "function _sf(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } var _sarr=[20,5,3]; fetch('/api/spread?v=' + _sf(..._sarr));"   /* f(...arr) SPREAD with a looping body -> OP_apply routes into do_apply_tramp -> 198 */
    "function _rf(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } fetch('/api/reflectapply?v=' + Reflect.apply(_rf, null, [20,5,3]));"   /* Reflect.apply(target, this, argsList) with a looping body -> do_apply_tramp -> 198 */
    "var _sa=[3,1,2,5,4]; _sa.sort(function(x,y){ var s=0; for(var i=0;i<5;i++) s++; return x-y; }); fetch('/api/sort?v=' + _sa.join(String.fromCharCode(44)));"   /* arr.sort(cmp) with a looping comparator -> CONT_SORT merge-sort coroutine, comparator body loop PARKS the base -> 1,2,3,4,5 */
    "var _sta=[3,1,2]; var _stc=0; try { _sta.sort(function(x,y){ throw 8; }); } catch(e){ _stc=e; } fetch('/api/sortthrow?v=' + _stc);"   /* comparator THROWS -> sort-state freed on unwind -> 8 */
    "var _jr = JSON.parse(String.fromCharCode(123,34,97,34,58,53,44,34,98,34,58,55,125), function(k,v){ if(typeof v===String.fromCharCode(110,117,109,98,101,114)){ var s=0; for(var i=0;i<v;i++) s+=i; return s; } return v; }); fetch('/api/jsonrevive?v=' + (_jr.a + _jr.b));"   /* JSON.parse reviver with a looping body -> CONT_JSON_REVIVE explicit-stack walk -> a:10 + b:21 = 31 */
    "var _jt=0; try { JSON.parse(String.fromCharCode(123,34,120,34,58,49,125), function(k,v){ if(k===String.fromCharCode(120)) throw 3; return v; }); } catch(e){ _jt=e; } fetch('/api/jsonthrow?v=' + _jt);"   /* reviver THROWS -> walk state freed on unwind -> 3 */
    "let _lex = 7; const _kex = 8; class _Cex { g(){ return _lex + _kex; } } fetch('/api/lexbind?v=' + new _Cex().g());"   /* TOP-LEVEL LEXICAL bindings (let/const/class) define into the SHARED global_var_obj; JS_DefineGlobalVar now COW-captures the creation so it is per-flow, not leaked into snapshot siblings (whose re-definition would else throw redeclaration and kill the sibling + its downstream forks � a silently dropped @H arm) -> 15 */
    "</script>"

    TF_CANVAS_INK
    "</body></html>";

#endif
