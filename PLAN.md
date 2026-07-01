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

## ARCHITECTURE CORRECTION (2026-07-01, user directive: "grind must not be a separate system — it's BFS")

**DONE + VERIFIED + PUSHED this session (each spa_gated netdiff --unused = 5, identical endpoints):**
1. engine 77f926a — residue grind's SEPARATE second scheduler DELETED (qjs_deep_flow/cow/vt registry +
   open-coded park/resume + spin-defer/pause-set/abandon-backstop). Orphans drive through THE ONE primitive
   qjs_run_forced_flow into the ONE registry qjs_drive_flow (drained by qjs_drive_repick).
2. engine 1a78e68 — qjs_grind_interrupt's WFQ-OFF no-progress throw-defer + the vestigial fixpoint counters
   (g_defer_fired / g_grind_completions / g_noprog_polls / g_progress_seen — banned no-progress counts) DELETED
   (dead once qjs_deep_flow was gone: grind drives always run under qjs_driving).
3. engine de2db63 — a PREEMPTIBLE BFS drive that spins now PARKS (defers to the YIELD_POLL park) instead of the
   g_bfs no-progress THROW that dropped/lost it. Throw-skip remains only for the not-yet-preemptible plain-call
   BFS drives.
4. engine f94ef60 — connectedCallback + handler-drain (hostedge) routed through DRIVEBREADTH.call(recv,fn,arg)
   = qjs_run_forced_flow, so a spinning callback PARKS+resumes instead of being thrown away.

**g_bfs_noprog (the last BFS no-progress count) — narrowed to the ASYNC JOB PUMPS (js_fe_drive_static DONE):**
- DONE (engine e7d1b69): js_fe_drive_static's 3 forced JS_Call sites now route through qjs_run_forced_flow
  (preemptible; a spinning static target PARKS, not the g_bfs throw). The B2a "hang" was actually
  NON-CONVERGENCE (the forced-flow revert undid the in-drive qjs_executed marking → targets re-collected every
  schedule → the BFS fixpoint never went flat); fixed by persisting b->qjs_driven at the drive-loop top (outside
  the revert) + skipping qjs_driven in the collection filter. Verified spa_gated converges to 5, faster (~5s),
  determinism preserved. So the static drives no longer need the g_bfs throw (they run under qjs_driving → park).
- REMAINING g_bfs_noprog users = the ASYNC JOB PUMPS, which still run continuations with qjs_driving OFF under
  g_bfs_drive_active: (a) js_fe_drive_static site-1's INLINE `while(JS_ExecutePendingJob)` async pump, and
  (b) the boot event-loop pump (driver.js flush()/pump() → JS_ExecutePendingJob). A spinning async continuation
  there is the last thing the g_bfs throw catches. Deleting g_bfs_noprog now would hang boot on a page with a
  spinning async continuation (spa_gated has none, so it would falsely "pass" — do NOT trust it). THE FIX: make
  the job pump preemptible (run JS_ExecutePendingJob continuations under qjs_driving so a spinner PARKS) — the
  "Boot is the hard gap" work. Only then is g_bfs_noprog fully dead. This is the concrete next one-BFS target.

**JOB-PUMP PREEMPTIBILITY — the real obstacle + the design (READ from JS_ExecutePendingJob @4969):** a pending
job runs as `res = e->job_func(e->ctx, e->argc, argv)` where job_func is a C function (js_promise_reaction_job /
the async-resume thunk) that does a JS_Call INTERNALLY and then post-processes (resolves/rejects the result
promise). So you CANNOT just wrap job_func in qjs_run_forced_flow: a trampoline yield from the inner JS_Call
would unwind THROUGH job_func's C code (which then tries to resolve the promise with a half-value) — the park
mechanism assumes a clean qjs_run_forced_flow→JS_CallInternal boundary, and a job_func doesn't have one. This is
WHY the boot job pump is the hard gap, and why B2a (routing the drive, whose caller-side pump still ran jobs the
old way) surfaced it. THE DESIGN (its own effort, verify on a REAL spinning-continuation page — spa_gated has
none): make the promise/async job machinery trampoline-AWARE, mirroring how OP_call already trampolines. When a
reaction/resume job's inner JS_CallInternal yields at the quantum, job_func must NOT complete-and-resolve; the
job itself must PARK (snapshot the reaction/async-resume state + the heap-stack chain into qjs_drive_flow) and
be re-enqueued so qjs_drive_repick resumes it — i.e. a job becomes a Flow like any other. Concretely: (1) a
"driving job pump" that sets qjs_driving and, on qjs_yielded, parks the CURRENT job's continuation instead of
letting job_func finish; (2) job_func variants (or a wrapper) that recognize the yield and defer their
post-JS_Call resolution until the parked continuation actually completes on resume. Until then g_bfs_noprog
stays (now @WHY-visible) as the ONLY non-preemptible-path backstop. Do NOT bolt a rushed version onto the core
event loop — a bug here breaks ALL async, and it can't be caught by spa_gated.

**ROOT CAUSE of the B2a hang — MEASURED, and the async hypothesis was DISPROVEN (this is why you measure):**
- First guess (WRONG): async frames live in async_func_init's malloc buffer, not the arena qjs_park_forced_flow
  snapshots, so parking a mid-async flow corrupts. Tested it and it was wrong — see below.
- PROBE: re-routed ONLY site 2 (the SYNC opaque static drive — no async pump, no directed state) through
  qjs_run_forced_flow. It STILL hung boot (spa_gated 5→0). So the hang is NOT async-specific — a plain sync
  static target routed through the primitive hangs too. The async-buffer theory is dead.
- The hang is SYNCHRONOUS + context-general: `harness worker "self._lastWhy"` TIMES OUT during the hang (a
  blocked worker can't service an eval) ⇒ the wasm boot is in a synchronous infinite loop/block, not an
  async-job spin. It is SPECIFIC to calling qjs_run_forced_flow from js_fe_drive_static's context — the WORKING
  DRIVEBREADTH path (__hostDrive) uses the IDENTICAL bracket (js_fe_flow_begin == qjs_flow_arm) and the IDENTICAL
  primitive, yet does NOT hang. So the difference is neither the bracket nor the primitive; it is something about
  js_fe_drive_static's boot position / state (candidate: parking a flow into qjs_drive_flow at THIS boot point,
  before the grind owns the registry, corrupts a later boot step; or the require/directed-drive globals; NOT
  confirmed).
