/* Per-flow COW isolation — the swappable delta that makes flows INTERLEAVE like a browser.
 *
 * Each flow owns a CowDelta: the baseline (obj,prop) slots IT wrote, layered over the shared baseline. The
 * scheduler does NOT revert-to-baseline-then-rerun (a serial undo-log that forces a writer to run to
 * completion); it SWAPS deltas on every context switch — cow_unapply the outgoing flow (restore baseline,
 * SAVING the flow's current value so it can return) then cow_apply the incoming flow (replay ITS writes). So
 * two flows can each mutate the same shared `state.x` and each sees only its own value across any number of
 * mid-execution preemptions. A flow-local object (created during the run) is never captured, so a delta is
 * O(shared baseline state touched), never O(the run's transients).
 *
 * cow_capture_hook (installed as JSTimeTravelHooks.prop_write) records a baseline slot's pre-write value into
 * the CURRENT flow's delta (set by cow_set_current before each resume). unapply/apply are inverse and
 * idempotent-in-pairs. */
#ifndef ENGINE_HOST_SOLVER_COW_H
#define ENGINE_HOST_SOLVER_COW_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

typedef struct CowDelta CowDelta;

CowDelta *cow_delta_new(void);
/* RELEASE ONE FLOW'S DELTA — the head it owns, and its reference on the frozen chain below it.
 *
 * IT IS THE DELTA THE SCHEDULER IS *NOT* SWITCHED INTO, and that is the whole contract rather than a caveat:
 * the head's entries are freed, never unapplied, so a head still applied to the live heap would leave this
 * flow's writes standing in the baseline with nothing left that could take them back out. The switch-out
 * (cow_unapply + cow_set_current(NULL)) is what makes that true and this asserts it.
 *
 * WHAT IT UNAPPLIES IS EXACTLY WHAT IT IS ABOUT TO FREE, which is the half a whole-engine park never needed.
 * `g_cow_installed` is not a counted reference, so a segment can be the one the heap is SHOWING and still be
 * held by nobody but this delta — which is every finishing flow. The heap is walked back down to the deepest
 * segment that SURVIVES the release, no further: a segment a sibling still holds stays applied, so releasing a
 * PARKED flow (an evicted tail, a foreign world's segment) costs nothing and — the part that matters — cannot
 * revert the RUNNING flow's heap out from under it, which a blanket revert-to-baseline silently did. */
void      cow_delta_release(JSContext *ctx, CowDelta *d);

/* Fork a delta at a branch: freeze src's HEAD into a shared immutable base segment (refcount 2) that both src
   and the returned sibling reference, so the sibling inherits src's branch-point state in O(1) — NOT a copy —
   then each diverges on its own head. The DOM twin is dom_cow_fork. Pairs with JS_FlowClone.
   This is what it does NOW; it said so while COPYING every entry and cloning every async blob, and the swap
   below then replayed that whole copied history on every context switch.
   WHERE THE BRANCH-POINT VALUES COME FROM depends on whether `src` is the delta the heap is currently showing,
   and this asks rather than making the caller say. For the RUNNING flow's delta they are in the LIVE HEAP and
   the unapply is what fetches them; for a PARKED one they are already in the entries (cow_unapply put them
   there when the flow was switched out), so the freeze is pure bookkeeping that must touch neither the heap nor
   the installed chain — the heap belongs to whichever flow is running, and replaying a parked delta's entries
   into it installs another timeline's writes under the running one with nothing to unapply them.
   It is one operation because it is one question about one fact cow.c already holds. The world registry
   materializes a foreign world's segment by forking an ancestor, and whether that ancestor happens to be
   applied depends on which host is asking; a caller that answered it wrong would corrupt both deltas silently. */
CowDelta *cow_delta_fork(JSContext *ctx, CowDelta *src);

/* HAS ANYTHING BEEN WRITTEN INTO THIS DELTA — head AND frozen chain, since a delta that forked a non-empty
   parent holds its writes in the chain and nothing in its head. The cross-instance seam asks it of a FOREIGN
   WORLD's segment: that segment is empty exactly when the world has written nothing in this instance, which is
   the one case in which conjoining that world with a local flow's timeline is the local flow's timeline. */
