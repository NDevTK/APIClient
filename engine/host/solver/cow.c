/* Per-flow swappable COW delta — see cow.h. */
#include "solver/cow.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* One captured baseline slot: base = the value the shared baseline holds (restored on unapply); cur = the
   running flow's latest value for it (saved on unapply, replayed on apply). existed = did the baseline own the
   slot (else the flow CREATED it, so unapply deletes and apply re-creates). */
/* A delta entry is EITHER a property slot (vref==NULL: obj/atom/existed) OR a closure cell (vref!=NULL: the
   shared JSVarRef, whose value is read/written via JS_VarRefGet/SetValue; obj/atom/existed unused). base/cur
   hold the baseline/flow values either way. */
/* A THIRD entry kind (is_gendata=1) swaps a shared GENERATOR object's execution-state pointer per flow: obj is
   the generator object, g0 its object-owned original state, g1 the per-flow clone (owned by this delta via
   JS_GenDataRef/Unref). Apply installs g1, unapply restores g0 — a fixed toggle, no cur/base value tracking. */
/* A FOURTH entry kind (is_map=1) is a reversible Set/Map record mutation by this flow (an UNDO-LOG entry, not a
   deduped slot): obj is the Set/Map, base holds the record's key, cur its new value, map_old its prior value.
   map_op selects the inverse pair — ADD (apply re-adds, unapply deletes), OVERWRITE (apply sets cur, unapply
   restores map_old), DELETE (apply deletes, unapply re-adds map_old). Entries replay forward on apply and invert
   in reverse on unapply, so a sequence of mutations to the same key reconstructs correctly with NO dedup and no
   SameValueZero key-hashing. Never slot-keyed (kept out of the hash index, like gendata). */
/* A FIFTH entry kind (is_async=1) is an opaque STATE BLOB over internal fields no property hook can see:
   a_base is the baseline state this flow found, a_cur the state this flow produced (saved at unapply, replayed
   at apply) — the same base/cur shape as a property slot. TWO TARGETS share it because they are one concept,
   "the state of this thing before this flow changed it": a shared ASYNC object's settlement (mod == NULL, obj
   is the promise or the resolving function whose already_resolved latch it is) and a MODULE record's EVALUATION
   state (mod != NULL, obj unused — a module is not a JSObject). A module's bindings are closure cells that
   cell_write already captures; what was missing is the state deciding whether a flow evaluates AT ALL, so the
   first flow to import a chunk left it EVALUATED for every sibling and the siblings read TDZ exports.
   Not slot-keyed (kept out of the hash index, like gendata and map): each settles once per flow, nothing to dedup. */
#define COW_MAP_ADD       0
#define COW_MAP_OVERWRITE 1
#define COW_MAP_DELETE    2
typedef struct { JSValue obj; JSAtom atom; int existed; JSValue base; JSValue cur; int cur_valid; void *vref;
                 int is_gendata; void *g0; void *g1; int is_map; int map_op; JSValue map_old;
                 int is_async; void *mod; void *a_base; void *a_cur; } CowEntry;

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
struct CowDelta { CowEntry *e; int n, cap; uint32_t fork_gen; int *hash; int hash_cap; };

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
    d->hash_cap = 16; while (d->hash_cap < d->n * 2) d->hash_cap *= 2;
    d->hash = realloc(d->hash, (size_t)d->hash_cap * sizeof(int));
    CHECK(d->hash, "cow: OOM hash index");
    memset(d->hash, 0, (size_t)d->hash_cap * sizeof(int));
    for (int i = 0; i < d->n; i++) if (!d->e[i].is_gendata && !d->e[i].is_async) cow_hash_put(d, i);   /* gendata/async aren't slot-keyed */
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

static CowDelta *g_current = NULL;   /* the flow whose writes are captured right now (scheduler-owned) */
static int g_capturing = 0;          /* re-entrancy guard: our own reads/restores must not re-capture */

