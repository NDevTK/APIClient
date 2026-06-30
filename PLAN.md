# Progress tracker

The persistent plan/progress file (there are no fresh sessions). Update it every working turn:
move items between DONE / IN PROGRESS / NEXT, record design decisions so they aren't re-litigated.

## Goal (from CLAUDE.md)

ONE continuous forced-execution system: run a page's unmodified bundle on patched QuickJS
(Lexbor DOM + Z3) under forced multi-path execution — **breadth-first** across the call/branch
frontier (FIFO/normal-QuickJS order explores narrowly and slow) — to learn the logged-in API
surface while logged out and detect XSS, surfacing interesting **unused** endpoints with computed
example KEYS/VALUES. Exploration is **UNBOUNDED until the disk limit**: COW per-flow snapshot +
park + evict-to-IDB + cross-session-resume let a flow parked today resume after a browser restart.
A **single attention (WFQ) system** runs across all websites, ordering everything by emitted
output value. Interpretation and recursion are never depth-capped (caps hide findings).

## Design decisions (settled — do not re-litigate)

- **The DETERMINISM BOUNDARY is the architecture (rewrite-grade framing).** Cross-session resume, the
  raw-address COW word-log, AND the "external input stays opaque" rule are ONE design: the engine must be
  a PURE FUNCTION of (bundle bytes, recorded external-input log). All non-determinism is handled exactly
  two ways — (a) ELIMINATED for engine-internal entropy (Math.random/Date.now → synth markers; the PRNG
  seed and the wall-clock `js__hrtime_ns`/`js__gettimeofday_us` → deterministic counters; real time is
  never WANTED — timers need only monotonic ORDER), or (b) RECORDED+REPLAYED for genuine external inputs
  (fetch reply, postMessage, location — opaque for control-flow, concrete example from one recorded
  sample). Then a re-boot is byte-identical and replaying the input log reproduces any state, so a parked
  flow's address-delta applies soundly next session. So the time/random determinism work is NOT patching —
  it COMPLETES the boundary the whole cross-session + opacity design already rests on. Measure the boundary
  (boot-heap hash across two boots), never assume it.
- **No bounds, ever.** A depth/step/RAM cap is a SECOND decider beside the WFQ → not one
  scheduler, and inefficient (it truncates work the WFQ would have ordered by value). The WFQ
  starves the unproductive flow to ~0 CPU (resumable), never drops it. "Smaller is better" is not
  a design argument.
- **Cross-session rests on determinism the engine NEVER actually delivered** — measurement (2026-06),
  not assumption, proved it: two boots of the same bundle gave DIFFERENT baseline heap hashes. The
  allocator IS deterministic (rtAddr/heapBytes/nzWords byte-identical across boots) — only a handful of
  VALUE words carried per-boot entropy, at THREE distinct layers (the key lesson: the determinism
  boundary spans the whole wasm instance, not just quickjs C):
  1. **quickjs PRNG seed.** `js_random_init` seeded `ctx->random_state = js__gettimeofday_us()` at
     `JS_NewContext` (before `qjs_forced_config` sets FE), and the word is DEAD under FE
     (`js_math_random` returns `qjs_synth_new`). FIX: seed a fixed constant in `js_random_init` (covers
     every context of every runtime; ordering-independent).
  2. **quickjs wall-clock.** `js__hrtime_ns`/`js__gettimeofday_us` (cutils.h) returned real time → timer
     `timeout` fields etc. in the heap. FIX: a deterministic monotonic counter (`qjs_det_time`/`qjs_det_time_ns`,
     default ON) backs both at the source (covers ms/sec/ns/μs granularities).
  3. **emscripten/WASMFS file timestamps.** `_emscripten_date_now = () => Date.now()` (emscripten libc,
     BELOW quickjs) feeds WASMFS atime/mtime/ctime for the host's infra files (/h.js,/d.js,/pre.js,/p.js,
     bundle .bc+slices), stored as ms doubles (~1.75e12) in the EARLIEST heap allocations. Decoded the
     IEEE-754 bit patterns to confirm. FIX: `detclock.js` (`--js-library`) overrides `_emscripten_date_now`
     with a deterministic counter — overrides ONLY the wasm's libc date source; the host worker's own
     global Date.now (scheduling) is untouched.
  Localized each by MEASUREMENT (boot @WHY: whole-heap hash → 64 address buckets → 256 sub-buckets →
  word-level index=value dump), never by guessing. So now: re-boot → byte-identical baseline → apply the
  persisted (address,word) delta. Key the persisted frontier by bundle hash. STANDING protection: the
  `reload_session` hash-mismatch refusal (fails safe) + the observable `bootHash` @WHY.
- **Refcounts are HEAP WORDS.** The COW word-log captures them — including the vt-trail's KEEP ref
  on each `old`. So cross-session apply re-applies the word-log (restores all refcounts) and
  re-pushes the vt-trail with NO incref. `old` is never freed mid-flow (the trail pinned it), so a
  slot's baseline value is its valid `old`. No reuse hazard.

## DONE (committed + pushed)

- Engine restored to fork `a212f9c`; design affirmed as the right architecture.
- Eviction-to-IDB: engine protocol + activation (fork `a212f9c`), host `feEvictDB` +
  consume-on-restore (main `2352cf8`). All 271 regression fixtures deleted (`5006814`) — testing
  policy is ONE targeted single-page design-correctness test, never a benchmaxxing battery.

