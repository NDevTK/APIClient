#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "quickjs.h"
#include "solver/realm_holders.h"
#include "solver/cow.h"
#include "solver/flow.h"
#include "core/frame/navigable.h"
#include "core/dom/document.h"

/* One frozen segment's answer: the realms whose Window its entries name, as a bitset over the census's realm
   order. Segments are shared by families, so each is walked once per census and its bits are ORed into every
   member that stands on it. */
typedef struct {
    const void *seg;
    uint64_t   *bits;
} SegMemo;

/* One of a live realm's handles — its global object (the Window), its WindowProxy, or its Document object —
   and the realm's index in navigable.h's list. A delta naming any of them keeps the realm reachable. */
typedef struct {
    const void *global;
    int         realm;
} RealmGlobal;

typedef struct {
    RealmGlobal *globals;   /* every live realm's handles, sorted by address */
    int          n, n_globals, words;
    SegMemo     *memo;
    size_t       memo_cap;   /* a power of two above twice the live segment count */
    uint64_t    *acc;        /* the member being walked */
    uint64_t    *cur;        /* the bits of the segment being walked, NULL for the head */
    const void  *cur_seg;
    bool         refused;    /* an allocation was refused, so the census is absent */
} HolderWalk;


static int global_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const RealmGlobal *)a)->global, y = (uintptr_t)((const RealmGlobal *)b)->global;
    return x < y ? -1 : x > y;
}

/* The census index of the realm whose global object `p` is, or -1. */
static int realm_of_global(const HolderWalk *w, const void *p) {
    int lo = 0, hi = w->n_globals - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (w->globals[mid].global == p) return w->globals[mid].realm;
        if ((uintptr_t)w->globals[mid].global < (uintptr_t)p) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static SegMemo *memo_slot(HolderWalk *w, const void *seg) {
    size_t i = ((uintptr_t)seg >> 4) & (w->memo_cap - 1);
    while (w->memo[i].seg != NULL && w->memo[i].seg != seg) i = (i + 1) & (w->memo_cap - 1);
    return &w->memo[i];
}

static bool holder_enter(const void *seg, void *arg) {
    HolderWalk *w = arg;
    SegMemo *m = memo_slot(w, seg);
    if (m->seg == seg) {
        for (int i = 0; i < w->words; i++) w->acc[i] |= m->bits[i];
        return false;
    }
    m->bits = calloc((size_t)w->words, sizeof *m->bits);
    if (m->bits == NULL) {
        /* Refused: the census answers absent (see realm_holders.h), so the rest of this walk records nothing. */
        w->refused = true;
        return false;
    }
    m->seg = seg;
    w->cur = m->bits;
    w->cur_seg = seg;
    return true;
}

static void holder_visit(const void *seg, JSValueConst v, void *arg) {
    HolderWalk *w = arg;
    int r = realm_of_global(w, JS_VALUE_GET_PTR(v));
    if (r < 0) return;
    DCHECK(seg == NULL || seg == w->cur_seg,
           "the delta walk visited a frozen segment's value without entering that segment first, so its realm "
           "would be credited to the wrong segment's answer");
    w->acc[r / 64] |= 1ull << (r % 64);
    if (seg != NULL) w->cur[r / 64] |= 1ull << (r % 64);
}

/* Whether a census has seen every live realm unheld (realm_holders.h). Lifetime, never reset. */
static bool g_all_unheld_seen;

bool realm_holders_all_unheld_seen(void) { return g_all_unheld_seen; }

/* See realm_holders.h. */
bool realm_holders_census(int *min, int *max, long *total, int *unheld) {
    HolderWalk w;
    long segs = 0, entries = 0, sum = 0;
    int *holders, lo = -1, hi = -1, none = 0;

    DCHECK(min && max && total && unheld, "the realm-holder census was given nowhere to write a row");
    memset(&w, 0, sizeof w);
    w.n = navigable_realm_count();
    if (w.n == 0) {
        *min = *max = -1;
        *total = 0;
        *unheld = 0;
        return true;
    }
    w.words = (w.n + 63) / 64;
    w.globals = malloc((size_t)w.n * 3 * sizeof *w.globals);
    holders = calloc((size_t)w.n, sizeof *holders);
    cow_chain_stats(&segs, &entries);
    for (w.memo_cap = 16; w.memo_cap < (size_t)segs * 2 + 1; w.memo_cap *= 2) {}
    w.memo = calloc(w.memo_cap, sizeof *w.memo);
    w.acc = calloc((size_t)w.words, sizeof *w.acc);
    if (!w.globals || !holders || !w.memo || !w.acc) w.refused = true;
    if (w.refused) goto out;
    for (int i = 0; i < w.n; i++) {
        JSContext *realm = navigable_realm_at(i);
        JSValue g = JS_GetGlobalObject(realm);
        JSValueConst handles[3] = { g, document_window_proxy(realm), document_object(realm) };
        for (int h = 0; h < 3; h++) {
            if (!JS_IsObject(handles[h])) continue;   /* a realm with no browsing context has no proxy */
            w.globals[w.n_globals].global = JS_VALUE_GET_PTR(handles[h]);
            w.globals[w.n_globals].realm = i;
            w.n_globals++;
        }
        JS_FreeValue(realm, g);   /* the realm holds its own global; the pointer is good while the realm is */
    }
    qsort(w.globals, (size_t)w.n_globals, sizeof *w.globals, global_cmp);

    for (int f = 0; f < flow_count() && !w.refused; f++) {
        Flow *fl = flow_at(f);
        if (fl->delta == NULL) continue;
        memset(w.acc, 0, (size_t)w.words * sizeof *w.acc);
        w.cur = NULL;
        w.cur_seg = NULL;
        cow_delta_objects(fl->delta, holder_enter, holder_visit, &w);
        for (int r = 0; r < w.n; r++)
            if (w.acc[r / 64] >> (r % 64) & 1) holders[r]++;
    }
    for (int r = 0; r < w.n && !w.refused; r++) {
        if (lo < 0 || holders[r] < lo) lo = holders[r];
        if (holders[r] > hi) hi = holders[r];
        sum += holders[r];
        if (holders[r] == 0) none++;
    }
out:
    if (w.memo != NULL)
        for (size_t i = 0; i < w.memo_cap; i++) free(w.memo[i].bits);
    free(w.memo);
    free(w.acc);
    free(holders);
    free(w.globals);
    if (w.refused) return false;
    *min = lo;
    *max = hi;
    *total = sum;
    *unheld = none;
    if (none == w.n) g_all_unheld_seen = true;
    return true;
}