CowDelta *cow_delta_new(void) {
    CowDelta *d = calloc(1, sizeof *d);
    CHECK(d, "cow: OOM new delta");
    return d;
}

void cow_set_current(CowDelta *d) { g_current = d; }

/* The is_async entry's four operations, dispatched by TARGET. One mechanism, two things it captures — the
   alternative was a sixth entry kind whose unapply/apply/fork/free arms would be the same four lines again. */
static void *cow_state_save(JSContext *ctx, const CowEntry *e) {
    return e->mod ? JS_ModuleEvalStateSave(ctx, e->mod) : JS_AsyncStateSave(ctx, e->obj);
}
static void cow_state_restore(JSContext *ctx, const CowEntry *e, void *blob) {
    if (e->mod) JS_ModuleEvalStateRestore(ctx, e->mod, blob);
    else JS_AsyncStateRestore(ctx, e->obj, blob);
}
static void *cow_state_clone(JSContext *ctx, const CowEntry *e, void *blob) {
    return e->mod ? JS_ModuleEvalStateClone(ctx, blob) : JS_AsyncStateClone(ctx, blob);
}
static void cow_state_free(JSRuntime *rt, const CowEntry *e, void *blob) {
    if (e->mod) JS_ModuleEvalStateFree(rt, blob); else JS_AsyncStateFree(rt, blob);
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
    JSValue base;
    /* the SLOT, not [[GetOwnProperty]]: this runs inside a write hook, so a Proxy trap or an accessor's getter
       here would be the page's code with no flow base. JS_GetOwnSlot asserts that neither is reachable. */
    int existed = JS_GetOwnSlot(ctx, &base, obj, atom) > 0;

    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta — a lost baseline write leaks state across flows");
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_DupAtom(ctx, atom);
    e->existed = existed;
    e->base = base;
    e->cur = JS_UNDEFINED;
    e->cur_valid = 0;
    e->vref = NULL;
    e->is_gendata = 0;
    e->is_async = 0; e->mod = NULL; e->a_base = e->a_cur = NULL;
    e->is_map = 0;
    cow_hash_add_last(d);
    g_capturing = 0;
}

/* Record a CLOSURE CELL's pre-write value (JSCowVarRefHook) into the running flow's delta, so a snapshot-forked
   sibling that shares the cell is isolated on write. Captured once per cell (first write is the baseline). */
/* Record a KNOWN-NEW fast-array APPEND slot (JSTimeTravelHooks.arr_append): the index == the array's current
   length, so it cannot already be in the delta (dedup unneeded) and its baseline is ABSENT (existed=0, no
   JS_GetOwnSlot). O(1) — this is the accumulator hot path (a shared array built one element at a time);
   routing it through cow_capture_hook's O(n) dedup scan makes an N-element build O(N^2). The flow-private skip is
   still applied (a post-fork private array — e.g. a per-flow accumulator built after the fork — is not captured). */
void cow_capture_arr_append(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;
    g_capturing = 1;
    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta (arr_append)");
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_DupAtom(ctx, atom);   /* a tagged-int index atom: dup is a no-op, kept for uniform free */
    e->existed = 0;                    /* an append creates the slot; unapply truncates it away */
    e->base = JS_UNDEFINED;
    e->cur = JS_UNDEFINED; e->cur_valid = 0;
    e->vref = NULL;
    e->is_gendata = 0;
    e->is_async = 0; e->mod = NULL; e->a_base = e->a_cur = NULL;
    e->is_map = 0;
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
    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta (map_add) — a lost Set/Map record leaks across flows");
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_ATOM_NULL;
    e->existed = 0;
    e->base = JS_DupValue(ctx, key);   /* the record's key (owned) */
    e->cur = JS_DupValue(ctx, val);    /* the record's value (owned; UNDEFINED for a Set) */
    e->cur_valid = 1;
    e->vref = NULL;
    e->is_gendata = 0;
    e->is_async = 0; e->mod = NULL; e->a_base = e->a_cur = NULL;
    e->is_map = 1;                     /* not slot-keyed — kept out of the hash index, like gendata */
    e->map_op = COW_MAP_ADD; e->map_old = JS_UNDEFINED;
    g_capturing = 0;
}