bool      cow_delta_empty(const CowDelta *d);

/* Route captures to `d` (the flow about to run). NULL = captures are dropped (baseline setup). */
void      cow_set_current(CowDelta *d);
/* …AND WHAT IS ROUTED NOW, for the ONE boundary that has to take the route off and put back what it found —
   engine_sched_step, around the return to the host. Not a question anybody else may ask: "which flow is
   running" is flow_running()'s to answer, and a second reader of this would be that question re-spelled. */
CowDelta *cow_current(void);

/* Install as JSTimeTravelHooks.prop_write: called before a write to a baseline object; appends to the current
   delta.
   A DELETE IS ONE OF THOSE WRITES — delete_property calls this before it removes the slot, and the entry
   therefore has to be able to say that the flow does NOT have the slot. It records a PAIR on each side (does
   the slot exist, and only then its §6.2.6 DESCRIPTOR — the value or the accessor pair, and the C/W/E bits):
   the baseline's read here, the flow's read at unapply. Without the flow's absent case a deletion came back as
   `obj.x = undefined`, so the property returned inside the flow that deleted it and two flows disagreed about
   the shape of shared state; without the ATTRIBUTES a flow's `Object.freeze` on shared state could not be
   widened back and a slot the flow created non-configurable could not be removed at all. See CowEntry in
   cow.c. */
void      cow_capture_hook(JSContext *ctx, JSValueConst obj, JSAtom prop);

/* Install as JSTimeTravelHooks.cell_write: called before a write to a shared CLOSURE CELL; captures it into the
   current delta so a snapshot-forked sibling that shares the cell stays isolated on write. */
void      cow_capture_varref(JSContext *ctx, void *vref);

/* Install as JSTimeTravelHooks.arr_append: O(1) capture of a KNOWN-NEW fast-array append slot (no dedup scan, no
   baseline lookup — an append is always a fresh existed=0 slot). The accumulator hot path. */
void      cow_capture_arr_append(JSContext *ctx, JSValueConst obj, JSAtom atom);

/* Install as JSTimeTravelHooks.buf_state: capture a shared ARRAY BUFFER's whole STORAGE STATE before this flow
   changes any of it — any of its BYTES, or the EXTENT they live in (`resize`/`grow`, `transfer`, a detach).
   `abuf` is the ArrayBuffer/SharedArrayBuffer OBJECT.
 *
 * THE UNIT IS THE BUFFER'S BYTES, NOT A VIEW'S ELEMENT, and that is decided by the code rather than by taste.
 * A typed array's elements are raw bytes in an ArrayBuffer, so `ta[i] = v` reached no property hook and no
 * element hook and was captured by nothing at all; the same is true of fill, copyWithin, set's memmove, sort's
 * writeback, reverse, DataView's setters and Atomics. Three things rule the element out as the entry:
 *   - a DATAVIEW write has no element. `dv.setFloat64(3, x)` writes bytes 3..10, a DataView exposes no indexed
 *     properties, and those bytes cross the element boundary of every view over the buffer, so an (object,
 *     index) entry cannot name the write at all;
 *   - a JSValue cannot CARRY the bytes back. Under JS_NAN_BOXING — every 32-bit build, and the wasm one is
 *     32-bit — __JS_NewFloat64 normalises a NaN, so a Float64Array element with a payload comes back canonical
 *     and the resume is not byte-identical, which §Time-travel's razor makes a cap;
 *   - the builtins write with memmove/memset, where an entry per element costs sizeof(CowEntry) per BYTE.
 * Aliasing then needs no handling: a Float64Array and a Uint8Array over one buffer ARE one entry, because they
 * are one storage. The entry names the BUFFER OBJECT and never its data pointer, which is why it cannot be the
 * existing COW_STATE_HOST byte arm: that one holds a raw `target` into a record its owner never moves, while an
 * ArrayBuffer's storage is freed by a detach and reallocated by a resize.
 * ONE entry per buffer per flow, holding the storage as this flow FIRST found it — so a loop overwriting one
 * element a million times costs one entry and the delta stays O(shared state touched), which an undo log of
 * ranges would not. A DETACHED buffer is skipped, and that is a positive statement: no operation in §25.1
 * re-attaches one, so its state is terminal and a swap has nothing to put back.
 *
 * THE EXTENT IS IN THE SAME ENTRY AS THE BYTES, AND IT WAS IN NO ENTRY AT ALL. This held `a_len` bytes read off
 * the buffer, so a `resize`/`grow` left it describing a length the buffer no longer had and a `transfer`/detach
 * left it naming freed storage; a separate mutation hook existed for exactly those three operations and its
 * host arm could only ABORT. Two orderings, one silent: write-then-resize reached a save-side check, while
 * resize-then-write created the entry AFTER the mutation over the post-resize bytes, so nothing ever disagreed
 * with itself and the sibling inherited both the new size and the write. A buffer EMPTY at first touch is that
 * ordering with no alternative, which is why the `len == 0` skip that used to stand here is gone.
 * GIVING THE EXTENT A SECOND ENTRY IS NOT THE FIX, and that is the part worth keeping. Two entries over one
 * buffer have an ORDER in the head, and apply replays forward: a flow that resized and then wrote gets its
 * bytes replayed before its length, into an extent they do not fit. So the blob holds the whole storage state —
 * the bytes, the byte length, the detached bit and the one per-view window a resize does not re-derive — the
 * two capture points become one capture, and a flow that reaches any part of a buffer's storage may write any
 * other. That is the same argument cow_capture_obj_state makes for its three fields, and the blob is the
 * engine's (JS_BufferStateSave) for the same reason: it must allocate and free real storage, and it holds a
 * counted reference on each of the views whose cached window a resize does not re-derive — which is
 * length-tracking DataViews and nothing else. */
