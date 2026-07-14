/* Per-flow HEAP COW delta — see heap_cow.h. The JS-heap twin of dom_cow.c. */
#include <string.h>
#include "check.h"          /* CHECK/DCHECK — a dropped/mis-sequenced entry silently corrupts flow isolation */
#include "quickjs.h"
#include "solver/heap_cow.h"

/* A delta entry. kind:
   COW_MUTATE  — a mutation of an existing own data property (slot!=NULL: a STABLE closure var-ref pvalue, never
                 moved by realloc; else identified by (obj,atom) and re-resolved pointer-safe every context-switch).
   COW_CREATE  — this flow CREATED the property (unapply DELETES it, apply RE-CREATES with cur).
   COW_DELETE  — the INVERSE of a creation (apply DELETES, unapply RE-CREATES from base): seeds a candidate/boot
                 flow's delta with the boot-undo, so a boot-created global is ABSENT in the flow yet restored on
                 suspend/revert. e->base holds its post-boot value across every toggle.
   base = the value UNDER this flow's write (held while APPLIED); cur = this flow's value (held while PARKED). */
enum { COW_MUTATE = 0, COW_CREATE = 1, COW_DELETE = 2 };
typedef struct { JSValue *slot; JSValue base; JSValue cur; JSValue obj; JSAtom atom; int kind; } CowEntry;

/* A flow's delta = the mutable HEAD (g_head) layered over a chain of IMMUTABLE, refcounted, structurally-shared
   base SEGMENTS (g_base) — the persistent-versioned heap. A FORK freezes the head into a segment BOTH the running
   flow and a snapshot-forked sibling reference (refcount 2), so the sibling shares the parent's O(N) delta in
   O(1). The base stays applied as the common prefix; a context-switch unapplies the outgoing chain and applies
   the incoming one. NULL base == flat behaviour (behaviour-preserving until a fork sets g_base). */
typedef struct CowSeg { CowEntry *e; int n; struct CowSeg *base; int refcount; } CowSeg;
static CowEntry *g_head = NULL;
static int g_head_n = 0, g_head_cap = 0;
static CowSeg *g_base = NULL;
static void seg_unref(JSContext *ctx, CowSeg *s);

/* grow + return the next head entry; NEVER NULL — OOM here silently breaks flow isolation, so it is FATAL. */
static CowEntry *head_slot(JSRuntime *rt) {
    if (g_head_n >= g_head_cap) {
        int nc = g_head_cap ? g_head_cap * 2 : 1024;
        CowEntry *nu = (CowEntry *)js_realloc_rt(rt, g_head, (size_t)nc * sizeof(CowEntry));
        CHECK(nu, "heap-cow-oom: COW head realloc failed — a dropped capture leaks this flow's write into the next flow (silent isolation corruption)");
        g_head = nu; g_head_cap = nc;
    }
    CowEntry *e = &g_head[g_head_n++];
    e->slot = NULL; e->base = JS_UNDEFINED; e->cur = JS_UNDEFINED; e->obj = JS_UNDEFINED; e->atom = JS_ATOM_NULL; e->kind = COW_MUTATE;
    return e;
}
/* Re-resolve a (obj,atom) mutation entry's value slot at USE time (pointer-safe across property-array realloc);
   NULL if the property vanished. The ONE quickjs-internal reach, wrapped in JS_CowPropSlot. */
static JSValue *reslot(JSContext *ctx, CowEntry *e) { return JS_CowPropSlot(ctx, e->obj, e->atom); }