/* Capture a reversible OVERWRITE / DELETE of an existing Set/Map record (JSTimeTravelHooks.map_mutate) as an
   undo-log entry — see the CowEntry comment. op is COW_MAP_OVERWRITE (base=key, cur=new, map_old=old) or
   COW_MAP_DELETE (base=key, map_old=old). Flow-private collections are skipped. */
void cow_capture_map_mutate(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst old_val, JSValueConst val, int op) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;   /* flow-private skip */
    g_capturing = 1;
    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta (map_mutate) — a lost Set/Map mutation leaks across flows");
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_ATOM_NULL;
    e->existed = 0;
    e->base = JS_DupValue(ctx, key);
    e->cur = JS_DupValue(ctx, val);        /* the new value (OVERWRITE); UNDEFINED for DELETE */
    e->cur_valid = 1;
    e->vref = NULL;
    e->is_gendata = 0;
    e->is_async = 0; e->mod = NULL; e->a_base = e->a_cur = NULL;
    e->is_map = 1;
    e->map_op = (op == JS_MAP_MUTATE_DELETE) ? COW_MAP_DELETE : COW_MAP_OVERWRITE;
    e->map_old = JS_DupValue(ctx, old_val);   /* the prior value, restored on unapply */
    g_capturing = 0;
}

/* Capture a shared async object's SETTLEMENT before this flow changes it (JSTimeTravelHooks.async_settle). A
   promise created before a fork is baseline state whose settle is a write no property hook can see, and the
   resolving function's already_resolved latch is the other half of it: without capturing the latch, the first
   arm to call the shared `resolve` wins and every sibling's call returns silently, so a value that differs per
   arm is lost rather than isolated. Flow-private promises (created after the fork) are skipped by the same
   generational test as every other capture — nothing else can observe them. */
void cow_capture_async_settle(JSContext *ctx, JSValueConst obj) {
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
        if (d->e[i].is_async && JS_VALUE_GET_PTR(d->e[i].obj) == JS_VALUE_GET_PTR(obj)) {
            JS_AsyncStateFree(JS_GetRuntime(ctx), blob);
            g_capturing = 0;
            return;
        }
    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta (async_settle) — a lost settlement leaks a flow's timeline");
        cow_hash_rebuild(d);
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_ATOM_NULL; e->existed = 0;
    e->base = JS_UNDEFINED; e->cur = JS_UNDEFINED; e->cur_valid = 0;
    e->vref = NULL;
    e->is_gendata = 0; e->g0 = e->g1 = NULL;
    e->is_map = 0; e->map_op = 0; e->map_old = JS_UNDEFINED;
    e->is_async = 1; e->mod = NULL; e->a_base = blob; e->a_cur = NULL;
    g_capturing = 0;
}

/* Capture a MODULE's evaluation state before this flow changes it (JSTimeTravelHooks.module_eval). Each flow is
   a possible world and evaluates the module once in it — the record's status/capability decide whether a flow
   evaluates at all, so leaving them baseline let the FIRST flow's evaluation stand for every sibling while the
   bindings it wrote stayed private to it. The sibling then read exports nothing had written in its world. There
   is no flow-private skip here: a module record is reachable from the realm's module map the moment it loads, so
   it is shared state whoever created it. */
