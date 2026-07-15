/* Per-flow swappable COW delta — see cow.h. */
#include "solver/cow.h"
#include "check.h"
#include <stdlib.h>

/* One captured baseline slot: base = the value the shared baseline holds (restored on unapply); cur = the
   running flow's latest value for it (saved on unapply, replayed on apply). existed = did the baseline own the
   slot (else the flow CREATED it, so unapply deletes and apply re-creates). */
typedef struct { JSValue obj; JSAtom atom; int existed; JSValue base; JSValue cur; int cur_valid; } CowEntry;

struct CowDelta { CowEntry *e; int n, cap; };

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
    g_capturing = 0;
}

void cow_unapply(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    g_capturing = 1;
    for (int i = d->n - 1; i >= 0; i--) {   /* reverse: symmetric with apply */
        CowEntry *e = &d->e[i];
        JSPropertyDescriptor pd;              /* save the flow's CURRENT value so apply can restore it */
        int has = JS_GetOwnProperty(ctx, &pd, e->obj, e->atom);
        if (e->cur_valid) JS_FreeValue(ctx, e->cur);
        if (has > 0) { e->cur = pd.value; JS_FreeValue(ctx, pd.getter); JS_FreeValue(ctx, pd.setter); }
        else e->cur = JS_UNDEFINED;
        e->cur_valid = 1;
        if (e->existed) JS_SetProperty(ctx, e->obj, e->atom, JS_DupValue(ctx, e->base));   /* -> baseline */
        else JS_DeleteProperty(ctx, e->obj, e->atom, 0);
    }
    g_capturing = 0;
}

void cow_apply(JSContext *ctx, CowDelta *d) {
    if (!d) return;
    g_capturing = 1;
    for (int i = 0; i < d->n; i++) {   /* forward: replay the flow's writes over the pristine baseline */
        CowEntry *e = &d->e[i];
        if (e->cur_valid) JS_SetProperty(ctx, e->obj, e->atom, JS_DupValue(ctx, e->cur));
    }
    g_capturing = 0;
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