- WFQ value-of-information: per-flow `value` = emitted output/run; repick resumes highest-value first;
  eviction targets the lowest-value tail (one metric). RAM-gate (a second decider) removed.
- **BYTE-REPRODUCIBLE BOOT** (fork `8231d48` / main `8fa0be1`, local — not pushed, sync pending): the
  determinism cross-session resume rests on, MEASURED not assumed (see Design-decisions). Three entropy
  layers closed (PRNG seed; quickjs wall-clock; emscripten `_emscripten_date_now`/WASMFS file timestamps
  via `detclock.js`). Verified live: bootHash matches across two boots, 0 differing heap words, drive=2.
- Cross-session resume END-TO-END (fork `ab721f9`): serialize/deserialize/`apply_xsession` (refcount is
  carried in the word-log heap words → no incref, no reuse hazard); `serialize_flow`/`reload_flow`
  (handle+chunks+delta one blob); `persist_session`/`reload_session` (IDB marker + `<hash>:<seq>`); boot
  `reload_session`; `--fe-persist` / `--fe-bundle-hash`; host FNV bundle hash. Eviction kept (in-session
  preemptive evict-to-IDB at the 128MB floor, value-ordered).
- De-risk of a forced "rush" phase: reverted quantum/floor tunings (fork `44651a7` / main `a4ec393`).
- VERIFIED on the live harness: drive learns endpoints (chain_direct = 2). Tree clean at `44651a7`.

## RESOLVED: upstream sync vs the COW substrate (arena allocator)

**FIXED + LANDED.** Root fix: `js_arena_malloc` routes flow-created objects to the #7 flow arena during a
flow (only reuse flow-arena-backed arenas, else force `arena_new`→`rt->mf.js_malloc`→`g_flow_arena`, which
is EXCLUDED from the COW barrier + bump-reset at FLOWEND; the `free_arena_list` add byte-reverts cleanly);
`js_arena_free` skips `flow_in_arena` blocks. So the byte-restore + defer-trail only ever touch BASELINE
objects — the inline-layout invariant — and NO defer-trail/refcount change is needed. VERIFIED on the live
harness: full gate-set build (0 behind upstream), drive recovered 0→2, bootHash determinism holds across the
merge, `js_free_shape0` underflow gone. The remaining `gc_decref_underflow` is the fork's PRE-EXISTING
~1/vendor-grind recycle abort (present + accepted at the baseline `8231d48` that already runs drive=2). The
historical investigation (two failed symptom-patch attempts that localized the root) is below for the record.

## (historical) BLOCKER: upstream sync vs the COW substrate (arena allocator)

Pushing the determinism work needs the fork current with upstream (the build enforces it). The merge of
upstream/master (11 commits) is MECHANICALLY done — 5 quickjs.c conflicts resolved (keep our diagnostics,
adopt upstream's `JS_REF_COUNT`/`JS_GC_MARK`/`JS_GC_TYPE` macros) + 36 field-access fixes
(`sh->prop`→`get_shape_prop(sh)`), full build OK — preserved on fork branch **`wip-merge-arena` (6e8bd27)**.
But it BREAKS the drive at runtime (drive=0, 68× `wasm_abort`: `JS_REF_COUNT(sh)==0` assert in
`js_free_shape0`). ROOT CAUSE (measured via the abort reason): upstream's **arena allocator** (commit
`9de2921`, NOT config-gated) merged the GC header into the allocator block header — `JSMallocBlockHeader`
packs allocator metadata (`block_idx`/`free_next`/`block_size_idx`, low 4 bytes, written during the
`--wrap`-SUPPRESSED malloc/free) AND `ref_count` (high 4 bytes, written during execution, COW-TRACKED)
into the SAME 8-byte word before each object. COW reverts at 8-byte-word granularity, so reverting a
refcount change also clobbers the allocator's free-list state in that word → corruption → shapes freed
non-zero. UPDATE (attempt 1, on `wip-merge-arena`): added a flow-suppression guard to `js_arena_free` (mirror
`__wrap_free`). It REDUCED aborts (68→57) but did NOT fix the drive (still drive=0). Re-decoding the abort:
`js_free_shape` does `if (--JS_REF_COUNT(sh) <= 0) js_free_shape0(sh)` then `js_free_shape0` asserts
`JS_REF_COUNT(sh) == 0` — so it is a shape refcount UNDERFLOW (decremented below 0), not (only) a free-list
clobber. So the real fault is refcount ACCOUNTING: a shape decref'd more than incref'd under the arena
layout. PRECISE ROOT CAUSE (traced to the vt-trail): `qjs_cow_vt_push(pv, js_dup(*pv), vr)` (quickjs.c ~5488)
KEEPs a ref on `old` via `js_dup` (a refcount INCREF) and the trail frees `old` on revert/commit (a
DECREF). With the OLD inline layout, `old`'s refcount lived INSIDE the object, and the revert order
(typed-trail FIRST, THEN word-log replay) was MEASURE-TUNED so the word-log's capture of that incref and
the trail's typed free balanced (the "free_zero_bad underflow if reversed" note). Upstream's arena
allocator SPLIT the refcount (now at p-8, the block header) from the object body (at p), desynchronizing
that tuned interaction: the word-log reverts the `js_dup` incref AND the trail's typed free decrements
again → shape refcount UNDERFLOW → `js_free_shape0` abort. FIX DIRECTIONS (deep, pick after study — NOTE the UAF trap):
  The `js_dup` incref is NOT removable naively — it is the KEEP ref that PINS `old` so it survives the flow
  (without it `old` can be freed mid-flow → revert restores a dangling value → UAF). So the fix is to make
  the pin's incref not get DOUBLE-reverted (once by the word-log capturing the p-8 write, once by the
  trail's typed free), NOT to drop the pin. Options:
  (a) exclude the p-8 refcount word from the WORD-LOG (so the trail's typed incref/free is the SOLE owner of
      `old`'s refcount across the flow) — needs the barrier/revert to skip block-header words, and the
      trail to also pin via the GC-mark/list so the object survives; or
  (b) keep the word-log owning p-8, and make the trail push WITHOUT a typed free on revert (the word-log
      already restores the refcount) BUT keep `old` pinned some other way for the flow's duration (e.g. a
      separate pin list freed after the word-log replay). cross-session `apply_xsession`'s "re-push vt-trail
      with NO incref" is the same idea — study why it is sound there and mirror the lifetime.