void cow_capture_module_eval(JSContext *ctx, void *mod) {
    if (g_capturing || !g_current || !mod) return;
    CowDelta *d = g_current;
    for (int i = 0; i < d->n; i++)                  /* one entry per module: the FIRST baseline is the baseline */
        if (d->e[i].is_async && d->e[i].mod == mod) return;
    g_capturing = 1;
    void *blob = JS_ModuleEvalStateSave(ctx, mod);
    CHECK(blob, "cow: a module's evaluation state could not be captured — a sibling flow would inherit this "
                "flow's evaluation and read exports it never wrote");
    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta (module_eval)");
        cow_hash_rebuild(d);
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_UNDEFINED;
    e->atom = JS_ATOM_NULL; e->existed = 0;
    e->base = JS_UNDEFINED; e->cur = JS_UNDEFINED; e->cur_valid = 0;
    e->vref = NULL;
    e->is_gendata = 0; e->g0 = e->g1 = NULL;
    e->is_map = 0; e->map_op = 0; e->map_old = JS_UNDEFINED;
    e->is_async = 1; e->mod = mod; e->a_base = blob; e->a_cur = NULL;
    g_capturing = 0;
}

void cow_capture_varref(JSContext *ctx, void *vref) {
    if (g_capturing || !g_current || !vref) return;
    CowDelta *d = g_current;
    if (cow_hash_find(d, NULL, 0, vref) >= 0) return;   /* capture each cell ONCE (O(1)) */
    g_capturing = 1;
    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta (var_ref)");
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_UNDEFINED; e->atom = JS_ATOM_NULL; e->existed = 0;
    e->base = JS_VarRefGetValue(vref);   /* owned dup of the cell's current (baseline) value */
    e->cur = JS_UNDEFINED; e->cur_valid = 0;
    e->vref = vref;                      /* the cell is kept alive by the shared function object */
    e->is_gendata = 0;
    e->is_async = 0; e->mod = NULL; e->a_base = e->a_cur = NULL;
    e->is_map = 0;
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
    if (d->n >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->e = realloc(d->e, (size_t)d->cap * sizeof(CowEntry));
        CHECK(d->e, "cow: OOM growing delta (gendata) — a lost generator swap corrupts per-flow state");
    }
    CowEntry *e = &d->e[d->n++];
    e->obj = JS_DupValue(ctx, genobj);
    e->atom = JS_ATOM_NULL; e->existed = 0; e->base = JS_UNDEFINED;
    e->cur = JS_UNDEFINED; e->cur_valid = 0; e->vref = NULL;
    e->is_gendata = 1; e->g0 = base_gd; e->g1 = cur_gd;
    e->is_async = 0; e->mod = NULL; e->a_base = e->a_cur = NULL;   /* adopts cur_gd's creation ref (freed on delta free) */
    e->is_map = 0;
}

void cow_unapply(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    g_capturing = 1;
    for (int i = d->n - 1; i >= 0; i--) {   /* reverse: symmetric with apply */
        CowEntry *e = &d->e[i];
        if (e->is_gendata) { JS_SetObjGenData(e->obj, e->g0); continue; }   /* restore the object-owned original */
        if (e->is_async) {   /* blob state: save what THIS flow produced, then rewind to the baseline */
            cow_state_free(JS_GetRuntime(ctx), e, e->a_cur);
            e->a_cur = cow_state_save(ctx, e);
            cow_state_restore(ctx, e, e->a_base);
            continue;
        }
        if (e->is_map) {   /* invert the flow's record mutation: ADD->delete, OVERWRITE/DELETE->restore old value */
            if (e->map_op == COW_MAP_ADD) JS_MapDeleteRecord(ctx, e->obj, e->base);
            else JS_MapAddRecord(ctx, e->obj, e->base, e->map_old);
            continue;
        }
        if (e->vref) {                        /* closure cell: save its current value, restore the baseline */
            if (e->cur_valid) JS_FreeValue(ctx, e->cur);
            e->cur = JS_VarRefGetValue(e->vref); e->cur_valid = 1;
            JS_VarRefSetValue(ctx, e->vref, JS_DupValue(ctx, e->base));
            continue;
        }
        JSValue cur;                          /* save the flow's CURRENT value so apply can restore it */
        JS_GetOwnSlot(ctx, &cur, e->obj, e->atom);
        if (e->cur_valid) JS_FreeValue(ctx, e->cur);
        e->cur = cur;
        e->cur_valid = 1;
        uint32_t ai;
        if (e->existed) JS_SetProperty(ctx, e->obj, e->atom, JS_DupValue(ctx, e->base));   /* -> baseline */
        else if (JS_IsArrayIndexSlot(e->obj, e->atom, &ai))
            /* flow-CREATED array append: TRUNCATE the array to this index (frees the tail), removing it. Entries
               are processed in reverse (highest index first), so a contiguous run of appends shrinks one-by-one
               back to the baseline length. cur was saved above so apply re-appends it. A JS_DeleteProperty would
               convert the fast array to slow and leave the element in place. */
            JS_ArraySetLength(ctx, e->obj, ai);
        else JS_DeleteProperty(ctx, e->obj, e->atom, 0);
    }
    g_capturing = 0;
}