void      cow_capture_buffer(JSContext *ctx, JSValueConst abuf);

/* Install as JSTimeTravelHooks.map_add: O(1) capture of a KNOWN-NEW Set/Map record (Set.add / Map.set of a fresh
   key on a shared collection). unapply deletes the flow's added record (JS_MapDeleteRecord), apply re-adds it
   (JS_MapAddRecord) — the Set/Map accumulator analogue of cow_capture_arr_append. */
void      cow_capture_map_add(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst val);

/* Install as JSTimeTravelHooks.map_mutate: capture a reversible OVERWRITE / DELETE of an existing Set/Map record
   as an undo-log entry (apply replays it, unapply inverts it), completing cow_capture_map_add's add-only capture
   so ALL shared Set/Map mutations are per-flow isolated. op is JS_MAP_MUTATE_OVERWRITE / _DELETE.
   `pos` is WHERE the inverse must put a deleted record back — its POSITION is part of its state, because a
   Set/Map iterates in insertion order and that order is observable. JS_MAP_POS_TAIL for an OVERWRITE, whose
   inverse creates nothing. */
void      cow_capture_map_mutate(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst old_val, JSValueConst val, int op, int pos);
/* Install as JSTimeTravelHooks.obj_state: capture a shared object's OWN state — its EXTENSIBLE bit, its
   PROTOTYPE, and its [[PrimitiveValue]]/[[DateValue]]/[[ErrorData]] internal slot — before this flow changes
   any of them. None is a property slot and none is a class opaque record, so no other capture here could see
   them: a flow's `Object.freeze(sharedCfg)`, `Object.setPrototypeOf(shared, x)` or `d.setHours(0)` on a
   baseline Date stood for every sibling, permanently. The prototype is not only fidelity — §@S names
   PROTOTYPE-POLLUTION as a gadget class this engine solves BY RUNNING it, which needs the arm that pollutes to
   be isolated from the arm that must not see it. ONE capture for the three because they are one object.
   The blob is engine-owned (JS_ObjStateSave/Restore/Free): two of the three fields are engine-internal and
   each holds a reference, so a byte copy would be the uncounted-reference bug cow_capture_host_state warns of. */
void      cow_capture_obj_state(JSContext *ctx, JSValueConst obj);

/* Install as JSTimeTravelHooks.async_state: capture a shared promise's settlement (state + result + pending
   reactions) or a resolving-function pair's already_resolved latch before this flow changes it, so each arm of
   a fork settles a pre-fork promise on its OWN timeline. */