/* per-entry UNAPPLY (flow -> parked): stash the flow's value into cur, restore the baseline under it. */
static void unapply_entry(JSContext *ctx, CowEntry *e) {
    if (e->slot) { e->cur = *e->slot; *e->slot = e->base; e->base = JS_UNDEFINED; }
    else if (e->kind == COW_CREATE) { e->cur = JS_GetProperty(ctx, e->obj, e->atom); JS_DeleteProperty(ctx, e->obj, e->atom, 0); }
    else if (e->kind == COW_DELETE) { JS_SetProperty(ctx, e->obj, e->atom, JS_DupValue(ctx, e->base)); }   /* baseline HAS it -> re-create post-boot value */
    else { JSValue *s = reslot(ctx, e); if (s) { e->cur = *s; *s = e->base; e->base = JS_UNDEFINED; }
           else { JS_FreeValue(ctx, e->base); e->base = JS_UNDEFINED; } }
}
/* per-entry APPLY (parked -> flow): restore the flow's value over the baseline. */
static void apply_entry(JSContext *ctx, CowEntry *e) {
    if (e->slot) { e->base = *e->slot; *e->slot = e->cur; e->cur = JS_UNDEFINED; }
    else if (e->kind == COW_CREATE) { JS_SetProperty(ctx, e->obj, e->atom, e->cur); e->cur = JS_UNDEFINED; }
    else if (e->kind == COW_DELETE) { JS_DeleteProperty(ctx, e->obj, e->atom, 0); }   /* flow state = absent */
    else { JSValue *s = reslot(ctx, e); if (s) { e->base = *s; *s = e->cur; e->cur = JS_UNDEFINED; }
           else { JS_FreeValue(ctx, e->cur); e->cur = JS_UNDEFINED; } }
}
/* apply a base chain FORWARD (deepest ancestor first, then up); unapply is the mirror. NULL-safe (flat delta). */
static void apply_seg(JSContext *ctx, CowSeg *s) { if (!s) return; apply_seg(ctx, s->base); for (int i = 0; i < s->n; i++) apply_entry(ctx, &s->e[i]); }
static void unapply_seg(JSContext *ctx, CowSeg *s) { if (!s) return; for (int i = s->n - 1; i >= 0; i--) unapply_entry(ctx, &s->e[i]); unapply_seg(ctx, s->base); }
/* drop a chain reference: refcount--, free the segment's entries (parked: base+cur held) at 0, recurse. */
static void seg_unref(JSContext *ctx, CowSeg *s) {
    while (s) {
        DCHECK(s->refcount > 0, "heap_cow: seg_unref on a zero-refcount segment — double free of a shared base");
        if (--s->refcount > 0) break;
        CowSeg *base = s->base;
        for (int i = 0; i < s->n; i++) { CowEntry *e = &s->e[i]; JS_FreeValue(ctx, e->base); JS_FreeValue(ctx, e->cur); if (!e->slot) { JS_FreeValue(ctx, e->obj); JS_FreeAtom(ctx, e->atom); } }
        js_free_rt(JS_GetRuntime(ctx), s->e); js_free_rt(JS_GetRuntime(ctx), s);
        s = base;
    }
}

/* ── CAPTURE IMPL — installed as the engine's capture hooks (heap_cow_init). The quickjs write-site dispatchers
   already gated on g_cow_active, so these record UNCONDITIONALLY (the boot-inverse seed also calls them, under an
   asserted-active invariant). They reach object internals only through JS_ObjCowCapturable / JS_CowPropSlot. */
static void heap_cow_cap_slot(JSRuntime *rt, JSValue *slot) {   /* STABLE slot (closure var-ref pvalue) */
    CowEntry *e = head_slot(rt);
    e->slot = slot;
    e->base = JS_DupValueRT(rt, *slot);   /* the under value, captured BEFORE the write overwrites */
}
static void heap_cow_cap_prop(JSContext *ctx, JSValueConst obj, JSAtom atom) {   /* mutation of an existing own property */
    if (!JS_ObjCowCapturable(obj)) return;   /* flow-private / host-ledger / still-constructing -> never capture */
    JSValue *slot = JS_CowPropSlot(ctx, obj, atom);
    if (!slot) return;                        /* not a plain own data property -> nothing to isolate */
    CowEntry *e = head_slot(JS_GetRuntime(ctx));
    e->obj = JS_DupValue(ctx, obj); e->atom = JS_DupAtom(ctx, atom); e->base = JS_DupValue(ctx, *slot);
}
static void heap_cow_cap_create(JSContext *ctx, JSValueConst obj, JSAtom atom) {   /* a CREATED property */
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && !JS_ObjCowCapturable(obj)) return;   /* same policy as a mutation */
    CowEntry *e = head_slot(JS_GetRuntime(ctx));
    e->obj = JS_DupValue(ctx, obj); e->atom = JS_DupAtom(ctx, atom); e->kind = COW_CREATE;
}

/* ── VERB-API (scheduler): swap the running flow's heap writes. Capture is suspended across the internal
   apply/unapply (JS_CowSetActive(0)) — else re-applying a CREATION would re-enter the capture dispatcher and
   record a spurious head entry. */
