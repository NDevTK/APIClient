/* Per-flow swappable COW delta — see cow.h. */
#include "solver/cow.h"
#include "check.h"
#include <stdlib.h>

/* One captured baseline slot: base = the value the shared baseline holds (restored on unapply); cur = the
   running flow's latest value for it (saved on unapply, replayed on apply). existed = did the baseline own the
   slot (else the flow CREATED it, so unapply deletes and apply re-creates). */
/* A delta entry is EITHER a property slot (vref==NULL: obj/atom/existed) OR a closure cell (vref!=NULL: the
   shared JSVarRef, whose value is read/written via JS_VarRefGet/SetValue; obj/atom/existed unused). base/cur
   hold the baseline/flow values either way. */
typedef struct { JSValue obj; JSAtom atom; int existed; JSValue base; JSValue cur; int cur_valid; void *vref; } CowEntry;

/* forked = has this flow snapshot-forked a sibling yet? Until it has, every object it CREATED (flow_local) is
   truly private — no other flow can observe it — so capturing its mutations is pure waste (keeps the delta
   O(shared baseline touched), not O(the run's transients). Once forked, the snapshot shares the frame's
   flow_local objects with the sibling, so their mutations become cross-flow state and MUST be captured. */
/* fork_gen = the fork GENERATION this flow last forked at (0 = never forked). An object is SHARED with a sibling
   iff it existed at that fork (JS_ObjFlowGen(obj) <= fork_gen); an object created AFTER (flow_gen > fork_gen) is
   flow-PRIVATE — never captured, keeping the delta O(shared-state-touched), not O(the run's transients). A single
   "forked" bit could not distinguish a pre-fork shared object from a post-fork private one, so it over-captured
   every post-fork transient (a delta bloat that violates the O(shared-state) invariant). */
struct CowDelta { CowEntry *e; int n, cap; uint32_t fork_gen; };

static CowDelta *g_current = NULL;   /* the flow whose writes are captured right now (scheduler-owned) */
static int g_capturing = 0;          /* re-entrancy guard: our own reads/restores must not re-capture */

CowDelta *cow_delta_new(void) {
    CowDelta *d = calloc(1, sizeof *d);
    CHECK(d, "cow: OOM new delta");
    return d;
}

void cow_set_current(CowDelta *d) { g_current = d; }

void cow_capture_hook(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    if (g_capturing || !g_current) return;
    CowDelta *d = g_current;
    /* FLOW-PRIVATE skip — the O(shared-state) invariant. An object created AFTER this flow's fork (flow_gen >
       fork_gen) is private to the flow: no sibling can observe it, so its writes are never captured. A baseline
       object (flow_gen 0) and any object that existed at the fork (flow_gen <= fork_gen) IS shared and captured. */
    if (JS_ObjFlowGen(obj) > d->fork_gen) return;
    for (int i = 0; i < d->n; i++)   /* capture each slot ONCE — first write is the true baseline */
        if (JS_VALUE_GET_PTR(d->e[i].obj) == JS_VALUE_GET_PTR(obj) && d->e[i].atom == atom) return;

    g_capturing = 1;
    JSPropertyDescriptor pd;
    int has = JS_GetOwnProperty(ctx, &pd, obj, atom);   /* 1 = own prop (pd filled), 0 = absent, -1 = exc */
    int existed = 0; JSValue base = JS_UNDEFINED;
    if (has > 0) { existed = 1; base = pd.value; JS_FreeValue(ctx, pd.getter); JS_FreeValue(ctx, pd.setter); }

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
    g_capturing = 0;
}

/* Record a CLOSURE CELL's pre-write value (JSCowVarRefHook) into the running flow's delta, so a snapshot-forked
   sibling that shares the cell is isolated on write. Captured once per cell (first write is the baseline). */
/* Record a KNOWN-NEW fast-array APPEND slot (JSTimeTravelHooks.arr_append): the index == the array's current
   length, so it cannot already be in the delta (dedup unneeded) and its baseline is ABSENT (existed=0, no
   JS_GetOwnProperty). O(1) — this is the accumulator hot path (a shared array built one element at a time);
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
    g_capturing = 0;
}

void cow_capture_varref(JSContext *ctx, void *vref) {
    if (g_capturing || !g_current || !vref) return;
    CowDelta *d = g_current;
    for (int i = 0; i < d->n; i++) if (d->e[i].vref == vref) return;
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
    g_capturing = 0;
}

void cow_unapply(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    g_capturing = 1;
    for (int i = d->n - 1; i >= 0; i--) {   /* reverse: symmetric with apply */
        CowEntry *e = &d->e[i];
        if (e->vref) {                        /* closure cell: save its current value, restore the baseline */
            if (e->cur_valid) JS_FreeValue(ctx, e->cur);
            e->cur = JS_VarRefGetValue(e->vref); e->cur_valid = 1;
            JS_VarRefSetValue(ctx, e->vref, JS_DupValue(ctx, e->base));
            continue;
        }
        JSPropertyDescriptor pd;              /* save the flow's CURRENT value so apply can restore it */
        int has = JS_GetOwnProperty(ctx, &pd, e->obj, e->atom);
        if (e->cur_valid) JS_FreeValue(ctx, e->cur);
        if (has > 0) { e->cur = pd.value; JS_FreeValue(ctx, pd.getter); JS_FreeValue(ctx, pd.setter); }
        else e->cur = JS_UNDEFINED;
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
        de->obj = JS_DupValue(ctx, se->obj);
        de->atom = JS_DupAtom(ctx, se->atom);
        de->existed = se->existed;
        de->base = JS_DupValue(ctx, se->base);
        de->vref = se->vref;
        /* Snapshot the CURRENT live value as the clone's restore point: at a branch the src flow's delta is
           APPLIED, so the heap/cell holds its branch-point value. Applying the clone later reproduces exactly
           that state for the forked sibling; the two then diverge, each capturing its own further writes. */
        if (se->vref) { de->cur = JS_VarRefGetValue(se->vref); de->cur_valid = 1; continue; }
        JSPropertyDescriptor pd;
        int has = JS_GetOwnProperty(ctx, &pd, se->obj, se->atom);
        if (has > 0) { de->cur = pd.value; JS_FreeValue(ctx, pd.getter); JS_FreeValue(ctx, pd.setter); }
        else de->cur = JS_UNDEFINED;
        de->cur_valid = 1;
    }
    g_capturing = 0;
    return d;
}

void cow_delta_free(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    for (int i = 0; i < d->n; i++) {
        JS_FreeValue(ctx, d->e[i].obj);
        JS_FreeAtom(ctx, d->e[i].atom);
        JS_FreeValue(ctx, d->e[i].base);
        if (d->e[i].cur_valid) JS_FreeValue(ctx, d->e[i].cur);
    }
    free(d->e);
    free(d);
}

void cow_free(JSContext *ctx) { (void)ctx; g_current = NULL; }