void      cow_capture_async_state(JSContext *ctx, JSValueConst obj);

/* Install as JSTimeTravelHooks.module_eval: capture a MODULE record's evaluation state (status + capability +
   cycle fields) before this flow changes it. Its BINDINGS are closure cells cell_write already captures; this is
   the state that decides whether a flow evaluates the module at all, so without it the first flow to import a
   chunk left it EVALUATED for every sibling and the siblings read its exports as TDZ. */
void      cow_capture_module_eval(JSContext *ctx, void *mod);

/* Capture a BROWSER COMPONENT's own mutable C record (`n` bytes at `p`) before this flow changes it. A component
   that keeps state in its class opaque writes it where no property hook and no engine hook can see, so that write
   does not time-travel: Response's body-used latch, set by whichever arm of a fork read the reply first, left the
   reply consumed for every sibling and the sibling's own first read threw. Called by the component at its
   mutation point, exactly as a DOM write host-edge calls dom_attr_capture — the browser owns the API, the solver
   owns the time travel. `owner` is the JS object the record belongs to and is HELD for the life of the entry,
   because `p` points into storage that object owns and a parked flow can outlive every other reference to it.

   THE ADDRESS IS THE CALLER'S — see THE SITE TRAVELS WITH THE OPERATION below, which governs all three of these
   entries. This one's asserts name a component's own capture call (`n` is not a scalar, `owner` is not an
   object), and a component is exactly what a line inside cow.c cannot name. */
void      cow_capture_host_state_at(JSContext *ctx, JSValueConst owner, void *p, size_t n,
                                    const char *file, int line);
#define cow_capture_host_state(ctx_, owner_, p_, n_) \
    cow_capture_host_state_at((ctx_), (owner_), (p_), (n_), __FILE__, __LINE__)

/* A COMPONENT RECORD THAT OWNS JSValues — the same isolation, for the state that is not a latch.
 *
 * cow_capture_host_state copies BYTES, which is right for a body-used flag and WRONG for a slot: a byte copy of
 * a JSValue makes a second reference and counts none of it, so the next restore frees a value the blob still
 * names and the one after that reads freed memory. Streams are made of such records — §4.2's stream holds its
 * reader, its queue and its stored error; §4.3's reader holds the stream it locked and the resolving functions
 * of its `closed` promise — and none of it was captured, so a flow that acquired a reader held it for every
 * sibling. `res.clone()` becoming a tee is what made that reachable: a body is a stream now, and two arms of a
 * fork reading one reply each took a reader on the same branch. It surfaced as "this reader was released while
 * it was still in use" and as a body read back with bytes that were not its own.
 *
 * `val_off` names the record's owned-value offsets — the same list the component's gc_mark walks, and for the
 * same reason: it is the one statement of what the record OWNS, and a field added to one and not the other is
 * the bug this exists to prevent.
 *
 * NO ctx: the call sites are a component's record ACCESSORS, which is where this belongs — a record captured
 * when the flow REACHES it cannot be missed at a write site, and there is no write site left to forget. Those
 * accessors take only the object, so the context is the session's, stashed by cow_set_ctx exactly as the DOM
 * delta's already is. */
typedef struct { size_t size; const uint16_t *val_off; int n_val; } CowRecord;
void      cow_capture_host_record_at(JSValueConst owner, void *p, const CowRecord *rec,
                                     const char *file, int line);
#define cow_capture_host_record(owner_, p_, rec_) \
    cow_capture_host_record_at((owner_), (p_), (rec_), __FILE__, __LINE__)

