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
- Cross-session resume END-TO-END (fork `ab721f9`): serialize/deserialize/`apply_xsession` (refcount is
  carried in the word-log heap words → no incref, no reuse hazard); `serialize_flow`/`reload_flow`
  (handle+chunks+delta one blob); `persist_session`/`reload_session` (IDB marker + `<hash>:<seq>`); boot
  `reload_session`; `--fe-persist` / `--fe-bundle-hash`; host FNV bundle hash. Eviction kept (in-session
  preemptive evict-to-IDB at the 128MB floor, value-ordered).
- De-risk of a forced "rush" phase: reverted quantum/floor tunings (fork `44651a7` / main `a4ec393`).
- VERIFIED on the live harness: drive learns endpoints (chain_direct = 2). Tree clean at `44651a7`.

## NOT yet verified at runtime

- The cross-session ROUND-TRIP (persist → restart → reload → state survives). Blocker: need a fixture
  that both DRIVES (global-assigned / class-method, like chain_direct) AND PARKS (silent for the
  quantum), then isolating reloaded-flow output from the fresh drive. A plain top-level orphan does
  not get driven; that's why the earlier attempt showed 0.

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