The two mechanisms must own `old`'s refcount EXACTLY ONCE while still pinning it. Re-derive the revert order
(currently typed-trail FIRST then word-log) under whichever wins — it was measure-tuned for the inline
layout and must be re-measured for the split (p-8) layout.
ATTEMPT 2 (shape refcount=1 before defer-free) FAILED — and revealed the DEEPER layer: setting
`JS_REF_COUNT(nw)=1` fixed the high 4 bytes, but the SAME 8-byte block-header word's LOW 4 bytes
(`block_idx`/`free_next`) were ALSO byte-restored to the baseline FREE-block state, so `js_free_shape0` →
`js_arena_free(nw)` reads a free/garbage `block_idx` and corrupts the arena (drive went from
recycling-abort to a silent HANG/no-output — strictly worse). REVERTED (wip back to `1ed3163`). KEY
REALISATION: for a shape CREATED during a flow, the word-log byte-restore ALREADY reverts its whole block
(memory + gc/hash links + block-header) to the baseline FREE state — i.e. the shape is already "unmade".
The defer-trail's `js_free_shape(nw)` is then a DOUBLE-free against the arena. So the real fix is NOT to
make the free succeed; it is to NOT free flow-created shapes on revert at all (the byte-restore is
sufficient) — but the defer-trail exists because the OLD layout needed the explicit free (the inline
refcount + the allocator state were NOT both reverted by the byte-restore there). So the defer-trail's
ROLE itself must be re-derived under the arena layout: which of {memory revert, gc/hash unlink, block free}
the byte-restore now covers vs what the trail must still do. THIS is the deep redesign — do it with the
arena block-header semantics fully in hand, not by patching the free. (Un-merging the arena header so the
refcount is its own word AND excluding the allocator-metadata word from the byte-restore is the likely
shape of the answer, but verify the free-list stays consistent.)
DEFINITIVE FIX DIRECTION (found by tracing the allocator — supersedes the defer-trail patching below):
the fork ALREADY isolates flow-created objects cleanly — `js_def_malloc` routes them to the #7 flow arena
(`g_flow_arena`), which is EXCLUDED from the COW barrier (never logged → never byte-reverted) and bump-RESET
at FLOWEND. The arena-allocator regression is purely an ALLOCATION-PATH gap: `js_malloc_rt`→`js_arena_malloc`
REUSES existing BASELINE arenas for small blocks (only a brand-new arena goes through `rt->mf.js_malloc` =
flow-aware `js_def_malloc`). So a flow object that lands in a reused baseline arena has its block-header
refcount written in the regular heap → logged → byte-reverted → the corruption. THE FIX is at allocation,
not free/defer: during `g_flow_capture`, `js_arena_malloc` must serve flow objects from FLOW-ARENA-BACKED
arenas (keep the `JSMallocBlockHeader` layout so `JS_REF_COUNT(p)` at p-8 still resolves, but the backing is
in `g_flow_arena` → excluded + bump-reset), NEVER reusing baseline arenas. Concretely: keep a SEPARATE
flow arena free-list (or force `arena_new` via `rt->mf.js_malloc` for every flow alloc and reset it at
FLOWEND), and have `js_arena_free` skip `flow_in_arena` blocks. Then the byte-restore + defer-trail only
ever touch BASELINE objects (the inline-layout invariant), and NO defer-trail shape-free change is needed.
This is the clean integration of upstream's arena with the #7 flow arena — careful but well-scoped; do it
with the arena block/free-list invariants fully in hand and verify park/evict (abandoned-pause) too.

REWRITE-GRADE DIRECTION (concrete, actionable): the byte-restore should own OBJECT BODIES + baseline-field
VALUES; a FLOW-CREATED object's block-header should be EXCLUDED from the byte-restore and owned by the
defer-trail, which returns the block to the arena free-list on revert with the mid-flow `block_idx` intact.
Two implementation hinges: (1) in `qjs_cow_defer_push`, when recording a flow-created new shape, mark its
block-header word in the COW shadow bitmap so the word-log SKIPS it (the header stays mid-flow-valid for the
free); (2) fix the `g_flow_capture` timing so the defer-trail's `js_arena_free` on revert actually RECLAIMS
the block (today `js_arena_free` is a no-op while capture is set — the revert must run with capture in the
right state, or call a capture-bypassing arena-free). Net: the byte-restore unmakes baseline-field changes;
the defer-trail unmakes flow-created allocations via the real allocator. Verify free-list consistency + no
leak after a park/evict (the abandoned-pause path) too.