void cow_apply(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    g_capturing = 1;
    for (int i = 0; i < d->n; i++) {   /* forward: replay the flow's writes over the pristine baseline */
        CowEntry *e = &d->e[i];
        if (e->is_gendata) { JS_SetObjGenData(e->obj, e->g1); continue; }   /* install this flow's generator clone */
        if (e->is_async) { cow_state_restore(ctx, e, e->a_cur); continue; }   /* re-install what this flow produced */
        if (e->is_map) {   /* replay the flow's record mutation: ADD/OVERWRITE->set new value, DELETE->delete */
            if (e->map_op == COW_MAP_DELETE) JS_MapDeleteRecord(ctx, e->obj, e->base);
            else JS_MapAddRecord(ctx, e->obj, e->base, e->cur);
            continue;
        }
        if (!e->cur_valid) continue;
        if (e->vref) JS_VarRefSetValue(ctx, e->vref, JS_DupValue(ctx, e->cur));   /* closure cell */
        else JS_SetProperty(ctx, e->obj, e->atom, JS_DupValue(ctx, e->cur));
    }
    g_capturing = 0;
}

/* Fork a delta at a branch (cow.h contract): an INDEPENDENT copy of src's captured slots whose restore point
   is src's CURRENT (branch-point) heap/cell values, so the snapshot-forked sibling inherits src's branch-point
   state then each diverges on its own writes. A full copy (not an O(1) shared base segment) — sound and simple;
   the two deltas share NO mutable state, so a sibling's later capture/apply can never corrupt the other. */
