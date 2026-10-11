/* THE DOM DOCUMENT: Web IDL and DOM member semantics — reflection, node algorithms, listeners, token lists, coercions.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DOM_H
#define ENGINE_HOST_FIXTURE_DOM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DOM[] =
    "<!doctype html><html>"
    "<body>"
    /* The boot fork: `cfg.admin` forks here, so every later test of it in this document is decided inside the
       two arms that already exist rather than forking where it stands. */
    "<script>var cfg = { admin: state.admin }; if (cfg.admin) { cfg.arm = 'A'; } else { cfg.arm = 'P'; }</script>"
    /* ---- CSS Conditional 5 §9.1 "The CSSContainerRule interface" — the CSSOM OBJECT, built and DISABLED in one
       script. The cascaded effect is unbuilt (§5.4's condition is per element against a query container, and
       core/css/css_rule.c's cascade arm crashes by name), so the sheet is disabled through CSSOM §6.1.1 "The
       StyleSheet Interface"'s setter ("On setting, the disabled attribute must set the disabled flag if the
       new value is true, or unset the disabled flag otherwise"). That a set flag keeps a sheet out of the
       cascade is an IMPLICATION — CSSOM §6.1 "CSS Style Sheets" says only "Either set or unset. Unset by
       default.", and HTML §4.2.4 "The link element"'s note speaks of removing the attribute — enough to rest a
       fixture on, not a quotable rule.
       SIX RULES, FOUR SURVIVE: `not (x) and (y)` and `@container { }` are grammar failures CSS Syntax §8 drops,
       so the LENGTH carries both; the four are a named condition, an unnamed one, a LIST (for which both
       attributes return "", so `conditions` is asserted beside them), and `not (…)` — `not` is the only bare
       ident a `<container-query>` may begin with, so it must mean "no name", not "invalid".
       `cssText` is asserted by its prefix: the body comes from the group-rule serializer other rows pin. */
    "<script>var cel = document.createElement('style');"
    "cel.textContent = '@container card (min-width: 400px) { .cq-a { color: red } }'"
    " + '@container (min-width: 10px) { .cq-b { color: red } }'"
    " + '@container card, wide (min-width: 1px) { .cq-c { color: red } }'"
    " + '@container not (min-width: 5px) { .cq-d { color: red } }'"
    " + '@container not (min-width: 1px) and (max-width: 9px) { .cq-bad { color: red } }'"
    " + '@container { .cq-bad2 { color: red } }';"
    "document.body.appendChild(cel);"
    "var csh = cel.sheet; csh.disabled = true;"
    "var cqr = csh.cssRules;"
    /* The interface, the chain and §6.4.2's frozen table answering 0 for a rule it never named. */
    "var cq1 = cqr.length + '|' + csh.disabled + '|' + (cqr[0] instanceof CSSContainerRule) + '|'"
    " + (cqr[0] instanceof CSSConditionRule) + '|' + (cqr[0] instanceof CSSGroupingRule) + '|' + cqr[0].type"
    " + '|' + cqr[0].cssRules.length;"
    "fetch('/api/csscontainer?v=' + (cq1 === '4|true|true|true|true|0|1' ? 'CQOK' : 'CQBAD:' + cq1));"
    /* §9.1's three members over the three shapes, and §7.2's `conditionText` redefined by §9.1's algorithm —
       which is where the name comes back through CSSOM §2.1's serialize-an-identifier and the conditions are
       joined by its ", " while each query keeps the author's own bytes. */
    "var cq2 = cqr[0].containerName + '|' + cqr[0].containerQuery + '|' + cqr[0].conditionText"
    " + '~' + cqr[1].containerName + '|' + cqr[1].containerQuery + '|' + cqr[1].conditionText"
    " + '~' + cqr[2].containerName + '|' + cqr[2].containerQuery + '|' + cqr[2].conditionText"
    " + '~' + cqr[3].containerName + '|' + cqr[3].containerQuery + '|' + cqr[3].conditionText;"
    "fetch('/api/csscontainerText?v=' + (cq2 === 'card|(min-width: 400px)|card (min-width: 400px)"
    "~|(min-width: 10px)|(min-width: 10px)~||card, wide (min-width: 1px)"
    "~|not (min-width: 5px)|not (min-width: 5px)'"
    " ? 'CQTEXTOK' : 'CQTEXTBAD:' + cq2));"
    /* §9.1's `conditions`: the pair the two legacy attributes collapse, FROZEN per Web IDL §3.2.27 and MINTED
       PER GET — it is not [SameObject] and its algorithm builds a new list of new dictionaries every run, so
       two reads must not be the same object and a write to one must not reach the rule. */
    "var cqc = cqr[2].conditions;"
    "var cq3 = cqc.length + '|' + cqc[0].name + '|' + cqc[0].query + '|' + cqc[1].name + '|' + cqc[1].query"
    " + '|' + Object.isFrozen(cqc) + '|' + (cqr[2].conditions === cqr[2].conditions);"
    "fetch('/api/csscontainerConds?v=' + (cq3 === '2|card||wide|(min-width: 1px)|true|false'"
    " ? 'CQCONDOK' : 'CQCONDBAD:' + cq3));"
    "fetch('/api/csscontainerText2?v=' + (cqr[0].cssText.indexOf"
    "('@container card (min-width: 400px) {') === 0 ? 'CQCSSTEXTOK' : 'CQCSSTEXTBAD:' + cqr[0].cssText));"
    /* HTML §8.1.4.6 "Runtime script errors"' `reportError`. SIX ROWS, AND EVERY ONE OF THEM ANSWERS OUT OF A
       VALUE THE ENGINE COMPUTES — the member's own Web IDL §3.7.7 descriptor, the ErrorEvent step 2 builds,
       the flag step 6 sets, and Web IDL §3.7.7's receiver steps. They are in THIS <script> rather than a new
       one on purpose:
       a new program shifts every later program's index, and the reachability of a row added at the END of the
       document is the one property a newly-added row has not got.
       THEY RUN BEFORE `globalThis.onerror` IS ASSIGNED, which is a later <script>'s first line. That matters:
       §8.1.8.1's special error event handling makes an `onerror` returning true a CANCEL, so a handler
       installed above these rows would decide step 7 for them and row 4's count would be a fact about that
       handler instead of about step 6's flag. */
    "var reLog = [], reSeen = 0;"
    "var reH = function(ev){ reSeen += 1; reLog.push(ev); ev.preventDefault(); };"
    "addEventListener('error', reH);"
    /* (1) WEB IDL §3.7.7 "Operations"' OWN DESCRIPTOR, which is the half no `typeof` can see. Its define step
       is "Let desc be the PropertyDescriptor{[[Value]]: method, [[Writable]]: modifiable, [[Enumerable]]:
       true, [[Configurable]]: modifiable}" with `modifiable` true for an operation that is not unforgeable,
       and `length` is computed rather than declared — "Let length be the length of the shortest argument list
       in the entries in S" over the effective overload set at argument count 0, which for `undefined
       reportError(any e)` is 1. IT IS AN OWN PROPERTY OF THE GLOBAL AND NOT AN INHERITED ONE, because
       §3.7.3's [Global] conditional sends a `Window` member to §3.8's instance arm — a build that put it on a
       prototype would satisfy every `typeof` beside this and fail this line. */
    "var reD = Object.getOwnPropertyDescriptor(globalThis, 'reportError');"
    "fetch('/api/reporterrordesc?v=' + (typeof reportError === 'function' && reportError.length === 1 &&"
    " reportError.name === 'reportError' && reD !== undefined && reD.writable === true &&"
    " reD.enumerable === true && reD.configurable === true && typeof reD.value === 'function'"
    " ? 'isREDESC' : 'wrong'));"
    /* (2) THE REPORT IS OBSERVED, which is the only row here that proves the member does anything at all:
       step 6.2 fires `error` at the global "using ErrorEvent, with the cancelable attribute initialized to
       true, and additional attributes initialized according to errorInfo". `error` is step 2's
       "Set attributes[error] to exception", so it must be the IDENTICAL value the page handed in — not a
       copy, not a string of it. And the method itself answers `undefined`: §8.2 declares
       `undefined reportError(any e)`, so a report that fired listeners and one that skipped step 6 entirely
       must be indistinguishable to the page. */
    "var reErr = new TypeError('reportError-fixture');"
    "var reRet = reportError(reErr);"
    "var reEv = reLog[0];"
    "fetch('/api/reporterrorfire?v=' + (reSeen === 1 && reRet === undefined && reEv instanceof ErrorEvent &&"
    " reEv.type === 'error' && reEv.cancelable === true && reEv.error === reErr &&"
    " reEv.defaultPrevented === true && reEv.target === globalThis ? 'isREFIRE' : 'wrong'));"
    /* (3) STEP 2's OTHER FOUR, which are "implementation-defined values derived from exception" and are
       therefore the engine COMPUTING rather than the standard dictating — so what this row can assert is
       their IDL types, which §8.1.4.6's own ErrorEvent block fixes as `DOMString message`,
       `USVString filename`, `unsigned long lineno` and `unsigned long colno`. The message is derived WITHOUT
       running the page's `toString`, which is why row 6 can report a value whose `toString` throws. */
    "fetch('/api/reporterrorinfo?v=' + (typeof reEv.message === 'string' && reEv.message.length > 0 &&"
    " typeof reEv.filename === 'string' && typeof reEv.lineno === 'number' &&"
    " typeof reEv.colno === 'number' && (reEv.lineno | 0) === reEv.lineno ? 'isREINFO' : 'wrong'));"
    /* (4) STEP 6's `in error reporting mode`, which is the ONE piece of state this algorithm keeps and the one
       thing a re-entrant report is decided by: "If global is not in error reporting mode" encloses the whole
       of steps 6.1-6.3, so a `reportError` called from INSIDE an `error` listener performs NO second fire and
       arrives at step 7 with notHandled still step 1's true. A build that set no flag would recurse here and
       a build that keyed the flag on this component instead of on the GLOBAL would answer the same for a
       second document's report. The inner call also returns `undefined`, which is how step 6 being SKIPPED
       stays indistinguishable from step 6 running. */
    "var reInner = 0, reInnerRet = 'unset';"
    "var reH2 = function(ev){ reInner += 1; if (reInner === 1) { reInnerRet ="
    " reportError(new RangeError('reentrant')); } ev.preventDefault(); };"
    "removeEventListener('error', reH); addEventListener('error', reH2);"
    "reportError(new TypeError('outer'));"
    "fetch('/api/reporterrormode?v=' + (reInner === 1 && reInnerRet === undefined"
    " ? 'isREMODE' : 'wrong:' + reInner));"
    /* (5) WEB IDL §3.7.7's RECEIVER STEPS, both arms. "Let jsValue be the this value, if it is not null or
       undefined, or realm's global object otherwise" is why the bare `reportError(e)` every bundle writes
       reaches this realm's own global; "If jsValue does not implement the interface target, throw a
       TypeError" is why a plain object is refused. The refusal is a THROW and not an answer, because a page
       distinguishes the two — and it must be a TypeError rather than an abort, since the receiver is page
       input like any other argument. `undefined` and `null` are the two spellings of §3.7.7's fallback arm
       and both are exercised, because a build testing only `undefined` would pass with `null` refused. */
    "var reTE = 'nothrow', reBare = 0, reNull = 0;"
    "try { Reflect.apply(reportError, {}, [new TypeError('foreign')]); }"
    " catch (e) { reTE = (e instanceof TypeError) ? 'TypeError' : ('other:' + e); }"
    "reInner = 0; Reflect.apply(reportError, undefined, [new TypeError('bare')]); reBare = reInner;"
    "reInner = 0; Reflect.apply(reportError, null, [new TypeError('nul')]); reNull = reInner;"
    "fetch('/api/reporterrorthis?v=' + (reTE === 'TypeError' && reBare === 1 && reNull === 1"
    " ? 'isRETHIS' : 'wrong:' + reTE + ':' + reBare + ':' + reNull));"
    /* (6) `any` CARRIES THE VALUE UNEXAMINED, which is the row that says nothing in this engine asserts
       anything about what a page reports. Web IDL §3.2.1 "any"'s Object arm is "If V is an Object, then
       return an IDL object value that references V" — a reference, so step 2's errorInfo[error] is the same
       object — and its Symbol arm converts to a symbol, so a Symbol is a reportable value too. Neither may
       reach the page's `toString`: the message is derived from the value's shape, so an object whose
       `toString` throws must still produce a string message rather than take the report down with it. */
    "var reLog2 = [];"
    "var reH3 = function(ev){ reLog2.push(ev); ev.preventDefault(); };"
    "removeEventListener('error', reH2); addEventListener('error', reH3);"
    "var reHostile = { toString: function(){ throw new Error('nope'); },"
    " get message(){ throw new Error('nope'); } };"
    "var reSym = Symbol('reported');"
    "reportError(reHostile); reportError(reSym); reportError(undefined);"
    "fetch('/api/reporterrorany?v=' + (reLog2.length === 3 && reLog2[0].error === reHostile &&"
    " reLog2[1].error === reSym && reLog2[2].error === undefined &&"
    " typeof reLog2[0].message === 'string' && typeof reLog2[1].message === 'string'"
    " ? 'isREANY' : 'wrong:' + reLog2.length));"
    "removeEventListener('error', reH3);"
    "</script>"
    "<script>"
    /* §13.15.3 ApplyStringOrNumericBinaryOperator STEP 1.c, ASKED OF THE EXAMPLE — "If leftPrimitive is a
       String or rightPrimitive is a String" — which is what decides whether `+` concatenates or ADDS. The
       concolic derivation had one arm, so `num + 1000000` carried the example "19201000000" where the code
       computed 1001920, and §@H publishes a computed value as an OBSERVED fact: every endpoint downstream of
       an arithmetic `+` named an address the bundle does not address. THREE SEPARATE CLAIMS.
       `addfork` is the one that must NOT change: the sum is still unknown, so the comparison forks and both
       arms' endpoints are learned — a numeric arm that collapsed to a bare number deletes one of them.
       `addnum` reads the example back through the path parameter endpoint.c aligns against the shape: `num`
       cancels, so the segment is `v1000000` under 6.1.6.1.7 Number::add ( x, y ) and `v19200998080` under a
       string concatenation, and the one exact match is the whole assertion.
       `addstr` is the regression that would cost the most — a String on the left and a concolic whose example
       is a NUMBER on the right is step 1.c's own case, and it is the `'/api/' + cfg.region` this surface is
       stated around; it reads `r1920` under both arms and must keep doing so. */
    "var addn = num + 1000000;"
    "if (addn > 0) { fetch('/api/addfork?v=addpos'); } else { fetch('/api/addfork?v=addneg'); }"
    "fetch('/api/addnum/v' + (addn - num));"
    "fetch('/api/addstr/r' + num);"
    /* THE REAL DOM, through document/Element/Node rather than a host-edge stand-in. Four things the tree
       components must get right and had no fixture for: interface members live on a PROTOTYPE (so two elements
       share one function object), `[LegacyNullToEmptyString] DOMString data` turns null into "" instead of the
       four characters `null`, `DOMString? textContent` set to null leaves NO child, and `nodeValue` is a NODE
       member that answers null on an element. */
    "var e1 = document.createElement('div'); var e2 = document.createElement('span');"
    "fetch('/api/protoid?v=' + (e1.getAttribute === e2.getAttribute ? 'shared' : 'percopy'));"
    "var tx = document.createTextNode('base'); e1.appendChild(tx); tx.data = null;"
    "fetch('/api/cdnull?v=' + (tx.data === '' ? 'empty' : 'wrong'));"
    "e1.textContent = null;"
    "fetch('/api/tcnull?v=' + (e1.childNodes.length === 0 ? 'nochild' : 'child'));"
    "fetch('/api/nodeval?v=' + (e1.nodeValue === null ? 'null' : 'wrong'));"
    "e1.textContent = 'tcSET'; fetch('/api/tcset?v=' + e1.textContent);"
    /* §4.4 THE NODE ALGORITHMS. Each is one spec sentence, and each answers with a token only the correct
       behaviour produces — every one of them was ABSENT, so a page doing any of this stopped there. */
    "fetch('/api/nodeconst?v=' + (document.body.nodeType === Node.ELEMENT_NODE ? 'isconst' : 'wrong'));"
    /* §4.4's baseURI IS THE DOCUMENT'S ADDRESS, so it is the address Location reports and not a different
       string. This fixture used to install the Document at "https://x.test" and Location at
       "https://x.test/p" — two answers to "where is this document", which no browser has and which only held
       because one caller passed each. One address now reaches both. */
    "fetch('/api/baseuri?v=' + (document.body.baseURI === 'https://x.test/p' ? 'base' : 'wrong'));"
    "var c1 = document.createElement('p'); c1.setAttribute('k','v'); var c2 = c1.cloneNode(true);"
    "fetch('/api/equalnode?v=' + (c1.isEqualNode(c2) && !c1.isSameNode(c2) ? 'iseq' : 'wrong'));"
    /* §4.4 cloneNode is a WALK and a COPY, and the oracles only ever compared a clone to its original with
       isEqualNode — which is itself a walk, so a matched pair of bugs in the two would agree with each other.
       These assert the copy's own properties instead: shallow takes no children, deep takes the whole subtree,
       attributes come with it, the copy is DETACHED and is not the original, and mutating the copy leaves the
       original alone. */
    "var cw = document.createElement('div'); cw.setAttribute('id', 'cwid');"
    "cw.innerHTML = '<p class=\"a\">x<b>y</b></p><i>z</i>';"
    "document.body.appendChild(cw);"
    "var csh = cw.cloneNode(); var cdp = cw.cloneNode(true);"
    "cdp.querySelector('b').setAttribute('k', 'v');"
    "fetch('/api/clone?sh=' + (csh.childNodes.length === 0 && csh.getAttribute('id') === 'cwid' ? 'shallow' : 'wrong')"
    " + '&dp=' + encodeURIComponent(cdp.innerHTML)"
    " + '&det=' + (cdp.parentNode === null && cdp !== cw && !cdp.isConnected ? 'detached' : 'wrong')"
    " + '&orig=' + (cw.querySelector('b').getAttribute('k') === null ? 'untouched' : 'shared'));"
    /* §4.10 a `<template>`'s children are on its CONTENT fragment, not under it, so a walk that follows
       first_child copies the template and none of its markup. Whatever this engine does, it should be stated by
       a test rather than discovered by a page. */
    "var ct = document.createElement('div');"
    "ct.innerHTML = '<template><b>tc</b></template>';"
    "var ctc = ct.cloneNode(true);"
    "fetch('/api/clonetpl?v=' + encodeURIComponent(ctc.innerHTML));"
    "fetch('/api/position?v=' + ((document.body.compareDocumentPosition(c1) & Node.DOCUMENT_POSITION_DISCONNECTED) ? 'isdisc' : 'wrong'));"
    /* The interface OBJECTS: `instanceof` up the whole chain, and a derived one inheriting Node's constants. */
    "fetch('/api/iface?v=' + (document.body instanceof Element && document.body instanceof Node &&"
    " document instanceof Document && tx instanceof Text && tx instanceof CharacterData &&"
    " Text.ELEMENT_NODE === 1 ? 'isiface' : 'wrong'));"
    /* ---- CSS 2.1 §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'" and §10.7 "Minimum and
       maximum heights: 'min-height' and 'max-height'" --------------------------------------------------------
       EVERY BOX HERE SITS INSIDE AN EXPLICITLY SIZED ANCESTOR, and that is what makes `===` legal in this block
       for the same reason the replaced block above states: a used width solved against the INITIAL CONTAINING
       BLOCK carries the viewport's fact, so comparing one would fork instead of deciding. Rooting the chain in
       a declared `600px` gives every number below a single-point domain — and the one probe that deliberately
       does depend on the viewport EMITS, at the end.
       THE FIRST PAIR IS THE WHOLE POINT OF THE SECTION. §10.4's clamp is not `min(tentative, max-width)`: it
       re-runs §10.3 "using the computed value of 'max-width' as the computed value for 'width'", and §10.3.3's
       margin rules BRANCH on that — rule 5 zeroes an `auto` margin when `width` is `auto`, rules 4 and 6 give
       it the SLACK when it is not. So `margin: 0 auto` under a `max-width` that binds is centred (100px each
       side of a 400px box in a 600px block) and a clamp on the number alone would leave both margins at 0. */
    "var cw = document.createElement('div'); document.body.appendChild(cw);"
    "cw.style.setProperty('width', '600px');"
    "var cx = document.createElement('div'); cw.appendChild(cx);"
    "cx.style.setProperty('max-width', '400px');"
    "cx.style.setProperty('margin-left', 'auto'); cx.style.setProperty('margin-right', 'auto');"
    "var cxc = getComputedStyle(cx);"
    "fetch('/api/clampcentre?v=' + (cxc.width === '400px' && cxc.marginLeft === '100px' &&"
    " cxc.marginRight === '100px' ? 'iscentred' : 'wrong'));"
    /* §10.4's THREE STEPS ARE ORDERED AND THE ORDER IS OBSERVABLE. Step 3 is stated over "the RESULTING width"
       — step 2's output — so when the two limits contradict each other `min-width` is the one that wins. A
       three-argument clamp written in either order gives 300 here; the sequence gives 500. */
    "var cm = document.createElement('div'); cw.appendChild(cm);"
    "cm.style.setProperty('min-width', '500px'); cm.style.setProperty('max-width', '300px');"
    "fetch('/api/clamporder?v=' + (getComputedStyle(cm).width === '500px' ? 'isminwins' : 'wrong'));"
    /* §10.4's `<percentage>`: "calculated with respect to the width of the generated box's containing block",
       which is the 600px content box above — and a `min()` is not a second implementation of anything, because
       core/css/css_math.c resolves a math function inside the COMPUTED value, so `max-width` receives one
       absolute length. */
    "var cp = document.createElement('div'); cw.appendChild(cp);"
    "cp.style.setProperty('max-width', '50%');"
    "var cf = document.createElement('div'); cw.appendChild(cf);"
    "cf.style.setProperty('max-width', 'min(500px, 200px)');"
    "fetch('/api/clamppct?v=' + (getComputedStyle(cp).width === '300px' &&"
    " getComputedStyle(cf).width === '200px' ? 'ispct' : 'wrong'));"
    /* css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property" — "It affects the interpretation of
       all sizing properties" — so a `max-width` under `border-box` bounds the BORDER box, and css-sizing-3
       §3.3's own floor then applies to the substituted pass exactly as it does to a declared length: the
       content box is "calculated by subtracting the border and padding in the corresponding axis from the
       specified <length-percentage>, and flooring the result at zero (as the inner size of a box cannot be
       negative)", so a 50px limit against an 80px surround leaves the border box at 80 and not at 50. This
       is the case a `min` over two numbers gets wrong in the other direction. */
    "var cb2 = document.createElement('div'); cw.appendChild(cb2);"
    "cb2.style.setProperty('box-sizing', 'border-box');"
    "cb2.style.setProperty('padding-left', '40px'); cb2.style.setProperty('padding-right', '40px');"
    "cb2.style.setProperty('max-width', '50px');"
    "fetch('/api/clampborderbox?v=' + (getComputedStyle(cb2).width === '80px' ? 'isfloored' : 'wrong'));"
    /* §4.2.10.1/§4.2.10.2: childNodes and children are LIVE, which is the difference a static array cannot express
       — read the length, append, read it again. querySelectorAll is the one that is genuinely STATIC, and all
       three are real interfaces now rather than Arrays, so `.map` is honestly absent as the spec has it. */
    "var lv = document.createElement('div'); document.body.appendChild(lv);"
    "lv.appendChild(document.createElement('i')); lv.appendChild(document.createTextNode('t'));"
    "var lvc = lv.childNodes; var lve = lv.children; var lvq = lv.querySelectorAll('i');"
    "var lvn0 = lvc.length + ':' + lve.length + ':' + lvq.length;"
    "lv.appendChild(document.createElement('b'));"
    "var lvn1 = lvc.length + ':' + lve.length + ':' + lvq.length;"
    "fetch('/api/live?v=' + (lvn0 === '2:1:1' && lvn1 === '3:2:1'"
    " && lv.childNodes === lvc && lv.children === lve && lvq instanceof NodeList"
    " && lve instanceof HTMLCollection && lvc[0] === lv.firstChild && typeof lvc.map === 'undefined'"
    " ? 'islive' : 'wrong'));"
    /* §4.2.10.2 Interface HTMLCollection's NAMED getter — `getter Element? namedItem(DOMString name)`, whose
       method steps are "If key is the empty string, return null" and then the first element matching the
       supported property names. `children.foo` is how a great deal of older code reaches its own markup. */
    "var nb = document.createElement('u'); nb.setAttribute('id', 'namedkid'); lv.appendChild(nb);"
    "fetch('/api/named?v=' + (lv.children.namedItem('namedkid') === nb && lv.children.namedkid === nb"
    " && lv.children.nosuch === undefined ? 'isnamed' : 'wrong'));"
    /* §4.9 matches / closest, and §7.1's DOMTokenList — the two questions a router asks and the way a bundle
       gates a branch of its UI. classList holds NO tokens of its own: they are the `class` attribute split, so
       a write through the list and a write through setAttribute are the same write, and it time-travels
       because the attribute does. `classList === classList` is [SameObject] and a page relies on it. */
    "var cl = document.createElement('div'); cl.className = 'a b'; document.body.appendChild(cl);"
    "var cll = cl.classList;"
    "cll.add('c'); cll.add('a');"          /* already present: a set, so no duplicate */
    "cll.remove('b');"
    "var tg1 = cll.toggle('d'); var tg2 = cll.toggle('d');"        /* on, then off */
    "var tg3 = cll.toggle('a', true);"     /* force keeps it, and answers force */
    "var rp = cll.replace('c', 'e');"
    /* 'a b' +c +a(dup) -b +d -d a(force) c->e  ==>  'a e' */
    "fetch('/api/classlist?v=' + (cl.getAttribute('class') === 'a e'"
    " && tg1 === true && tg2 === false && tg3 === true && rp === true"
    " && cll.contains('e') && !cll.contains('b') && cll.length === 2 && cll.item(0) === 'a'"
    " && cl.classList === cll ? 'iscl' : 'wrong'));"
    /* §3.9's INDEXED PROPERTY GETTER — `list[0]` is a LOOKUP, not a property, which is the whole difference:
       it answers against the attribute as it is NOW, so it cannot go stale the way a written-out index would.
       And §3.7.9 Iterable declarations gives an interface with an INDEXED PROPERTY GETTER one
       %Array.prototype.values% as its @@iterator — that is its first arm, and §3.7.10 is the ASYNCHRONOUS
       declaration, which a DOMTokenList does not make — so `for..of` and
       spread over a classList are ordinary code that had nothing. */
    "var clj = [];"
    "for (var cq = 0; cq < cll.length; cq++) clj.push(cll[cq]);"
    "var clspread = [].concat.apply([], [[...cll]]);"
    "var clfor = ''; for (var ct of cll) clfor += ct;"
    "var clkeys = Object.keys(cll).join(',');"
    "cll.add('z');"   /* the lookup is live: index 2 exists only after this */
    "fetch('/api/clindex?v=' + (clj.join('') === 'ae' && clspread.join('') === 'ae' && clfor === 'ae'"
    " && clkeys === '0,1' && cll[2] === 'z' && cll[9] === undefined && !(5 in cll) && (1 in cll)"
    " ? 'isindex' : 'wrong'));"
    "cll.remove('z');"
    /* The list is a VIEW: a write through the attribute is visible through the list, with nothing to sync. */
    "cl.setAttribute('class', 'x y'); "
    "fetch('/api/clview?v=' + (cll.length === 2 && cll.contains('y') && String(cll) === 'x y'"
    " ? 'isview' : 'wrong'));"
    /* DOM §7.1 Interface DOMTokenList's `contains` OVER UNKNOWN EXTERNAL INPUT, which is the one question
       about this member no browser corpus can ask. That section states the steps whole — "The contains(token) method
       steps are to return true if this's token set[token] exists; otherwise false" — and a membership test
       against a token nobody knows has BOTH answers open, so the honest result is an unknown NAMING ITS
       SOURCE. A concrete `false` here is the collapse §solver forbids: `if (el.classList.contains(x))` then
       takes one arm for the flow and the gated branch behind the other is never explored, which is the exact
       shape §Attacker-sources calls a bundle gating a whole branch of its UI.
       THREE PARAMS ON ONE FETCH, because one of them cannot separate the three states this row is about.
       `c` is the member answering a KNOWN token, so it says the statement RAN and DOM §7.1 Interface
       DOMTokenList is right where the
       answer is determined. `h` is the control `idb-record`'s own `h` is: the same source reaching the same
       param list with no DOMTokenList in between, so a `v` with no shape in it can be charged to this member
       rather than to a source that never reaches an @H param at all. `v` is the claim. */
    "fetch('/api/clunknown?v=' + cll.contains(location.hash)"
    " + '&c=' + cll.contains('x') + '&h=' + location.hash);"
    "var m1 = cl.matches('div.x'); var m2 = cl.matches('span');"
    "var inner = document.createElement('b'); cl.appendChild(inner);"
    "var c1 = inner.closest('div'); var c2 = inner.closest('nav');"
    "fetch('/api/matches?v=' + (m1 && !m2 && c1 === cl && c2 === null"
    " && inner.closest('b') === inner ? 'ismatch' : 'wrong'));"
    /* §4.9: a selector that does not parse is a SyntaxError, never a quiet `false`. */
    "var msy = 'wrong'; try { cl.matches('!!!'); } catch (e) { msy = 'issyn'; }"
    "fetch('/api/matchbad?v=' + msy);"
    /* §2.9 THE PROPAGATION PATH. One dispatch walks the target and then its ancestors: `target` stays put,
       `currentTarget` moves, and the phase goes AT_TARGET then BUBBLING_PHASE. The old delivery enqueued each
       listener as its own job with no walk between them, so none of this could be observed. */
    "var bpar = document.createElement('section'); var bkid = document.createElement('em');"
    "bpar.appendChild(bkid); document.body.appendChild(bpar); var btrace = '';"
    "bkid.addEventListener('bub', function(e){ btrace += 'K' + e.eventPhase + (e.target === bkid ? 't' : '?')"
    " + (e.currentTarget === bkid ? 'c' : '?'); });"
    "bpar.addEventListener('bub', function(e){ btrace += 'P' + e.eventPhase + (e.target === bkid ? 't' : '?')"
    " + (e.currentTarget === bpar ? 'c' : '?'); });"
    "bkid.dispatchEvent(new Event('bub', { bubbles: true }));"
    "fetch('/api/bubble?v=' + (btrace === 'K2tcP3tc' ? 'isbubble' : 'wrong'));"
    /* An event that does not bubble reaches the target alone, and stopPropagation ends the walk after it. */
    "btrace = ''; bkid.dispatchEvent(new Event('bub'));"
    "var nb = btrace;"
    "btrace = ''; bkid.addEventListener('stp', function(e){ btrace += 'K'; e.stopPropagation(); });"
    "bpar.addEventListener('stp', function(e){ btrace += 'P'; });"
    "bkid.dispatchEvent(new Event('stp', { bubbles: true }));"
    "fetch('/api/nobubble?v=' + (nb === 'K2tc' && btrace === 'K' ? 'isnobub' : 'wrong'));"
    /* §2.9's CAPTURING leg. A capturing listener on the ancestor runs BEFORE the target's, at eventPhase 1 —
       the walk goes down the path and then back up it, which is why the target is its own leg and not an end
       of one. A registration with no options is not capturing, so the two must interleave in that order. */
    "btrace = '';"
    "bpar.addEventListener('cap', function(e){ btrace += 'Pc' + e.eventPhase; }, true);"
    "bpar.addEventListener('cap', function(e){ btrace += 'Pb' + e.eventPhase; });"
    "bkid.addEventListener('cap', function(e){ btrace += 'K' + e.eventPhase; });"
    "bkid.dispatchEvent(new Event('cap', { bubbles: true }));"
    "fetch('/api/capture?v=' + (btrace === 'Pc1K2Pb3' ? 'iscap' : 'wrong'));"
    /* …AND THE SAME LEG DECIDED OUT OF UNKNOWN EXTERNAL INPUT, which is the options bag a real bundle passes:
       `el.addEventListener(t, f, cfg.listen)` where `cfg` came from somewhere this engine cannot see. Web IDL
       §3.2.25 Union types picks the arm and DOM §2.7 Interface EventTarget's flatten options step 2 then reads
       `capture` off it — and on either arm that value is another unknown, so ONE registration is TWO: a
       capturing listener and a bubbling one, which the dispatch tells apart by the phase it runs at. A
       ToBoolean at the boundary answered `true` for every unknown there has ever been (a concolic wears an
       Object and every Object is truthy), so only the capturing world existed and the bubbling one — the
       DEFAULT the IDL writes, `boolean capture = false` — was the arm that never ran. Both phases must appear;
       one of them alone is the collapse. */
    "bpar.addEventListener('unkcap', function(e){ fetch('/api/aelunk?v='"
    " + (e.eventPhase === 1 ? 'aelcapturing' : 'aelbubbling')); }, state.listen);"
    "bkid.dispatchEvent(new Event('unkcap', { bubbles: true }));"
    /* …AND THE SAME COLLAPSE ONE LAYER OUT, AT A `boolean` ARGUMENT: Web IDL §3.2.3 boolean at a declared
       position, the conversion in core/idl_args.c every member and reflected setter declaring IDL_BOOLEAN
       shares. ECMAScript §7.1.2 ToBoolean ( arg ) ends "Return true" and unknown input wears an ordinary
       Object, so an unforked conversion runs only the true world; both values of each pair are the claim, by
       three routes:
         - `toggle(token, force)`, an argument declared IDL_BOOLEAN (DOM §7.1 Interface DOMTokenList's
           `[CEReactions] boolean toggle(DOMString token, optional boolean force)`);
         - `hidden`, a `[Reflect] boolean` content attribute typed from element.c's reflection registry;
         - `cloneNode(deep)`, an argument on a STEP MACHINE, whose own conversion parks.
       THE NEGATIVE CONTROL: `toggle('bctl', true)` is a value the page determined, so it must NOT fork —
       exactly one value under `/api/boolctl`. */
    "cll.toggle('bfork', state.beta);"
    "fetch('/api/boolarg?v=' + (cll.contains('bfork') ? 'boolon' : 'booloff'));"
    "bpar.hidden = state.beta2;"
    "fetch('/api/boolattr?v=' + (bpar.hasAttribute('hidden') ? 'attrset' : 'attrclear'));"
    "fetch('/api/boolclone?v='"
    " + (cl.cloneNode(state.beta3).childNodes.length ? 'clonedeep' : 'cloneshallow'));"
    "cll.toggle('bctl', true);"
    "fetch('/api/boolctl?v=' + (cll.contains('bctl') ? 'ctlon' : 'ctloff'));"
    /* §2.7's dedup key is (type, callback, CAPTURE), so ONE function registered both ways is TWO listeners —
       and removing it with the wrong flag removes neither. */
    "var dupn = 0; var dupf = function(){ dupn++; };"
    "bpar.addEventListener('dup', dupf, true); bpar.addEventListener('dup', dupf);"
    "bpar.addEventListener('dup', dupf, true);"   /* the exact same registration: does nothing */
    "bpar.dispatchEvent(new Event('dup'));"
    "var dup1 = dupn; bpar.removeEventListener('dup', dupf); dupn = 0;"
    "bpar.dispatchEvent(new Event('dup'));"
    "fetch('/api/dedup?v=' + (dup1 === 2 && dupn === 1 ? 'isdedup' : 'wrong'));"
    /* §2.9 "inner invoke" step 2: `once` removes the listener BEFORE calling it, so a re-entrant dispatch from
       inside the handler cannot run it a second time — which is exactly what `once` exists to prevent, and it
       is also the only way to tell "removed before" from "removed after". */
    "var oncen = 0;"
    "bpar.addEventListener('one', function(){ oncen++; if (oncen < 5) bpar.dispatchEvent(new Event('one')); },"
    " { once: true });"
    "bpar.dispatchEvent(new Event('one'));"
    "bpar.dispatchEvent(new Event('one'));"
    "fetch('/api/once?v=' + (oncen === 1 ? 'isonce1' : 'wrong'));"
    /* …AND THE REGRESSION GUARD ON THE OTHER MEMBER OF THAT UNION SHAPE, which is the half this change could
       have broken rather than fixed. `addEventListener`'s third argument is `(AddEventListenerOptions or
       boolean)`, so an omitted or null one now runs the §3.2.17 walk where it used to get a hand-built
       `{capture: false}`. DOM §2.7 Interface EventTarget's flatten options is "If options is a boolean, then
       return options" over the CONVERTED value, so `capture` must still be false on both spellings: two
       non-capturing listeners on the ANCESTOR, dispatched from the child, run in registration order at
       BUBBLING_PHASE. A capturing one would run first and at phase 1, which `D3N3` refuses. */
    "var uacap = '';"
    "bpar.addEventListener('uadef', function(e){ uacap += 'D' + e.eventPhase; });"
    "bpar.addEventListener('uadef', function(e){ uacap += 'N' + e.eventPhase; }, null);"
    "bkid.dispatchEvent(new Event('uadef', { bubbles: true }));"
    "fetch('/api/aelcapdef?v=' + (uacap === 'D3N3' ? 'iscapdef' : 'cap' + uacap));"
    /* …AND `passive` MUST STILL BE UNSET, which is the half the walk could have filled in. DOM §2.7's flatten
       more options leaves it null when the member does not exist and add an event listener step 4 then fills
       it from the DEFAULT PASSIVE VALUE — true for a "wheel" listener on a Window, a Document, its document
       element or its body, and false anywhere else. Both sides are asked with the SAME registration spelling
       (options omitted) and differ only in the TARGET, so what the pair reads is the tristate itself: the
       body's listener is passive, so DOM §2.2's in passive listener flag is set around it and
       `preventDefault` does nothing; the `<section>`'s is not, so the same call cancels. A walk that placed
       `passive` as `false` instead of leaving it absent would read `U-cancel` here and would be the same
       IDL_BOOLEAN-folds-absence defect the popover rows above are about, arriving at a different member. */
    "var uawh = '';"
    "document.body.addEventListener('wheel', function(e){ e.preventDefault();"
    " uawh += e.defaultPrevented ? 'U-cancel' : 'U-passive'; });"
    "document.body.dispatchEvent(new Event('wheel', { bubbles: true, cancelable: true }));"
    "var uawhx = '';"
    "bpar.addEventListener('wheel', function(e){ e.preventDefault();"
    " uawhx += e.defaultPrevented ? 'E-cancel' : 'E-passive'; });"
    "bpar.dispatchEvent(new Event('wheel', { cancelable: true }));"
    "fetch('/api/aelunset?v=' + (uawh === 'U-passive' && uawhx === 'E-cancel' ? 'isunsetpassive'"
    " : uawh + '/' + uawhx));"
    /* …AND THE SECOND MEMBER THAT DECLARES A `(boolean or D)` UNION. CSSOM VIEW §6 Extensions to the Element
       Interface's `Promise<undefined> scrollIntoView(optional (boolean or ScrollIntoViewOptions) arg = {})`:
       step 5 is the dictionary clause and step 6 "Otherwise, if arg is false, then set block to "end"". This
       asserts REACHABILITY AND THE DECLARATION, not the alignment: the viewport scrolling area equals the
       viewport here, so "start" and "end" are the same observable zero.
       A PROMISE-RETURNING OPERATION NEVER THROWS SYNCHRONOUSLY: Web IDL §3.7.7 Operations ends "if an exception
       E was thrown: If op has a return type that is a promise type, then return ! Call(%Promise.reject%,
       %Promise%, «E»)" (element_view.c's `idl_returns_promise()` says the same), so each arm attaches a
       handler and no rejection goes unhandled (HTML §8.1.4.7 would report it).
       THE ARMS: `o`/`n`/`d`/`b` each answer a thenable; `X` is §3.2.18's membership test delivered as a
       rejection, `x` the engine accepting `'bogus'`, `W` a rejection that is not a TypeError, and `T` a
       synchronous throw — the regression this shape catches. `o` and `n` route through §3.2.25 step 4's
       dictionary arm (element_view.c's `JS_IsObject(argv[0])` and idl_dict_walk_begin's frames DCHECK).
       The emit rides the reaction, so a run whose microtask never ran has no /api/scrollarm record. */
    "var uasc = document.createElement('p'); bpar.appendChild(uasc); var uascarm = ''; var uascp;"
    "try { uascp = uasc.scrollIntoView();"
    " uascarm += (uascp && typeof uascp.then === 'function') ? 'o' : 'O'; } catch (e) { uascarm += 'T'; }"
    "try { uascp = uasc.scrollIntoView(null);"
    " uascarm += (uascp && typeof uascp.then === 'function') ? 'n' : 'N'; } catch (e) { uascarm += 'T'; }"
    "try { uascp = uasc.scrollIntoView({});"
    " uascarm += (uascp && typeof uascp.then === 'function') ? 'd' : 'D'; } catch (e) { uascarm += 'T'; }"
    "try { uascp = uasc.scrollIntoView(false);"
    " uascarm += (uascp && typeof uascp.then === 'function') ? 'b' : 'B'; } catch (e) { uascarm += 'T'; }"
    /* THE REJECTION HANDLER IS ATTACHED SYNCHRONOUSLY ON THE RETURNED PROMISE, which is the half that removes
       the page error rather than declaring it: HTML §8.1.4.7's notify-about-rejected-promises runs at the end of the
       turn, so a promise already handled here is never reported at all and there is nothing to retract. Both
       arms emit, so a run that reached this statement always carries a record — an ABSENT /api/scrollarm and an
       `arm…` one are different facts. Neither handler can throw, so the derived promise cannot become a second
       unhandled rejection. */
    "try { uasc.scrollIntoView({ block: 'bogus' }).then(function () { uascarm += 'x';"
    "   fetch('/api/scrollarm?v=arm' + uascarm); }, function (e) {"
    "   uascarm += (e instanceof TypeError ? 'X' : 'W');"
    "   fetch('/api/scrollarm?v=' + (uascarm === 'ondbX' ? 'isscrollarm' : 'arm' + uascarm)); }); }"
    " catch (e) { uascarm += 'T'; fetch('/api/scrollarm?v=arm' + uascarm); }"
    /* …AND THE MEASUREMENT THE ROW ABOVE REFUSES TO ASSERT, EMITTED ANYWAY AND ASSERTED BY NOTHING. `na` says
       CSSOM VIEW §5's `scrollingElement` is not on this Document, and two equal numbers say the layout model
       has no scrolling area to align within — either answer is the reason the alignment has no row yet, and
       the day they differ this becomes one. An absent record and a zero are different facts, so it is emitted
       unconditionally rather than only when it has something to say. */
    "var uapos = 'na';"
    "if (document.scrollingElement) {"
    " uasc.scrollIntoView(); var uap0 = document.scrollingElement.scrollTop;"
    " uasc.scrollIntoView(false); var uap1 = document.scrollingElement.scrollTop;"
    " uapos = 'start' + uap0 + '-end' + uap1; }"
    "fetch('/api/scrollpos?v=' + uapos);"
    /* Pointer Events 4 §4 "Extensions to the Element interface" — `hasPointerCapture`, the one of its three
       members this engine installs (core/events/pointer_capture.h says why the other two are absent and what
       installing them costs). Every row here stays true after the other two land.
       THE FIRST ROW IS THE ANSWER AND THE CONVERSION TOGETHER: the member returns "true if the pending pointer
       capture target override for pointerId is set to the element on which this method is invoked, and false
       otherwise", and a new element has captured nothing, so `false` is a real browser's answer too; `v` is
       Web IDL §3.2.4.5 "long" having run at the declared position, evidenced by the `valueOf` side effect. */
    "var pca = document.createElement('i'); bpar.appendChild(pca); var pcans = ''; var pcv = '';"
    "pcans += (pca.hasPointerCapture(1) === false) ? 'f' : 'F';"
    "pcans += (pca.hasPointerCapture({ valueOf: function () { pcv = 'v'; return 7; } }) === false) ? 'f' : 'F';"
    "pcans += pcv;"
    "fetch('/api/pchas?v=' + (pcans === 'ffv' ? 'ispchas' : 'arm' + pcans));"
    /* THE SECOND ROW IS THE THROW ARMS, AND THE RECEIVER ONES ARE THE REASON IT EXISTS RATHER THAN DECORATION.
       Web IDL §3.7.7 "Operations" asks the receiver's implementation check BEFORE the argument conversion, so
       a foreign receiver is a TypeError with none of the page's code having run; the body then reaches
       `element_of_value` only for a receiver the declared predicate admitted, which is exactly what its
       `DCHECK` asserts the two agree about. IF THAT ORDER EVER BREAKS, THAT `DCHECK` IS A PAGE-HELD ABORT
       SWITCH: `Element.prototype.hasPointerCapture` is reachable from any page, and a FORCING solver calls
       members with unusual receivers constantly, so the distance between a TypeError this page catches and a
       `@WHY` that ends the run is one declaration. `n` is the primitive receiver and `o` the plain object; `a`
       is Web IDL §3.6 "Overload resolution algorithm" refusing a call shorter than the one REQUIRED position
       Pointer Events 4 §4 declares. All three are the ENGINE computing a refusal, not the absence of one. */
    "var pcb = '';"
    "try { Element.prototype.hasPointerCapture.call(null, 1); pcb += 'N'; }"
    " catch (e) { pcb += (e instanceof TypeError) ? 'n' : 'W'; }"
    "try { Element.prototype.hasPointerCapture.call({}, 1); pcb += 'O'; }"
    " catch (e) { pcb += (e instanceof TypeError) ? 'o' : 'W'; }"
    "try { pca.hasPointerCapture(); pcb += 'A'; }"
    " catch (e) { pcb += (e instanceof TypeError) ? 'a' : 'W'; }"
    "fetch('/api/pcbrand?v=' + (pcb === 'noa' ? 'ispcbrand' : 'arm' + pcb));"
    /* THE THIRD ROW IS AN INVARIANT OVER WHICH MEMBERS EXIST, AND IT IS NORMATIVE RATHER THAN A PREFERENCE OF
       THIS FILE'S. Pointer Events 4 §8.3 "Releasing pointer capture" step 2 is "If hasPointerCapture is false
       for the Element with the specified pointerId, then terminate these steps" — so `releasePointerCapture`
       READS this member, and a tree carrying the release side without the read side would be a §8.3 with no
       step 2 to run. `g` is that invariant holding: either the release side is absent, or the read side is
       installed too. True today, true after the diff that lands both, false only for the incoherent middle
       state — so it asserts a pairing without forbidding anything the next diff does. The corpus's most
       defensive real site depends on exactly this pairing, reaching its release call only when this member
       answers, which is the shape core/events/pointer_capture.h's ledger reads off it. */
    "var pcg = (('releasePointerCapture' in pca) ? ('hasPointerCapture' in pca) : true) ? 'g' : 'G';"
    "fetch('/api/pcgate?v=' + (pcg === 'g' ? 'ispcgate' : 'arm' + pcg));"
    /* THE FOURTH ROW ASSERTS NOTHING AND IS EMITTED UNCONDITIONALLY, which is what the `/api/scrollpos` row
       above already does and for the same reason: an ABSENT record and a token are different facts, and this
       one's question is one no run today can answer either way. What it DOES is REACH the two absent members
       on the day they exist, with a pointerId THIS PAGE supplied — so a landing that puts a `DCHECK` on that
       argument turns this line into a `@WHY` rather than into an exception the page catches. A page-supplied
       `pointerId` is INPUT, and Pointer Events 4 §8.2 "Setting pointer capture"' own step 1 answer for one
       that matches no active pointer is a "NotFoundError" DOMException. `-` is the member absent, `n` that
       refusal, `s` a call that returned, `T` a TypeError and `W` anything else. */
    "var pcr = '';"
    "if (typeof pca.setPointerCapture === 'function') {"
    " try { pca.setPointerCapture(0); pcr += 's'; }"
    " catch (e) { pcr += (e && e.name === 'NotFoundError') ? 'n' : ((e instanceof TypeError) ? 'T' : 'W'); }"
    "} else { pcr += '-'; }"
    "if (typeof pca.releasePointerCapture === 'function') {"
    " try { pca.releasePointerCapture(0); pcr += 's'; }"
    " catch (e) { pcr += (e && e.name === 'NotFoundError') ? 'n' : ((e instanceof TypeError) ? 'T' : 'W'); }"
    "} else { pcr += '-'; }"
    "fetch('/api/pcreach?v=pc' + pcr);"
    /* §4.10 FORMS — a submission is a REQUEST the page's own code composes, and submit() DERIVES it without
       sending anything, which is the rule for a state-mutating request. The value a field carries is the
       endpoint's example value, and when it is an ATTACKER SOURCE the finding says so rather than inventing
       one — which is why the value STATE holds a JSValue and not bytes. */
    "var fq = document.createElement('form'); fq.action = '/api/search';"
    "var fi = document.createElement('input'); fi.name = 'q'; fq.appendChild(fi); fi.value = state.q;"
    "var fd = document.createElement('input'); fd.name = 'off'; fd.setAttribute('disabled',''); fq.appendChild(fd);"
    "var fc = document.createElement('input'); fc.name = 'agree'; fc.setAttribute('type','checkbox');"
    "fq.appendChild(fc); document.body.appendChild(fq); fq.submit();"
    "fc.checked = true; fq.action = '/api/consent'; fq.submit();"
    /* NOT `fi.value === state.q` — that compares two CONCOLICS, which forks both ways and reports both
       answers. The state's presence is proved by the submission carrying {state}.q; what this asserts is the
       tree-shaped half, which is concrete. */
    "fetch('/api/formstate?v=' + (fq.elements.length === 3 && document.forms.length >= 1 ? 'isform' : 'wrong'));"
    /* §4.10.21.4 requestSubmit() fires a CANCELABLE `submit` FIRST, and submits only if nothing cancelled it —
       a page that cancels is doing its own request instead, so recording the form's would be a finding the page
       never makes. Only an ABSENCE can prove that half. */
    "var fr = document.createElement('form'); fr.action = '/api/never'; document.body.appendChild(fr);"
    "fr.addEventListener('submit', function(e){ e.preventDefault(); }); fr.requestSubmit();"
    "var fo = document.createElement('form'); fo.action = '/api/didsubmit';"
    "var foi = document.createElement('input'); foi.name = 'ok'; fo.appendChild(foi);"
    "document.body.appendChild(fo); foi.value = 'rs'; fo.requestSubmit();"
    /* Web IDL §3.7.6 Attributes: an enumeration attribute's setter ignores a string outside the enumeration —
       no TypeError, and the attribute keeps its value. An argument position still throws; ceext above is one. */
    "var scrign = 'wrong';"
    "try { history.scrollRestoration = 'manual'; history.scrollRestoration = 'bogus';"
    " scrign = history.scrollRestoration === 'manual' ? 'isignored' : 'changed'; } catch (e) { scrign = 'threw'; }"
    "fetch('/api/scrign?v=' + scrign);"
    "</script>"
    /* §4.2.4 getElementById OVER AN ELEMENT WHOSE `id` THIS FLOW HAS NOT DETERMINED — the statement that
       reaches core/dom/node.c's attribute value seam; every other getElementById here asks a concrete id over a
       tree in which no `id` is a source.
       `bidu` is a derived source (`{location.hash}.slice(1)`), so after the write `#bidseed`'s `id` is
       undetermined and the comparison against `bidwant` has two feasible answers: the IS arm returns the
       element and the IS NOT arm advances past it, §4.2.4's own two answers.
       `arm=isnot` IS THE REACHABILITY WITNESS AND `arm=is` THE SUBJECT: `isnot` is outcome 0, the arm a run
       with no forking takes, and also what a build without the seam answers (element.c stores the display
       shape for a concolic attribute write, so the comparison against `{location.hash}.slice(1)` is false).
       The payloads are literals, so neither address can become unknown, and `#bidwant` is named by nothing
       else in this document, so the IS arm cannot be answered concretely by a second element.
       IT IS LAST: it leaves an undetermined `id` in the tree, so every later getElementById would fork there. */
    "<span id=bidseed></span>"
    "<script>"
    "var bidu = location.hash.slice(1);"
    "var bidel = document.getElementById('bidseed');"
    "bidel.setAttribute('id', bidu);"
    "fetch('/api/byidseam?arm=' + (document.getElementById('bidwant') ? 'is' : 'isnot'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