CONFIRMED EXACT MECHANISM (read the revert code): the typed trails suppress re-logging via `g_cow_busy=1`,
and the order is "typed trail FIRST, then raw word log" (quickjs.c ~21271) — EXCEPT the SHAPE path: the
word-log revert (`qjs_cow_undo_revert_to`) byte-restores first, THEN calls `qjs_cow_defer_revert_to`
("AFTER the byte-restore — field is now old: free the orphaned new shape by kind", ~21074). For a shape
CREATED during a flow: the word-log byte-restores its refcount word to the BASELINE value (the shape did
not exist → the slot is free/zero), THEN the defer-trail does `js_free_shape(new)` → `--JS_REF_COUNT(sh)`
→ underflow → `js_free_shape0` assert. With the INLINE layout the refcount lived in the shape BODY and this
order balanced; relocating it to the block header (which the word-log treats as raw allocator state) broke
it. So the precise fix site is the shape/refcount ownership between `qjs_cow_undo_revert_to`'s byte-restore
and `qjs_cow_defer_revert_to`'s typed free — the refcount word must NOT be both byte-restored AND typed-freed.
GUIDING PRINCIPLE (rewrite-grade, separation by data-kind): the root fragility is that COW is COUPLED to
the physical layout of refcounts — the word-log captures refcount words by address AND the vt-trail manages
them typed, kept in balance only by a measure-tuned revert order. That coupling is what upstream's
relocation broke. The robust design separates by KIND: the word-log owns raw OBJECT BODIES (layout-agnostic
bytes), the typed vt-trail owns REFCOUNTS/GC-STATE EXCLUSIVELY (via the `JS_REF_COUNT`/`JS_GC_*` macros, so
it works wherever those fields physically live). Then COW is DECOUPLED from refcount layout — upstream can
move refcounts anywhere and COW keeps working. So prefer direction (a): exclude block-header words from the
word-log; the vt-trail becomes the sole, layout-agnostic owner of refcount snapshot/restore.
This is a multi-part COW-substrate re-tuning — iterate carefully, do NOT rush. Determinism (bootHash)
STILL holds across the merge; the official fork stays at the verified `8231d48` until this lands.

ORIGINAL (free-list clobber hypothesis — partially right): the existing invariant is that NO
free mutates allocator state during a flow — `__wrap_free` is already a NO-OP under `g_flow_capture`
("the block survives; the revert restores references to it"). So during a flow the block-header word only
ever takes REFCOUNT writes (logged) over STABLE allocator metadata → revert is consistent. Upstream's arena
added a SECOND free path, `js_arena_free` (upstream quickjs.c ~1804: writes `block_idx`/`free_next`, pushes
onto `first_free_block`), which `__wrap_free`'s suppression does NOT cover — so frees-during-flow now mutate
block-header allocator metadata, and the word-granular refcount revert clobbers it. THE NEXT DIFF: guard
`js_arena_free` with `if (g_flow_capture || g_grind_drive_active) return;` (mirror `__wrap_free`), on branch
`wip-merge-arena`; rebuild; verify the `js_free_shape0` asserts vanish and drive≥2. THEN check the
malloc/flow-arena (#7) coordination still holds under the arena allocator (flow-local allocs must still
route to / be discarded with the flow arena), and re-verify determinism (bootHash) + `apply_xsession`
(refcounts now at p-8 — confirm the word-log still carries them, since the block header IS in the hashed
heap). Un-merging the arena header (separate refcount word) is the fallback if the free-suppression proves
insufficient.

## RESOLVED (2026-06, cdba329): gc_decref_underflow — single-owner refcount for OBJECTS

ROOT, CONFIRMED model-free on chain_direct (supersedes the earlier in-edge-mismatch guess, which was
WRONG — there was no missing slot edge): the COW word log byte-reverts the 8-byte arena block-header word
at `p-8`, and the arena merged `ref_count` (int at header offset 4) INTO that word. So the revert clobbered
the live ref_count that the refcount-EXACT typed trail (`cow_set_slot` / `cow_put_var_ref`) had set. The
revert ORDER (typed trail FIRST, word log second) put the word log's raw byte-restore LAST → it always won.
MEASURED decisively (instrumented, then stripped): `svWrites=0` (the victim's slot was never written by
set_value during the flow → not a missed slot capture) AND `revDecFreeVal=0`/`revDecVarRef=0` (the typed
trail did not decref it) AND `hdrHit=2`/`rcForced=1` (the word log restored the victim's HEADER word with
ref_count=1 while the cycle collector saw 2 live in-edges). i.e. the typed trail correctly wanted rc=2; the
word log erased its +1.
FIX (landed): `qjs_cow_undo_revert_to` — when a logged word is an arena block-START word
(`cow_is_header_word`, sound via the per-arena block stride, no false positives) restore only the LOW 4
bytes (block_idx/block_size_idx/gc_type/mark — constant post-alloc since frees are flow-suppressed, or
GC-transient for mark) and PRESERVE the live ref_count (high 4 bytes). The typed trail + balanced live
execution own the count. CORRECTION to the earlier "DESIGN" note: it claimed BOTH halves were required
(word-log-skips-refcount AND typed-trail-covers-every-write) and "either half alone is worse." That was
WRONG — half 1 alone sufficed for objects, because the typed slot coverage (`cow_set_slot` is THE baseline
property/element setter) is far more complete than that note assumed. RESULT: gc_decref_underflow 46 → 0,
free_zero_bad 0, endpoints maintained (learnedCount=2), boot determinism unaffected (revert is post-baseline).
KNOWN GAP: large (non-arena) allocations aren't covered by `cow_is_header_word`; if a large-object victim
ever underflows, extend detection to the large-block list (none observed on chain_direct).