void heap_cow_unapply(JSContext *ctx) {   /* flow -> parked: head reverse, then the shared base chain */
    int sv = JS_CowGetActive(); JS_CowSetActive(0);
    for (int i = g_head_n - 1; i >= 0; i--) unapply_entry(ctx, &g_head[i]);
    unapply_seg(ctx, g_base);
    JS_CowSetActive(sv);
}
void heap_cow_apply(JSContext *ctx) {     /* parked -> flow: base chain forward, then the head on top */
    int sv = JS_CowGetActive(); JS_CowSetActive(0);
    apply_seg(ctx, g_base);
    for (int i = 0; i < g_head_n; i++) apply_entry(ctx, &g_head[i]);
    JS_CowSetActive(sv);
}
void heap_cow_revert(JSContext *ctx) {    /* running APPLIED flow ends: drop head writes + free them, restore baseline, drop one base ref */
    int sv = JS_CowGetActive(); JS_CowSetActive(0);
    for (int i = g_head_n - 1; i >= 0; i--) {
        CowEntry *e = &g_head[i];
        if (e->slot) { JS_FreeValue(ctx, *e->slot); *e->slot = e->base; }
        else if (e->kind == COW_CREATE) { JS_DeleteProperty(ctx, e->obj, e->atom, 0); JS_FreeValue(ctx, e->obj); JS_FreeAtom(ctx, e->atom); }
        else if (e->kind == COW_DELETE) { JS_SetProperty(ctx, e->obj, e->atom, e->base); JS_FreeValue(ctx, e->obj); JS_FreeAtom(ctx, e->atom); }   /* consumes e->base */
        else { JSValue *s = reslot(ctx, e); if (s) { JS_FreeValue(ctx, *s); *s = e->base; } else JS_FreeValue(ctx, e->base);
               JS_FreeValue(ctx, e->obj); JS_FreeAtom(ctx, e->atom); }
    }
    g_head_n = 0;
    unapply_seg(ctx, g_base);
    seg_unref(ctx, g_base); g_base = NULL;
    JS_CowSetActive(sv);
}
void *heap_cow_fork(JSContext *ctx) {   /* freeze the applied HEAD into a shared immutable base (refcount 2), continue byte-identically */
    DCHECK(g_head_n >= 0, "heap_cow_fork: negative head count");
    int sv = JS_CowGetActive(); JS_CowSetActive(0);
    for (int i = g_head_n - 1; i >= 0; i--) unapply_entry(ctx, &g_head[i]);   /* head applied -> parked (values into cur) */
    CowSeg *seg = (CowSeg *)js_malloc_rt(JS_GetRuntime(ctx), sizeof(CowSeg));
    CHECK(seg, "heap_cow_fork: segment alloc failed — a shared delta would be corrupted");
    seg->e = g_head; seg->n = g_head_n; seg->base = g_base; seg->refcount = 2;   /* running flow + the sibling */
    g_head = NULL; g_head_n = 0; g_head_cap = 0;   /* fresh empty head for the running flow */
    g_base = seg;
    for (int i = 0; i < seg->n; i++) apply_entry(ctx, &seg->e[i]);   /* re-apply head -> running flow continues */
    JS_CowSetActive(sv);
    return seg;
}
void *heap_cow_buf_take(int *n, int *cap) { void *b = g_head; *n = g_head_n; *cap = g_head_cap; g_head = NULL; g_head_n = 0; g_head_cap = 0; return b; }
void heap_cow_buf_load(void *buf, int n, int cap) { g_head = (CowEntry *)buf; g_head_n = n; g_head_cap = cap; }
void *heap_cow_base_take(void) { void *b = g_base; g_base = NULL; return b; }
void heap_cow_base_load(void *base) { g_base = (CowSeg *)base; }
/* Add ONE reference to a base chain's top segment (the whole document-script flow's delta becomes a shared base
   that every seeded orphan forks from — each orphan calls this so the chain outlives all of them; released by
   heap_cow_base_free per flow). No-op on a NULL (flat) base. */
