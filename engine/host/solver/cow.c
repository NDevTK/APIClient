/* Per-flow swappable COW delta — see cow.h. */
#include "solver/cow.h"
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* ONE CAPTURED SLOT IS TWO STATES OF IT, AND A STATE IS A PAIR: does the slot EXIST, and — only then — what
   DESCRIPTOR does it hold. The baseline's pair is (existed, base), read once at capture and restored by unapply;
   the running flow's is (cur_state, cur), read at unapply and replayed by apply. The round trip IS those two
   pairs, which is why each is asserted where it is read.
   THE STATE IS A §6.2.6 PROPERTY DESCRIPTOR AND NOT A VALUE, which is what it used to be. A slot is
   {[[Value]],[[Writable]]} XOR {[[Get]],[[Set]]}, both carrying [[Enumerable]] and [[Configurable]], and an
   entry that held only the value could express NONE of the rest: a flow's `Object.freeze(shared)` came back
   still frozen because the unapply had no attributes to widen; a flow's `Object.defineProperty(shared,"x",{})`
   — whose C/W/E all default to false — created a slot no delete could remove, so the flow's own creation stood
   in the baseline for every sibling; and an accessor slot could only ABORT, on both sides, because half a
   record cannot say which shape a slot is in. It is JSPropertyDescriptor, the engine's own record for exactly
   this, so the read fills one and the write consumes one with nothing packed or unpacked in between; the
   accessor bit inside `flags` IS the kind field, so there is no second `is_accessor` spelling to drift from it.
   THE FLOW'S HALF OF THE PAIR HAD NO ABSENT CASE, and `cur` alone cannot spell one. So a flow's own
   `delete obj.x` unapplied correctly — the baseline value came back — and then replayed on apply as
   `obj.x = undefined`: the property came back with it, `'x' in obj` answered true inside the very flow that
   deleted it, and two flows disagreed about the SHAPE of shared state, which is the isolation this file exists
   to provide. The same hole swallowed every capture whose write never landed (the hook runs BEFORE the write,
   and delete_property's runs before it even knows the slot is there), each of which apply turned into a
   property nobody ever created.
   One field with three states, so "recorded, but neither present nor absent" cannot be written down:
     COW_CUR_UNRECORDED — this flow has not been switched out since the capture, so there is nothing to replay.
     COW_CUR_ABSENT     — the flow does NOT own the slot. Apply DELETES it; `cur` is undefined and carries
                          nothing, which the DCHECK at the recording point holds it to.
     COW_CUR_PRESENT    — the flow owns the slot with value `cur`, which apply writes.
   The baseline's pair needs no UNRECORDED: a capture reads it before it returns. */
enum { COW_CUR_UNRECORDED = 0, COW_CUR_ABSENT = 1, COW_CUR_PRESENT = 2 };
/* A delta entry is EITHER a property slot (vref==NULL: obj/atom/existed) OR a closure cell (vref!=NULL: the
   shared JSVarRef, whose value is read/written via JS_VarRefGet/SetValue; obj/atom/existed unused). base/cur
   hold the baseline/flow states either way — a CELL is not a property, so only their `.value` is its state and
   the attributes stay zero (the cell arms assert that). A cell has no ABSENT state — it holds a value or does
   not exist at all — so its cur_state is only ever PRESENT, and apply asserts that rather than assuming it. */
/* A THIRD entry kind (is_gendata) swaps a shared COROUTINE object's execution-state pointer per flow: obj is
   the object, g0 its original state, g1 the per-flow clone (owned by this delta). Apply installs g1, unapply
   restores g0 — a fixed toggle, no cur/base value tracking. TWO OBJECT KINDS share it because they are one
   concept — "which activation does this shared object name while I run" — and differ only in the setter: a
   GENERATOR object (is_gendata=1) and an async resolve/reject CLOSURE (is_gendata=2), whose activation is
   consumed by being resumed. */
/* A FOURTH entry kind (is_map=1) is a reversible Set/Map record mutation by this flow (an UNDO-LOG entry, not a
   deduped slot): obj is the Set/Map, base.value holds the record's key, cur.value its new value, map_old its
   prior value — a record is a key and a value and has no attributes, so the descriptors' other fields stay zero.
   map_op selects the inverse pair — ADD (apply re-adds, unapply deletes), OVERWRITE (apply sets cur, unapply
   restores map_old), DELETE (apply deletes, unapply re-adds map_old). Entries replay forward on apply and invert
   in reverse on unapply, so a sequence of mutations to the same key reconstructs correctly with NO dedup and no
   SameValueZero key-hashing. Never slot-keyed (kept out of the hash index, like gendata).
   map_pos is WHERE a re-added record goes, and it is the fourth thing a record's state is made of: a Set/Map
   iterates in INSERTION order and that order is observable, so a DELETE whose inverse APPENDS rebuilds a
   different collection — `m.delete('a')` on ['a','b','c'] unapplied to ['b','c','a'], and a clear() came back
   reversed. A DELETE carries the position it was removed from; an ADD and an OVERWRITE carry JS_MAP_POS_TAIL,
   which is the append an add replays and the nothing an overwrite places (its record is already there). */
/* A FIFTH entry kind (is_state=1) is an opaque STATE BLOB over internal fields no property hook can see:
   a_base is the baseline state this flow found, a_cur the state this flow produced (saved at unapply, replayed
   at apply) — the same base/cur shape as a property slot. THREE TARGETS share it because they are one concept,
   "the state of this thing before this flow changed it", and `state_kind` says which:
     COW_STATE_ASYNC  — a shared ASYNC object's settlement (obj is the promise, or the resolving function whose
                        already_resolved latch it is; target unused).
     COW_STATE_MODULE — a MODULE record's EVALUATION state (target is the record; obj unused, a module is not a
                        JSObject). Its bindings are closure cells cell_write already captures; what was missing
                        is the state deciding whether a flow evaluates AT ALL, so the first flow to import a
                        chunk left it EVALUATED for every sibling and the siblings read TDZ exports.
     COW_STATE_HOST   — a BROWSER COMPONENT's own mutable C record (target is the bytes, a_len their count; obj
                        is the JS object that owns them, held so the storage outlives this delta). A component
                        that keeps state in its class opaque — Response's body-used latch is the first — writes
                        it where no property hook can see, so one arm of a fork consumed the reply for its
                        sibling and the sibling's own read threw. The field is not a JSValue and there is no
                        engine call that saves it, so the blob IS a byte copy; everything else about the entry
                        is what the other two targets already do.
   `state_kind` rather than a nullness test on the target: a third target cannot be told from the second by
   asking whether a pointer is set, and a fourth could not be told from any of them.
   Not slot-keyed (kept out of the hash index, like gendata and map): each is captured once per flow. */
#define COW_MAP_ADD       0
#define COW_MAP_OVERWRITE 1
#define COW_MAP_DELETE    2
/*   COW_STATE_HOST_REC — a component record that OWNS JSValues (target is the record, `rec` its layout). The
 *                        byte arm above cannot serve it: a memcpy of a JSValue makes a reference it does not
 *                        count, so the next restore frees a value the blob still names. See cow.h.
 *   COW_STATE_BUFFER   — an ARRAY BUFFER's BYTES (obj is the buffer object, a_len its byte length; target
 *                        unused). A typed array's elements are not JSValues and not slots — they are raw bytes
 *                        in an ArrayBuffer — so `ta[i] = v`, fill, copyWithin, set, sort, reverse, DataView's
 *                        setters and Atomics all wrote where no capture could see them. It is not the HOST byte
 *                        arm above even though both copy bytes: that one holds a raw `target` pointer into a
 *                        record whose owner never moves it, while an ArrayBuffer's storage is FREED by a detach
 *                        and REALLOCATED by a resize, so this entry names the buffer OBJECT and asks it for the
 *                        bytes at each save and restore (JS_GetBufferBytes / JS_SetBufferBytes). Why the unit is
 *                        the bytes and not the element is in cow.h. */
enum { COW_STATE_ASYNC = 0, COW_STATE_MODULE = 1, COW_STATE_HOST = 2, COW_STATE_HOST_REC = 3,
       COW_STATE_BUFFER = 4 };
typedef struct { JSValue obj; JSAtom atom; int existed; JSPropertyDescriptor base; JSPropertyDescriptor cur;
                 int cur_state; void *vref;
                 int is_gendata; void *g0; void *g1; int is_map; int map_op; int map_pos; JSValue map_old;
                 int is_state; int state_kind; void *target; size_t a_len; void *a_base; void *a_cur;
                 const CowRecord *rec; } CowEntry;

/* EVERY FIELD OF A NEW ENTRY, IN ONE PLACE. Eleven producers each spelled the whole struct out, which is a list
   that drifts exactly as §Architecture's hand-copied `_install` lines do: a field added to the struct is a field
   ten of them do not mention, and the one that forgets it hands the swap a value nobody wrote — a JSValue read
   as whatever the last entry at that address held. Each producer now writes only the fields ITS KIND owns, and
   what a kind leaves alone is a positive statement (a cell has no attributes; a map record has no atom) rather
   than a hole. */
static void cow_entry_init(CowEntry *e) {
    memset(e, 0, sizeof *e);
    e->obj = JS_UNDEFINED;
    e->atom = JS_ATOM_NULL;
    e->base.value = e->base.getter = e->base.setter = JS_UNDEFINED;
    e->cur.value = e->cur.getter = e->cur.setter = JS_UNDEFINED;
    e->map_old = JS_UNDEFINED;
    e->map_pos = JS_MAP_POS_TAIL;   /* an add appends; only a DELETE's inverse has a place to put a record back */
    e->cur_state = COW_CUR_UNRECORDED;
    e->state_kind = COW_STATE_ASYNC;
}
/* ONE STATE, DUPLICATED — a restore hands the slot write a descriptor it CONSUMES, and the entry must still
   hold its own for the next switch. Every owned half is dup'd; which halves are live is the flags' business
   and not this function's, because an absent half is JS_UNDEFINED and a dup of that is free. */
static JSPropertyDescriptor cow_desc_dup(JSContext *ctx, const JSPropertyDescriptor *d) {
    JSPropertyDescriptor c;
    c.flags = d->flags;
    c.value = JS_DupValue(ctx, d->value);
    c.getter = JS_DupValue(ctx, d->getter);
    c.setter = JS_DupValue(ctx, d->setter);
    return c;
}
static void cow_desc_free(JSContext *ctx, JSPropertyDescriptor *d) {
    JS_FreeValue(ctx, d->value);
    JS_FreeValue(ctx, d->getter);
    JS_FreeValue(ctx, d->setter);
    d->flags = 0;
    d->value = d->getter = d->setter = JS_UNDEFINED;
}

/* forked = has this flow snapshot-forked a sibling yet? Until it has, every object it CREATED (flow_local) is
   truly private — no other flow can observe it — so capturing its mutations is pure waste (keeps the delta
   O(shared baseline touched), not O(the run's transients). Once forked, the snapshot shares the frame's
   flow_local objects with the sibling, so their mutations become cross-flow state and MUST be captured. */
/* fork_gen = the fork GENERATION this flow last forked at (0 = never forked). An object is SHARED with a sibling
   iff it existed at that fork (JS_ObjFlowGen(obj) <= fork_gen); an object created AFTER (flow_gen > fork_gen) is
   flow-PRIVATE — never captured, keeping the delta O(shared-state-touched), not O(the run's transients). A single
   "forked" bit could not distinguish a pre-fork shared object from a post-fork private one, so it over-captured
   every post-fork transient (a delta bloat that violates the O(shared-state) invariant). */
/* hash/hash_cap: an open-addressing index over the entries (slot key -> entry index +1; 0 = empty) so a capture
   dedups in O(1). Without it the dedup was an O(n) linear scan — O(n^2) to build a delta that touches n distinct
   shared slots (a flow mutating many shared globals/objects). Rebuilt on entry-array grow and on fork. */
/* THE HEAP'S BASE CHAIN — a mutable HEAD over IMMUTABLE, refcounted, structurally-shared segments, which is
   what the DOM half has had all along and what this file's header has claimed all along without doing it: the
   fork COPIED every entry, duplicating each value and CLONING each async blob, so N sibling flows each carried
   the whole history and every switch between them replayed all of it. A frozen segment is never written again,
   so two flows that reach the same segment agree about everything from there down and neither the fork nor the
   swap has to look at it. */
typedef struct CowSeg { CowEntry *e; int n; struct CowSeg *base; int refcount; } CowSeg;
struct CowDelta { CowEntry *e; int n, cap; uint32_t fork_gen; int *hash; int hash_cap; CowSeg *base; };

/* THE CHAIN CURRENTLY APPLIED TO THE HEAP — a property of the heap, not of any flow, exactly as the DOM's is of
   the document. A switch moves it to the incoming flow's chain by walking to the two chains' lowest common
   segment and touching only what lies above; the shared suffix is left alone. */
static CowSeg *g_cow_installed = NULL;
/* WHAT THE CHAIN IS HOLDING — see cow.h. Counted at the two points a segment's lifetime begins and ends (the
   fork that freezes one, and the unref that frees it), so the pair cannot drift from the thing it counts. */
static long g_seg_live, g_seg_entries_live;
void cow_chain_stats(long *segs, long *entries) {
    if (segs) *segs = g_seg_live;
    if (entries) *entries = g_seg_entries_live;
}
/* The same two numbers in the unit the cold tier pages in — see cow.h. A frozen segment's entry array is
   allocated at exactly its entry count (the head's array is handed over whole at the freeze), so the entries
   are the array and there is no capacity slack to account for. */
long cow_chain_bytes(void) {
    return g_seg_live * (long)sizeof(CowSeg) + g_seg_entries_live * (long)sizeof(CowEntry);
}

void cow_delta_head_stats(const CowDelta *d, long *entries, long *bytes) {
    if (entries) *entries = d ? d->n : 0;
    if (bytes) *bytes = d ? (long)sizeof *d + (long)d->cap * (long)sizeof(CowEntry)
                                           + (long)d->hash_cap * (long)sizeof(int)
                          : 0;
}
static void cow_install_chain(JSContext *ctx, CowSeg *want);   /* defined with the rest of the chain walk */

static uint32_t cow_slot_hash(void *p, uint32_t atom) {
    uint64_t h = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull ^ ((uint64_t)atom * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)(h ^ (h >> 32));
}
/* The slot key: a closure cell keys on its vref; a property slot on (obj pointer, atom). */
static void cow_key(const CowEntry *e, void **key, uint32_t *atom) {
    if (e->vref) { *key = e->vref; *atom = 0; }
    else { *key = JS_VALUE_GET_PTR(e->obj); *atom = e->atom; }
}
static void cow_hash_put(CowDelta *d, int idx) {   /* insert entry idx; caller guarantees room */
    void *key; uint32_t atom; cow_key(&d->e[idx], &key, &atom);
    uint32_t m = (uint32_t)d->hash_cap - 1, h = cow_slot_hash(key, atom) & m;
    while (d->hash[h]) h = (h + 1) & m;
    d->hash[h] = idx + 1;
}
static void cow_hash_rebuild(CowDelta *d) {   /* size to >= 2*n (power of two), re-insert every entry */
    /* SIZED IN A LOCAL AND PUBLISHED AFTER — same rule as cow_room_for_one below, and here it is the INDEX
       rather than the entries: a `hash_cap` advertising the new size over the old, smaller table makes every
       cow_hash_find that ran during the sale read past its end. */
    int nc = 16;
    int *nh;
    while (nc < d->n * 2) nc *= 2;
    nh = reclaim_realloc(d->hash, (size_t)nc * sizeof(int));
    CHECK(nh, "cow: OOM hash index");
    d->hash = nh; d->hash_cap = nc;
    memset(d->hash, 0, (size_t)d->hash_cap * sizeof(int));
    /* gendata / state / MAP entries are not slot-keyed. The map arm was missing, so a rebuild put every map
       entry into the index under (collection, JS_ATOM_NULL) — an entry the CowEntry comment says is kept out of
       it, indexed under a key no capture ever looks up. It never returned a wrong entry; it filled the table
       with rows nothing could find, which is the same statement being true in one place and false in another. */
    for (int i = 0; i < d->n; i++)
        if (!d->e[i].is_gendata && !d->e[i].is_state && !d->e[i].is_map) cow_hash_put(d, i);
}
/* Record entry (d->n-1) in the hash, growing/rebuilding the table when it would exceed half-full. */
static void cow_hash_add_last(CowDelta *d) {
    if (!d->hash || d->hash_cap < d->n * 2) cow_hash_rebuild(d);
    else cow_hash_put(d, d->n - 1);
}
/* Find an existing entry for a slot: returns its index or -1. vref!=NULL keys on the cell; else on (objptr,atom). */
static int cow_hash_find(CowDelta *d, void *objptr, uint32_t atom, void *vref) {
    if (!d->hash) return -1;
    uint32_t m = (uint32_t)d->hash_cap - 1, h = cow_slot_hash(vref ? vref : objptr, vref ? 0 : atom) & m;
    while (d->hash[h]) {
        CowEntry *e = &d->e[d->hash[h] - 1];
        if (vref ? (e->vref == vref)
                 : (e->vref == NULL && JS_VALUE_GET_PTR(e->obj) == objptr && e->atom == atom))
            return d->hash[h] - 1;
        h = (h + 1) & m;
    }
    return -1;
}

/* ROOM FOR ONE MORE ENTRY, in ONE place, and the reason it is one place is the refusal edge underneath it.
   `reclaim_realloc` can SELL A FLOW from inside the ask (solver/reclaim.h), and selling one runs this file's
   own release code, so for the whole of that call `d` must describe the buffer that ACTUALLY EXISTS: the new
   capacity is PUBLISHED ONLY AFTER the allocation returns. Doubling `d->cap` first — which is what eleven
   copies of this block did — leaves `d->e` pointing at the old, smaller array while `d->cap` advertises the
   new size, and anything that reaches `d` during the sale writes past its end. That was safe only because
   every capture entry point happens to be closed by `g_capturing`, which is an argument rather than a
   construction, and an argument the twelfth copy would not have carried.
   Answers whether the array actually GREW, because the five callers that keep a sized hash index rebuild it
   exactly then — rebuilding on every capture instead is quadratic. */
static int cow_room_for_one(CowDelta *d, const char *why) {
    int nc;
    CowEntry *ne;

    if (d->n < d->cap)
        return 0;
    nc = d->cap ? d->cap * 2 : 32;
    ne = reclaim_realloc(d->e, (size_t)nc * sizeof(CowEntry));
    /* THE EDGE MAKES THE FAILURE RECOVERABLE; THIS IS STILL THE ANSWER WHEN THERE IS NOTHING LEFT TO SELL.
       The caller's own sentence is carried through rather than replaced by one generic line, because which
       capture was lost is the whole of what a reader needs here. */
    CHECK(ne, why);
    d->e = ne; d->cap = nc;
    return 1;
}

static CowDelta *g_current = NULL;   /* the flow whose writes are captured right now (scheduler-owned) */
static int g_capturing = 0;          /* re-entrancy guard: our own reads/restores must not re-capture */

CowDelta *cow_delta_new(void) {
    CowDelta *d = reclaim_calloc(1, sizeof *d);
    CHECK(d, "cow: OOM new delta");
    return d;
}

/* The engine asks the same shared-vs-private question this delta asks (JS_IsFlowShared), so the generation
   travels with the delta: setting one without the other would have the two disagree about what is shared. */
static int       g_engine_write_depth;
static CowDelta *g_engine_write_saved;
void cow_set_current(CowDelta *d) {
    DCHECK(g_engine_write_depth == 0,
           "the running flow's delta was swapped inside an engine-bookkeeping write — the bracket would put "
           "the OUTGOING flow's delta back and the incoming flow would run with captures pointed at it");
    g_current = d; JS_SetFlowForkGen(d ? d->fork_gen : 0);
}

/* WHERE CAPTURES ARE ROUTED RIGHT NOW, so the one boundary that has to take them OFF can put back exactly what
   it found. It is deliberately the only reader: a caller that asked this in order to DECIDE something would be
   asking which flow is running, and flow_running() is the authority for that. */
CowDelta *cow_current(void) { return g_current; }

/* THE SCHEDULER'S OWN BOOKKEEPING — see cow.h. It is expressed as "there is no current delta", which is the
   statement every hook here already understands (`!g_current` is how baseline setup drops its captures), so
   nothing else has to learn about it and no hook can be added that forgets to ask. */
void cow_engine_write_begin(void) {
    if (g_engine_write_depth++ == 0) { g_engine_write_saved = g_current; g_current = NULL; }
}
void cow_engine_write_end(void) {
    DCHECK(g_engine_write_depth > 0,
           "cow_engine_write_end with no matching begin — an unbalanced bracket leaves the PAGE's writes "
           "uncaptured from here on, which is cross-flow leakage with nothing to report it");
    if (--g_engine_write_depth == 0) { g_current = g_engine_write_saved; g_engine_write_saved = NULL; }
}

/* The is_state entry's four operations, dispatched by TARGET KIND. One mechanism, three things it captures —
   the alternative was another entry kind whose unapply/apply/fork/free arms would be the same four lines again.
   The default arm asserts rather than falling through: a kind nobody wrote an arm for must abort at the
   capture, not silently restore the wrong thing on the next context switch. */
static void *cow_state_save(JSContext *ctx, const CowEntry *e) {
    switch (e->state_kind) {
    case COW_STATE_MODULE: return JS_ModuleEvalStateSave(ctx, e->target);
    case COW_STATE_HOST: {
        void *blob = reclaim_malloc(e->a_len);
        CHECK(blob, "cow: OOM saving a component's own state — a lost latch leaks one flow's read into another");
        memcpy(blob, e->target, e->a_len);
        return blob;
    }
    case COW_STATE_HOST_REC: {
        /* The bytes, then ONE REFERENCE PER OWNED VALUE. The memcpy already put the value's bits in the blob;
           the dup is what makes that a reference the blob holds rather than one it silently shares. */
        void *blob = reclaim_malloc(e->rec->size);
        int i;
        CHECK(blob, "cow: OOM saving a component's record");
        memcpy(blob, e->target, e->rec->size);
        for (i = 0; i < e->rec->n_val; i++) {
            JSValue *v = (JSValue *)((char *)blob + e->rec->val_off[i]);
            *v = JS_DupValue(ctx, *v);
        }
        return blob;
    }
    case COW_STATE_BUFFER: {
        /* THE BYTES ARE ASKED OF THE BUFFER, never read through a pointer the entry kept: a detach frees that
           storage and a resize reallocates it, which is the whole reason this is not the HOST arm above. */
        uint32_t len;
        const uint8_t *bytes = JS_GetBufferBytes(e->obj, &len);
        void *blob;
        /* THE BACKSTOP, NOT THE STATEMENT — cow_capture_buffer_lifetime refuses the detach and the resize AT
           the mutation, where neither ordering can slip past. These two said the same thing from here, and
           from here they could only see a buffer this flow had already captured: reaching either one now means
           storage moved without passing that capture point, which is a route to build rather than a resize to
           report. */
        CHECK(bytes != NULL,
              "a COW delta is saving the bytes of a DETACHED buffer, and the detach did not pass "
              "cow_capture_buffer_lifetime — route whatever freed this storage through it");
        CHECK(len == (uint32_t)e->a_len,
              "a COW delta is saving a different number of bytes than it captured, and the resize did not pass "
              "cow_capture_buffer_lifetime — route whatever moved this storage through it");
        blob = reclaim_malloc(e->a_len);
        CHECK(blob, "cow: OOM saving a buffer's bytes — a lost baseline write leaks one flow's typed-array "
                    "state into every sibling");
        memcpy(blob, bytes, e->a_len);
        return blob;
    }
    default:
        DCHECK(e->state_kind == COW_STATE_ASYNC, "a COW state entry names a target kind with no save");
        return JS_AsyncStateSave(ctx, e->obj);
    }
}
static void cow_state_restore(JSContext *ctx, const CowEntry *e, void *blob) {
    switch (e->state_kind) {
    case COW_STATE_MODULE: JS_ModuleEvalStateRestore(ctx, e->target, blob); break;
    case COW_STATE_HOST:
        /* A re-apply reads what the UNAPPLY recorded, so reaching here with nothing recorded means this flow
           was switched INTO without ever having been switched out of — the swap is not a pair. The async arm
           tolerates a NULL blob; this one must not, because a byte copy from NULL is a segfault whose cause is
           two switches away from where it lands. */
        DCHECK(blob != NULL, "a component's state was re-applied before any unapply had recorded one — the "
                             "context switch that parked this flow did not run");
        memcpy(e->target, blob, e->a_len);
        break;
    case COW_STATE_HOST_REC: {
        int i;
        DCHECK(blob != NULL, "a component's record was re-applied before any unapply had recorded one");
        /* WHAT THE RECORD HOLDS NOW GOES FIRST — it is about to be overwritten, and nothing else names it. Then
           the blob's bytes land, and each owned value is dup'd so the blob keeps the reference it saved: a
           re-apply reads the same blob again, and a blob that had handed its only reference away would be
           restoring freed values the second time. */
        for (i = 0; i < e->rec->n_val; i++)
            JS_FreeValue(ctx, *(JSValue *)((char *)e->target + e->rec->val_off[i]));
        memcpy(e->target, blob, e->rec->size);
        for (i = 0; i < e->rec->n_val; i++) {
            JSValue *v = (JSValue *)((char *)e->target + e->rec->val_off[i]);
            *v = JS_DupValue(ctx, *v);
        }
        break;
    }
    case COW_STATE_BUFFER:
        DCHECK(blob != NULL, "a buffer's bytes were re-applied before any unapply had recorded them — the "
                             "context switch that parked this flow did not run");
        JS_SetBufferBytes(e->obj, blob, (uint32_t)e->a_len);
        break;
    default:
        DCHECK(e->state_kind == COW_STATE_ASYNC, "a COW state entry names a target kind with no restore");
        JS_AsyncStateRestore(ctx, e->obj, blob);
    }
}
/* There is no CLONE arm, and that is not an omission: a fork FREEZES the running flow's head into a segment both
   arms reference, so no entry is ever copied and no blob ever duplicated. One was written here for the fork that
   did copy, and it outlived that fork by long enough to be extended for a third target before anyone noticed it
   had no caller. */
static void cow_state_free(JSRuntime *rt, const CowEntry *e, void *blob) {
    switch (e->state_kind) {
    case COW_STATE_MODULE: JS_ModuleEvalStateFree(rt, blob); break;
    case COW_STATE_HOST:
    case COW_STATE_BUFFER: free(blob); break;   /* both are POD bytes: nothing in them holds a reference */
    case COW_STATE_HOST_REC:
        if (blob) {
            int i;
            for (i = 0; i < e->rec->n_val; i++)
                JS_FreeValueRT(rt, *(JSValue *)((char *)blob + e->rec->val_off[i]));
            free(blob);
        }
        break;
    default:
        DCHECK(e->state_kind == COW_STATE_ASYNC, "a COW state entry names a target kind with no free");
        JS_AsyncStateFree(rt, blob);
    }
}

void cow_capture_hook(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    /* FLOW-PRIVATE skip — the O(shared-state) invariant. An object created AFTER this flow's fork (flow_gen >
       fork_gen) is private to the flow: no sibling can observe it, so its writes are never captured. A baseline
       object (flow_gen 0) and any object that existed at the fork (flow_gen <= fork_gen) IS shared and captured. */
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;
    if (cow_hash_find(d, JS_VALUE_GET_PTR(obj), atom, NULL) >= 0) return;   /* capture each slot ONCE (O(1)) */

    g_capturing = 1;
    JSPropertyDescriptor base;
    /* the SLOT, not [[GetOwnProperty]]: this runs inside a write hook, so a Proxy trap here would be the page's
       code with no flow base, and JS_GetOwnSlotDesc asserts that it is not reachable. An ACCESSOR is READ, not
       refused: its getter and setter are function objects the descriptor carries and neither side ever calls
       one, so a flow redefining a shared accessor is isolated like any other slot. */
    int existed = JS_GetOwnSlotDesc(ctx, &base, obj, atom) > 0;

    cow_room_for_one(d, "cow: OOM growing delta — a lost baseline write leaks state across flows");
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_DupAtom(ctx, atom);
    e->existed = existed;
    e->base = base;
    cow_hash_add_last(d);
    g_capturing = 0;
}

/* Record a CLOSURE CELL's pre-write value (JSCowVarRefHook) into the running flow's delta, so a snapshot-forked
   sibling that shares the cell is isolated on write. Captured once per cell (first write is the baseline). */
/* Record a fast-array APPEND slot (JSTimeTravelHooks.arr_append). What is KNOWN about it is its BASELINE: the
   index is at the array's dense count, so no slot is there and existed=0 without a JS_GetOwnSlotDesc. That read
   is what this saves, and it is why this is the accumulator hot path (a shared array built one element at a
   time); routing it through cow_capture_hook's baseline read on every push is what the separate hook avoids.
   IT STILL DEDUPS, and the sentence that said it "cannot already be in the delta" was false in both directions:
   `Object.defineProperty(sharedArr, "3", …)` captures the absent slot through prop_write and THEN appends
   through here, so one slot got two entries; and `a[3]='x'; a.length=3; a[3]='y'` appends the same index twice.
   Neither corrupted — two entries for one absent slot invert to the same shrink — but a `while(1){a.push(x);
   a.pop();}` loop appends the same index without bound, and an entry per iteration over ONE slot is the
   O(shared-state-touched) invariant broken. The dedup is the hash index's O(1) find, the same one every other
   capture uses. The flow-private skip is still applied (a post-fork private array — e.g. a per-flow accumulator
   built after the fork — is not captured). */
void cow_capture_arr_append(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;
    if (cow_hash_find(d, JS_VALUE_GET_PTR(obj), atom, NULL) >= 0) return;   /* capture each slot ONCE (O(1)) */
    g_capturing = 1;
    cow_room_for_one(d, "cow: OOM growing delta (arr_append)");
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_DupAtom(ctx, atom);   /* a tagged-int index atom: dup is a no-op, kept for uniform free */
    e->existed = 0;                    /* an append creates the slot; unapply shrinks the dense part past it */
    cow_hash_add_last(d);   /* in the index so a later OVERWRITE of this index dedups to the append entry */
    g_capturing = 0;
}

/* Capture a KNOWN-NEW Set/Map record (JSTimeTravelHooks.map_add): the key is not already in the collection, so
   like an array append it is a fresh entry needing no dedup and no baseline lookup. Flow-private collections (a Set
   built after the fork) are skipped. base = key, cur = value; unapply deletes, apply re-adds. */
void cow_capture_map_add(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst val) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;   /* flow-private skip — the O(shared-state) invariant */
    g_capturing = 1;
    cow_room_for_one(d, "cow: OOM growing delta (map_add) — a lost Set/Map record leaks across flows");
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, obj);
    e->base.value = JS_DupValue(ctx, key);   /* the record's key (owned) */
    e->cur.value = JS_DupValue(ctx, val);    /* the record's value (owned; UNDEFINED for a Set) */
    e->cur_state = COW_CUR_PRESENT;   /* the record's value is the entry's own, not a slot read back off the heap */
    e->is_map = 1;                     /* not slot-keyed — kept out of the hash index, like gendata */
    e->map_op = COW_MAP_ADD;
    g_capturing = 0;
}