## OPEN ROOT (next): STALE SHAPE-kind defer entry — a use-after-free (LOCALIZED model-free)

Once the object underflow was fixed, execution proceeds further and hits a PRE-EXISTING bug — formerly
MASKED by the object underflow aborting first, NOT introduced by cdba329 (still recycled past; learnedCount
stays 2; survives BOTH the low-byte-restore and the whole-header-preserve forms of the fix, so it is
independent of the header handling). CHARACTERIZED model-free (instrumented, then stripped) — and it is NOT
the refcount over-decref earlier guessed; it is a USE-AFTER-FREE:
- `js_free_shape` is called on a block whose `gc_obj_type` is JS_OBJECT (gt=0), not SHAPE, with rc=0 — a
  dangling "shape" pointer. The `js_free_shape0` assert was only the downstream symptom.
- The CALLER is the COW SHAPE-kind defer trail: `qjs_cow_defer_revert_to` → `js_free_shape(neww)` (~21593).
- Both the entry's `old` AND `neww` pointers are now baseline JS_OBJECTs (newGt=0 newFlowArena=0, oldGt=0
  oldFlowArena=0), consumed while `g_flow_capture=1`. i.e. a SHAPE defer entry pushed in an earlier flow was
  NOT consumed at that flow's boundary, the two shapes it referenced were later freed and their BASELINE
  blocks reused as objects (frees run between flows, where they are not suppressed), and a LATER flow's
  revert frees those long-dead pointers → UAF / type-confusion.
ROOT CLASS: defer-trail lifecycle / wpos-mark accounting — a SHAPE entry outlives the flow that pushed it
(`qjs_cow_defer_push` is gm-backed and persists; `qjs_cow_defer_revert_to(mark)` reverts entries with
`wpos >= mark`, `qjs_cow_defer_commit` clears all — an entry orphaned by a segment/nested-flow mark mismatch
lingers). The FIX is in that lifecycle (ensure every SHAPE entry is consumed — reverted or committed — at the
flow that created it, so no entry outlives its shapes); a "skip if gt != SHAPE" guard at the free site would
be MASKING (banned). This is the substrate that regressed twice during the arena work — fix it CAREFULLY with
per-step verification (defer_revert_nonshape GONE + uf still 0 + drive + determinism), NOT a tail-of-session
rush. Do NOT call the engine "clean" until this is GONE, not recycled past.

DEEPER MECHANISM (traced 2026-06): the SHAPE-kind defer entry holds a RAW shape pointer with NO ref of its
own. `cow_shape_transition` KEEPS old's ref and lets `p->shape` hold new's ref; the entry just records the
two pointers. When a flow PARKS (`qjs_cow_park` ~21126 copies defer entries to a delta; `qjs_cow_undo_revert_to`
then reverts `p->shape` back to old) the new shape becomes ORPHANED — alive only by its un-decremented refcount
but UNREACHABLE from the live graph — so the cycle collector can free it; its baseline block is then reused as
an object, and the delta's `df_new` (re-pushed on resume, 21181) or a lingering live entry dangles → the gt=0
free. `delta_free` (21201) frees only BUFFER-kind `df_new`, never SHAPE — so a discarded park also leaks shapes.
CANDIDATE FIX (mirror the var_ref pin in `cow_put_var_ref`): in `qjs_cow_defer_push`, for is_shape, PIN both
old and new (`js_dup_shape`, rc++) so neither can be freed while the entry/delta references them; the existing
`js_free_shape(new)` on revert and `js_free_shape(old)` on commit then release the pins (re-balance the
accounting: drop the reliance on the orphaned p-ref, since the pin now owns it), and `delta_free` must
`js_free_shape` SHAPE-kind `df_new` too.
BUT MEASURE FIRST (do not implement on this model alone): it is UNCONFIRMED what actually frees the orphaned
shapes. The kept/orphaned ref INFLATES their refcount, which should protect them from BOTH refcount-free AND
the cycle collector (gc_decref finds fewer in-edges than rc → gc_scan keeps them) — so the pin may be
targeting the wrong free-path. Next decisive instrument: when a SHAPE `df_new`/defer pointer's block flips to
gt!=SHAPE, capture WHAT freed it (instrument `js_free_shape0`/`remove_gc_object`/the arena free for that exact
block, or tag the defer entry and watch its `neww`), distinguishing cycle-GC vs a discard/park path vs a
double-consume (entry reverted AND committed). Only then implement the matching fix and verify stepwise
(defer_revert_nonshape GONE + uf 0 + no new free_zero_bad + drive + determinism).

