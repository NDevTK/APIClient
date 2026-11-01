/* THE VALUELESS ATTRIBUTE DOCUMENT. DOM §4.9.2 "Interface Attr": an attribute's value is a string, so a parsed
   `<div inert>` holds the empty string exactly as setAttribute('inert', '') does. Each row reads one engine path
   that asks a parsed valueless attribute's presence or value: getAttribute, inertness and the focusable-area
   list (`inert`, `contenteditable` in its true and invalid states, `<a href>`, `draggable`), `dataset`,
   isEqualNode, a MutationObserver's oldValue, DOMTokenList's update steps, a custom element upgrade's
   attributeChangedCallback, `pattern` under requestSubmit's interactive validation (a `method=dialog` form,
   whose submission closes its dialog; `closedby=any` keeps that dialog's close watcher enabled, so this
   document asks nothing of a disabled one), the declarative shadow root's boolean attributes, the parser's Noah's
   Ark clause (HTML §13.2.4.3 "The list of active formatting elements") pairing `x` with `x=""`, and
   `[inert=""]`.
   Chrome 153 (Playwright) answers ga=empty inert=0 ce=1 cb=0 ah=1 dr=00 ds=empty eq=11 mo=empty-vx cl=1
   up=null-empty pt=10 sh=11 na=7 sel=1 for this markup.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ATTRPRES_H
#define ENGINE_HOST_FIXTURE_ATTRPRES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ATTRPRES[] =
    "<!doctype html><html><body>"
    "<div id=ga inert><button id=ib>i</button></div>"
    "<div id=ce contenteditable>e</div><div id=cb contenteditable=bogus>b</div>"
    "<a id=ah href>a</a><div id=dr draggable>d</div><div id=dt draggable=true>t</div>"
    "<div id=ds data-x></div>"
    "<p id=eq><span x></span><span x></span></p>"
    "<div id=mo title></div><div id=cl class></div>"
    "<x-up id=up foo></x-up>"
    "<dialog id=pd open closedby=any><form id=pf method=dialog><input id=pt pattern value=abc></form></dialog>"
    "<div id=sh><template shadowrootmode=open shadowrootclonable shadowrootdelegatesfocus><button>s</button>"
    "</template></div>"
    "<div id=na><p><b x><b x=\"\"><b x><b x=\"\">t</p>u</div>"
    "<script>"
    "requestAnimationFrame(function () {"
    "  var $ = function (i) { return document.getElementById(i); };"
    "  var t = function (v) { return v === null ? 'null' : v === undefined ? 'undef' : v === '' ? 'empty' : 'v' + v; };"
    "  var r = {};"
    "  r.ga = t($('ga').getAttribute('inert'));"
    "  function foc(id) { var e = $(id); e.focus(); var f = document.activeElement === e ? 1 : 0; e.blur(); return f; }"
    "  r.inert = foc('ib');"
    "  r.ce = foc('ce');"
    "  r.cb = foc('cb');"
    "  r.ah = foc('ah');"
    "  r.dr = foc('dr') + '' + foc('dt');"
    "  r.ds = t($('ds').dataset.x);"
    "  var s3 = document.createElement('span'); s3.setAttribute('x', '');"
    "  var q = $('eq').children;"
    "  r.eq = (q[0].isEqualNode(q[1]) ? 1 : 0) + '' + (q[0].isEqualNode(s3) ? 1 : 0);"
    "  var m = new MutationObserver(function () {});"
    "  m.observe($('mo'), { attributes: true, attributeOldValue: true });"
    "  $('mo').setAttribute('title', 'x');"
    "  $('mo').removeAttribute('title');"
    "  r.mo = m.takeRecords().map(function (x) { return t(x.oldValue); }).join('-');"
    "  var m2 = new MutationObserver(function () {});"
    "  m2.observe($('cl'), { attributes: true });"
    "  $('cl').classList.remove('z');"
    "  r.cl = m2.takeRecords().length;"
    "  var cev = 'none';"
    "  customElements.define('x-up', class extends HTMLElement {"
    "    static get observedAttributes() { return ['foo']; }"
    "    attributeChangedCallback(n, o, v) { cev = t(o) + '-' + t(v); }"
    "  });"
    "  r.up = cev;"
    "  var inv = 0;"
    "  $('pt').addEventListener('invalid', function () { inv = 1; });"
    "  $('pf').requestSubmit();"
    "  r.pt = inv + '' + ($('pd').open ? 0 : 1);"
    "  var sr = $('sh').shadowRoot;"
    "  r.sh = sr ? (sr.clonable ? 1 : 0) + '' + (sr.delegatesFocus ? 1 : 0) : 'none';"
    "  r.na = $('na').querySelectorAll('b').length;"
    "  r.sel = document.querySelectorAll('[inert=\"\"]').length;"
    "  fetch('/api/attrpres?' + Object.keys(r).map(function (k) { return k + '=' + r[k]; }).join('&'));"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