/* Capture a reversible OVERWRITE / DELETE of an existing Set/Map record (JSTimeTravelHooks.map_mutate) as an
   undo-log entry — see the CowEntry comment. op is COW_MAP_OVERWRITE (base=key, cur=new, map_old=old) or
   COW_MAP_DELETE (base=key, map_old=old). Flow-private collections are skipped. */
void cow_capture_map_mutate(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst old_val, JSValueConst val, int op, int pos) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;   /* flow-private skip */
    g_capturing = 1;
    cow_room_for_one(d, "cow: OOM growing delta (map_mutate) — a lost Set/Map mutation leaks across flows");
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, obj);
    e->base.value = JS_DupValue(ctx, key);
    e->cur.value = JS_DupValue(ctx, val);  /* the new value (OVERWRITE); UNDEFINED for DELETE */
    e->cur_state = COW_CUR_PRESENT;   /* the record's value is the entry's own, not a slot read back off the heap */
    e->is_map = 1;
    e->map_op = (op == JS_MAP_MUTATE_DELETE) ? COW_MAP_DELETE : COW_MAP_OVERWRITE;
    e->map_pos = pos;                         /* where the inverse must put the record back — see CowEntry */
    e->map_old = JS_DupValue(ctx, old_val);   /* the prior value, restored on unapply */
    g_capturing = 0;
}