TESTED 2026-06-30 — TWO theories FALSIFIED on the live harness (A/B verified, engine restored to b5eea41):
1. "The defer revert `js_free_shape(new)` is a redundant DOUBLE-decrement (word-log already byte-reverts a
   BASELINE new shape's rc), so skip it for baseline new" — WRONG. Building exactly that (free new only when
   `flow_in_arena(new)`) REGRESSED the drive: learnedCount 2→0 and the offscreen worker hung (no stderr
   response). Reverting restored learnedCount=2. CONCLUSION: the defer revert-free is LOAD-BEARING — it is the
   sole release of new's p-ref; the word-log does NOT reliably byte-revert new's rc (if it did, removing the
   free would be a no-op, not a regression). So the over-decref is NOT a double-decrement.
2. Therefore the over-decref (`JS_REF_COUNT(sh)==0` at js_free_shape0, reliably ab=3/drive) is the STALE/
   DANGLING entry after all: the defer-free correctly single-decrements in general, but occasionally its
   `neww` already points at a freed+reused block (defer_revert_nonshape, gt=0) → decrements a reused object.
PIN remains the only sound direction (keep new alive so the entry can't go stale) — but its accounting is
NOT a simple "+pin, existing frees release it": the existing revert-free is load-bearing for the p-ref, so a
pin needs its OWN release on BOTH revert and commit without disturbing that free, and whether that nets out
depends on the same unknown (is new's rc byte-reverted). RESOLVE the unknown FIRST with a LIGHT probe (a
single global counter/flag — NOT the O(n²) per-free defer-array scan used this session, which itself starved
the drive to 0 endpoints and produced a false "regression" signal before being isolated). Measure: for one
baseline new shape in a defer entry, does its rc word appear in the word-log (is it byte-reverted on revert)?
That single bit determines the correct pin accounting. NOT a tail-of-session rush; one regression already
happened here.

PROBE BLOCKED (2026-06-30): a read-only `@SHAPEPROBE` counter (logged vs unlogged vs flow-arena new shapes
at defer_push) emitted NOTHING via `fprintf(stderr)` across two runs while endpoints stayed 2 — so the
measurement CHANNEL is the blocker, not the gate: raw `fprintf(stderr)` during normal (non-abort) execution
does NOT reach the worker's `_lastStderr` (only `__assert_fail`/onAbort reliably does — same buffer-loss noted
for aborts). NEXT measurement must use a captured channel: a ONE-SHOT `__assert_fail` carrying the counts when
the count first reaches ~40 (accept the early abort — the drive aborts anyway). ALSO: SHAPE defer pushes are
FLAKY run-to-run (defer_revert_nonshape fired in some runs, not others) — the DRIVE is not deterministic for
shape transitions (boot is byte-reproducible; the WFQ/timing of which flows run is not), so the probe needs a
run where shape transitions actually occur, or a more deterministic drive.

REAL DESIGN DIRECTION (the rewrite, not a patch): the COW system has THREE revert mechanisms — word-log
(byte-restore, type-blind), typed trail (refcount-exact JSValue slots), defer trail (buffer/shape pointers).
Objects are now SINGLE-OWNER (header-preserve: word-log never reverts their rc; the typed trail + live
execution own it) and that fixed the underflow. SHAPES still have SPLIT ownership (word-log byte-reverts the
header for non-preserved shapes AND the defer trail frees new/old) — and the regression proved the split is
load-bearing-in-a-tangled-way (the word-log does NOT reliably revert shape rc, so the defer-free can't simply
be dropped). The sound fix is to make SHAPES single-owner too: header-PRESERVE shapes (stop the word-log from
touching shape rc) and route EVERY flow-time shape rc change (js_dup_shape/js_free_shape, not just the
transition) through the typed/defer trail so nothing leaks. That is the same unification that fixed objects,
applied to shapes — bigger, because direct (non-transition) shape dups during a flow must also be trail-
tracked. This is the genuine rewrite; do it with the captured-channel measurement in hand, fresh, verified
stepwise (shapeAssert 0 + endpoints 2 + no fzb + determinism).