CowDelta *cow_delta_fork(JSContext *ctx, CowDelta *src) {
    CowDelta *d = cow_delta_new();
    /* Both flows now SHARE every object that existed at THIS fork (the frame snapshot dup'd their heap refs), so
       both must capture writes to any object with flow_gen <= this generation. Record the CURRENT generation as
       both deltas' fork_gen, then BUMP it so objects created after the fork get a strictly-higher generation and
       are correctly treated as flow-private. Updating the parent's fork_gen (it may be > its previous value) is
       correct: the parent must isolate from its NEWEST sibling, which shares everything up to now. */
    src->fork_gen = d->fork_gen = JS_FlowGen();
    JS_FlowBumpGen();
    if (src->n == 0) return d;
    d->e = malloc((size_t)src->n * sizeof(CowEntry)); CHECK(d->e, "cow: OOM fork");
    d->cap = src->n; d->n = src->n;
    g_capturing = 1;
    for (int i = 0; i < src->n; i++) {
        CowEntry *se = &src->e[i], *de = &d->e[i];
        /* ZEROED FIRST, so a branch below states only the fields ITS kind uses. Each branch used to spell out
           every field of every other kind, which made adding one an obligation at four sites — and the fifth
           kind's flag was left as malloc garbage at two of them, so a plain property slot was replayed as an
           async settlement and dereferenced a wild blob. A zeroed JSValue is the integer 0, which is
           non-refcounted and safe to free, so this is a valid empty entry and not just a memset. */
        memset(de, 0, sizeof *de);
        if (se->is_gendata) {   /* generator swap: the sibling shares the SAME per-flow clone (a ref), same toggle */
            de->obj = JS_DupValue(ctx, se->obj);
            de->is_gendata = 1; de->g0 = se->g0; de->g1 = se->g1;
            JS_GenDataRef(se->g1);   /* the copy holds its own ownership ref on the clone */
            continue;
        }
        if (se->is_async) {   /* blob state: the sibling inherits its OWN copy of both blobs — the two deltas
                                 must share no mutable state, and a blob is mutable (unapply rewrites cur) */
            de->obj = JS_DupValue(ctx, se->obj);
            de->is_async = 1;
            de->mod = se->mod;
            de->a_base = cow_state_clone(ctx, se, se->a_base);
            /* the sibling's restore point is the branch-point state, which is LIVE right now (src's delta is
               applied at a fork) — read it rather than copying src's cur, which is only valid after an unapply
               has written it. */
            de->a_cur = cow_state_save(ctx, de);
            continue;
        }
        if (se->is_map) {   /* Set/Map record mutation: the sibling inherits the same reversible undo-log entry */
            de->is_map = 1;
            de->obj = JS_DupValue(ctx, se->obj);
            de->base = JS_DupValue(ctx, se->base);   /* key */
            de->cur = JS_DupValue(ctx, se->cur);     /* new value */
            de->cur_valid = 1;
            de->map_op = se->map_op;
            de->map_old = JS_DupValue(ctx, se->map_old);   /* prior value (OVERWRITE/DELETE) */
            continue;
        }
        de->obj = JS_DupValue(ctx, se->obj);
        de->atom = JS_DupAtom(ctx, se->atom);
        de->existed = se->existed;
        de->base = JS_DupValue(ctx, se->base);
        de->vref = se->vref;
        /* Snapshot the CURRENT live value as the clone's restore point: at a branch the src flow's delta is
           APPLIED, so the heap/cell holds its branch-point value. Applying the clone later reproduces exactly
           that state for the forked sibling; the two then diverge, each capturing its own further writes. */
        if (se->vref) { de->cur = JS_VarRefGetValue(se->vref); de->cur_valid = 1; continue; }
        JS_GetOwnSlot(ctx, &de->cur, se->obj, se->atom);
        de->cur_valid = 1;
    }
    cow_hash_rebuild(d);   /* build the clone's O(1) dedup index over the copied entries */
    g_capturing = 0;
    return d;
}

void cow_delta_free(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    for (int i = 0; i < d->n; i++) {
        if (d->e[i].is_gendata) { JS_FreeValue(ctx, d->e[i].obj); JS_GenDataUnref(ctx, d->e[i].g1); continue; }
        if (d->e[i].is_async) {
            JS_FreeValue(ctx, d->e[i].obj);
            cow_state_free(JS_GetRuntime(ctx), &d->e[i], d->e[i].a_base);
            cow_state_free(JS_GetRuntime(ctx), &d->e[i], d->e[i].a_cur);
            continue;
        }
        if (d->e[i].is_map) {   /* obj + key(base) + new(cur) + old(map_old); atom is NULL */
            JS_FreeValue(ctx, d->e[i].obj);
            JS_FreeValue(ctx, d->e[i].base);
            JS_FreeValue(ctx, d->e[i].cur);
            JS_FreeValue(ctx, d->e[i].map_old);
            continue;
        }
        JS_FreeValue(ctx, d->e[i].obj);
        JS_FreeAtom(ctx, d->e[i].atom);
        JS_FreeValue(ctx, d->e[i].base);
        if (d->e[i].cur_valid) JS_FreeValue(ctx, d->e[i].cur);
    }
    free(d->e);
    free(d->hash);
    free(d);
}

void cow_free(JSContext *ctx) { (void)ctx; g_current = NULL; }