- LOCALIZED (cow_-prefixed probes + `harness diag` on a fresh post-restart boot — the tooling that works;
  `diag` captures the worker/background_page console incl. cow_ @WHY, and `restart` clears the AST cache so the
  wasm actually re-boots): it is NOT a hang and NOT async. Every forced-flow drive RETURNS (all 4 targets print
  cow_static_pre AND cow_static_post, kind=0 sync). The real failure is NON-CONVERGENCE: `cow_static_loop_begin`
  fires 90+ times (js_fe_drive_static re-invoked per schedule) and learnedCount stays 0 — boot never finishes.
- MECHANISM (CONFIRMED by the probe: all 90 loop_begins report nt=4 — the SAME 4 targets re-collected every
  schedule; if the marking persisted the 2nd schedule would collect 0): the plain-JS_Call path leaves the
  target's driven-marking PERSISTED so `if (b->qjs_executed) continue;` (quickjs.c ~66325) skips it next
  schedule; routing through qjs_run_forced_flow loses that marking, so every target is re-collected + re-driven
  every schedule → the BFS drive-count never goes flat → the driver's `while (m!==n||hm!==hp)` fixpoint never
  converges → 90+ re-boots, 0 emit (boot never completes so nothing is learned).
- THE FIX (well-specified, its own focused effort — verify carefully, do NOT rush at a session tail): persist a
  driven-marking OUTSIDE the flow revert, exactly like the grind's post-`qjs_run_forced_flow` `b->qjs_driven=1`.
  Use `qjs_driven` (+`qjs_driven_opaque` for the opaque drive), NOT `qjs_executed` — `qjs_executed` means "real
  dynamic values captured", and setting it for an opaque force-drive would hide the target from the grind's
  real-instance re-drive (value-depth loss). So: (1) set `b->qjs_driven=1; b->qjs_driven_opaque=1;` AFTER
  `qjs_flow_revert(ctx)` for each of the 3 sites; (2) add `|| b->qjs_driven` to the collection filter at ~66325
  so a boot-force-driven target isn't re-collected. RISK: `qjs_driven` is shared with the grind — verify the
  grind still drives the object-graph residue (its re-drive-eligibility already keys on qjs_driven_opaque) and
  spa_gated still = 5 AND now CONVERGES (learnedCount reaches 5, loop_begin count small/bounded).
  g_bfs_noprog now emits @WHY bfs_spin_skip so its firing stays measurable. (Determinism separately CONFIRMED
  this session: bootHash 16779628741915285990 reproducible across restarts — the resume foundation is intact.)

