/* Per-flow COW isolation — see cow.h. */
#include "solver/cow.h"
#include "check.h"
#include <stdlib.h>

typedef struct { JSValue obj; JSAtom atom; int existed; JSValue old; } CowEntry;
static CowEntry *g_delta = NULL;
static int g_delta_n = 0, g_delta_cap = 0;
static int g_capturing = 0;   /* re-entrancy guard: our own reads/reverts must not re-capture */

void cow_capture_hook(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    if (g_capturing) return;
    /* Capture each (obj, atom) ONCE per run — the first record is the true baseline; later writes overwrite a
       value we already saved. Linear scan (a run touches few baseline slots). */
    for (int i = 0; i < g_delta_n; i++)
        if (JS_VALUE_GET_PTR(g_delta[i].obj) == JS_VALUE_GET_PTR(obj) && g_delta[i].atom == atom) return;

    g_capturing = 1;
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, obj, atom);   /* 1 = own prop exists (d filled), 0 = absent, -1 = exc */
    int existed = 0; JSValue old = JS_UNDEFINED;
    if (has > 0) { existed = 1; old = d.value; JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter); }

    if (g_delta_n >= g_delta_cap) {
        g_delta_cap = g_delta_cap ? g_delta_cap * 2 : 64;
        g_delta = realloc(g_delta, (size_t)g_delta_cap * sizeof(CowEntry));
        CHECK(g_delta, "cow: OOM growing the delta — a lost baseline write leaks state across flows");
    }
    CowEntry *e = &g_delta[g_delta_n++];
    e->obj = JS_DupValue(ctx, obj);
    e->atom = JS_DupAtom(ctx, atom);
    e->existed = existed;
    e->old = old;
    g_capturing = 0;
}

void cow_revert(JSContext *ctx) {
    g_capturing = 1;   /* the restores below are baseline writes — must NOT re-capture */
    for (int i = g_delta_n - 1; i >= 0; i--) {   /* reverse: an added-then-changed prop reverts to absent */
        CowEntry *e = &g_delta[i];
        if (e->existed) JS_SetProperty(ctx, e->obj, e->atom, e->old);   /* consumes old */
        else { JS_DeleteProperty(ctx, e->obj, e->atom, 0); JS_FreeValue(ctx, e->old); }
        JS_FreeValue(ctx, e->obj);
        JS_FreeAtom(ctx, e->atom);
    }
    g_delta_n = 0;
    g_capturing = 0;
}

void cow_free(JSContext *ctx) {
    for (int i = 0; i < g_delta_n; i++) { JS_FreeValue(ctx, g_delta[i].obj); JS_FreeAtom(ctx, g_delta[i].atom); JS_FreeValue(ctx, g_delta[i].old); }
    free(g_delta); g_delta = NULL; g_delta_n = g_delta_cap = 0;
}
