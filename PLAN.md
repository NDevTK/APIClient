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

- **No bounds, ever.** A depth/step/RAM cap is a SECOND decider beside the WFQ → not one
  scheduler, and inefficient (it truncates work the WFQ would have ordered by value). The WFQ
  starves the unproductive flow to ~0 CPU (resumable), never drops it. "Smaller is better" is not
  a design argument.
- **Cross-session rests on determinism the engine already requires.** Math.random/Date.now are
  synth-opaque; forced-exec is reproducible build-to-build. So: re-boot → same baseline → apply
  persisted delta. Key the persisted frontier by bundle hash (a changed bundle must not resume a
  stale frontier).
- **Refcounts are HEAP WORDS.** The COW word-log captures them — including the vt-trail's KEEP ref
  on each `old`. So cross-session apply re-applies the word-log (restores all refcounts) and
  re-pushes the vt-trail with NO incref. `old` is never freed mid-flow (the trail pinned it), so a
  slot's baseline value is its valid `old`. No reuse hazard.

## DONE (committed + pushed)

- Engine restored to fork `a212f9c`; design affirmed as the right architecture.
- Eviction-to-IDB: engine protocol + activation (fork `a212f9c`), host `feEvictDB` +
  consume-on-restore (main `2352cf8`). All 271 regression fixtures deleted (`5006814`) — testing
  policy is ONE targeted single-page design-correctness test, never a benchmaxxing battery.

## IN PROGRESS (uncommitted — user directed "do not commit" pending review)

- WFQ value-of-information: per-flow `value` = emitted output per run; repick resumes highest-value
  first; eviction targets the lowest-value tail (one metric). Removed the RAM-gate (a second decider).
- Cross-session ENGINE side complete (compiles): `qjs_cow_delta_serialize`/`_deserialize` (refcount-free
  data) + `qjs_cow_apply_xsession` (re-apply word-log + re-push vt/defer, no incref); `qjs_drive_serialize_flow`
  (handle+chunks+delta → one blob) + `qjs_drive_reload_flow` (re-inject, flag cross_session); repick
  dispatches cross_session flows to `apply_xsession`. JSFlow.cross_session flag.

## Cross-session WIRED end-to-end (uncommitted, compiles)

- Engine: serialize/deserialize/`apply_xsession`; `serialize_flow`/`reload_flow` (handle+chunks+delta);
  `persist_session`/`reload_session` (IDB marker `<hash>:0xFFFFFFFF` = [count, len…] + `<hash>:<seq>`);
  repick dispatches `cross_session` flows to `apply_xsession`; `set_bundle_hash`.
- Boot glue (qjsmain): `reload_session` after the boot baseline; `--fe-persist` command; `--fe-bundle-hash=`.
- Host (ast-thread): 32-bit FNV-1a content hash of the bundle → `--fe-bundle-hash=` on boot.

## NEXT

1. Persist TRIGGER: the host runs `callMain --fe-persist` at a checkpoint (analysis settle) so the
   frontier is in IDB before a restart. (Non-freeing copy variant for continuous checkpointing without
   evicting hot flows = follow-up; v1's freeing serialize is fine for session-end / the test.)
2. TARGETED TEST (the one design-correctness test): drive a page whose endpoint value is computed
   THROUGH a parked flow → `--fe-persist` → harness `restart` (new wasm instance; IDB persists) → boot
   auto-`reload_session` → drive → assert the endpoint value is exact (state survived the restart).
   Ad-hoc fixture, deleted after.
3. WFQ relief via virtual-time: low-value flows resume RARELY (starved not dropped) — dual-WFQ
   unification (engine `qjs_deep_vt` + host `priority.js` → one policy).
4. Evicted-flow cross-session persist (the in-session consume-on-restore entries need the non-consuming
   + handle/delta variant) — follow-up.

## Verification policy (CLAUDE.md)

Live Chrome harness (`testing/harness.js restart`), ONE targeted single-page design-correctness
test of the actual property (not `alive`, not a count). Regression batteries / flat metrics across
commits = benchmaxxing, banned. Build = a build step, not a verdict.