CONFIRMED 2026-06-30 — the defer-trail patch path is EXHAUSTED (two attempts, both refuted on the live
harness): (a) dropping the defer revert-free for baseline new → REGRESSED (endpoints 0, worker hung). (b) a
full defer PIN — `cow_shape_transition` takes an extra UNLOGGED `js_dup_shape(new)`, revert frees new 2× (p-ref
+ pin), commit 1× (pin), delta_free releases the SHAPE pin, pinned in the transition (not defer_push) so the
park/resume re-push doesn't double-pin → a complete NO-OP: shapeAssert stayed 2, ab stayed 3, endpoints stayed
2, BYTE-IDENTICAL to baseline across 3 cycles. DECISIVE CONCLUSION: the reliable `js_free_shape0` over-decref
(JS_REF_COUNT(sh)==0, ~2/drive) is NOT the defer-trail stale entry — keeping new alive changed nothing. It is a
DIFFERENT, NON-defer shape rc imbalance, i.e. the shape header-revert exclusion itself (word-log byte-reverts a
baseline shape's rc, dropping a reference that should persist past the flow). The flaky `defer_revert_nonshape`
(gt=0, some runs) is a SEPARATE, rarer defer-staleness symptom. So the ONLY remaining fix is the single-owner-
shapes rewrite above (header-preserve shapes + route ALL flow-time shape rc through the trail) — and it must
resolve the COUPLING: header-preserving shapes ALONE already asserted historically (bby44iozd) because the
defer trail RELIES on shape rc being byte-reverted, so both halves must land together. Engine stays at verified
b5eea41 until that lands; do NOT attempt another defer-only patch.

MEASURED 2026-06-30 (reliable abort channel) — two more facts that SHARPEN the single-owner design:
1. The both-baseline-shape transition probe (gate `!flow_in_arena(new) && !flow_in_arena(old)` in
   cow_shape_transition) NEVER fired → in EVERY defer-tracked transition, new OR old is FLOW-ARENA (a shape
   freshly created during the flow). So the defer trail mostly carries flow-arena shapes, whose rc the
   bump-reset reclaims regardless of refcount — THAT is why the pin was a no-op (a pin can't protect arena
   memory from the reset).
2. At the over-decref, the victim block has inFlowArena=0, gt=0 (a BASELINE object) — a baseline shape that
   was freed and its baseline block reused as an object, then the defer revert frees it again. The pin not
   helping means the shape is released MORE times than rc+pin absorbs (a multi-free / double-consume), not a
   single premature free.
SINGLE-OWNER DESIGN SKETCH (the clean rewrite): a flow's only legitimate effect on a BASELINE shape's rc is
the dups from FLOW-LOCAL objects it creates that reference that shape (object creation js_dup_shape's the
shape). Today the word-log byte-reverts those (type-blindly, tangling with the defer trail). Instead, at
FLOWEND BEFORE the arena bump-reset, FINALIZE the flow-local objects being discarded — decref each one's shape
(and other baseline refs) exactly once — then reset; and header-PRESERVE shapes so the word-log stops touching
shape rc. That makes the flow→baseline-shape rc delta single-owned by the finalize pass, removes the defer
trail's reliance on byte-revert, and is symmetric with how flow-local objects are already discarded. NEXT:
read the FLOWEND path (qjs_cow_undo_revert + the arena reset ~21831) and how flow-local objects' refs are
handled today, then implement the finalize pass + shape header-preserve TOGETHER, verified stepwise.

RULED OUT 2026-06-30 (reliable abort-channel measurements on the `js_free_shape0` rc=-1 over-decref, ~2/drive):
- NOT a defer-stale entry (the PIN was a byte-identical no-op).
- NOT fixable by dropping the defer free for non-flow-arena new (REGRESSED: endpoints 0 + worker HANG, 3×).
- NOT flow-arena reclamation (victim inFlowArena=0).
- NOT an arena-exhaustion FALLBACK and NOT a large block: `shape_fb[rc=-1 largeOrFallback=0 fallbackCount=0
  inArena=0]` — fallbackCount=0 (arena never exhausted), block_idx valid (small arena block).
=> The victim is a TRUE-BASELINE small shape (allocated pre-flow, in a baseline arena) driven to rc=-1. A
single byte-revert + single defer-free can only reach R-1 (≥0 for R≥1), so rc=-1 needs the shape released
MORE than twice — a MULTI-free the global 256-entry freed-ring missed (firstRa=0 = ring wrapped). And the
"drop the defer free" hang proves that free is load-bearing for SOME non-flow-arena new (a flow-created shape
that landed in a BASELINE arena, needing the free) even though fallbackCount=0 — so the arena routing is NOT
catching every flow-created shape, yet not via the fallback path. CONTRADICTION-LADEN — every model has been
refuted by the next measurement. NEXT APPROACH (stop assert-iterating): instrument a PER-SHAPE complete
rc-history log (every js_dup_shape/js_free_shape on a victim address, with caller) — a ring keyed by address,
not global — to see the FULL sequence of every rc change on one victim. That is the only way to see a
multi-free whose individual steps each look locally valid. OR commit to the full single-owner rewrite above
and accept it subsumes this. Engine stays at verified b5eea41.

DEFINITIVE 2026-06-30 (per-shape free-history): `shape_frhist[rc=-1 frees=1 (rc0@0xce6c4)]` — the victim is
freed exactly ONCE during the flow, and its rc was ALREADY 0 at that free (the single defer-free 0→-1). So it
is NOT a multi-free: the BYTE-REVERT drove the rc to 0, then one defer-free underflowed it. The rc word's
first flow-write had old=0 ⇒ the shape's block was rc=0 (a free block) when the flow first touched it ⇒ a
FLOW-CREATED shape whose creation `rc=1` write (old=0) got byte-reverted to 0. THE ACTIONABLE ROOT + FIX:
shapes are NOT header-preserved, so the word-log byte-reverts their rc; objects ARE fine under the same
header-preserve because their refcount is reverted EXACTLY by the TYPED TRAIL (`cow_set_value` on baseline
JSValue slots), whereas shapes have NO typed trail for rc — only the defer trail for `p->shape` transitions.
So the single-owner shape fix is concretely: ADD a typed-trail layer for shape rc (capture every flow-time
`js_dup_shape`/`js_free_shape` on a BASELINE shape, refcount-exact, like cow_set_value) AND header-preserve
shapes — together. That removes the byte-revert from shape rc (so a flow-created shape's creation dup is never
reverted-to-0) and makes the defer trail's transition handling consistent. (UNRESOLVED by measurement: how a
flow-created shape landed in a BASELINE arena with fallbackCount=0 — the arena routing should send flow allocs
to the flow arena; this contradiction means either shape allocation has a path that bypasses js_arena_malloc's
flow routing, or flow_in_arena mis-classifies it. Worth checking js_malloc→js_arena_malloc for shapes as part
of the rewrite.) ASSERT-ITERATION IS EXHAUSTED here (62+ builds, every point-model refuted) — the fix is the
typed-trail+preserve rewrite, done as its own careful pass, not more diagnostics.