/* AND THE WRITE, WHICH THE LAYOUT HAD NO OPERATION FOR — PUBLISH BEFORE RELEASE.
 *
 * `val_off` is the one statement of what a record OWNS and it has FOUR consumers, not three: the finalizer
 * frees it, the gc_mark walks it, the capture above dups it, and something WRITES it. Only the write had no
 * operation, so every component spelled it by hand as `JS_FreeValue(ctx, d->f); d->f = <build a new one>;` —
 * and that order leaves the slot naming FREED STORAGE for the whole of the build.
 *
 * IT IS NOT A NARROW WINDOW, AND THAT IS THE WHOLE POINT: an object allocation IS a collection
 * (JS_NewObjectFromShape calls js_trigger_gc), so `d->f = JS_NewArray(ctx)` collects with the slot dangling,
 * the collector reaches the record through the component's own gc_mark — which walks exactly this list — and
 * decrefs a JSObject whose storage is already back on the allocator's free list. quickjs asserts the child's
 * refcount at that decrement, which is the ONLY reason the defect has a symptom rather than a silent
 * corruption; the assert names quickjs's collector and says nothing about the component that dangled the slot.
 * The release of the OLD value is the same hazard from the other side: a host class's finalizer is the page's
 * platform code, and it may allocate.
 * quickjs's own name for the correct order is `set_value`, which is `static inline` inside quickjs.c and so
 * cannot be reached by a component under engine/host/browser. This is that operation, plus the one assert the
 * layout makes possible: the slot written must be a slot the layout NAMES, so a field added to the struct and
 * not to `*_VALS[]` crashes at its FIRST write rather than being missed by the walk and the finalizer both.
 * A record's INITIALIZATION is not a write and must not come here — before JS_SetOpaque the collector cannot
 * reach the record and there is no previous value to release.
 *
 * THE SITE TRAVELS WITH THE OPERATION, AND IT GOVERNS THE THREE ENTRIES ABOVE AS WELL AS THIS ONE.
 * A DCHECK stamps the file and line it is WRITTEN at, so an invariant checked INSIDE a shared helper reports
 * THE HELPER for every caller — and the three asserts below are all about the SLOT, whose only wrong value is
 * one a caller passed. Written as a plain function they said "add the field's offsetof to the record's layout"
 * and named ONE line in cow.c for every component in the tree, which is a correct instruction with no object:
 * a reader who tries to obey it has nowhere to apply it, and the crash is rediscovered instead of fixed.
 * So the caller's `__FILE__`/`__LINE__` are captured HERE, at the expansion, and threaded to the check, which
 * is why each of these is a macro over an `_at` function rather than a function. It is the same mechanism
 * core/html/sanitizer.c's san_require_known_* pair uses, and for the same reason.
 * AND AN INTERMEDIATE MUST NOT ABSORB THE ADDRESS. A component that wraps this in a plain function of its own
 * — binding its record and layout once, which is worth doing — becomes the new single line the abort names,
 * for every write in that file: the defect one level down, and invisible, because the wrapper supplies its own
 * `__FILE__`/`__LINE__` and cannot be told from a converted caller. Every such wrapper therefore takes
 * `file`/`line` and forwards them, behind a macro of its own (`fr_set` over `fr_set_at`), or is a macro
 * outright (core/css/css_rule.c's `rule_set`, which expands this one at the leaf and needs nothing else).
 * A wrapper written as a plain function is an unconverted caller, not a supported spelling. */
void      cow_record_set_at(JSContext *ctx, void *p, const CowRecord *rec, JSValue *slot, JSValue v,
                            const char *file, int line);
#define cow_record_set(ctx_, p_, rec_, slot_, v_) \
    cow_record_set_at((ctx_), (p_), (rec_), (slot_), (v_), __FILE__, __LINE__)

/* The session's context, for the captures that have no call-site one. The DOM delta's twin is dom_cow_set_ctx. */
void      cow_set_ctx(JSContext *ctx);

/* THE SCHEDULER'S OWN BOOKKEEPING IS NOT A FLOW'S WRITE, and this is what says so at the write.
 *
 * The flows' pending register (solver/pending.h) is a JS object graph the SCHEDULER owns: it records what the
 * host still owes ONE flow. The host reads EVERY flow's register from outside any flow's delta — engine_provide
 * fills whichever flows parked on a REQUEST, engine_host_requests joins what is outstanding across all of them, and
 * the preempt hook asks whether the running flow is blocked. If a delta captured those writes, an entry's
 * contents would depend on which delta happened to be applied: an entry appended after a fork would be
 * TRUNCATED away the moment a sibling switched in, and the reply would be delivered into a slot that no longer
 * exists. That is not an isolation question — the register is not state a page can observe — so it is answered
 * where the write is made rather than by a generational accident that a later fork invalidates.
 * Nestable, and asserted balanced: an unclosed bracket would silently stop capturing the PAGE's writes. */