REMAINING toward full one-BFS: (i) localize + fix the js_fe_drive_static / boot-job-pump sync hang (above) →
route them through the primitive → delete g_bfs_noprog; (ii) __hostDrive's separate top-level-global breadth
pass (load-bearing: disabling it alone regressed 5→4 — MERGE into the one frontier, don't delete); (iii) the
single-Flow-registry rewrite below.


MEASURED + READ, the CURRENT reality violates "one BFS": there are TWO drive systems, not one.
- **Breadth phase:** `hostedge.js __hostDrive` enumerates TOP-LEVEL GLOBAL FUNCTIONS and force-invokes each
  via `__feDriveBreadth` → `qjs_run_forced_flow` (preemptible under `qjs_driving`, parks via
  `qjs_park_forced_flow`). It DELIBERATELY does NOT drive object-graph methods (a `new Client()`'s methods,
  a `window.__d={…}` namespace) — hostedge.js:1418-1427 hands those to "the deep grind."
- **Grind phase (SEPARATE):** quickjs.c ~64989 flat-scans ALL un-fired `JSFunctionBytecode`, appends to
  `qjs_deep_rb`, relevance-sorts, and drives them in its OWN WFQ loop (~65505: `qjs_deep_flow` registry +
  `qjs_cow_capture` park) — distinct from `qjs_run_forced_flow`, even though that primitive's own comment says
  "THE ONE... ONE preempt loop here, used everywhere — not a separate breadth path."
This is the banned "second scheduler beside the one policy." Both are FLAT FRONTIERS of un-fired functions
(the grind is not internally DFS — that part is fine); the sin is that they are SEPARATE (separate collection,
separate loop, separate WFQ, sequential "breadth THEN depth"). The stale justification for the split
(hostedge.js:1421 "object graphs can't be preempted, so don't walk them here") is FALSE now — `__feDriveBreadth`
IS preemptible.

TARGET (one BFS): a SINGLE frontier = {top-level fns ∪ object-graph orphans ∪ parked/resumable flows},
collected once, WFQ-ordered by emitted-output value, driven by the ONE `qjs_run_forced_flow` primitive
(preemptible + parkable + IDB-evictable + cross-session-resumable), in ONE continuous loop — NOT boot-epilogue
breadth then a separate grind. Delete the grind's separate WFQ loop; delete `__hostDrive`'s "don't walk object
graphs" carve-out (drive them through the same primitive with the same receiver-map/cold-instance capture).
The CLAUDE.md phrase "Run BREADTH first THEN DEPTH (residue grind)" should be read as ONE BFS whose frontier
happens to reach top-level fns before residue by WFQ value — NOT two systems.

REWRITE TARGET (the "assuming rewrite" design — dissolves the two-registry mess, not patches it). There is ONE
type: a **Flow** = { resume-entry (either FRESH: fn+this+args+is_ctor, or PARKED: heap-stack chunk-chain +
COW delta), wfqValue (emitted @H/@S this flow produced — the ONLY ordering key), state (READY|PARKED|DONE),
optional receiver (real-instance `this` from qjs_deep_capture_inst) }. Everything the current code splits into
"top-level global (breadth)", "object-graph orphan (grind)", "callback/orphan-enum arm", "re-entrant sub-flow"
is JUST A Flow with a different resume-entry — no separate populations, no `qjs_deep_rb` vs `qjs_drive_flow`
vs `qjs_deep_flow`. ONE registry: a growable Flow[] with a stable handle per Flow (the index question I asked
is moot — a Flow gets its slot when it ENTERS the frontier, whatever its origin). ONE loop:
  while (frontier has a READY|PARKED Flow):
    f = pick-max-wfqValue (least-served among ties; DONE/firing removed)   // the single WFQ, priority.js's comparator
    drive-or-resume f via qjs_run_forced_flow under qjs_driving for ONE quantum   // the ONE primitive
    if it yielded-at-quantum: COW-capture into f's slot, state=PARKED, re-enqueue   // preempt, ~0 CPU
    elif completed: state=DONE
    (under memory pressure: serialize the coldest PARKED Flow's {COW delta + resume-entry} to IDB, evict)
Seeding the frontier is ONE pass that collects {top-level fns ∪ every un-fired JSFunctionBytecode ∪ methods
reachable on captured instances} as FRESH Flows — no boot-epilogue-breadth-then-grind. Cross-session: the
PARKED Flow blobs in IDB are keyed by bundle-hash; deterministic reboot reloads + re-enqueues them (state
still PARKED) so the loop resumes them — THAT is "unbounded until disk limit, resumable after restart."
This makes `qjs_deep_flow`, the 65505 residue loop, and hostedge.js's separate `__hostDrive` breadth pass all
redundant: delete them; keep only qjs_run_forced_flow + one registry + one loop + the receiver-map capture as
a Flow field. It is a scheduler rewrite (multi-day, cross hostedge.js/quickjs.c), NOT a tail-of-session patch;
the honest blocker to a partial edit is that today's two registries key resume DIFFERENTLY (per-orphan `_ix`
vs arrival counter), so half-migrating parks-into-one / resumes-from-the-other and re-drives from scratch
(lost progress) — the unified Flow handle is what removes that. Build it as its own effort, verified: after
seeding, chain_direct still learns 2; a heavy orphan PARKS (its slot gets a COW delta) then RESUMES + emits;
then the IDB round-trip.

WHY IT MATTERS (and connects to "nothing parks"): with two systems, park lives mostly in the grind; a heavy
flow that blocks anywhere non-preemptible stalls the whole thing, and the cross-session round-trip (park →
evict-to-IDB → restart → reload → resume) — the actual "UNBOUNDED until disk limit" — stays unverified.
STATUS: this is a substantial, correctness-critical refactor of the drive across hostedge.js + quickjs.c. Do
it INCREMENTALLY with live verification at each step (analysis still emits @H, no drive hang, park demonstrably
fires), NOT a big-bang. Do NOT rush it at the tail of a long session — that risks breaking the whole drive.

## SECURITY VIEW — two next targets (2026-07-01, from an XSS fixture: location.hash/search -> innerHTML/insertAdjacentHTML/document.write)

The "two views" second half. On a fixture flowing OPAQUE input (location.hash, location.search) into DOM
sinks, the engine analyzed fully (nWhy:209) and the `static_sites` @WHY showed `sink_wr:2, emitted:0` — it
FOUND 2 static sink sites but EMITTED 0 `@S`. So the taint->sink->PoC EMISSION did not fire for a clear
opaque->innerHTML flow. NEXT (security value area): trace WHY no @S emits — is the opaque location.hash taint
reaching the innerHTML sink in the forced trace, and does the Z3-PoC emitter run? (measure via _lastWhy at the
sink-write + taint-join). Repro fixture:
```html
<div id="out"></div><script>document.getElementById('out').innerHTML = location.hash.slice(1);</script>
```
ALSO SURFACED: a NEW `free_zero_bad` @WHY phase on this fixture (a refcount free-at-zero, not present on
chain_direct) — a separate refcount issue the DOM-sink path triggers; instrument it (via _lastWhy) next.

## MOAT DEMONSTRATED 2026-07-01 (engine 3bb3c96, fixture `testing/fixtures/spa_gated.html`)

End-to-end proof of the core value on a realistic gated mini-SPA (the first non-diagnostic fixture in-tree):
logged OUT, forced-exec learned the **logged-IN API surface** — `learnedCount:5, liveDistinct:1` (only
`/feed` fires live). Surfaced the auth-gated, never-fired-logged-out endpoints WITH computed values where
derivable: `GET /api/v1/feed [key=pub_abc123]` (live), `GET /users/{id}/profile [id opaque]`, `POST
/billing/subscription [account opaque, plan=pro]`, `DELETE /admin/users/{id} [id opaque]`. Concrete literals
computed (key, plan); auth-gated params (user.id/token/account) correctly OPAQUE. No hang, no over-decref — the
3bb3c96 fix holds on a richer bundle. **Use spa_gated.html as the moat regression/coverage fixture.**
FLAG-GATED COVERAGE VALIDATED (2026-07-01, second variant): with the auth flag from OPAQUE external input
(`state.role = new URLSearchParams(location.search).get('role')`, the REALISTIC case), the engine FORCED the
guard `if (state.role !== 'admin') return` and LEARNED `GET /api/v1/admin/metrics [scope=all]` while logged
out. So the moat surfaces opaque-auth-gated admin endpoints exactly as intended. The original fixture's
`/admin/metrics` miss was a FIXTURE ARTIFACT, not an engine gap: `role:'guest'` is a CONCRETE literal, and
forcing a concrete branch would be INVENTING a path real execution can't take (banned — "RUN, DON'T INVENT").
Real auth flags are opaque-sourced, so they ARE forced. Net: flag-gated coverage works; no engine change needed.
The fixture (testing/fixtures is gitignored, so preserved here for exact recreation):
```html
<!doctype html><html><body><script>
var API = { base: '/api/v1', key: 'pub_abc123' };
var state = { user: null, role: 'guest' };
function publicFeed(){ return fetch(API.base + '/feed?key=' + API.key); }
function loadProfile(){ return fetch(API.base + '/users/' + state.user.id + '/profile', { headers: { Authorization: 'Bearer ' + state.user.token } }); }
function adminDashboard(){ if (state.role !== 'admin') return; return fetch(API.base + '/admin/metrics?scope=all'); }
function billingInfo(){ return fetch(API.base + '/billing/subscription', { method: 'POST', body: JSON.stringify({ account: state.user.accountId, plan: 'pro' }) }); }
function deleteUser(id){ return fetch(API.base + '/admin/users/' + id, { method: 'DELETE' }); }
window.__app = { loadProfile:loadProfile, adminDashboard:adminDashboard, billingInfo:billingInfo, deleteUser:deleteUser };
publicFeed();
</script></body></html>
```

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

## RESOLVED 2026-07-01 (engine 3bb3c96): non-park shape over-decref — arena-aliased defer trail

**FIXED + VERIFIED + PUSHED.** The reliable `js_free_shape0` rc=-1 over-decref (chain_direct's `shape0:2`,
scenario #1 below) was the deferred-restore trail array being ARENA-backed: `qjs_cow_defer_push` grew it via
`js_realloc_rt` → `js_def_realloc` → the flow arena, which bump-RESETS at FLOWEND, so the kept array pointer's
addresses got re-handed to the next flow's objects → `defer_revert` read a CORRUPT entry (`neww` → a reused JS
object) and `js_free_shape`'d it. MEASURED definitively via the new `_lastWhy` channel: `shape_overdecref
{gt:0, inArena:0, deferSite:1, hashed:garbage}`. FIX (2 parts, both needed): (1) gm-back the array
(`__real_malloc`+memcpy+leak, survives arena resets, mirrors `qjs_cow_undo_grow`); (2) make
`qjs_cow_defer_push` `QJS_JSEXPORT`(KEEPALIVE) so the cow-barrier pass SKIPS instrumenting its stores
(cow-barrier.mjs:85) — the instrumentation was why the 6 prior gm attempts HUNG (barrier log/revert of the
trail + cow_g clobber); export-skip mirrors the word-log's export-skipped writer `qjs_cow_undo_log`. VERIFIED
live (chain_direct + trivial, plain restart): `shape0` 2→0, aborts 3→1 (both shape aborts gone), learnedCount
stays 2, no hang, stable across reruns. Scenario #2 (park/resume orphan) is SEPARATE and
still open — the sound park-pin diff + parking-fixture recipe are preserved below.

NEXT TARGET (the now-ONLY abort on chain_direct, 1/drive) — `list_empty(&rt->gc_obj_list)` at JS_FreeRuntime.
CHARACTERIZED via `_lastWhy` FreeRuntime_residue: `{obj:918, bytecode:553, shape:222, varref:164, cfunc:260,
context:1, ctxRc:1734, promise:1}`. The CONTEXT (rc 1734) is rooted at teardown, so it roots the whole bundle
(every cfunc/bytecode holds a realm = a ctx ref) -> nothing frees -> the assert. This is the KNOWN, extensively-
worked context-rooting issue (in-code machinery: `qjs_settle_pending_promises` ~61283, `free_generator_stack_rt`,
the async-generator unwind ~61322 — they release ctx refs held by forced-exec's SUSPENDED frames). They didn't
fully clear here: the residue's `1 promise` (a PENDING promise a reaction still points at, that settle missed)
is the likely last ctx-rooter. PRE-EXISTING (baseline b5eea41's 3rd abort) + LATENT (endpoints emit before
teardown) but real for the unbounded multi-page design (each page leaks its whole runtime). NEXT: instrument
via `_lastWhy` WHICH promise/frame survives the settle/unwind pass and WHY, then extend the unwind. Measure-first.
INVESTIGATED 2026-07-01 (bounded — no fix yet, but scoped for a fresh effort): the FreeRuntime_residue is on
STDERR (5225, reliably in `_lastStderr`, NOT `_lastWhy`). Built a `survivor_promise` probe — a helper
`qjs_promise_dbg(JSObject*, int*st,*fr,*rr)` after the JSPromiseData def (~61094) dumping {state,
fulfillReact, rejectReact, rc}, called from the residue loop's JS_CLASS_PROMISE branch (~5209) via an inline
`extern` decl (JS_FreeRuntime precedes the struct). KEY FINDINGS: (a) the abort is INTERMITTENT — fired ~1/3
drives of chain_direct, 0/4 on another run; the residue is lost across the abort's worker reset unless caught
on stderr. (b) an EXPLICIT deterministic-leak fixture (a never-resolving `new Promise` with a `.then`
reaction that fetches, + an `async` awaiting a never-resolving promise) did NOT leak (abort:0) — the settle
loop (`qjs_settle_pending_promises` + the fixpoint at ~65948) robustly UNWINDS those common forms. ⇒ the
chain_direct survivor is a RARE, non-obvious form the settle misses (not a plain pending-promise-with-then,
not a plain async-await). NEXT: needs a DETERMINISTIC repro of that specific form (or drive chain_direct in a
loop with the survivor probe until it fires, then read state/reactions) before extending the unwind. The probe
code is preserved here for re-application. Deep + latent (endpoints emit first); not a tail-of-session fix.
## (historical) OPEN ROOT: STALE SHAPE-kind defer entry — a use-after-free (LOCALIZED model-free)

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

LOCALIZED 2026-06-30 — THE ACTUAL ROOT (supersedes the typed-trail guess above): two more decisive measures.
(1) header-preserve-shapes is a NO-OP (3 cycles byte-identical) ⇒ the over-decref is NOT the byte-revert; the
shape's rc is 0 in LIVE execution when the defer-free hits it. (2) `shape_site[rc=-1 deferSite=1]` ⇒ it's the
defer REVERT freeing NEW. Reconciling with the transition-PIN also being a no-op: there are exactly TWO
is_shape `qjs_cow_defer_push` callers — `cow_shape_transition` (21647, the pin covered it) and
`qjs_cow_apply`'s PARK/RESUME RE-PUSH (21181, NEVER pinned). So the victim is a RE-PUSHED entry whose NEW shape
lost its refcount WHILE PARKED. ROOT: `qjs_cow_park` (~21126-21131) copies df_old/df_new as RAW pointers +
truncates the live trail; across the pause the shape's only liveness is an orphaned heap ref that the immediate
`to_baseline` revert + the GLOBAL word-log shadow dedup (shared across sibling grind orphans) can drop → on
resume the re-pushed entry's new is rc=0 → defer-revert → -1. This EXPLAINS every prior result: header-preserve
no-op (rc dropped while parked, not by byte-revert), transition-pin no-op (wrong caller), defer-removal regress
(load-bearing for non-parked entries), the inArena=0/no-fallback "contradiction" (the re-pushed pointer is to a
shape captured in a PRIOR flow's state, classified against the CURRENT arena). FIX (careful, fresh, NOT
tail-of-session): pin SHAPE df_new/df_old at `qjs_cow_park` so they survive the pause; release refcount-exactly
at `qjs_cow_apply` (resume) / `delta_free` (abandon) / post-resume consume — accounting for the orphaned-but-
uncounted heap ref `to_baseline` leaves (the phantom-ref subtlety, like cow_set_value; naive pinning over/under
-counts — that's why the transition-pin attempt mis-accounted). Verify shapeAssert 0 + endpoints 2 +
determinism + that the grind's pause/resume is actually exercised by the fixture. Engine verified b5eea41.

CORROBORATED 2026-07-01 (fixture-differential — a NEW axis confirming the park/resume root above) + the
defer-STORAGE patch path is now CATALOGUED-EXHAUSTED (do not retry any of these). This session wrongly
chased the defer-ARRAY storage (arena-aliasing) instead of the park/resume REFCOUNT the line-above
localization already named — 8 builds, every one either hung or leaked, ALL on the SAME chain_direct flow:
- **The over-decref/hang is PARK-SPECIFIC, proven by a fixture A/B.** A TRIVIAL fixture (object property
  adds + two `fetch()` calls, NO fetch→fetch chain ⇒ NO park/resume) gives `shape0:0` + `learnedCount:2`
  with EVERY defer patch; `chain_direct` (the `fromReply` fetch→fetch chain ⇒ parks the flow at the reply)
  HANGS (`learnedCount:0`, worker stops yielding) with the same patches. Baseline b5eea41 handles BOTH
  (chain_direct 2 / `shape0:2`; trivial 2). ⇒ what breaks is exactly the park/resume path of #21181, not the
  storage — direct, independent confirmation of the `shape_site deferSite=1` + park re-push localization.
- **Defer-STORAGE patches that are DEAD (8 builds, all refuted on the live harness):** (a) gm-back the trail
  array via `__real_realloc` — HANG (realloc's internal free mutates gm free-list mid-flow, which the flow's
  free-suppression forbids). (b) gm via `__real_malloc`+memcpy+leak — HANG. (c) +`rc>0` guard at the revert
  free — HANG. (d) pop-only revert (free nothing) — HANG **and** leaves `neww` dangling in the shape hash
  (next flow's `find_hashed_shape_prop` chain-walk loops). (e) arena-grow + pop-only — HANG (isolates: it's
  not the grow). (f) PRE-GROW the gm reserve at FLOWBEG/`pool_warm` + keep the free (mirrors the word-log's
  warm-then-bump) — HANG (so gm storage of THIS array is incompatible for a reason that stays unpinned; the
  word-log's gm array is written only inside the export-SKIPPED `qjs_cow_undo_log`, whereas `qjs_cow_defer_push`
  is `static` ⇒ INSTRUMENTED, the relevant asymmetry). (g) NULL `qjs_cow_defer`+cap at FLOWEND so each flow
  re-allocates fresh (kills the cross-flow arena-reuse) — works on TRIVIAL (`shape0:0`) but HANGS chain_direct
  AND adds a `list_empty(&rt->gc_obj_list)` teardown abort (leaks shapes). CONCLUSION: the defer trail's arena
  storage is LOAD-BEARING and entangled with park/resume; the lever is the park/resume SHAPE refcount (pin at
  `qjs_cow_park` / release at `qjs_cow_apply`+`delta_free`), tested against a PARKING fixture (chain_direct),
  done FRESH. The PLAN said "do NOT attempt another defer-only patch" — heed it; this session proved it again.

DISAMBIGUATED 2026-07-01 (decisive — corrects a MULTI-SESSION conflation, incl. my line just above): a
one-shot `parkprobe` abort at the top of `qjs_cow_capture` (fires on the first capture, reports dfn/nShape)
did **NOT fire** on chain_direct (`anyCapture:false`) while `shape0` stayed 2. ⇒ **chain_direct NEVER PARKS**
(no `qjs_cow_capture`/`apply` at all — its `fromReply` fetch→fetch chain is a synchronous host round-trip,
not a COW suspend/resume). Therefore chain_direct's reliable `shape0:2` over-decref is **ENTIRELY NON-PARK** —
it is the plain `cow_shape_transition`-push → `qjs_cow_defer_revert_to`-free path, NOT the park re-push. So the
`shape_site[deferSite=1]` + "re-pushed entry whose new lost its rc while PARKED" localization above was measured
on the **deep-GRIND** scenario (which DOES park), a DIFFERENT code path from chain_direct. TWO separate shape
over-decrefs have been conflated across sessions:
  1. **NON-PARK (chain_direct):** the live defer trail's revert frees a stale/aliased `neww` (the arena
     cross-flow reuse — the defer array is arena-backed and its addresses get re-handed to the next flow's
     objects). Every STORAGE fix for it (gm-back, NULL-at-FLOWEND) HANGS chain_direct (storage is load-bearing).
     This is the one blocking a "clean" chain_direct. Fix is NOT storage and NOT the park pin — likely making
     the non-park defer-revert free skip/repair the aliased entry at its root (why the arena addresses alias:
     the trail array shares the bump region with flow objects and outlives a single flow's revert).
     **DEFINITIVE 2026-07-01 (non-abort @WHY probe at js_free_shape0, read via _lastWhy — resolves the
     multi-session contradiction):** `shape_overdecref{rc:-1, gt:0, inArena:0, deferSite:1, hashed:248}` ×2.
     gt=0 (JS_OBJECT not SHAPE) + hashed=248 (garbage; a real shape's is_hashed is 0/1) ⇒ `neww` is a
     DANGLING pointer into a baseline block REUSED as a JS object — read from a CORRUPT defer entry. deferSite=1
     ⇒ the defer revert. inArena=0 ⇒ baseline block. This KILLS both rival theories: NOT byte-revert-to-0 of a
     flow-created shape (that would be a real shape, gt=SHAPE, inArena often=1), and NOT park (captures:0). It
     is PURELY the arena-aliased trail array: its stale pointer overlaps the next flow's objects, so defer_revert
     reads garbage {old,neww,...} and js_free_shape's a reused object (decrements THAT object's rc 0→-1 + would
     double-tear it down). Analysis SURVIVES it (learnedCount stayed 2 with the free skipped) ⇒ the corrupted
     object isn't fatally used here → latent, non-blocking. THE FIX must (a) stop the array aliasing AND (b) the
     free-logic bug the aliasing masks (freeing the REAL neww then cascades → the gm/NULL hangs). The gm-hang is
     unmeasurable-while-hanging (the @WHY channel needs the worker to yield); next: bisect the gm change
     natively, or a dedicated non-arena non-gm trail region, or eliminate the trail for the non-park path.
  2. **PARK/GRIND:** the capture→pause→resume orphan. The SHAPE-pin fix (js_dup_shape old+new at
     `qjs_cow_capture` ~21131; release js_free_shape at `qjs_cow_apply` after re-push ~21181 + at
     `qjs_cow_delta_free` ~21201) was DESIGNED + IMPLEMENTED this session — sound by construction (mirrors the
     typed-trail JS_DupValueRT-at-capture / release-at-apply lifecycle EXACTLY; also fixes the documented
     `delta_free` SHAPE-ref leak) — but REVERTED UNVERIFIED because **no current fixture parks** (chain_direct
     doesn't; the trivial fixture doesn't). **THE missing prerequisite for ALL park/resume work is a fixture
     that actually drives the deep-grind PARK** (COW capture/apply). Build that FIRST (a bundle heavy enough to
     trigger the orphan-suspend, or drive the grind directly), then re-apply the pin (it is a ~10-line 3-site
     diff) and verify shape0/no-hang + the cross-session round-trip. Until a parking fixture exists, park fixes
     are un-testable — do not push them. Engine stays at verified b5eea41.

PARK TRIGGER + FIXTURE RECIPE (2026-07-01, so the next session builds it in one shot):
- **What parks:** `qjs_drive_run` (~64107) resumes a flow WHILE it emits @H/@S; on the FIRST window with
  `QJS_DEFER_QUANTUM`(=64) consecutive SILENT yield-windows (~640k ops, no new emit) it PARKS — snapshot +
  `qjs_cow_capture` + `qjs_cow_to_baseline` (~64115-64123). So parking = **a heavy, SILENT flow on the drive
  loop.** This is a **deep-GRIND orphan-drive** path, NOT the BFS force-invoke path.
- **A BFS heavy loop does NOT park (measured):** a fixture whose `window.go` runs a 250k-iter object-churn
  loop then fetches — the parkprobe NEVER fired (worker blocked grinding ~170s, no abort ⇒ no capture). BFS
  force-invokes don't route through `qjs_drive_run`, so a heavy *called* function is not enough.
- **The recipe that SHOULD park:** make the heavy silent compute an **UNCALLED ORPHAN** (so the residue GRIND
  drives it through `qjs_drive_run`), with object churn for shape entries, emitting only AFTER the silent span:
  `window.go=()=>fetch('/api/quick');  window.heavy=function(){var t=0;for(var i=0;i<80000;i++){var o={};o.a=i;o.b=i*2;t+=o.a+o.b;}return fetch('/api/after?t='+t);};`
  (heavy is uncalled ⇒ grind orphan ⇒ silent ~640k ops ⇒ parks ⇒ resumes.) Tune the count to just exceed one
  quantum; caveat: park tests are SLOW (640k instrumented ops ≈ 1-2 min, and the worker blocks — poll _lastStderr
  via a one-shot `__assert_fail`, the only channel that survives). Confirm parking BEFORE testing the fix.
  UPDATE 2026-07-01: measurement channel FIXED — `self._lastWhy` now buffers all @WHY (readable via
  `harness worker "self._lastWhy"`), so `cow_stats` (captures/applies) is readable NON-destructively; no more
  abort-probes. Used it to reconfirm chain_direct never parks (`captures:0`) cleanly. BUT the recipe above is
  STILL UNVERIFIED and harder than hoped: an uncalled heavy orphan (`window.heavy` = 90k-iter object-churn loop,
  ~1.1M ops) ALSO did not visibly park — the worker BLOCKED grinding it (@WHY stuck at 11 records for 250s, no
  `cow_stats`, `orphan_drive` present but no `captures`). So BOTH a BFS heavy loop AND an uncalled heavy orphan
  fail to trigger a clean park. OPEN: the `qjs_drive_run` park-quantum (64 silent windows) may not fire for
  these drives, or the orphan drive routes differently, or forced-exec doesn't run the concrete loop as
  sequential silent ops. Next session: instrument the silent-window counter / `qjs_drive_run` entry via the NEW
  @WHY channel (non-destructive) to see WHY the quantum doesn't fire, before assuming any fixture parks.
  ARCH CLARIFICATION 2026-07-01 (why the worker blocks, so the next session doesn't mis-diagnose it as a
  bug): the per-opcode return-to-scheduler goes to the wasm-INTERNAL WFQ, NOT the host (by design — CLAUDE.md
  "per-opcode RETURN-to-scheduler ... NOT a JSPI suspend"). ast-thread.js yields to the HOST event loop only
  at SCRIPT / deep-resume boundaries (`setTimeout(r,0)` ~1554/2137/2280, `_resumeIncompleteDeep` ~3163/3253),
  never mid-script. So one heavy script/segment blocks the worker synchronously until it finishes — the host
  schedules across PAGES (one wasm instance each), not within a page. ⇒ a park test inherently ties up the
  worker for the whole heavy drive (minutes at the instrumented op-rate); read `cow_stats` via `_lastWhy`
  AFTER it returns, and size the orphan to just exceed 640k ops so it completes. This also means the #5/#10
  host-level attention (cross-page fibers, IDB eviction) can only act at those boundaries today — if within-a-
  page eviction under memory pressure is ever needed mid-heavy-script, that host-yield gap is the design item.
- **THE PARK-PIN FIX (sound-by-construction, mirrors the vt-trail; re-apply verbatim, ~10 lines, 3 sites):**
  1. `qjs_cow_capture` defer-capture loop (~21131), after copying df_old/df_new/df_kind:
     `if (qjs_cow_defer[k].is_shape) { if (d->df_new[k]) js_dup_shape((JSShape*)d->df_new[k]); if (d->df_old[k]) js_dup_shape((JSShape*)d->df_old[k]); }`
  2. `qjs_cow_apply` re-push loop (~21181), after `qjs_cow_defer_push(...)`:
     `if (dm->df_kind[j] && grt) { if (dm->df_new[j]) js_free_shape(grt,(JSShape*)dm->df_new[j]); if (dm->df_old[j]) js_free_shape(grt,(JSShape*)dm->df_old[j]); }`
  3. `qjs_cow_delta_free` (~21201), replace the BUFFER-only free with kind-split (SHAPE releases BOTH pins),
     wrapped in `g_cow_busy=1`: BUFFER→`js_free_rt(r,df_new)`, SHAPE→`js_free_shape(r,df_new)`+`js_free_shape(r,df_old)`.
  Accounting: net-zero on steady state (+1 at capture / −1 at apply-after-re-push, or −1 at delta_free-abandon);
  it only keeps `new`/`old` alive across the orphan window `to_baseline` opens. This fixes BOTH the park orphan
  over-decref AND the documented `delta_free` SHAPE-ref leak. NOTE: it does NOT touch chan_direct's NON-PARK
  over-decref (scenario #1 above) — that one is separate.

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

## grind_orphan.html fixture (2026-07-01) — design-correctness attempt + a coverage LEAD
Created testing/fixtures/grind_orphan.html (gitignored; content = an ApiClient class constructed at
boot with methods NEVER called: listInvoices() [0-arg], refund(id), closeAccount(uid)).
GOAL was to trigger the DEEP GRIND (spa_gated never reaches it — no orphan_drive phase, feDeepDB
empty). RESULT: it did NOT trigger the grind — the STATIC phase's real-receiver map
(js_fe_drive_static site 1, target_this) absorbed it (boot-constructed instance is found there, so
no grind needed). It DID demonstrate the static real-receiver value: `this.base` resolved CONCRETELY
=> `GET /api/v1/invoices?status=unpaid` from an uncalled method ("logged-in surface while logged out").
LEAD (NOT yet confirmed real — it's a self-written fixture, do not inflate): the ARG-TAKING methods
refund (POST /invoices/{id}/refund) and closeAccount (DELETE /accounts/{uid}) emitted NOTHING —
not even reachedButOpaque/resolverError. closeAccount's URL shape (concrete-base + opaque-arg-at-end)
is IDENTICAL to spa_gated's WORKING admin/users/{id}, so the differentiator is that these are
parameterized METHODS driven via the static real-receiver path (site 1), whose opaque method-arg
apparently doesn't flow to an emitted endpoint. NEXT (endpoint-coverage value area): trace whether
site-1 drives arg-taking methods and why the opaque-arg URL doesn't emit — confirm real vs fixture
artifact BEFORE acting. To trigger the GRIND specifically (not static), the instance must be created
by a FACTORY driven DURING the grind (qjs_deep_capture_inst), not constructed at top-level boot.

### grind_orphan LEAD RETRACTED (2026-07-01) — it was a premature read, NOT a gap
Re-ran with adequate wait: ALL parameterized methods DO emit — refund POST /invoices/{id}/refund,
closeAccount DELETE /accounts/{uid}, plus the isolation top-level topRefund POST /orders/{id}/cancel
(both {arg0} and {id} shapes). The earlier "refund/closeAccount emit nothing" was reading netdiff
BEFORE analysis completed (learnedCount 2 was mid-flight, final is 6). So there is NO parameterized-
method coverage gap — the static real-receiver drive emits concrete-base + opaque-arg URLs correctly.
Discipline note: confirmed before acting, disproved the lead, corrected the record — do not inflate a
mid-analysis read into a finding. grind_orphan remains a valid STATIC real-receiver demo; it does not
reach the deep grind (would need a factory-created instance driven during the grind).

## MAJOR FINDING (2026-07-01, VERIFIED) — parks fire but NEVER resume; the UNBOUNDED core is broken
Built park_test.html (gitignored): a heavy NEVER-CALLED global `heavyReport()` with a 20M-iteration
loop then a fetch. Added observability @WHY at every park/resume site: flow_park (qjs_park_forced_flow),
grind_park (qjs_drive_run), flow_resume (qjs_drive_repick). MEASUREMENT (read via `harness worker
"self._lastWhy"` — NOT `diag`, which only console.log's cow_-prefixed @WHY, so it SILENTLY DROPPED these
and gave a false "0 parks"; that cost a wrong conclusion — fix the diag filter or always read the worker):
  - heavyReport DID PARK: flow_park fired 6x, each after ff_yielded slices:64 (hit the QJS_DEFER_QUANTUM
    exactly). So the PARK half works — a heavy forced flow is preempted + snapshotted.
  - flow_resume fired ZERO times. The parked flows are NEVER resumed. The endpoint still emitted only
    because a SEPARATE non-parking drive ran the full loop — the parked flow's progress was discarded.
  - All 6 parks are ix:0 => qjs_drive_n is reset to 0 before each park => the registry is wiped between
    parks. qjs_drive_repick (the ONLY resume path) is called ONLY in qjsmain.c's do_drive (--fe-drive)
    branch @1168, NEVER in the boot (--fe-boot rc==0) drain @1130-1149. And the per-drive IMAGE RESTORE
    (linear-memory snapshot restore between callMains) wipes qjs_drive_flow (a linear-memory structure)
    before any repick can drain it. So boot-time parks are LOST, not resumed.
This is the concrete root of PLAN.md's long-standing "nothing parks/resumes" + "cross-session round-trip
unverified": the PARK primitive is real and fires, but the RESUME half is unwired for the boot/BFS parks
(and the IDB evict->reload cross-session path is downstream of that same unresumed registry). THE FIX
(its own careful effort — boot/drive callMain + COW-baseline + image-restore interaction): parked flows
must SURVIVE the per-drive image restore (persist qjs_drive_flow across it, or evict to IDB immediately)
AND be resumed — either wire qjs_drive_repick into the boot drain once the baseline exists, or guarantee
a --fe-drive pass drains them. Until resume works, "UNBOUNDED until disk limit, resumable" is ASPIRATIONAL,
not real. The flow_park/grind_park/flow_resume @WHY are KEPT (this was invisible before — no-silent).

### boot-resume EXPERIMENT (2026-07-01) — repick-at-baseline is a NO-OP; the parks reset BEFORE the baseline
Wired qjs_drive_repick(g_boot_ctx) into the boot path right after qjs_cow_boot_baseline (qjsmain.c:1150).
RESULT: flow_resume STILL fired 0 times (park stayed 6, park_test endpoints unchanged/correct, no crash).
So the boot-time parks are NOT in qjs_drive_flow by the time the post-boot baseline exists — the registry
is reset/emptied BETWEEN the park (during page/driver.js eval) and the baseline. Evidence: all 6 flow_park
are ix:0 (each is the FIRST park in a fresh registry => qjs_drive_n is reset to 0 between parks). So the fix
is NOT simply "call repick at the baseline" — it is understanding WHERE qjs_drive_n/qjs_drive_flow gets reset
(candidates: a per-callMain re-init, the image restore, or the parks happen in a callMain that never reaches
the boot-baseline block). REVERTED the no-op repick (don't ship a speculative stopgap that might be wrong when
it does activate). NEXT DEBUG: probe qjs_drive_n at park time + at baseline time (are they the same callMain?),
and find every writer of qjs_drive_n=0 / qjs_drive_flow=NULL besides qjs_drive_repick. This is the concrete
path to making the UNBOUNDED resume real; it is a focused effort, not a session-tail patch.

### park-registry reset points TRACED (2026-07-01) — the concrete resume-gap mechanism
qjs_drive_n / qjs_drive_flow (the park registry) is reset/freed at exactly these sites:
  - 64407: inside qjs_drive_repick (resets to 0 AFTER draining+resuming — correct).
  - 65012-65015: the DEEP-GRIND SETUP unconditionally `js_free(qjs_drive_flow); qjs_drive_n=0`
    (comment: "#5/#9 drive park registry") — it DISCARDS any parked flows without resuming them.
  - (+ the per-callMain image restore wipes the whole linear-memory registry between callMains.)
So a boot/BFS-time parked flow is destroyed by whichever comes first — the grind-setup free (65012)
or the next callMain's image restore — and qjs_drive_repick (the ONLY resume) runs only in do_drive
(qjsmain.c:1168), too late / in the wrong callMain. THE FIX must resume (repick) parked flows BEFORE
any of these reset points AND make them survive the image restore (persist/evict), across the
boot->grind->drive callMain lifecycle. This is the real, well-scoped work to make "UNBOUNDED,
resumable" actually function; the PARK half already works (verified: flow_park fires at the quantum).

### resume-fix attempts — both quick wires FAILED to demonstrably fire; need a deterministic park->resume test
Two targeted attempts to wire the resume, both REVERTED (couldn't verify the active path):
  1. qjs_drive_repick at the boot baseline (qjsmain.c:1150): flow_resume=0 — parks not in the registry there.
  2. qjs_drive_repick right before the grind-setup free (quickjs.c:65012): flow_resume STILL 0 in the test
     runs (the park that the earlier PROBE saw as n:1 there is INTERMITTENT — timing-dependent; in the
     repick runs qjs_drive_n was 0, so repick was a no-op). No regression (spa_gated 5, grind_orphan 6), but
     the ACTIVE path (repick with n>0 at grind-setup) never actually executed => unverified => not shipped
     (B2a lesson: don't ship untested boot-context paths).
ROOT of the difficulty: a boot/BFS park does NOT reliably survive within a callMain to any resume point — it
is wiped early (image restore between callMains, and the grind-setup free). So the resume can't be
demonstrated by opportunistically calling repick; it needs the flow to SURVIVE (persist/evict-to-IDB
immediately on park, reachable independent of do_drive) THEN be resumed. PREREQUISITE for the fix: a
DETERMINISTIC park->resume test (a fixture + instrumentation that reliably parks a flow AND observes
flow_resume with correct output) — without it every fix is coded blind. That test + the persist-on-park
wiring is the real, focused UNBOUNDED-resume effort. The PARK half + full observability are DONE and committed.

## DEFINITIVE ROOT of "nothing resumes" (2026-07-01) — the per-schedule image restore wipes the park registry
Traced end to end. The host boots the bundle ONCE, saves the linear-memory image, and per BFS schedule does
`mod.HEAPU8.set(bootImage)` (ast-thread.js restoreSync :2009 / :2015) to reset the heap to baseline before the
next drive. That full-heap restore WIPES qjs_drive_flow (the park registry lives in that heap), so:
  - every schedule's parks are destroyed at the next schedule's image restore (hence all flow_park are ix:0 —
    a fresh, just-reset registry each time);
  - qjs_drive_persist_session (--fe-persist, ast-thread.js:2267) sees qjs_drive_n==0 -> persists nothing;
  - qjs_drive_reload_session / repick have nothing to reload/resume.
The persist/evict/reload/repick INFRA all exists and is correct in isolation; it is just never reachable
because the parks don't survive the HEAPU8.set. do_drive uses dirty-pages (:2032, no full restore) so
WITHIN a do_drive callMain qjs_drive_run resumes fine — the break is strictly ACROSS the image-restore boundary.
REWRITE-ALIGNED FIX (the honest best design, not a patch): a parked Flow must NOT live only in the wasm heap
that HEAPU8.set wipes. Two coherent options:
  (a) evict parks OUT of HEAPU8 to host-held JS blobs (qjs_drive_serialize/_evict already produce these) BEFORE
      each restoreSync, and re-inject (qjs_drive_restore, address-fixup) AFTER — the eviction round-trip applied
      at the image-restore boundary (within-session, no determinism-hash gate; that gate is only for the
      cross-SESSION reload). This is the minimal wiring of existing primitives.
  (b) the PLAN.md TARGET rewrite: ONE Flow registry where parks are first-class {RAM-hot | IDB-cold} entities
      owned by the HOST scheduler across callMains, so the per-schedule heap reset never touches them — parks
      and the WFQ live above the wasm-instance lifecycle, not inside its wiped heap.
Either way, the PARK half works (verified) and observability (flow_park/grind_park/flow_resume) is in place;
the RESUME half needs the flow to outlive HEAPU8.set. VERIFY WITH: park park_test's heavyReport, confirm
flow_resume fires with the correct acc AFTER an image restore. This is the concrete UNBOUNDED-resume effort.