## NOT yet verified at runtime — but now UNBLOCKED

- The cross-session ROUND-TRIP (persist → restart → reload → state survives) — **THE payoff of the
  determinism work, now unblocked.** Flow: the host calls `--fe-persist` every analysis (ast-thread.js
  ~2260) → serialises parked flows to `feEvictDB`; boot's `reload_session` restores them IF
  `g_boot_heap_hash` matches. Until determinism landed, the guard would have ALWAYS refused
  (`xsession_boot_mismatch`) because the boot heap differed every restart — so the round-trip could never
  have worked. Now boot is byte-reproducible, so the FIRST concrete check is: drive a page, restart with
  IDB surviving, confirm boot emits `reload_session` ACCEPT (hash match) not `xsession_boot_mismatch`, then
  that the reloaded flow RESUMES (emits @H). OPEN test-mechanics question: does `harness restart` preserve
  the IDB (persistent profile) so the persisted session survives? — resolve before testing (start reuses a
  stale profile; restart's IDB-survival must be confirmed). Still need a fixture that DRIVES and PARKS, and
  to isolate reloaded-flow output from the fresh drive (recover chain_direct transiently; a plain top-level
  orphan is not driven → the earlier 0).

## NEXT (best design assuming rewrite)

1. ~~Relocation-independent snapshot~~ — REJECTED after review (don't re-propose). The idea was to key
   the COW delta by `(object-id, offset)` instead of `(raw-address, word)` to drop the byte-identical-
   re-boot dependency. But it conflicts with the substrate: the binaryen word-barrier logs RAW word
   addresses and has no idea which object a word belongs to; mapping address→object per write needs a
   sorted object index in the hot path (costly), and the only other route — serializing the object
   graph — is explicitly banned ("move the bytes WITHOUT serializing the object graph"). So cross-
   session genuinely rests on byte-identical-boot determinism. We then MEASURED that determinism instead
   of assuming it: a boot-heap hash compared across two boots (`qjs_cow_boot_baseline` emits
   `bootHash`/`heapBytes`/`nzWords`/`rtAddr`; host captures via `self.__bootHash` in ast-thread.js's
   print tap). It DIFFERED — the engine did not actually deliver the determinism the rejection assumed.
   Source #1 fixed (the `random_state` gettimeofday seed, dead under FE — now a fixed constant in
   `js_random_init`, covering every context of every runtime). RE-MEASURED: hash STILL differs, with
   rtAddr/heapBytes/nzWords STILL byte-identical → at least one MORE per-boot value-entropy word remains,
   not a layout issue. Now LOCALIZING by measurement: `qjs_cow_boot_baseline` scans the baseline heap for
   words in the wall-clock magnitude band (≥1e12, far above any wasm32 pointer/refcount/small-int) and
   emits `word-index=value`; two boots compare them to pin the exact words (suspect: an `hrtime`-based
   timer `timeout` field from the async endpoint's setTimeout, `quickjs-libc.c:2486/2547`).
   DESIGN DIRECTION (best long-term, not per-source whack-a-mole): a SINGLE determinism boundary — under
   FE, route `js__gettimeofday_us`/`js__hrtime_ns` (cutils.h) through a deterministic monotonic counter,
   exactly as Math.random/Date.now already return synth markers. Real time is never WANTED in the analysis
   engine (timers need only monotonic ORDER, which a counter gives); it is pure entropy that breaks the
   byte-identical boot. The burden is small + bounded (allocator is already deterministic — only a handful
   of entropy words). The boot-determinism GUARD (`reload_session` refuses on hash mismatch) stays
   regardless — it fails safe either way.
2. ~~Value function: UCB-explore for unrun/late flows.~~ — ALREADY DONE (verified by code-read 2026-06).
   `priority.js flowWeight = 1 + epRate + exploreBonus` and the scheduler FEEDS all inputs: ast-thread.js
   maintains `epRate` (EWMA decay 0.75 of @H+@S per resume, EQUAL weight — line ~3091), `exploreBonus`
   (UCB1 √(2·ln N / nᵢ) — line ~3101), and advances `flowVt` by `sliceCost/weight` (line ~3115). So an
   unrun flow gets the big UCB burst (nᵢ small) and a flat flow decays to the floor — exactly the
   breadth-first + reach-deep-gated reconciliation. The only un-built sub-term was the STATIC `λ·(PC can
   reach an un-hit host-edge)` predictor — and that was deliberately DROPPED (priority.js header): a static
   "reaches a network edge" bit can't be answered statically and is XSS-blind, so weight is RUN-OUTPUT only.
   Nothing to build here.
3. **Fork-as-BFS** replacing schedule-rerun: snapshot a sibling at the not-taken opaque arm; fork only
   when Z3 says the arm is path-SAT and it can reach un-hit output (feasibility + novelty filtered, not a
   fork cap).
4. WFQ relief via virtual-time (dual-WFQ unification). Evicted-flow cross-session persist.

## Verification policy (CLAUDE.md)

Live Chrome harness (`testing/harness.js restart`), ONE targeted single-page design-correctness
test of the actual property (not `alive`, not a count). Regression batteries / flat metrics across
commits = benchmaxxing, banned. Build = a build step, not a verdict.