void heap_cow_base_ref(void *base) { if (base) ((CowSeg *)base)->refcount++; }
void heap_cow_base_free(JSContext *ctx, void *base) { if (base) seg_unref(ctx, (CowSeg *)base); }
/* free an EVICTED parked head buffer: one of base/cur is held (applied/parked); create entries hold obj+atom. */
void heap_cow_buf_free(JSContext *ctx, void *buf, int n) {
    if (!buf) return;
    CowEntry *u = (CowEntry *)buf;
    for (int i = 0; i < n; i++) {
        JS_FreeValue(ctx, u[i].base); JS_FreeValue(ctx, u[i].cur);
        if (!u[i].slot) { JS_FreeValue(ctx, u[i].obj); JS_FreeAtom(ctx, u[i].atom); }
    }
    js_free_rt(JS_GetRuntime(ctx), buf);
}
/* MERGE the running active head INTO a host boot-delta buffer (dst,*pn,*pcap): append its entries, then DETACH
   the head (entries MOVED, ownership transferred). A post-boot lazy chunk runs COW-active at baseline mark, so
   its global creations/mutations extend the ONE canonical boot delta exactly like boot's own globals. */
void *heap_cow_boot_delta_merge(JSContext *ctx, void *dst, int *pn, int *pcap) {
    if (g_head_n == 0) return dst;
    int need = *pn + g_head_n;
    if (need > *pcap) {
        int nc = *pcap ? *pcap : 1024;
        while (nc < need) nc *= 2;
        CowEntry *nd = (CowEntry *)js_realloc_rt(JS_GetRuntime(ctx), dst, (size_t)nc * sizeof(CowEntry));
        CHECK(nd, "heap-cow-merge-oom: boot-delta merge realloc failed — a chunk's baseline globals would be silently lost");
        dst = nd; *pcap = nc;
    }
    memcpy((CowEntry *)dst + *pn, g_head, (size_t)g_head_n * sizeof(CowEntry));
    *pn = need;
    js_free_rt(JS_GetRuntime(ctx), g_head); g_head = NULL; g_head_n = 0; g_head_cap = 0;   /* entries moved; free the shell */
    return dst;
}
/* Seed the RUNNING flow's delta with the INVERSE of the (still-applied) boot delta: every boot MUTATION is
   undone (recording an applied entry whose base = the post-boot value, slot/prop now = pre-boot) and every boot
   CREATION becomes a COW_DELETE entry (property removed now, base = its post-boot value). The heap lands at
   PRE-boot; heap_cow_unapply/revert restore the post-boot baseline automatically. Iterated in REVERSE (boot's
   own unapply order). boot_buf is only READ (stays the canonical post-boot baseline for non-candidate flows). */
void heap_cow_seed_boot_inverse(JSContext *ctx, void *boot_buf, int boot_n) {
    DCHECK(JS_CowGetActive(), "heap_cow_seed_boot_inverse: capture must be ACTIVE — else the pre-boot heap mutations record no undo entries -> irreversible corruption");
    CowEntry *boot = (CowEntry *)boot_buf;
    for (int i = boot_n - 1; i >= 0; i--) {
        CowEntry *b = &boot[i];
        if (b->slot) {
            heap_cow_cap_slot(JS_GetRuntime(ctx), b->slot);              /* applied entry: base = *slot (post-boot) */
            JS_FreeValue(ctx, *b->slot); *b->slot = JS_DupValue(ctx, b->base);   /* slot -> pre-boot */
        } else if (b->kind == COW_CREATE) {
            CowEntry *e = head_slot(JS_GetRuntime(ctx));
            e->obj = JS_DupValue(ctx, b->obj); e->atom = JS_DupAtom(ctx, b->atom); e->kind = COW_DELETE;
            e->base = JS_GetProperty(ctx, b->obj, b->atom);             /* post-boot value to re-create on unapply */
            JS_DeleteProperty(ctx, b->obj, b->atom, 0);                 /* -> pre-boot (global absent) */
        } else if (b->kind == COW_MUTATE) {
            if (JS_VALUE_GET_TAG(b->obj) != JS_TAG_OBJECT) continue;
            heap_cow_cap_prop(ctx, b->obj, b->atom);                    /* applied entry: base = current (post-boot) prop */
            JSValue *s = JS_CowPropSlot(ctx, b->obj, b->atom);
            if (s) { JS_FreeValue(ctx, *s); *s = JS_DupValue(ctx, b->base); }   /* -> pre-boot */
        }
    }
}

void heap_cow_init(JSContext *ctx) {
    (void)ctx;
    JS_SetCowCaptureHooks(heap_cow_cap_slot, heap_cow_cap_prop, heap_cow_cap_create);   /* install the write-site capture impl */
}
