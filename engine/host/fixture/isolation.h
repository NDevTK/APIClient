/* THE ISOLATION DOCUMENT: the boot fork on injected state, and per-flow isolation of every shared object kind under it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ISOLATION_H
#define ENGINE_HOST_FIXTURE_ISOLATION_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ISOLATION[] =
    "<!doctype html><html>"
    "<body>"
    "<script>var cfg = { admin: state.admin };"
    "var delObj = { k: 'keepVAL' };"   /* a shared BASELINE object; a forked flow will DELETE its k -> must revert per-flow */
    "var rx = { _f: 'base' };"   /* a reactive-framework style object: `flag` is an ACCESSOR backed by _f (Vue does exactly this) */
    "Object.defineProperty(rx, 'flag', { get: function(){ return this._f; }, set: function(v){ this._f = v; }, configurable: true });"
    /* THE WEAK COLLECTIONS ARE MINTED HERE, BEFORE THE DOCUMENT'S FIRST FORK, so their four rows are about
       isolation. cow.c skips an object the running flow created (`JS_ObjFlowGen(obj) > d->fork_gen`), so a
       collection minted after a fork is flow-private in every arm, each arm mints its own, and a row over it
       reads 1 with the capture deleted. This <script> is the first to name `state`, so nothing above it has
       forked: these are baseline and both `cfg.admin` arms share them.
       core-js's internal-state module has this shape (a boot-time `new WeakMap` keyed by the object being
       initialized); that it is what a real bundle's "Object already initialized" throw is remains a
       hypothesis, and the isolation is what this fixture states.
       The overwrite and delete keys start with a record, or their arms would be adds under another name;
       wmAddA/wmAddP start absent because an add has no before. */
    "var wmIso = new WeakMap(); var wmAddA = {}, wmAddP = {}, wmOv1 = {}, wmOv2 = {}, wmDel1 = {}, wmDel2 = {};"
    "wmIso.set(wmOv1, 'wo1B'); wmIso.set(wmOv2, 'wo2B'); wmIso.set(wmDel1, 'wdB'); wmIso.set(wmDel2, 'wdB');"
    "var wsIso = new WeakSet(); var wsAddA = {}, wsAddP = {}, wsDelA = {}, wsDelP = {};"
    "wsIso.add(wsDelA); wsIso.add(wsDelP);"
    /* THE STRONG-COLLECTION AND ARRAY BASELINES, MINTED HERE FOR THE SAME REASON. solver/decide.c's
       `decide_arm` asks `concolic_branch_decided(key)` before the replay and the fork, so a flow that has
       answered a predicate refines a second test of it: `cfg.admin` is one value on an ordinary object, every
       `if (cfg.admin)` composes the same branch key, and only the first one forks. A fork sets both deltas'
       fork_gen to the live generation and bumps it, so an object minted after the first fork is flow-private
       in every arm and the capture under it never runs. Minted here, `JS_ObjFlowGen` is <= every later
       fork_gen and the arms share these objects.
       The overwrite and delete subjects carry a baseline value so the three mutation kinds stay distinct;
       `pfa` is empty because an append's claim is what the array accumulates.
       This document is one line, so a `@WHY` column is the only coordinate into it; these must precede the
       first fork, so they sit at the top and every column after them moves with them. */
    "var pfa = []; var owa = ['ow1B', 'ow2B']; var _sad = new Set();"
    "var _mm = new Map([['mmk1', 'base'], ['mmk2', 'base']]);"
    /* WEB IDL §3.7.9.1 "Default iterator objects"' ITERATOR OVER A BASELINE TARGET, minted before the first fork
       for the reason above and one more: the per-flow state is owned by the ITERATOR, not the target. The
       cursor §3.7.9.1 calls `index` lives in the malloc'd record behind the iterator's class opaque
       (core/idl_iter.c), and `idl_pair_iter_of` captures it with that iterator as the owner handed to
       cow_capture_host_record, so `itcur`'s generation decides whether the capture runs at all.
       URL §6.2 "URLSearchParams class" rather than `Headers`: a Headers iteration sorts by name and combines
       each name's values, so a walk that lost turns could still spell a plausible string, while a
       URLSearchParams list keeps insertion order and one entry per pair. The claim below is which pairs the
       cursor reached. */
    "var itsp = new URLSearchParams('itk1=1&itk2=2'); var itcur = itsp.entries();"
    /* AND THE POSITIVE WITNESS, WHICH IS WHY IT STANDS HERE AND NOT AFTER THE FORK. Every other row of this
       family is a two-world claim whose 0 is "never reached" OR "a world was lost"; this one is a SINGLE
       world emitted by the pre-fork flow, so its 1 says the statement RAN with nothing else folded into it.
       §AND-THE-SAME-HOLE-SWALLOWS-A-PREDICTION is the reason it exists at all: without it every witness this
       family has is an ABSENCE — a DCHECK that did not fire and a leak walk that found nothing — and those
       report the same byte for a correct capture and for a path nobody took.
       THE FIRST CLAUSE IS WHAT MAKES THE OTHER FOUR ABOUT A *WEAK* COLLECTION AT ALL. A strong Map accepts a
       primitive key, so `set('notanobject', …)` THROWING is the page-visible statement that this object is on
       JS_IsWeakCollection's side of the branch and that its records are therefore captured through a
       JSCowWeakRef cell rather than through a dup of the key. A `new WeakMap` that had quietly become a Map
       would satisfy all four isolation rows below and assert nothing this family exists to assert. The key is
       a concrete string, so is_valid_weakref_target decides it and nothing forks. */
    "var wmWeak = 'wmSTRONG'; try { wmIso.set('notanobject', 1); } catch (e) { wmWeak = 'wmWEAK'; }"
    "fetch('/api/weakreach?v=' + wmWeak + ((wmIso instanceof WeakMap) ? 'MAP' : 'NOMAP')"
    " + ((wsIso instanceof WeakSet) ? 'SET' : 'NOSET') + (wmIso.get(wmOv1) === 'wo1B' ? 'BASE' : 'NOBASE'));"
    "</script>"
    "<script>"
    "if (cfg.admin) { setBodyAttr('data-tt','ttADMIN'); appendChild('kidADMIN'); rx.flag='flagADMIN'; fetch('/api/data?role=admin'); loadScript('/chunk/admin.js'); } else { setBodyAttr('data-tt','ttPUBLIC'); appendChild('kidPUBLIC'); rx.flag='flagPUBLIC'; fetch('/api/data?role=public'); }"   /* admin arm: same endpoint MERGES + a LAZY CHUNK loads. Each arm ALSO writes an attribute, appends a child node, AND assigns the ACCESSOR rx.flag (invokes the setter -> rx._f) -> per-flow DOM + heap-accessor writes across the EXISTING fork. */
    "fetch('/api/whoami?tt=' + getBodyAttr('data-tt'));"   /* DOM ATTR READ-BACK after the fork: per-flow -> admin flow reads ttADMIN, public flow reads ttPUBLIC */
    "fetch('/api/kid?mark=' + lastChildMark());"   /* DOM NODE READ-BACK: each flow's appended child is its OWN last child -> admin reads kidADMIN, public reads kidPUBLIC (neither's inserted node leaks) */
    "fetch('/api/flag?v=' + rx.flag);"   /* ACCESSOR READ-BACK: rx.flag reads rx._f (per-flow). WITHOUT the accessor-skip fix, cow_unapply would JS_SetProperty(rx.flag, undefined) -> re-invoke the setter -> corrupt _f -> v=undefined */
    "if (state.region === 'us-east-1') { fetch('/api/region/' + state.region); }"   /* EQ gate: true arm PINS region -> real @H value /api/region/us-east-1 */
    "Promise.resolve(cfg.admin ? 'thenADMIN' : 'thenPUBLIC').then(function(v){ fetch('/api/then?v=' + v); });"   /* ASYNC-AS-FLOW: a microtask reaction fires as part of THIS flow, under its COW -> admin flow reads thenADMIN, public thenPUBLIC (per-flow reaction isolation) */
    "(async function(){ var w = await Promise.resolve(cfg.admin ? 'awADMIN' : 'awPUBLIC'); fetch('/api/await?w=' + w); })();"   /* ASYNC FUNCTION + await: suspend at await, resume the continuation with the settled value under THIS flow's COW */
    "(async function(){ function h(f){ return f ? 'acADMIN' : 'acPUBLIC'; } fetch('/api/asynccall?w=' + h(cfg.admin)); })();"   /* ASYNC body calls a HELPER whose concolic branch forks DEEP (chain base->async->helper): the async frame is a CALLER, not the deepest — exercises clone_deep_flow's async-as-caller buffer sourcing (tramp_buf_base/tramp_live_sf) -> both acADMIN and acPUBLIC */
    "(async function(){ throw 'asyncThrew'; })().catch(function(e){ fetch('/api/caught?e=' + e); });"   /* async THROW -> rejected promise -> .catch reaction fires */
    "(async function(){ var s=0; for(var i=0;i<2000;i++){ s=s+1; } fetch('/api/asyncloop?s='+s); })();"   /* async body with a LOOP -> preempt may fire inside the async tramp frame */
    "if (state.gamma) { delete delObj.k; } fetch('/api/tok?t=' + (delObj.k ? delObj.k : 'wasDeleted'));"   /* DELETE time-travel: the gamma-true flow deletes delObj.k; the gamma-false flow must STILL see keepVAL (the delete is per-flow) */
    /* THE SAME DELETE ON THE GLOBAL, WHICH IS A DIFFERENT OBJECT KIND AND WAS THE ONE THIS DOCUMENT NEVER MADE.
       Every `delete` above is on an ordinary object; the global is EXOTIC (the Window class's supported-index
       hooks), so a delete of a name it does not own misses the shape and lands in the exotic dispatch — and a
       creation this flow then removes is the same arrival by the other route. Both are what a real bundle does
       (`delete globalThis.__loggedIn` sits in a logout path), and both reach the swap as a slot removal that
       must consult neither 10.1.10.1 step 3's [[Configurable]] nor the class's own handler. gdSOLE on BOTH arms
       is the two-sided claim: each arm read back ITS OWN global creation, and neither absent-slot delete left
       anything behind for the other to find. */
    "delete globalThis.gdGone; globalThis.gdTag = cfg.admin ? 'gdADMIN' : 'gdPUBLIC';"
    " fetch('/api/gdel?v=' + globalThis.gdTag + (globalThis.gdGone === undefined ? 'gdSOLE' : 'gdLEAK'));"
    " delete globalThis.gdTag;"
    /* A FLOW-LOCAL OBJECT UNDER A REFINED BRANCH. `floc()` runs after the document's first `cfg.admin` branch,
       so this `if` is refined rather than forked (solver/decide.c's `decide_arm` asks
       `concolic_branch_decided` first): each existing arm runs `var o = { x: 'base' }` itself,
       `JS_ObjFlowGen(o) > d->fork_gen` holds in both, and cow.c's flow-private skip returns before any
       capture. So the row measures that both arms reach this statement; each arm reads back only its own
       write, so it could not see a leak either.
       NAMED RESIDUAL. Not covered: that a flow-private object's writes are skipped rather than captured, the
       "only shared baseline state is captured" invariant, which has no row. The next diff builds a count
       asserting that a flow-private write adds no delta entry, a fact about the delta's size rather than an
       emitted value. Its absence shows as a delta that grows with a loop body's per-iteration transients
       rather than with shared state touched, observed in the entry count a flow parks with. */
    "function floc(){ var o = { x: 'base' }; if (cfg.admin) { o.x = 'flocADMIN'; } else { o.x = 'flocPUBLIC'; } fetch('/api/floc?x=' + o.x); } floc();"
    "[1,2].forEach(function(e){ var w = cfg.admin ? 'feADMIN' : 'fePUBLIC'; fetch('/api/fefork?e=' + e + w); });"   /* CONCOLIC branch INSIDE a forEach callback: forks DEEP (chain base->iter-callback). clone_deep_flow must clone the callback frame's cont_state (JSArrayEvery) so the sibling continues the iteration independently -> both feADMIN and fePUBLIC over elements 1 AND 2 */
    /* SHARED-ARRAY append isolation over the BASELINE `pfa` minted in the first <script> — see the paragraph
       there for why it may not be minted on this line, and for the read of `decide_arm` that settles it. The
       append's read is already order-independent: `join` renders what the array ACCUMULATED, so whichever arm
       runs second reads 'pushA,pushB' the instant the fast-array-append capture is missing, and that value
       names NEITHER world. */
    "if (cfg.admin) { pfa.push('pushA'); } else { pfa.push('pushB'); } fetch('/api/pushfork?a=' + pfa.join(','));"
    /* SHARED-ARRAY OVERWRITE isolation over the BASELINE `owa`. EACH ARM OVERWRITES ITS OWN SLOT AND THE READ
       RENDERS BOTH, which is what makes this a claim about isolation rather than about reachability: an arm
       that writes a slot and reads back only THAT slot reads its own write whether or not the write was
       captured, so a leak is invisible and the row degenerates. This statement previously had exactly that
       shape — both arms wrote `owa[0]` and each read `owa[0]` back — so 'owA' and 'owB' were both emitted
       under EITHER ordering with the element capture deleted.
       READING THE SIBLING'S SLOT MEANS WHICHEVER ARM RUNS SECOND SEES THE FIRST ARM'S WRITE the instant the
       capture is missing, for either ordering, and emits 'owA-owP' — a value NEITHER world names. The two
       worlds are 'owA-ow2B' (admin overwrote slot 0, slot 1 still baseline) and 'ow1B-owP' (public overwrote
       slot 1, slot 0 still baseline). This is `weakover`'s `wo1A-wo2P` shape over a plain array. */
    "if (cfg.admin) { owa[0] = 'owA'; } else { owa[1] = 'owP'; } fetch('/api/owfork?a=' + owa[0] + '-' + owa[1]);"
    "fetch('/api/mapfork?r=' + [1,2].map(function(e){ return (cfg.admin ? 'mA' : 'mP') + e; }).join('-'));"   /* CONCOLIC branch inside a MAP callback: the deep-fork clones the JSArrayEvery cont_state (shared ret array, COW-isolated per arm via fast-array capture). cfg.admin is config (symbolic, forks per element) -> 4 clean combos incl mA1-mA2 and mP1-mP2, none with a dropped element */
    /* GENERATOR-BODY CONCOLIC FORK — placed LAST so its arms don't multiply the downstream fixture subtree (a
       branch's cost is (upstream flows) × (downstream work); at the tail the downstream work is just the fetch). */
    "function* ggf(){ if (cfg.admin) { yield 'ggADMIN'; } else { yield 'ggPUBLIC'; } } var ggi = ggf(); fetch('/api/genfork?v=' + ggi.next().value);"   /* CONCOLIC branch INSIDE a synchronously-driven generator body: the .next() drive runs the body on the tramp chain, so the branch snapshot-forks the tramp-driven generator activation (the named nested-activation hold-out) -> both ggADMIN and ggPUBLIC, never a DFAIL */
    "function* g2f(){ var a = cfg.admin ? 'A' : 'P'; var b = state.beta ? 'X' : 'Y'; yield 'g2:'+a+b; } var g2i = g2f(); fetch('/api/gen2fork?v=' + g2i.next().value);"   /* TWO concolic branches in ONE generator body: the FIRST fork installs a per-flow gen_data clone on the sibling; when that sibling hits the SECOND branch it re-forks the SAME generator -> exercises cow_delta_add_gendata's dedup-REPLACE (keep the original base, swap the clone). All four combos g2:AX/AY/PX/PY ⇒ each fork advanced its own generator state */
    "function* gof(){ if (cfg.admin) { yield 'ofA'; } else { yield 'ofP'; } } var ofr=''; for (const x of gof()) { ofr += x; } fetch('/api/genofork?v=' + ofr);"   /* FOR-OF generator-body concolic fork: the generator is driven by for-of (its object lives on the caller stack, not the tramp frame), so clone_deep_flow recovers it from caller_sp[forof_off] to record the per-flow gen_data swap -> both ofA and ofP */
    "function* aff(){ if (cfg.admin) { yield 'afA'; } else { yield 'afP'; } } fetch('/api/afromfork?v=' + Array.from(aff())[0]);"   /* Array.from(GEN) consumer fork: the gen body branches while CONSUMED by Array.from on the tramp (CONT_ITER_CONSUME), so clone_deep_flow's gen-branch clones the JSIterConsume state -> both afA and afP */
    "function* spf(){ if (cfg.admin) { yield 'spA'; } else { yield 'spP'; } } fetch('/api/spreadfork?v=' + [...spf()][0]);"   /* [...GEN] spread consumer fork: same CONT_ITER_CONSUME machinery, SPREAD sink (append to the literal's array), forks mid-consume -> both spA and spP */
    /* SHARED-SET record isolation over the BASELINE `_sad` minted in the first <script>. THE READ IS A
       POSITION AND NOT A KEY, which is what keeps it order-independent: §24.2 Set Objects iterates in
       INSERTION order, so `[0]` is whichever arm added FIRST. With the map_add capture present each arm's
       set holds one record and `[0]` is its own; with it missing the arm that runs SECOND reads the FIRST
       arm's token, so one of the two worlds is never emitted under either ordering and the row is 0. */
    "if (cfg.admin) { _sad.add('sadA'); } else { _sad.add('sadP'); } fetch('/api/setaddfork?v=' + [...(_sad)][0]);"
    "function* sef(){ if (cfg.admin) { yield 'seA'; } else { yield 'seP'; } } fetch('/api/setfork?v=' + [...new Set(sef())][0]);"   /* new Set(GEN) consumer fork: the Set consumer (CONT_ITER_CONSUME, SET sink) forks mid-consume; now fork-SAFE via the map_add COW capture -> both seA and seP */
    /* SHARED-MAP overwrite/delete isolation over the BASELINE `_mm` minted in the first <script>. EACH ARM
       MUTATES ITS OWN KEY AND THE READ RENDERS BOTH, for the reason spelled at `owfork` above and at the weak
       family below. THE PREVIOUS SHAPE COULD NOT DETECT A LEAK AT ALL and this is the row that shape was
       named from: both arms mutated `'k'` and each read back only `'k'`, so the overwrite arm emitted 'mmA'
       and the delete arm emitted 'gone' under EITHER ordering with the map_mutate undo log deleted — the row
       measured that both arms were REACHED while reading as a two-world isolation claim, which is the
       §A-FIELD-A-CONSUMER-DEFAULTS defect performed on the verification layer.
       THE TWO WORLDS ARE 'mmA-has' AND 'base-gone', and a leak emits 'mmA-gone' in EITHER ordering — admin
       first leaves `mmk1` overwritten under the delete arm, public first leaves `mmk2` absent under the
       overwrite arm, and both compose a value neither world names. `base-gone` carries the English word
       `gone` this fixture emits from four earlier statements, which costs nothing: `param_value_is` compares
       the WHOLE value of this endpoint's own `v`, so no other statement's record can satisfy it. */
    "if (cfg.admin) { _mm.set('mmk1','mmA'); } else { _mm.delete('mmk2'); }"
    "fetch('/api/mapmutfork?v=' + _mm.get('mmk1') + '-' + (_mm.has('mmk2') ? 'has' : 'gone'));"
    /* THE SAME THREE MUTATIONS OVER A WEAK COLLECTION, over records captured through a JSCowWeakRef cell
       instead of a dup of the key, on the baseline collections the first <script> mints. One branch feeds
       all four rows: they need one pair of siblings sharing those collections, not four more forks.
       EACH ARM MUTATES ITS OWN KEY AND READS BOTH, which makes the rows order-independent: an arm reading back
       only its own key sees its own write whether or not it was captured. Reading the sibling's key, the arm
       that runs second sees the first arm's mutation as soon as a capture is missing, and emits a value
       neither world names — `wa11` (add), `wo1A-wo2P` (overwrite), `wd00` (delete), `ws1100` (WeakSet).
       Those four come from running both arms' bodies over one set of collections with no unapply between
       them, which needs only ECMAScript §24.3 "WeakMap Objects" and §24.4 "WeakSet Objects".
       The WeakSet is a different class id reaching the same two hooks, the one statement here that asks
       JS_IsWeakCollection about JS_CLASS_WEAKSET; a class list that omitted it would send a WeakSet record
       down the strong arm, which dups the key and pins every object the set has held for the session. */
    "if (cfg.admin) { wmIso.set(wmAddA, 1); wmIso.set(wmOv1, 'wo1A'); wmIso.delete(wmDel1);"
    " wsIso.add(wsAddA); wsIso.delete(wsDelA); }"
    " else { wmIso.set(wmAddP, 1); wmIso.set(wmOv2, 'wo2P'); wmIso.delete(wmDel2);"
    " wsIso.add(wsAddP); wsIso.delete(wsDelP); }"
    "fetch('/api/weakadd?v=wa' + (wmIso.has(wmAddA) ? '1' : '0') + (wmIso.has(wmAddP) ? '1' : '0'));"
    "fetch('/api/weakover?v=' + wmIso.get(wmOv1) + '-' + wmIso.get(wmOv2));"
    "fetch('/api/weakdel?v=wd' + (wmIso.has(wmDel1) ? '1' : '0') + (wmIso.has(wmDel2) ? '1' : '0'));"
    "fetch('/api/weakset?v=ws' + (wsIso.has(wsAddA) ? '1' : '0') + (wsIso.has(wsAddP) ? '1' : '0')"
    " + (wsIso.has(wsDelA) ? '1' : '0') + (wsIso.has(wsDelP) ? '1' : '0'));"
    /* WEB IDL §3.7.9.1 "Default iterator objects"' `index` IS PER-FLOW STATE — the claim the weak family
       above makes for a record inside a collection, made for a record behind a CLASS OPAQUE, where the write is
       seen by no property hook and no engine hook. `itcur` is ONE iterator over ONE baseline
       URLSearchParams, minted in the document's FIRST <script>; see the paragraph there for why it may not be
       minted on this line, and for what makes the ITERATOR rather than its target the object whose generation
       decides the capture. Both arms reach this line and each steps that one cursor to exhaustion.
       THE READ IS ORDER-INDEPENDENT, WHICH IS WHAT MAKES THIS AN ISOLATION CLAIM RATHER THAN A REACHABILITY
       ONE — the repair `owfork` and `mapmutfork` above carry. The cursor is not a slot each arm writes and
       reads back: it is CONSUMED. The instant idl_pair_iter_of's capture is missing, whichever arm runs SECOND
       finds `index` already standing at the length, its `while` makes no turn at all, and it emits `-itADMIN`
       or `-itPUBLIC` with an EMPTY walk — a value NEITHER world names, under either ordering. With the capture
       each arm's delta holds its own `index` off a base of 0 and both walk the whole list.
       THE PAIR KEYS ARE CARRIED AND NOT THE VALUES, because §3.7.9's `entries` arm yields [key, value] and the
       key is what the cursor's position selects; a row built on the values of `itk1=1&itk2=2` would read `12`
       and pass over a list walked in the wrong order. */
    "(function(){ var s = ''; var e; while (!(e = itcur.next()).done) { s += e.value[0]; }"
      " fetch('/api/iteriso?v=' + s + '-' + (cfg.admin ? 'itADMIN' : 'itPUBLIC')); })();"
    "(async function(){ function* afsf(){ if (cfg.admin) { yield 'afsA'; } else { yield 'afsP'; } } var out=[]; for await (var x of afsf()) { out.push(x); } fetch('/api/afsfork?v=' + out[0]); })();"   /* for-await(GEN) consumer fork: the sync gen body branches while driven by the async-from-sync consumer (CONT_ASYNC_FROM_SYNC) on the tramp — clone_deep_flow clones the JSAsyncFromSync state with a FRESH wrapper promise per arm -> both afsA and afsP */
    "function* paf(){ if (cfg.admin) { yield Promise.resolve('pafA'); } else { yield Promise.resolve('pafP'); } } Promise.all(paf()).then(function(a){ fetch('/api/paffork?v=' + a[0]); });"   /* Promise.all(GEN) consumer fork at index==0: the gen branches during the FIRST .next() before any element .then is attached (CONT_PROMISE_ALL) — clone_deep_flow clones the JSPromiseAll aggregate fresh per arm -> both pafA and pafP */
    "function* paf2(){ yield Promise.resolve('p0'); if (cfg.admin) { yield Promise.resolve('pf2A'); } else { yield Promise.resolve('pf2P'); } } Promise.all(paf2()).then(function(a){ fetch('/api/paf2fork?v=' + a[0] + '-' + a[1]); });"   /* Promise.all(GEN) consumer fork at index>0: the gen yields element 0 THEN branches (fork during .next() #2, index==1). The retained pre-fork element wrapper (p0) is RE-ATTACHED to the sibling aggregate -> BOTH arms resolve a[0]=='p0' AND their own a[1] (p0-pf2A and p0-pf2P) */
    "var _rr = 'x1y2'.replace(/\\d/g, function(d){ return cfg.admin ? 'rrA'+d : 'rrP'+d; }); fetch('/api/rerepfork?v=' + _rr);"   /* @@replace CALLBACK fork (JSReRep): an OBJECT searchValue dispatches to RegExp.prototype[@@replace], whose machine holds THREE things across each callback — the collected match array, the spec's captures List in its own block, and the StringBuffer accumulator with nextSourcePosition. The replacer branches on opaque state at the FIRST of two matches, so the sibling must get its own copy of all three or the second match lands in the wrong accumulator. Pure paths xrrA1yrrA2 and xrrP1yrrP2 both present => each arm substituted BOTH matches into its OWN buffer. */
    "var _red=[1,2].reduce(function(acc,x){ return acc + (cfg.admin ? 'rA' : 'rP') + x; }, 'r:'); fetch('/api/redfork?v=' + _red);"   /* reduce ACCUMULATOR fork (CONT_ARRAY_REDUCE): the reducer branches on opaque state mid-fold, so clone_deep_flow clones the JSArrayReduce accumulator per arm. The pure paths r:rA1rA2 and r:rP1rP2 both present ⇒ each arm threaded its OWN accumulator across both elements (never a shared/contaminated acc) */
    "var _tpj={toString:function(){ return cfg.admin ? 'tpA' : 'tpP'; }}; fetch('/api/toprimfork?v=' + ['x','y'].join(_tpj));"   /* MACHINE-MODE ToPrimitive fork: 23.1.3.18 Array.prototype.join coerces its SEPARATOR, so §7.1.1 ToPrimitive runs the page's toString ON THE TRAMP with the JSArrayJoin machine as the JSToPrim's OUTER requester — a machine that owns no frame and is reachable only through `outer`. The branch is INSIDE that toString. Both pure paths xtpAy and xtpPy present ⇒ tramp_cont_relink_outer cloned the machine as well as the sequence, so each arm finished its own join with its own separator (a shared machine would leave one arm's separator in the other's buffer, or free the cursor twice) */
    "var _cb=[{u:'cb0'},{u:'cb1'}].sort(function(a,b){ return cfg.admin ? 1 : -1; }); fetch('/api/sortbranch?v=' + _cb[0].u + _cb[1].u);"
    "function* gcf(){ if (cfg.admin) { yield 'gcA'; } else { yield 'gcP'; } } var gci=gcf(); fetch('/api/gcallfork?v=' + gci.next.call(gci).value);"   /* generator .next() driven via .call BYPASS: gci.next.call(gci) reshapes at do_forward_call to the [this=gen, f=next] shape and is now routed onto do_generator_tramp (not the js_generator_next drive-to-completion). The body branches -> both gcA and gcP, never a DFAIL */
    "function* gapf(){ if (cfg.admin) { yield 'gapA'; } else { yield 'gapP'; } } var gapi=gapf(); fetch('/api/gapplyfork?v=' + gapi.next.apply(gapi, []).value);"   /* generator .next() via Function.prototype.apply BYPASS: reshaped at OP_call_method to [this=gen, next, arg0] and routed onto do_generator_tramp -> both gapA and gapP, never drive-to-completion */
    "function* graf(){ if (cfg.admin) { yield 'graA'; } else { yield 'graP'; } } var grai=graf(); fetch('/api/grefapplyfork?v=' + Reflect.apply(grai.next, grai, []).value);"   /* generator .next() via Reflect.apply BYPASS: [Reflect,apply,target,this,argsList] reshaped to [this=gen, next, arg0] and routed onto do_generator_tramp -> both graA and graP */
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