/* Capture a shared async object's SETTLEMENT before this flow changes it (JSTimeTravelHooks.async_state). A
   promise created before a fork is baseline state whose settle is a write no property hook can see, and the
   resolving function's already_resolved latch is the other half of it: without capturing the latch, the first
   arm to call the shared `resolve` wins and every sibling's call returns silently, so a value that differs per
   arm is lost rather than isolated. Flow-private promises (created after the fork) are skipped by the same
   generational test as every other capture — nothing else can observe them. */
void cow_capture_async_state(JSContext *ctx, JSValueConst obj) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;   /* flow-private skip — the O(shared-state) invariant */
    g_capturing = 1;
    void *blob = JS_AsyncStateSave(ctx, obj);
    /* The hook fires only for an object that HAS settlement state, so a NULL here is either that contract
       broken or an allocation failure — and both mean this flow's settle goes uncaptured and leaks into every
       sibling, which is a corrupted frontier rather than a degraded one. */
    CHECK(blob, "cow: a shared async object's settlement could not be captured — the flow's timeline would leak");
    for (int i = 0; i < d->n; i++)                  /* one entry per object: the FIRST baseline is the baseline */
        if (d->e[i].is_state && d->e[i].state_kind == COW_STATE_ASYNC &&
            JS_VALUE_GET_PTR(d->e[i].obj) == JS_VALUE_GET_PTR(obj)) {
            JS_AsyncStateFree(JS_GetRuntime(ctx), blob);
            g_capturing = 0;
            return;
        }
    if (cow_room_for_one(d, "cow: OOM growing delta (async_state) — a lost settlement leaks a flow's timeline"))
        cow_hash_rebuild(d);
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, obj);
    e->is_state = 1; e->state_kind = COW_STATE_ASYNC; e->a_base = blob;
    g_capturing = 0;
}

