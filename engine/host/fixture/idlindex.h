/* THE INDEX ARGUMENT DOCUMENT: one unknown index or offset through two members of an indexed family.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDLINDEX_H
#define ENGINE_HOST_FIXTURE_IDLINDEX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDLINDEX[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"


    /* ONE UNKNOWN `unsigned long index` THROUGH TWO MEMBERS OF core/idl_index_arg.c's FAMILY — whether a fork
       key names the PREDICATE or the SITE. Each `item(index)` decomposes into the elimination chain
       `index == 0`, `index == 1`, … over the value's own identity, so a flow that answered them at one member
       must answer them the same way at the next: the fact is about the NUMBER, not the collection.
       THE ASSERTION IS THE ONE FACT NO CONSISTENT WORLD CAN CARRY: the NodeList is built two longer than
       `document.styleSheets`, so an index past the end of the short one is past the end of the long one in
       every world; `split` is unreachable by arithmetic. `state.ix` carries no example, so every link is asked
       with the machine stating nothing.
       TWO DIFFERENT ALGORITHM NAMES ARE REQUIRED (CSSOM §6.2.2's StyleSheetList and DOM §4.2.10's NodeList),
       because two reads of one interface already shared a key. */
    "var _ixL = document.styleSheets.length;"
    "var _ixh = document.createElement('div');"
    "for (var _ixj = 0; _ixj < _ixL + 2; _ixj++) { _ixh.appendChild(document.createElement('p')); }"
    "document.body.appendChild(_ixh);"
    "var _ixn = _ixh.querySelectorAll('p');"
    /* ONE READ AND ONE VALUE. Two `state.ix` reads would be two operands of whatever identity the exotic get
       composes, and the assertion is about ONE unknown answered twice — a second read is the one way this
       statement could pass while saying nothing. */
    "var _ixi = state.ix;"
    "var _ixa = document.styleSheets.item(_ixi);"
    "var _ixb = _ixn.item(_ixi);"
    "fetch('/api/idxkey?v=' + (_ixa && !_ixb ? 'split' : 'agree'));"

    /* THE SAME TWO MEMBERS OVER AN INDEX THAT CARRIES AN EXAMPLE — the completion half of the fork key, which
       the statement above cannot reach. solver/decide.c's walk asks about the machine's real completion first,
       so the link the example names files "is it completion 1" where every other flow files "is it completion
       0": a predicate and its negation under two keys. The outcome sibling minted at that link stands on the
       arm the example does not name (decide_note_forced_arm contradicts the example as it replays), so its
       next member must not re-fork a fact the flow already holds.
       AGAIN ARITHMETIC: both collections are EIGHT long, so `!_pka && _pkb` (past the end of the NodeList,
       inside the CSSRuleList) is impossible for any index; the contradicting flow's FIRST member is the one
       that exhausted. `screen.width & 3` is an unknown with an example in [0, 3] (ECMAScript §6.1.6.1.16
       NumberBitwiseOp runs on the example), strictly below eight; `location.hash` would be an @S source whose
       candidate sessions substitute bytes at the read.
       DOM §4.2.10's NodeList and CSSOM §6.4.1's CSSRuleList both CALL core/idl_index_arg.c's chain — check the
       call site, not a banner. One read of the operand: a second would be a second identity. */
    "var _pks = document.createElement('style');"
    "var _pkt = ''; for (var _pkj = 0; _pkj < 8; _pkj++) { _pkt += '.pk' + _pkj + ' { color: red }'; }"
    "_pks.textContent = _pkt;"
    "var _pkh = document.createElement('div');"
    "for (var _pkq = 0; _pkq < 8; _pkq++) { _pkh.appendChild(document.createElement('b')); }"
    "document.body.appendChild(_pks); document.body.appendChild(_pkh);"
    "var _pkn = _pkh.querySelectorAll('b');"
    "var _pkr = _pks.sheet.cssRules;"
    "var _pki = screen.width & 3;"
    "var _pka = _pkn.item(_pki);"
    "var _pkb = _pkr.item(_pki);"
    "fetch('/api/pkkey?v=' + (!_pka && _pkb ? 'split' : 'agree')"
    " + '&n=' + _pkn.length + '&c=' + _pkr.length);"

    /* THE TWO MEMBERS OF THAT FAMILY WHOSE COLLECTIONS HANG OFF ONE ELEMENT — DOM §7.1 Interface DOMTokenList
       and DOM §4.9.1 Interface NamedNodeMap — over ONE unknown index. Both REFUSED an unknown index by name
       until they became step machines, so before that this statement did not emit a wrong verdict: it took the
       whole document down at whichever of the two DCHECKs the flow reached first, and every row after it read
       NOT REACHED.
       IT IS A FRESH UNKNOWN AND NOT `state.ix`, and that is load-bearing rather than tidy. The constraint key
       these chains file under is the PREDICATE over the VALUE's own identity, so a chain drawn over an operand
       another statement has already walked would find its questions ANSWERED and fork nothing — the statement
       would pass by reading a record instead of by asking, which is the one way it could look right while
       exercising none of this. One read into `_nmi`, for the `_ix` statement's reason exactly.
       THE ASSERTION IS AN ARITHMETIC IMPOSSIBILITY AND THE PREMISE IS EMITTED. The token set is built TWO long
       and the attribute list FIVE (`class` and four `data-*`), so `split` claims one index is INSIDE a
       two-token set and PAST THE END of a five-attribute map at once — false for every index there is, so
       reaching it is a flow contradicting itself and never a length this fixture guessed. Both lengths ride
       the record so the row can refuse to read the verdict at all when they are not 2 and 5. */
    "var _nmh = document.createElement('u');"
    "_nmh.className = 'nm0 nm1';"
    "_nmh.setAttribute('data-a', '1'); _nmh.setAttribute('data-b', '2');"
    "_nmh.setAttribute('data-c', '3'); _nmh.setAttribute('data-d', '4');"
    "document.body.appendChild(_nmh);"
    "var _nmi = state.nmix;"
    "var _nma = _nmh.classList.item(_nmi);"
    "var _nmb = _nmh.attributes.item(_nmi);"
    "fetch('/api/nnmkey?v=' + (_nma !== null && _nmb === null ? 'split' : 'agree')"
    " + '&t=' + _nmh.classList.length + '&a=' + _nmh.attributes.length);"

    /* CSSOM §6.6.1 The CSSStyleDeclaration Interface's `item(index)` OVER AN UNKNOWN INDEX — the one member of
       that family whose past-the-end world is not null, and therefore the one whose TERMINAL is worth a row of
       its own. §6.6.1: "If there is no indexth object in the collection, then the method must return the empty
       string", where every `item(index)` beside it answers null.
       THE STATEMENT MAKES NO VERDICT AND EMITS THE ANSWER ITSELF, which is why no length can make it lie. A
       two-declaration block asked with an example-free index emits `color` from the world that answered
       `index == 0`, `display` from the world that answered `index == 1`, and `past` from the parent that
       eliminated both — three values from ONE line, which a single-world engine cannot produce whatever the
       block's length turns out to be. A WRONG TERMINAL IS VISIBLE RATHER THAN SILENT: `_cdv === ''` is false
       for null, so a member answering null past the end emits the four characters `null` instead of `past`,
       and the row below refuses that spelling by name.
       `&n=` CARRIES THE BLOCK'S OWN LENGTH so the emitted set can be read against the collection it came from
       rather than against this comment. */
    "var _cdh = document.createElement('q');"
    "_cdh.setAttribute('style', 'color: red; display: block');"
    "document.body.appendChild(_cdh);"
    "var _cdi = state.cdix;"
    "var _cdv = _cdh.style.item(_cdi);"
    "fetch('/api/cssditem?v=' + (_cdv === '' ? 'past' : _cdv) + '&n=' + _cdh.style.length);"

    /* DOM §4.11 Interface Text's `splitText` AND DOM §4.10 Interface CharacterData's `substringData` OVER ONE
       UNKNOWN OFFSET, ON TWO Text NODES OF KNOWN DIFFERENT LENGTHS, both members step machines.
       IT IS A FRESH UNKNOWN, for the reason the `/api/nnmkey` statement above gives: a chain drawn over an
       operand another statement has already walked would find its questions ANSWERED and fork nothing, so the
       row would pass by reading a record instead of by asking. One read into `_cxo`.
       THE ASSERTION IS AN ARITHMETIC IMPOSSIBILITY AND THE PREMISE IS EMITTED. §4.11's split a Text node step 2
       and §4.10's substring data step 2 are the same sentence — "If offset is greater than length, then throw
       an "IndexSizeError" DOMException" — so `split` claims one offset is WITHIN a two-unit node and PAST THE
       END of a nine-unit one at once, false for every offset there is. The other direction is perfectly
       ordinary (offset 5 throws on the short node and not on the long one) and is exactly why the verdict is
       written this way round. Both lengths are captured BEFORE the calls and ride the record, because
       `splitText` TRUNCATES the node it splits and a length read afterwards would describe a different node
       than the one the question was asked of. */
    "var _cxp = document.createElement('p'); _cxp.appendChild(document.createTextNode('ab'));"
    "var _cxq = document.createElement('p'); _cxq.appendChild(document.createTextNode('abcdefghi'));"
    "document.body.appendChild(_cxp); document.body.appendChild(_cxq);"
    "var _cxa = _cxp.firstChild, _cxb = _cxq.firstChild;"
    "var _cxal = _cxa.length, _cxbl = _cxb.length;"
    "var _cxo = state.cxoff;"
    "var _cxr = 'ok'; try { _cxa.splitText(_cxo); } catch (_cxe) { _cxr = 'throw'; }"
    "var _cxs = 'ok'; try { _cxb.substringData(_cxo, 0); } catch (_cxe2) { _cxs = 'throw'; }"
    "fetch('/api/cdsplit?v=' + (_cxr === 'ok' && _cxs === 'throw' ? 'split' : 'agree')"
    " + '&a=' + _cxal + '&b=' + _cxbl);"

    /* DOM §4.10's `substringData(offset, count)` WITH BOTH OPERANDS UNKNOWN — the row for the SECOND chain,
       which is the operand that names no position in anything and is therefore the one a reader is most
       likely to think the family does not cover.
       THE STATEMENT MAKES NO VERDICT AND EMITS THE ANSWER ITSELF, so no length can make it lie: it sends the
       LENGTH OF THE RETURNED STRING, which substring data steps 3-4 make exactly `min(count, length - offset)`.
       Over a three-unit node that is `1` only from a world that answered `count == 1` (offset 0, 1 or 2), and
       `2` only from `count == 2` (offset 0 or 1) — neither is reachable from the offset chain alone, whose
       worlds with `count` unasked would all carry the clamp. So `2` among the emitted values is the count
       chain having run, and `throw` among them is the offset chain having reached step 2's IndexSizeError at
       exhaustion. `&n=` carries the node's own length so the set can be read against the node it came from
       rather than against this comment. */
    "var _cun = document.createElement('b'); _cun.appendChild(document.createTextNode('xyz'));"
    "document.body.appendChild(_cun);"
    "var _cut = _cun.firstChild, _cutl = _cut.length;"
    "var _cuo = state.cuoff, _cuc = state.cucnt;"
    "var _cuv; try { _cuv = '' + _cut.substringData(_cuo, _cuc).length; } catch (_cue) { _cuv = 'throw'; }"
    "fetch('/api/cdsub?v=' + _cuv + '&n=' + _cutl);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
