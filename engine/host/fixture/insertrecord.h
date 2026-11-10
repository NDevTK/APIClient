/* THE INSERT RECORD DOCUMENT. DOM §4.3.3 "Interface MutationRecord"'s childList records of DOM §4.2.3's insert,
   pre-insert, replace and replace all, as each member that reaches them queues them: appendChild and insertBefore
   of a node (al, ib) and of a DocumentFragment (fr, fi, fe), ParentNode's append and prepend and ChildNode's
   before, after and replaceWith (ap, pp, bf, af, rw), replaceChildren (rc), replaceChild of a node, of a sibling
   and of a fragment (rp, rs, rf), insertAdjacentElement/Text (ia), and HTML §8.5.4-§8.5.6's innerHTML,
   outerHTML and insertAdjacentHTML (ih, oh, ah). Each value lists the records, then the children (`_c`).
   Chrome 153 (Playwright) answers every value test_forced.c's rows expect except al and ib, where it reports the
   node's previous sibling after adopt's removal (al=p_a_rb_pa_n0~p_ab_r_pa_n0_ca.b,
   ib=p_a_rb_pa_nc~p_ab_r_pa_nc_ca.b.c); see node_insert.c's step 6.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INSERTRECORD_H
#define ENGINE_HOST_FIXTURE_INSERTRECORD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INSERTRECORD[] =
    "<!doctype html><html><body>"
    "<script>"
    "requestAnimationFrame(function () {"
    "  var r = {};"
    "  function d(n) { return n === null ? '0' : n.nodeType === 3 ? 't' + n.data : n.id || "
    "n.nodeName.toLowerCase(); }"
    "  function ids(l) { var o = []; for (var i = 0; i < l.length; i++) o.push(d(l[i])); return o.join('.'); }"
    "  function mk(id) { var e = document.createElement('i'); e.id = id; return e; }"
    "  function box(list) { var p = document.createElement('p'); p.id = 'p'; list.forEach(function (i) { "
    "p.appendChild(mk(i)); }); document.body.appendChild(p); return p; }"
    "  function frag(list) { var f = document.createDocumentFragment(); list.forEach(function (i) { "
    "f.appendChild(mk(i)); }); return f; }"
    "  function run(k, p, also, act) {"
    "    var m = new MutationObserver(function () {});"
    "    m.observe(p, { childList: true });"
    "    if (also) m.observe(also, { childList: true });"
    "    act();"
    "    r[k] = m.takeRecords().map(function (x) {"
    "      return (x.target === p ? 'p' : 'f') + '_a' + ids(x.addedNodes) + '_r' + ids(x.removedNodes) + '_p' + "
    "d(x.previousSibling) + '_n' + d(x.nextSibling);"
    "    }).join('~') + '_c' + ids(p.childNodes);"
    "    m.disconnect();"
    "    p.remove();"
    "  }"
    "  var p, f;"
    "  p = box(['a', 'b']); run('al', p, null, function () { p.appendChild(p.lastChild); });"
    "  p = box(['a', 'b', 'c']); run('ib', p, null, function () { p.insertBefore(p.childNodes[1], p.lastChild); });"
    "  p = box(['a']); f = frag(['x', 'y']); run('fr', p, f, function () { p.appendChild(f); });"
    "  p = box(['a', 'b']); f = frag(['x', 'y']); run('fi', p, f, function () { p.insertBefore(f, p.lastChild); });"
    "  p = box(['a']); f = frag([]); run('fe', p, f, function () { p.appendChild(f); });"
    "  p = box(['a']); run('ap', p, null, function () { p.append(mk('x'), 'k'); });"
    "  p = box(['a']); run('pp', p, null, function () { p.prepend('k', mk('x')); });"
    "  p = box(['a', 'b']); run('bf', p, null, function () { p.lastChild.before(mk('x'), p.firstChild); });"
    "  p = box(['a', 'b']); run('af', p, null, function () { p.firstChild.after(p.lastChild, mk('x')); });"
    "  p = box(['a', 'b', 'c']); run('rw', p, null, function () { p.childNodes[1].replaceWith(mk('x'), 'k'); });"
    "  p = box(['a', 'b']); run('rc', p, null, function () { p.replaceChildren(mk('x'), p.firstChild); });"
    "  p = box(['a', 'b']); run('rp', p, null, function () { p.replaceChild(mk('x'), p.firstChild); });"
    "  p = box(['a', 'b']); run('rs', p, null, function () { p.replaceChild(p.lastChild, p.firstChild); });"
    "  p = box(['a', 'b']); f = frag(['x', 'y']); run('rf', p, f, function () { p.replaceChild(f, p.firstChild); });"
    "  p = box(['a']); run('ia', p, null, function () { p.firstChild.insertAdjacentElement('afterend', mk('x')); "
    "p.firstChild.insertAdjacentText('beforebegin', 'k'); });"
    "  p = box(['a', 'b']); run('ih', p, null, function () { p.innerHTML = '<i id=x></i><i id=y></i>'; });"
    "  p = box(['a', 'b', 'c']); run('oh', p, null, function () { p.childNodes[1].outerHTML = "
    "'<i id=x></i><i id=y></i>'; });"
    "  p = box(['a']); run('ah', p, null, function () { p.firstChild.insertAdjacentHTML('afterend', "
    "'<i id=x></i><i id=y></i>'); p.insertAdjacentHTML('afterbegin', 'k'); });"
    "  fetch('/api/insertrecord?' + Object.keys(r).map(function (k) { return k + '=' + r[k]; }).join('&'));"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