/* Capture a MODULE's evaluation state before this flow changes it (JSTimeTravelHooks.module_eval). Each flow is
   a possible world and evaluates the module once in it — the record's status/capability decide whether a flow
   evaluates at all, so leaving them baseline let the FIRST flow's evaluation stand for every sibling while the
   bindings it wrote stayed private to it. The sibling then read exports nothing had written in its world. There
   is no flow-private skip here: a module record is reachable from the realm's module map the moment it loads, so
   it is shared state whoever created it. */
/* The setter/ownership pair for a coroutine-state entry, chosen by its kind. */
static void cow_gd_install(const CowEntry *e, void *gd) {
    if (e->is_gendata == 2) JS_SetObjAsyncData(e->obj, gd);
    else JS_SetObjGenData(e->obj, gd);
}
static void cow_gd_ref(const CowEntry *e, void *gd) {
    if (e->is_gendata == 2) JS_AsyncDataRef(gd); else JS_GenDataRef(gd);
}
static void cow_gd_unref(JSContext *ctx, const CowEntry *e, void *gd) {
    if (e->is_gendata == 2) JS_AsyncDataUnref(ctx, gd); else JS_GenDataUnref(ctx, gd);
}

/* JSTimeTravelHooks.async_fork — the async twin of the generator swap. The engine has already cloned the
   activation; this records the toggle so the clone is what this flow's closure names and the original stays the
   baseline every other arm finds. The delta ADOPTS the clone's creation reference. */
void cow_capture_async_fork(JSContext *ctx, JSValueConst closure, void *base_data, void *cur_data) {
    CowDelta *d = g_current;
    if (!d) {
        /* NO FLOW OWNS THE SWAP, so there is nothing to undo it and nothing to own the clone: DECLINE. The
           closure keeps naming the original, which is correct — with no delta there is no sibling to isolate
           from. The engine re-reads the closure and proceeds with whichever activation it finds. */
        JS_AsyncDataUnref(ctx, cur_data);
        (void)base_data;
        return;
    }
    if (cow_room_for_one(d, "cow: OOM growing delta (async_fork) — a lost activation swap corrupts a flow's timeline"))
        cow_hash_rebuild(d);
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, closure);
    /* The POINTER is swapped, not the ownership: the closure's own reference stays on the original exactly as a
       generator object's stays on its object-owned state, and the delta owns the clone by adopting its creation
       reference. */
    e->is_gendata = 2; e->g0 = base_data; e->g1 = cur_data;
    JS_SetObjAsyncData(closure, cur_data);
}

void cow_capture_module_eval(JSContext *ctx, void *mod) {
    if (g_capturing || !g_current || !mod) return;
    CowDelta *d = g_current;
    for (int i = 0; i < d->n; i++)                  /* one entry per module: the FIRST baseline is the baseline */
        if (d->e[i].is_state && d->e[i].state_kind == COW_STATE_MODULE && d->e[i].target == mod) return;
    g_capturing = 1;
    void *blob = JS_ModuleEvalStateSave(ctx, mod);
    CHECK(blob, "cow: a module's evaluation state could not be captured — a sibling flow would inherit this "
                "flow's evaluation and read exports it never wrote");
    if (cow_room_for_one(d, "cow: OOM growing delta (module_eval)"))
        cow_hash_rebuild(d);
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->is_state = 1; e->state_kind = COW_STATE_MODULE; e->target = mod; e->a_base = blob;
    g_capturing = 0;
}

