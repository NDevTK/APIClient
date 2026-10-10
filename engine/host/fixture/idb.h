/* THE INDEXED DATABASE DOCUMENT: two databases opened from the page's own door, one program.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDB_H
#define ENGINE_HOST_FIXTURE_IDB_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDB[] =
    "<!doctype html><html><body><script>"
    /* INDEXED DATABASE FROM THE PAGE'S OWN DOOR — `indexedDB.open` through to `success`, and every part of
       the marker is a different sentence of the standard.
       `0:1` is §4.2's IDBVersionChangeEvent carrying §5.7 step 7's OLD VERSION and the version being opened,
       which is what every migration in every bundle branches on. `versionchange` is §4.10's mode getter over
       the transaction §5.7 step 2 created and §2.8.1's "the transaction of an open request is null unless an
       upgradeneeded event has been fired" made reachable. `pending` is §4.1's readyState while the `get`'s
       done flag is false — the request is returned BEFORE its operation runs, which is the whole of what
       "asynchronously execute" means. `99:done` is the value §6.1 stored under key 2 and §6.2 read back,
       delivered by §5.9's fire-a-success-event with the flag already set by
       IndexedDB §5.6 "Asynchronously executing a request" step 5.6.2.
       AND THE ORDER IS THE PROOF: `success` at the open request fires only after the upgrade transaction has
       FINISHED (§4.3's note says so, and §5.7 step 10 is what waits), and nothing calls `commit()` — §5.9
       step 9.3 found the request list empty once the `get` handler returned. `_r.result.version` is §4.4's
       version getter over the CONNECTION, which §5.1 step 9 set to the version that was opened. */
    /* AND §2.5's LIST KEY PATH, whose whole observable surface is the `keyPath` getter. `a+b` is §4.5's
       conversion of the list, and Web IDL §3.2.21 makes it "a new Array object created as if by the expression
       []" — `plain`: not a FrozenArray, which neither `readonly attribute any keyPath` nor §4.5 names. `same`
       is §4.5's note ("it returns the same object instance every time it is inspected"), which §3.2.21 alone
       does not give. `perhandle` is the other half of that note over a second handle opened after the upgrade,
       and the `a+b` before it says the `push` into the first reached no store ("Changing the properties of the
       object has no effect on the object store"). `transaction('kp')` takes §3.2.25's STRING arm: a primitive
       is not an Object, so step 11.2's GetMethod is never reached and the name is not iterated.
       AND §7.1's IN-LINE KEYS — a `put` with no key, the store's key path read out of the value. `7:u-9` is the
       record found under a key nothing passed, read back off `.id.v`, so the key was extracted rather than
       moved. `DataError` is §7.1's `failure` arm reported where §4.5 puts it, SYNCHRONOUSLY out of `put` where
       the page's try/catch sees it, for a value with nothing at the key path and no key generator. */
    /* AND THE ROUND TRIP ITSELF, WHICH USED TO BE AN IN-C FIXTURE. §6.1 is a step machine now (its step 5
       drives §7.1 and therefore §7.4's array arm), so there is no C entry for it and these four claims run
       where a page runs them — which is strictly more than the C fixture asserted, because each goes through
       IndexedDB §5.6's request and §5.9/§5.10's event rather than calling the operation directly.
       `ConstraintError` is §6.1 step 2's no-overwrite refusal — what tells `add` from `put` — delivered as an
       `error` event that `preventDefault()` keeps from aborting the transaction (§5.10 step 9.3, which is the
       reason that event is cancelable at all). `first1` is §2.2's ordering read through §6.2: the records
       arrive 3, 1, 2 and the first in an unbounded-below range is the SMALLEST key, not the one that arrived
       first. `over21` is §6.1 step 3: a second put under one key REPLACES the record rather than filing a
       second one. `undoneAD` is §5.5 step 2 over the two RECORD changes, reached through a real `abort()`:
       the record the transaction ADDED is gone and the record it REMOVED is back with its own value.
       AND THE TAINT, which is the half a browser's own test suite cannot have: `location.hash` goes into a
       store and has to come back out still concolic, because a page keeps its session in here and reads it
       into gated code. §Attacker-sources names IndexedDB as a source; a store that de-tainted on the way in
       would answer with a plausible datum and delete the fork the read should produce. */
    "var _r = indexedDB.open('fixture', 1); var _idb = ''; var _k; var _rec = ''; var _tv; var _ix = '';"
    "_r.onupgradeneeded = function(e){"
    " var _s = _r.result.createObjectStore('s');"
    " _s.put(99, 2); var _g = _s.get(2);"
    " _idb += e.oldVersion + ':' + e.newVersion + ':' + _r.transaction.mode + ':' + _g.readyState;"
    " _g.onsuccess = function(){ _idb += ':' + _g.result + ':' + _g.readyState; };"
    " _k = _r.result.createObjectStore('kp', { keyPath: ['a', 'b'] });"
    " _idb += ':' + (Array.isArray(_k.keyPath) ? _k.keyPath.join('+') : 'notalist')"
    "       + ':' + (_k.keyPath === _k.keyPath ? 'same' : 'fresh')"
    "       + ':' + (Object.isFrozen(_k.keyPath) ? 'frozen' : 'plain');"
    " _k.keyPath.push('c');"
    " var _i = _r.result.createObjectStore('in', { keyPath: 'id.v' });"
    " _i.put({ id: { v: 'u-9' }, n: 7 });"
    " try { _i.put({ q: 1 }); _idb += ':nothrow'; } catch (_e) { _idb += ':' + _e.name; }"
    " var _ig = _i.get('u-9');"
    " _ig.onsuccess = function(){ _idb += ':' + _ig.result.n + ':' + _ig.result.id.v; };"
    /* THE ORDER OF THESE HANDLERS IS IndexedDB §5.6 STEP 5.1's, not this statement's: a transaction's
       requests are
       executed in the order they were placed, so the tokens accumulate in placement order. */
    " var _o = _r.result.createObjectStore('ord');"
    " _o.put(30, 3); _o.put(10, 1); _o.put(20, 2); _o.put(21, 2);"
    " var _oa = _o.add(11, 1);"
    " _oa.onerror = function(ev){ ev.preventDefault(); _rec += _oa.error.name; };"
    " var _ok = _o.getKey(IDBKeyRange.lowerBound(0));"
    " _ok.onsuccess = function(){ _rec += ':first' + _ok.result; };"
    " var _o2 = _o.get(2);"
    " _o2.onsuccess = function(){ _rec += ':over' + _o2.result; };"
    " var _t = _r.result.createObjectStore('t'); _t.put(location.hash, 1);"
    " var _tg = _t.get(1); _tg.onsuccess = function(){ _tv = _tg.result; };"
    /* §2.6's INDEX, created into an EMPTY store — the arm where the population request finds nothing to walk.
       The two indexes are the two arms of §6.1 step 5: `by_name` files ONE record whose key is the index key, and
       `by_tag` is multiEntry, so an ARRAY key files one record PER SUBKEY — which is what makes `multi2`
       load-bearing, both records carrying tag 'b'. `ref` is §6.3's retrieve-a-referenced-value (the index
       record's value is a store key, and the answer is the record THAT key names), `pk` is §6.3's
       retrieve-a-value (the store key itself), and `meta` is §4.6's attributes over the index handle. */
    " var _n = _r.result.createObjectStore('idx');"
    " var _x = _n.createIndex('by_name', 'name');"
    " var _mx = _n.createIndex('by_tag', 'tags', { multiEntry: true });"
    " _ix += _x.name + ':' + _x.keyPath + ':' + (_x.unique ? 'u' : 'nu')"
    "      + ':' + (_mx.multiEntry ? 'me' : 'nome')"
    "      + ':' + (_x.objectStore === _n ? 'own' : 'other')"
    "      + ':' + (_n.index('by_name') === _x ? 'same' : 'fresh');"
    " _n.put({ name: 'alice', tags: ['a', 'b'] }, 1);"
    " _n.put({ name: 'bob', tags: ['b', 'c'] }, 2);"
    " var _xg = _x.get('bob'); _xg.onsuccess = function(){ _ix += ':ref' + _xg.result.name; };"
    " var _xk = _x.getKey('bob'); _xk.onsuccess = function(){ _ix += ':pk' + _xk.result; };"
    /* AND §4.5's NOTE, THE HALF THAT IS NOT A FAILURE — the records go in FIRST and the index is created over
       a store that already holds them, so the only thing that can make these two retrievals answer is the
       POPULATION REQUEST walking the store's records through §6.1 step 5. `popben` is §6.3's
       retrieve-a-referenced-value over a record no `put` ever filed an index entry for, and `back2` is the
       multiEntry arm doing the same thing — one index record per subkey of a key extracted from a value the
       store was already holding, both records carrying tag 'x'. Created eagerly, as this file used to, both
       would answer with an empty index. */
    " var _pp = _r.result.createObjectStore('pop');"
    " _pp.put({ name: 'ann', tags: ['x'] }, 1);"
    " _pp.put({ name: 'ben', tags: ['x', 'y'] }, 2);"
    " var _px = _pp.createIndex('by_name', 'name');"
    " var _pm = _pp.createIndex('by_tag', 'tags', { multiEntry: true });"
    " var _pg = _px.get('ben'); _pg.onsuccess = function(){ _ix += ':pop' + _pg.result.name; };"
    " var _pc = _pm.count('x'); _pc.onsuccess = function(){ _ix += ':back' + _pc.result; };"
    /* §5.12 Creating a request to retrieve multiple items' `count`, ASKED OF A VALUE NOBODY KNOWS — the
       question a browser's own corpus cannot ask, having no unknown. §4.5 declares `getAll(optional any
       queryOrOptions, optional [EnforceRange] unsigned long count)` and Web IDL passes unknown input through a
       declared type AS ITSELF, so §6.2's retrieve-multiple-items opens with the honest arm — "If count is not
       given or is 0 (zero), let count be infinity" — a SUPERSET a later example can only narrow.
       `k` IS THE CONTROL: a `getAll` that ignored its count would answer all three records for the unknown and
       satisfy `u`; two records for a count of two says the not-given arm was CHOSEN.
       AND THE THIRD FACT IS AN ORDER. `count` is `[EnforceRange] unsigned long` in `IDBGetAllOptions` too, and
       Web IDL §3.2.17 Dictionary types converts members "in lexicographical order" (step 4.1.4.1), so `count`
       is refused before `direction` is read and the page's `toString` never runs. `e` and `d` are two rows
       because a TypeError raised after `direction` was read is a different defect from none at all. */
    " var _ga = _r.result.createObjectStore('ga');"
    " _ga.put('g1', 1); _ga.put('g2', 2); _ga.put('g3', 3);"
    " var _gr = IDBKeyRange.lowerBound(0), _gs = '', _gd = 0, _ge = 'nothrow';"
    " var _gu = _ga.getAll(_gr, location.hash.length);"
    " var _gk = _ga.getAll(_gr, 2);"
    " _gu.onsuccess = function(){ _gs = _gu.result.join(''); };"
    " try { _ga.getAllRecords({ count: -1,"
    "  direction: { toString: function(){ _gd = 1; return 'next'; } } }); }"
    " catch (_gx) { _ge = _gx.name; }"
    " _gk.onsuccess = function(){ fetch('/api/idbcount?u=' + _gs + '&k=' + _gk.result.join('')"
    "  + '&e=' + _ge + '&d=' + (_gd ? 'READ' : 'unread')); };"
    " var _xc = _mx.count('b');"
    " _xc.onsuccess = function(){ fetch('/api/idbidx?v=' + _ix + ':multi' + _xc.result); }; };"
    "_r.onsuccess = function(){"
    " var _k2 = _r.result.transaction('kp').objectStore('kp');"
    " _idb += ':' + _k2.keyPath.join('+') + ':' + (_k2.keyPath === _k.keyPath ? 'shared' : 'perhandle');"
    " fetch('/api/idbopen?v=' + _idb + ':' + _r.result.version + ':' + _r.result.name);"
    /* §5.5 STEP 2 THROUGH §5.5 ITSELF. One record is added under a key the store has none for and one the
       store does hold is deleted, and the abort has to undo BOTH — which is also the pair whose inverses are
       a removal and a re-file, run backwards. The verification is a second transaction, because the aborted
       one is FINISHED and every member of §4.5 refuses one. */
    " var _at = _r.result.transaction('ord', 'readwrite'), _as = _at.objectStore('ord');"
    " _as.put(77, 9); _as.delete(1);"
    " var _m9 = _as.get(9), _m1 = _as.get(1);"
    /* THE ABORT RUNS FROM A HANDLER, NOT FROM THIS TASK. `abort()` called here would reach §5.5 step 6 before
       any of the four requests had executed — the operations would never run, there would be nothing to
       revert, and both `undone` terms would read TRUE for a transaction that had changed nothing. So the two
       writes are read back first, and `made77gone` is what makes the claim below about a real state. */
    " _m1.onsuccess = function(){"
    "  _rec += ':made' + _m9.result + (_m1.result === undefined ? 'gone' : 'LEFT'); _at.abort(); };"
    " _at.onabort = function(){"
    "  var _vs = _r.result.transaction('ord').objectStore('ord');"
    "  var _v9 = _vs.get(9), _v1 = _vs.get(1);"
    "  _v1.onsuccess = function(){"
    /* `h` IS THE CONTROL FOR `t`, ON THE SAME FETCH so that one endpoint record carries both. `t` is
       `location.hash` after a round trip through an object store; `h` is the SAME source reaching the SAME
       param list without one. Without it a `t` that does not read `{hash}` cannot say whether the STORE
       de-tainted the value or whether this source never reaches an @H param as a shape at all — two unrelated
       mechanisms under one 0, which is the defect the row above was just split to remove. */
    "   fetch('/api/idbrec?v=' + _rec + ':undone' + (_v9.result === undefined ? 'A' : 'LEAKA')"
    "        + (_v1.result === 10 ? 'D' : 'LEAKD') + '&t=' + _tv + '&h=' + location.hash); }; }; };"

    /* §4.5's WORKED EXAMPLE, IN ITS OWN DATABASE because its whole point is that the UPGRADE TRANSACTION dies:
       two records queued under one name, then a UNIQUE index over that name. The standard's own words are that
       "the index's uniqueness constraint does not cause the second request to fail. Instead, the transaction
       will be aborted when the index is created and the constraint fails." So `ConstraintError` is the
       TRANSACTION's error (§4.10's getter over what §5.10 step 9.3 aborted with) and NOT any request's — the
       second `put` succeeded — and the `AbortError` beside it is §5.1 step 10.8's, which is what a page's own
       `open` reports for an upgrade that aborted. Both halves are load-bearing: the first says the failure was
       the index creation's, and the second says the open FAILED rather than handing the page a connection to a
       half-migrated database. */
    "var _u = indexedDB.open('uniqfix', 1); var _uq = 'NOABORT';"
    "_u.onupgradeneeded = function(){"
    " var _us = _u.result.createObjectStore('u');"
    " _us.put({ name: 'betty' }, 1);"
    " _us.put({ name: 'betty' }, 2);"
    " _us.createIndex('by_name', 'name', { unique: true });"
    " var _ut = _u.transaction;"
    " _ut.onabort = function(){ _uq = _ut.error.name; }; };"
    "_u.onerror = function(){ fetch('/api/idbuniq?v=' + _uq + ':' + _u.error.name); };"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