void      cow_engine_write_begin(void);
void      cow_engine_write_end(void);

/* Install as JSTimeTravelHooks.async_fork: record the per-flow swap of a shared suspended ASYNC activation. The
   engine cloned it because resuming CONSUMES it; this makes the clone what this flow's resolve/reject closure
   names, leaving the original as the baseline every other arm still finds. */
void      cow_capture_async_fork(JSContext *ctx, JSValueConst closure, void *base_data, void *cur_data);

/* Record a per-flow GENERATOR-STATE swap into delta `d` (JSTimeTravelHooks.gen_fork): the shared generator
   object `genobj` must resolve to `cur_gd` (a per-flow clone `d` OWNS) while `d` runs and to `base_gd` (its
   object-owned original) otherwise. Applied/unapplied like any other slot on context-switch. Dedup-REPLACES an
   existing gendata entry for `genobj` (a re-fork of the same generator inside this flow), keeping the original
   base. `d` takes ownership of one ref on `cur_gd` (freed via JS_GenDataUnref on delta free/replace). */
void      cow_delta_add_gendata(JSContext *ctx, CowDelta *d, JSValueConst genobj, void *base_gd, void *cur_gd);

/* Context-switch AWAY from `d`: save each HEAD slot's current (flow) value and restore what the chain below it
   holds. The shared base chain is left APPLIED — the incoming flow's cow_apply moves it only as far as the two
   flows actually diverge, so a switch costs what they differ by rather than everything either has written. */
void      cow_unapply(JSContext *ctx, CowDelta *d);

/* Context-switch INTO `d`: move the installed base chain to this flow's (touching only the segments above the
   two chains' lowest common one), then replay its head on top. After this the heap shows exactly what this flow
   last saw. */
void      cow_apply(JSContext *ctx, CowDelta *d);

/* INSTALL THE TIME-TRAVEL RECORD BOUNDARY — the per-flow COW capture set, declared once for the reason the
   concolic set is: two entries each spelled it out as a struct literal, which is a list that can drift, and one
   of them already had. `.gen_fork` is the scheduler's, which is why this lives with the capture hooks that make
   up the rest of it rather than at either entry. */
/* `gen_fork` is the caller's: the nine other hooks are this file's capture points, and the tenth belongs to
   whoever assembles the sibling flow. See the definition. */
void cow_install_time_travel_hooks(JSTimeTravelGenFork gen_fork);

/* What the delta swaps have cost so far: how many chain installs, how many entries they touched in total, and
   the worst single one. A switch is supposed to be O(divergence); this says what the divergence actually is. */
void cow_swap_stats(long *count, long *total, long *max);

/* WHAT THE CHAIN IS HOLDING RIGHT NOW — the frozen segments still referenced and the entries in them. The swap
   counters above say what a SWITCH costs and say nothing about what is RETAINED, so a run whose frontier is
   four flows while its allocator holds gigabytes had no counter that could tell a chain nobody released from
   memory belonging somewhere else entirely. A segment is freed when its last holder drops it, so this number
   falling to nothing as flows finish is the whole claim that the structural sharing is also a lifetime. */
void cow_chain_stats(long *segs, long *entries);
/* …AND WHAT IT COSTS IN BYTES. The pair above counts segments and entries, which is the right unit for "is the
   sharing working"; the cold tier needs the other unit, because what it pages is bytes. Asked of this file
   rather than multiplied out by the caller: `sizeof(CowEntry)` is private, and a caller that guessed it would
   report a number that drifts the next time an entry kind is added. */
long cow_chain_bytes(void);

/* ONE FLOW'S OWN HEAD — the writes it has made since its last fork, which is the part of the heap delta that
   is NOT shared with any sibling and therefore the part a pager pays for once per parked flow. `bytes` is the
   delta's whole host allocation (the struct, its entry array at capacity, and its hash index). */
void cow_delta_head_stats(const CowDelta *d, long *entries, long *bytes);

#endif