/* Capture a BROWSER COMPONENT's own mutable C record before this flow changes it. A component that keeps state
   in its class opaque writes it where no property hook and no engine hook can see, so the write does not
   time-travel: Response's body-used latch, set by whichever arm of a fork read the reply first, left the reply
   consumed for every sibling, and the sibling's own first read threw `body stream already read`. The flow that
   read it is the only one that should see it read.
   The OWNER is held for the life of the entry, because `p` points INTO the record that object owns and a parked
   flow can outlive every other reference to it — a delta naming freed storage would write those bytes back over
   whatever now occupies them. The flow-private skip is the same generational test every other capture uses. */
void cow_capture_host_state(JSContext *ctx, JSValueConst owner, void *p, size_t n) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    DCHECK(p != NULL && n > 0, "a component asked to capture no state — the call site has nothing to isolate");
    /* A POD LATCH ONLY, and this is where that is stated rather than only described. The blob is a memcpy, so a
       JSValue inside it becomes a reference nothing counts and the next restore frees a value the blob still
       names — cow.h's whole reason for the record arm below. A latch is a SCALAR; a record with several fields
       goes through cow_capture_host_record, which takes the layout (with n_val 0 when the record owns no value,
       as BroadcastChannel's does). Anything wider than a scalar reaching here is a record captured as bytes. */
    DCHECK(n == 1 || n == 2 || n == 4 || n == 8,
           "a component captured MORE THAN A SCALAR through the byte arm — cow_capture_host_state is for a POD "
           "latch, and a memcpy of a struct makes an uncounted reference out of any JSValue in it; capture the "
           "record through cow_capture_host_record with a CowRecord layout naming its owned values");
    DCHECK(JS_IsObject(owner), "a component's state was captured with no owning object — the storage it points "
                               "into would be freed out from under a parked flow's delta");
    if (JS_ObjFlowGen(owner) > d->fork_gen) return;   /* flow-private skip — the O(shared-state) invariant */
    for (int i = 0; i < d->n; i++)                    /* one entry per record: the FIRST baseline is the baseline */
        if (d->e[i].is_state && d->e[i].state_kind == COW_STATE_HOST && d->e[i].target == p) return;
    g_capturing = 1;
    if (cow_room_for_one(d, "cow: OOM growing delta (host_state)"))
        cow_hash_rebuild(d);
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, owner);
    e->is_state = 1; e->state_kind = COW_STATE_HOST; e->target = p; e->a_len = n;
    e->a_base = cow_state_save(ctx, e);   /* the bytes as this flow found them */
    g_capturing = 0;
}

/* A SHARED ARRAY BUFFER'S BYTES — see cow.h for why the unit is the bytes and not a view's element.
   The identity of the entry is the buffer OBJECT and not a `target` pointer, because the storage a pointer
   would name is freed by a detach and reallocated by a resize; that is also why the save and the restore ask
   the buffer for its bytes rather than keeping the address they first saw. */
void cow_capture_buffer(JSContext *ctx, JSValueConst abuf) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    uint32_t len;
    DCHECK(JS_IsObject(abuf), "a buffer's bytes were captured with no buffer object — the capture named a VIEW "
                              "where it must name the storage the view is a window onto");
    if (JS_ObjFlowGen(abuf) > d->fork_gen) return;   /* flow-private skip — the O(shared-state) invariant */
    /* A DETACHED buffer answers NULL and an empty one answers zero bytes. Neither is a failure and neither is a
       hole a default is filling: a buffer with no bytes has nothing a flow could have written, so there is
       nothing to isolate, and this entry is over CONTENTS.
       IT WAS READ AS THE HOLE THROUGH WHICH A ZERO-LENGTH RESIZABLE BUFFER ESCAPED, and it is not: a flow that
       resizes a zero-length buffer up and writes into it does capture — after the resize, with the post-resize
       bytes as its baseline — and what escaped is the RESIZE, which no length this line could have recorded
       would have held. The same escape happens with a buffer that was never empty (resize FIRST, write second),
       so removing this skip would have closed one ordering and left the other. It is closed at the mutation
       instead: cow_capture_buffer_lifetime below. */
    if (!JS_GetBufferBytes(abuf, &len) || len == 0) return;
    for (int i = 0; i < d->n; i++)                   /* one entry per buffer: the FIRST bytes are the baseline */
        if (d->e[i].is_state && d->e[i].state_kind == COW_STATE_BUFFER &&
            JS_VALUE_GET_PTR(d->e[i].obj) == JS_VALUE_GET_PTR(abuf)) return;
    g_capturing = 1;
    if (cow_room_for_one(d, "cow: OOM growing delta (buffer bytes)"))
        cow_hash_rebuild(d);
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, abuf);
    e->is_state = 1; e->state_kind = COW_STATE_BUFFER; e->a_len = len;
    e->a_base = cow_state_save(ctx, e);   /* the bytes as this flow found them */
    g_capturing = 0;
}

/* A SHARED BUFFER'S LIFETIME — see cow.h for why this is asked at the mutation and not at the save.
   It captures nothing, because there is nothing yet to capture it INTO: the byte entry holds contents and this
   is the storage those contents live in. What it does is turn the silent half of one defect into the loud half
   it already had, at the one point both halves pass through. */
void cow_capture_buffer_lifetime(JSContext *ctx, JSValueConst abuf) {
    CowDelta *d = g_current;
    (void)ctx;
    if (g_capturing || !d) return;
    DCHECK(JS_IsObject(abuf), "a buffer's lifetime changed on something that is not a buffer object");
    if (JS_ObjFlowGen(abuf) > d->fork_gen) return;   /* flow-private skip — the O(shared-state) invariant */
    CHECK_FAIL("a flow RESIZED, TRANSFERRED or DETACHED a SHARED ArrayBuffer: the byte entry is over the "
               "buffer's CONTENTS and this moves the storage they live in, so the sibling would inherit both "
               "the new length and this flow's writes — build the buffer-LIFETIME entry (the buffer object's "
               "byte length, its detached state, and the count each view over it carries), swapped like every "
               "other entry, beside COW_STATE_BUFFER");
}

/* THE SESSION'S CONTEXT — see cow.h. One document, one context; the DOM delta keeps its own for the same
   reason and by the same route. */
static JSContext *g_cow_ctx;
void cow_set_ctx(JSContext *ctx) { g_cow_ctx = ctx; }

void cow_capture_host_record(JSValueConst owner, void *p, const CowRecord *rec) {
    JSContext *ctx = g_cow_ctx;
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    DCHECK(ctx != NULL, "a component record was captured before cow_set_ctx named the session's context");
    DCHECK(p != NULL && rec != NULL && rec->size > 0, "a component record was captured with no layout");
    DCHECK(JS_IsObject(owner), "a component record was captured with no owning object — the storage it points "
                               "into would be freed out from under a parked flow's delta");
    /* THE LAYOUT IS A REFERENCE-COUNT CONTRACT, so its shape is asserted where it is used and not left to the
       reader of the offset list. Each entry names a JSValue this capture will DUP and this delta will later
       FREE, so an offset that runs off the end of the record dups whatever is next in memory, an unaligned one
       reads a value that is not there, and a DUPLICATED offset dups one field twice and frees it twice — which
       is the failure a hand-written `*_VALS[]` produces when a field is added by copying the line above it. */
    for (int vi = 0; vi < rec->n_val; vi++) {
        DCHECK((size_t)rec->val_off[vi] + sizeof(JSValue) <= rec->size,
               "a component record's layout names an owned value PAST THE END of the record — the capture would "
               "dup whatever follows it in memory and the restore would free it");
        DCHECK(rec->val_off[vi] % _Alignof(JSValue) == 0,
               "a component record's layout names an owned value at an offset no JSValue can sit at — the field "
               "named is not the field the finalizer frees");
        for (int vj = 0; vj < vi; vj++)
            DCHECK(rec->val_off[vj] != rec->val_off[vi],
                   "a component record's layout names one owned value TWICE — the capture dups it twice and the "
                   "delta frees it twice, which is a refcount underflow on a value the page still holds");
    }
    if (JS_ObjFlowGen(owner) > d->fork_gen) return;   /* flow-private skip — the O(shared-state) invariant */
    for (int i = 0; i < d->n; i++)
        if (d->e[i].is_state && d->e[i].state_kind == COW_STATE_HOST_REC && d->e[i].target == p) return;
    g_capturing = 1;
    if (cow_room_for_one(d, "cow: OOM growing delta (host_record)"))
        cow_hash_rebuild(d);
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, owner);
    e->is_state = 1; e->state_kind = COW_STATE_HOST_REC; e->target = p; e->rec = rec;
    e->a_base = cow_state_save(ctx, e);   /* the record as this flow found it */
    g_capturing = 0;
}

void cow_capture_varref(JSContext *ctx, void *vref) {
    if (g_capturing || !g_current || !vref) return;
    CowDelta *d = g_current;
    if (cow_hash_find(d, NULL, 0, vref) >= 0) return;   /* capture each cell ONCE (O(1)) */
    g_capturing = 1;
    cow_room_for_one(d, "cow: OOM growing delta (var_ref)");
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->base.value = JS_VarRefGetValue(vref);   /* owned dup of the cell's current (baseline) value; a cell has
                                                  no attributes and no accessor half, so the rest stays zero */
    e->vref = vref;
    JS_VarRefRef(vref);                  /* the DELTA owns it: the cell's own frames may die before the delta does */
    cow_hash_add_last(d);
    g_capturing = 0;
}

/* Record a per-flow generator-state swap (see cow.h). Dedup-REPLACES an existing entry for the same generator
   (a re-fork inside this flow): release the previous clone, adopt the new one, keep the ORIGINAL base pointer. */
void cow_delta_add_gendata(JSContext *ctx, CowDelta *d, JSValueConst genobj, void *base_gd, void *cur_gd) {
    void *gp = JS_VALUE_GET_PTR(genobj);
    for (int i = 0; i < d->n; i++) {
        CowEntry *e = &d->e[i];
        if (e->is_gendata && JS_VALUE_GET_PTR(e->obj) == gp) {
            JS_GenDataUnref(ctx, e->g1);   /* drop the previous clone; the new clone's creation ref transfers here */
            e->g1 = cur_gd;
            (void)base_gd;                 /* base stays e->g0 (the object-owned original), not the intermediate clone */
            return;
        }
    }
    cow_room_for_one(d, "cow: OOM growing delta (gendata) — a lost generator swap corrupts per-flow state");
    CowEntry *e = &d->e[d->n++];
    cow_entry_init(e);
    e->obj = JS_DupValue(ctx, genobj);
    e->is_gendata = 1; e->g0 = base_gd; e->g1 = cur_gd;   /* adopts cur_gd's creation ref (freed on delta free) */
}

/* PASS 1 of an unapply — READ this entry's flow-side state off the live heap. Writes nothing to the heap. */
static void cow_save_cur(JSContext *ctx, CowEntry *e) {
    if (e->is_gendata) return;   /* a fixed toggle between two activations: nothing is read back */
    if (e->is_state) {           /* blob state: what THIS flow produced, for apply to re-install */
        cow_state_free(JS_GetRuntime(ctx), e, e->a_cur);
        e->a_cur = cow_state_save(ctx, e);
        return;
    }
    if (e->is_map) return;       /* an UNDO-LOG entry: it inverts, it does not read the collection back */
    if (e->vref) {               /* closure cell: a value, never a descriptor — see the CowEntry comment */
        if (e->cur_state != COW_CUR_UNRECORDED) cow_desc_free(ctx, &e->cur);
        e->cur.value = JS_VarRefGetValue(e->vref); e->cur_state = COW_CUR_PRESENT;
        return;
    }
    JSPropertyDescriptor cur;
    int has = JS_GetOwnSlotDesc(ctx, &cur, e->obj, e->atom);
    DCHECK(has >= 0, "reading back a captured slot threw — a delta holds something that is not an object, "
                     "and a context switch has no flow base to run an exception on");
    DCHECK(has > 0 || (JS_IsUndefined(cur.value) && cur.flags == 0),
           "an ABSENT slot was recorded carrying a descriptor — apply would write it back and re-create the "
           "property this flow deleted");
    if (e->cur_state != COW_CUR_UNRECORDED) cow_desc_free(ctx, &e->cur);
    e->cur = cur;
    e->cur_state = has > 0 ? COW_CUR_PRESENT : COW_CUR_ABSENT;
}

/* PASS 2 of an unapply — put the BASELINE back. Writes the heap; reads nothing of the flow's. */
static void cow_restore_base(JSContext *ctx, CowEntry *e) {
    if (e->is_gendata) { cow_gd_install(e, e->g0); return; }   /* restore the shared original */
    if (e->is_state) { cow_state_restore(ctx, e, e->a_base); return; }
    if (e->is_map) {   /* invert the flow's record mutation: ADD->delete, OVERWRITE/DELETE->restore old value */
        if (e->map_op == COW_MAP_ADD) JS_MapDeleteRecord(ctx, e->obj, e->base.value);
        /* AT ITS POSITION — a DELETE's inverse CREATES the record, and where it lands is observable. An
           OVERWRITE's carries JS_MAP_POS_TAIL and never reaches the placement: its record is still there. */
        else JS_MapAddRecord(ctx, e->obj, e->base.value, e->map_old, e->map_pos);
        return;
    }
    if (e->vref) {
        DCHECK(e->base.flags == 0 && JS_IsUndefined(e->base.getter),
               "a closure cell was recorded carrying property ATTRIBUTES — a cell is storage and not a "
               "property, so whatever wrote those read the cell as something it is not");
        JS_VarRefSetValue(ctx, e->vref, JS_DupValue(ctx, e->base.value));
        return;
    }
    uint32_t ai;
    if (e->existed)
        /* THE BASELINE DESCRIPTOR, PUT BACK — as a SLOT WRITE, which is the same thing the read side has
           always been. This was a JS_SetProperty: a [[Set]], an operation THROUGH the slot, which walks the
           prototype chain, calls a setter, and consults `writable` and `extensible` — and refuses by THROWING
           (it carries JS_PROP_THROW) inside a context switch with no flow base to run the exception on. Three
           shapes reached it and each left a pending TypeError belonging to no flow, which the next thing to
           check for an exception read as its own: a slot the flow narrowed with a define (Object.freeze,
           `{writable:false}`); a slot the flow DELETED off a non-extensible object; and a SPURIOUS capture
           whose write then threw (`Math.PI = 1` in a flow captures, fails, and comes back here to put back a
           value the slot never stopped holding). None of them can arise: JS_SetOwnSlotDesc asks none of those
           questions. And what it puts back is now the WHOLE state — the C/W/E bits and, where the slot is an
           accessor, its getter/setter pair — so the flow's own `Object.freeze` on shared state comes back off,
           which is the half that used to have an assert standing where the mechanism belonged. */
        {
            JSPropertyDescriptor d = cow_desc_dup(ctx, &e->base);
            JS_SetOwnSlotDesc(ctx, e->obj, e->atom, &d);
        }
    else if (JS_IsArrayDenseSlot(e->obj, e->atom, &ai))
        /* flow-CREATED DENSE ELEMENT: shrink the dense part past it, which is what removing one is. A
           JS_DeleteOwnSlot would convert the fast array to a slow one for every sibling. Entries are processed
           in reverse (highest index first), so a contiguous run of appends shrinks one-by-one; cur was saved in
           pass 1 so apply re-appends it.
           THE LENGTH IS NOT TOUCHED, and the sentence that used to stand here — "the array's length is derived
           from these entries rather than captured as the slot it is" — was true only while `length == count`.
           This reached the shrink through a LENGTH WRITE, so a flow's `buf[0]='a'` on a baseline
           `new Array(10)` unapplied to `buf.length === 0` for every sibling. The length is a captured slot now
           (cow_capture_append takes it beside the element), which also makes these two entries order-INDEPENDENT
           rather than order-critical: neither one's restore can move what the other one restores. */
        JS_ArrayTruncateDense(ctx, e->obj, ai);
    else
        /* THE FLOW'S OWN CREATION, REMOVED — as a SLOT REMOVAL, for the reason the write above is a slot write.
           This was JS_DeleteProperty, the [[Delete]] OPERATION, which 10.1.10.1 makes refuse a non-configurable
           slot: `Object.defineProperty(shared,"x",{value:1})` defaults C/W/E to false, so the commonest way a
           flow creates a slot was one the unapply could not take back out — it stood in the baseline for every
           sibling, and the assert that reported it is gone because the mechanism it named is here. */
        JS_DeleteOwnSlot(ctx, e->obj, e->atom);
}

/* A SAVE IS A READ OF THE SAME HEAP A RESTORE WRITES, so ALL of one segment's entries are read BEFORE ANY of
   them is put back. Interleaving them — one loop that saved entry i and immediately restored it — made an
   entry's record of "what this flow has" depend on what a LATER-CAPTURED entry's restore had already changed,
   and the two entries that do that to each other are an array's length and its elements: `arr.length = 5`
   followed by `arr[3] = v` restores the element first, which truncates the array back to 3 (see
   cow_restore_base), and only then reads the length — recording the 3 that truncation had just produced
   instead of the 5 the flow set, so the flow resumed holding a length it never had. The two
   passes cost one extra walk of an array the swap already walks twice, and they are what make the record of a
   flow's state a statement about the flow rather than about the order its writes happened to be captured in.
   Pass 2 runs in REVERSE (symmetric with apply's forward replay); pass 1's order cannot matter, because it
   writes nothing the next entry could read.
   ACROSS segments the interleave is CORRECT and stays: cow_install_chain unapplies the topmost segment whole
   before the one below it, because the lower segment's flow-side state is precisely what the upper one's
   restore puts back. The invariant is within ONE frozen unit, whose entries are one flow's writes at one
   instant and are deduped, so no slot appears twice. */
static void cow_unapply_entries(JSContext *ctx, CowEntry *ents, int n) {
    for (int i = 0; i < n; i++) cow_save_cur(ctx, &ents[i]);
    for (int i = n - 1; i >= 0; i--) cow_restore_base(ctx, &ents[i]);
}

/* UNAPPLY (flow -> parked): ONLY THE HEAD. The base chain stays applied and g_cow_installed says so — the
   incoming flow's apply decides how much of it has to move, which for a sibling is none of it. */
void cow_unapply(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    g_capturing = 1;
    cow_unapply_entries(ctx, d->e, d->n);
    g_capturing = 0;
    DCHECK(g_cow_installed == d->base,
           "the applied heap chain is not the running flow's — a delta was swapped without going through "
           "cow_apply, so the heap is showing a chain nobody is running");
}

static void cow_apply_entries(JSContext *ctx, CowEntry *ents, int n) {
    for (int i = 0; i < n; i++) {   /* forward: replay the writes over the chain below */
        CowEntry *e = &ents[i];
        if (e->is_gendata) { cow_gd_install(e, e->g1); continue; }   /* install this flow's own clone */
        if (e->is_state) { cow_state_restore(ctx, e, e->a_cur); continue; }   /* re-install what this flow produced */
        if (e->is_map) {   /* replay the flow's record mutation: ADD/OVERWRITE->set new value, DELETE->delete */
            if (e->map_op == COW_MAP_DELETE) JS_MapDeleteRecord(ctx, e->obj, e->base.value);
            /* AT THE TAIL, and that is the OPERATION's position rather than the entry's: an add appended when
               it happened, and a forward replay runs the entries in the order they were captured over the state
               they were captured against, so appending reproduces it. */
            else JS_MapAddRecord(ctx, e->obj, e->base.value, e->cur.value, JS_MAP_POS_TAIL);
            continue;
        }
        if (e->cur_state == COW_CUR_UNRECORDED) continue;   /* never switched out: nothing of this flow's to replay */
        if (e->vref) {
            DCHECK(e->cur_state == COW_CUR_PRESENT,
                   "a closure cell was recorded ABSENT — a cell holds a value or does not exist at all, so "
                   "there is no deletion of one for apply to replay");
            JS_VarRefSetValue(ctx, e->vref, JS_DupValue(ctx, e->cur.value));
        } else if (e->cur_state == COW_CUR_PRESENT) {
            /* THE FLOW'S OWN STATE, REPLAYED — the same slot write cow_restore_base makes, and for the same
               reason, with one difference that made it worse here: it was a JS_SetProperty whose result nothing
               looked at at all, so a slot narrowed to non-writable (by this flow's own `Object.freeze` on
               shared state, or by a sibling's) refused the replay by THROWING a TypeError into a context switch
               that had no flow to own it, and the flow then resumed without the value it wrote. The descriptor
               carries the flow's own attributes with its value, so a freeze this flow made is re-applied as the
               freeze it was rather than lost or refused. */
            JSPropertyDescriptor d = cow_desc_dup(ctx, &e->cur);
            JS_SetOwnSlotDesc(ctx, e->obj, e->atom, &d);
        } else {
            /* THE FLOW'S OWN DELETE, REPLAYED AS A DELETE. The chain below has just put the slot back (it is
               baseline state, which is the only kind that gets captured), and writing `cur` over it would put
               back a PROPERTY holding undefined — the slot this flow removed, standing in the flow that
               removed it. A deletion is not a value, so it is replayed by the operation the flow performed.
               It cannot be refused, and that is now true by CONSTRUCTION rather than by an argument about
               what the chain below can hold: JS_DeleteOwnSlot removes the slot whatever its attributes, so
               there is no status left for an assert to read. The argument it replaced was also wrong — it
               reasoned that a non-configurable slot below could only come from a sibling, and the flow's own
               `Object.defineProperty` on shared state makes one every time. */
            JS_DeleteOwnSlot(ctx, e->obj, e->atom);
        }
    }
}

/* APPLY (parked -> flow): move the installed chain to this flow's, then its head on top. */
void cow_apply(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    g_capturing = 1;
    cow_install_chain(ctx, d->base);
    cow_apply_entries(ctx, d->e, d->n);
    g_capturing = 0;
}

/* FORK a delta at a branch: FREEZE the running flow's head into a shared immutable segment BOTH flows
   reference, exactly as the DOM half does — the sibling inherits the parent's whole history in O(1).
   It used to COPY: every entry duplicated, every async blob CLONED, and the sibling's restore point read back
   out of the live heap one slot at a time. So a delta was O(everything the flow had ever written) rather than
   O(what this arm has written since it branched), and N siblings each carried the same history and replayed all
   of it on every switch. Freezing gives the same semantics for nothing: the head is unapplied first, which is
   what stashes each entry's branch-point value into `cur`, and THAT is the sibling's restore point — the same
   state the copy went and fetched, already sitting in the entries being handed over. */
CowDelta *cow_delta_fork(JSContext *ctx, CowDelta *src) {
    CowDelta *d = cow_delta_new();
    CowSeg *seg;

    /* IS `src` WHAT THE HEAP IS SHOWING? That one fact decides where the branch-point values are, and it is
       asked here rather than by the caller — see cow.h. `g_current` is the delta being captured into, which is
       the running flow's and no other; a delta reached from the world registry is a parked segment.
       A delta being captured into whose chain is NOT installed is incoherent whichever path runs: the writes
       being captured are landing on a heap showing somebody else's timeline. */
    const int applied = (src == g_current);
    DCHECK(!applied || g_cow_installed == src->base,
           "the delta being captured into is not the one the heap is showing — a write captured now records a "
           "baseline value that belongs to another flow's timeline, and the fork would freeze that");
    /* Both flows now SHARE every object that existed at THIS fork (the frame snapshot dup'd their heap refs), so
       both must capture writes to any object with flow_gen <= this generation. Record the CURRENT generation as
       both deltas' fork_gen, then BUMP it so objects created after the fork get a strictly-higher generation and
       are correctly treated as flow-private. Updating the parent's fork_gen (it may be > its previous value) is
       correct: the parent must isolate from its NEWEST sibling, which shares everything up to now. */
    src->fork_gen = d->fork_gen = JS_FlowGen();
    /* ONLY THE RUNNING DELTA PUBLISHES ITS THRESHOLD. JS_SetFlowForkGen sets what the capture hook compares
       against, which is a statement about the flow the heap is showing; a parked segment saying it would make
       the running flow capture by somebody else's fork point. */
    if (applied) JS_SetFlowForkGen(src->fork_gen);
    JS_FlowBumpGen();
    d->base = src->base;
    if (src->n == 0) {                  /* nothing to freeze: the sibling just takes a reference on the chain */
        if (d->base) d->base->refcount++;
        return d;
    }
    /* ALLOCATED WHILE THE WORLD IS CONSISTENT — before the unapply, not after it, which is the same rule as
       publishing a capacity late. This allocation can SELL A FLOW (solver/reclaim.h), and a sale walks the
       shared chain and may re-install it; asking for the memory here asks it while the running flow's head is
       still applied and the heap shows exactly the timeline the chain names. Between the unapply and the
       re-apply the head is half-taken-down, which is a state no other code in this file is written to meet. */
    seg = reclaim_malloc(sizeof *seg);
    CHECK(seg, "cow: OOM fork segment — a shared delta would be corrupted");
    g_capturing = 1;
    /* THE FETCH, for an applied delta only: unapply is what puts each entry's branch-point value into `cur`. A
       parked delta's entries hold it already, and running this on one would read the heap of whatever flow IS
       applied and record another timeline's values as this delta's. */
    if (applied) cow_unapply_entries(ctx, src->e, src->n);
    seg->e = src->e; seg->n = src->n; seg->base = src->base; seg->refcount = 2;   /* running flow + sibling */
    g_seg_live++; g_seg_entries_live += seg->n;
    src->e = NULL; src->n = 0; src->cap = 0;   /* fresh empty head for the running flow */
    free(src->hash); src->hash = NULL; src->hash_cap = 0;
    src->base = d->base = seg;
    if (applied) {
        g_cow_installed = seg;                 /* the frozen head is part of the applied chain now */
        cow_apply_entries(ctx, seg->e, seg->n);/* re-apply -> the running flow continues byte-identically */
    }
    /* A PARKED FORK LEAVES g_cow_installed WHERE IT WAS, and that is coherent rather than a loose end: it names
       a chain below an unapplied head, exactly the state cow_unapply leaves, and cow_install_chain walks from
       it to whatever comes next. */
    g_capturing = 0;
    return d;
}

/* EMPTY = nothing in the head and no frozen chain under it. A segment is only ever created from a non-empty
   head (both forks return early when there is nothing to freeze), so a chain that exists holds writes and the
   two tests together are the whole answer rather than an approximation of it. */
bool cow_delta_empty(const CowDelta *d) { return d == NULL || (d->n == 0 && d->base == NULL); }

static void cow_entries_free(JSContext *ctx, CowEntry *e, int n);

/* Drop a chain reference: refcount--, free the segment's entries at zero, continue into its base. A loop, not
   recursion — the chain's depth is the fork depth and C stack cannot be parked. */
static void cow_seg_unref(JSContext *ctx, CowSeg *s) {
    while (s && --s->refcount <= 0) {
        CowSeg *base = s->base;
        /* THE HEAP MAY NOT BE SHOWING WHAT IS ABOUT TO BE FREED, asserted at the free rather than at each caller
           that could arrange it. `g_cow_installed` is a property of the HEAP and holds no reference, so nothing
           about a refcount reaching zero says the segment is unapplied — and an installed pointer into freed
           memory is what the very next context switch walks. cow_delta_release is what brings the chain down to
           the deepest SURVIVOR first; reaching here with the installed segment means a reference was dropped
           without going through it. */
        DCHECK(s != g_cow_installed,
               "a frozen heap segment was freed while the heap was still SHOWING it — the installed chain would "
               "point into freed memory and the next context switch walks it, and every write in the segment "
               "stays standing in the baseline with nothing left that can unapply it");
        cow_entries_free(ctx, s->e, s->n);
        g_seg_live--; g_seg_entries_live -= s->n;
        free(s->e); free(s);
        s = base;
    }
}

static int cow_seg_depth(const CowSeg *s) { int d = 0; for (; s; s = s->base) d++; return d; }

/* The deepest segment BOTH chains hold — pointer identity, since a segment is frozen once and never rewritten. */
static CowSeg *cow_seg_common(CowSeg *a, CowSeg *b) {
    int da = cow_seg_depth(a), db = cow_seg_depth(b);
    while (da > db) { a = a->base; da--; }
    while (db > da) { b = b->base; db--; }
    while (a != b) { a = a->base; b = b->base; }
    return a;
}

/* Apply `s` down to (not including) `stop`, deepest-first, by reversing the base pointers in place and putting
   them back — O(depth), no allocation, and no recursion on the hottest path the scheduler has. */
static void cow_apply_seg_until(JSContext *ctx, CowSeg *s, CowSeg *stop) {
    CowSeg *prev = stop, *cur = s, *next;
    while (cur != stop) { next = cur->base; cur->base = prev; prev = cur; cur = next; }
    cur = prev; prev = stop;
    while (cur != stop) {
        cow_apply_entries(ctx, cur->e, cur->n);
        next = cur->base; cur->base = prev; prev = cur; cur = next;
    }
}

/* Make `want` the installed chain: the ONE place the heap's applied state changes, and the reason a switch
   between two related flows costs what they DIVERGED by rather than everything either of them has done. */
/* WHAT A CONTEXT SWITCH COSTS, counted where it is paid. The design says a switch "touches only what lies
   above the common segment", which is O(divergence) and cheap between siblings — but a breadth-first rotation
   picks the next flow from anywhere in the fork TREE, and two distant cousins diverge near the root, so the
   walk covers most of both histories. Whether that is tens of entries or hundreds of thousands decides whether
   the scheduler's ordering policy needs to change or the swap does, and it was being argued about rather than
   measured. Counted, not sampled: every entry this walk touches is one slot restored in the live heap. */
static long g_swap_entries, g_swap_max;
static long g_swap_count;
void cow_swap_stats(long *count, long *total, long *max) {
    if (count) *count = g_swap_count;
    if (total) *total = g_swap_entries;
    if (max)   *max   = g_swap_max;
}

static void cow_install_chain(JSContext *ctx, CowSeg *want) {
    CowSeg *common = cow_seg_common(g_cow_installed, want), *s;
    long touched = 0;
    for (s = g_cow_installed; s != common; s = s->base) {
        touched += s->n;
        cow_unapply_entries(ctx, s->e, s->n);
    }
    for (s = want; s != common; s = s->base)
        touched += s->n;
    cow_apply_seg_until(ctx, want, common);
    g_cow_installed = want;
    g_swap_count++;
    g_swap_entries += touched;
    if (touched > g_swap_max) g_swap_max = touched;
}

void cow_delta_release(JSContext *ctx, CowDelta *d) {
    CowSeg *surv, *s;

    if (!d) return;
    /* THE SCHEDULER IS NOT SWITCHED INTO THIS DELTA, which is the release's whole contract — see cow.h. The head
       is FREED below and never unapplied, so an applied one leaves this flow's writes standing in the shared
       baseline with nothing left that could take them back out, and every other flow reads them as baseline from
       then on. `g_current` is exactly "the delta the scheduler is switched into" — cow_set_current(NULL) is what
       a switch-out does after cow_unapply — so the two facts are one fact and this is where it is asked. */
    DCHECK(d != g_current,
           "a COW delta was released while the scheduler was still switched into it — its head is APPLIED to the "
           "live heap, so freeing the entries leaves this flow's writes standing in the shared baseline with "
           "nothing that can unapply them; switch the flow out first");
    /* THE DEEPEST SEGMENT THAT SURVIVES. A segment dies exactly when this delta held its last reference, and the
       dying set is a contiguous prefix from `d->base` down — freeing one drops its own reference on the one
       below it. Everything from `surv` down has another holder and must not be touched at all. */
    for (surv = d->base; surv && surv->refcount == 1; surv = surv->base) ;
    /* IS THE HEAP SHOWING SOMETHING THAT IS ABOUT TO BE FREED? Only then does anything move, and then it moves
       exactly as far as the free reaches. One walk for the two cases this used to answer with a blanket revert
       to the baseline: a FINISHING flow, whose own chain is installed and held by nobody else (the heap now
       comes down to the deepest sibling-held segment rather than all the way to the baseline, which the next
       switch-in then had to replay in full), and a PARKED one — an evicted tail, a foreign world's segment —
       where the installed chain belongs to whichever flow is RUNNING and a blanket revert silently rewound that
       flow's heap and left `g_cow_installed` NULL under it. */
    g_capturing = 1;   /* the scheduler's own unapply is not a flow's write */
    for (s = d->base; s != surv; s = s->base)
        if (s == g_cow_installed) { cow_install_chain(ctx, surv); break; }
    g_capturing = 0;
    cow_seg_unref(ctx, d->base); d->base = NULL;
    cow_entries_free(ctx, d->e, d->n);
    /* AND THE DELTA'S OWN ALLOCATIONS, which this function did not free — it released what the entries POINTED
       AT and left the entry array, the hash index and the struct itself behind, on every flow that ever
       finished. Nothing else frees them: `cow_delta_new` is the only allocator and this is its only pair, so a
       delta outlived its flow as pure garbage no GC walk could see (it holds no live JSValue by then) and no
       counter named (the frontier stayed at ~30 live flows while the process grew by a delta per flow). At the
       fixture's ~14000 flows that is hundreds of megabytes, which is the difference between a run that fits in
       the address space and one that does not.
       AFTER the two calls above and not before: `cow_entries_free` reads `d->e`, and the segment unref may walk
       the chain this delta's head sits over. */
    free(d->e);
    free(d->hash);
    free(d);
}

static void cow_entries_free(JSContext *ctx, CowEntry *e, int n) {
    for (int i = 0; i < n; i++) {
        if (e[i].is_gendata) { JS_FreeValue(ctx, e[i].obj); cow_gd_unref(ctx, &e[i], e[i].g1); continue; }
        if (e[i].is_state) {
            JS_FreeValue(ctx, e[i].obj);
            cow_state_free(JS_GetRuntime(ctx), &e[i], e[i].a_base);
            cow_state_free(JS_GetRuntime(ctx), &e[i], e[i].a_cur);
            continue;
        }
        if (e[i].is_map) {   /* obj + key(base.value) + new(cur.value) + old(map_old); atom is NULL */
            JS_FreeValue(ctx, e[i].obj);
            cow_desc_free(ctx, &e[i].base);
            cow_desc_free(ctx, &e[i].cur);
            JS_FreeValue(ctx, e[i].map_old);
            continue;
        }
        JS_FreeValue(ctx, e[i].obj);
        JS_FreeAtom(ctx, e[i].atom);
        /* THE WHOLE DESCRIPTOR, BOTH SIDES — an accessor entry owns two function objects where a data entry
           owns one value, and a free that knew only about the value would have leaked the pair. That is the
           obligation §Architecture names: a field added to the record is an obligation at every clone and every
           free, which is why the dup and this free are one pair (cow_desc_dup / cow_desc_free). */
        cow_desc_free(ctx, &e[i].base);
        if (e[i].vref) JS_VarRefUnref(ctx, e[i].vref);   /* the delta's own reference on the cell */
        if (e[i].cur_state != COW_CUR_UNRECORDED) cow_desc_free(ctx, &e[i].cur);   /* ABSENT holds nothing */
    }
}

/* `cow_free` — a session teardown that set `g_current` to NULL and did nothing else — is DELETED: it had NO
   CALLER, which is the defect class this codebase has been bitten by repeatedly (a producer nobody runs reads
   exactly like one that works). What it claimed to do is now a property the frontier's teardown ASSERTS instead
   of a function nobody called: with every delta released, no frozen segment may still be live and the heap may
   not still be showing one — see the chain assertions in flow_registry_free. */

/* THE time-travel hook set — see cow.h. Installed AFTER the context's own globals exist, so the baseline is
   pre-flow and nothing set up before it lands in a delta.
   `gen_fork` IS THE CALLER'S, and it is the only one of the ten that is: the other nine are this file's own
   capture points, while a generator-state fork is stashed by whoever assembles the SIBLING FLOW — the
   scheduler. Naming that function here made this primitive depend on the dispatch loop, and through it on the
   whole DOM, so nothing could take the COW delta without taking the browser too. It is a parameter now, which
   is what it always was. */
void cow_install_time_travel_hooks(JSTimeTravelGenFork gen_fork)
{
    static JSTimeTravelHooks HOOKS = {
        .prop_write = cow_capture_hook, .cell_write = cow_capture_varref,
        .arr_append = cow_capture_arr_append, .buf_write = cow_capture_buffer,
        .buf_lifetime = cow_capture_buffer_lifetime,
        .map_add = cow_capture_map_add, .map_mutate = cow_capture_map_mutate,
        .async_state = cow_capture_async_state, .module_eval = cow_capture_module_eval,
        .async_fork = cow_capture_async_fork };
    DCHECK(gen_fork != NULL,
           "the time-travel hooks were installed with no generator-fork handler — a concolic branch inside a "
           "generator body would fork a sibling that shares the parent's execution state");
    HOOKS.gen_fork = gen_fork;
    JS_SetTimeTravelHooks(&HOOKS);
}
