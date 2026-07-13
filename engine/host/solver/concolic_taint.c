/* Cross-flow concolic-taint set — see concolic_taint.h. The HEAP taint-shadow (twin of attr_shadow.c for DOM). */
#include <stdlib.h>
#include "check.h"          /* CHECK — a dropped taint silently loses a cross-flow fork; OOM is fatal */
#include "solver/concolic_taint.h"

typedef struct { JSValue obj; JSAtom atom; JSValue taint; } CTaint;   /* taint = the concolic WRITTEN here, so a cross-flow read reconstructs the REAL value (shape/src/example), never a bare {} */
static CTaint *g_tab = NULL;
static int g_n = 0, g_cap = 0;
static JSContext *g_ctx = NULL;   /* the analysis ctx, for value dup/free from the read hook (which is arg-less on ctx) */

/* READ hook: the concolic written to (obj,atom), for a cross-flow read to recover its provenance/example — or
   JS_UNDEFINED if untainted (the interpreter then keeps the slot's own value). Installed only while non-empty. */
static JSValueConst ctaint_lookup(JSValueConst obj, JSAtom atom) {
    void *po = JS_VALUE_GET_PTR(obj);
    for (int i = 0; i < g_n; i++)
        if (JS_VALUE_GET_PTR(g_tab[i].obj) == po && g_tab[i].atom == atom) return g_tab[i].taint;
    return JS_UNDEFINED;
}

/* WRITE hook: record that a CONCOLIC value was written to (obj,atom). The interpreter guards this with
   js_is_concolic_val(val) && !obj->flow_local, so every value reaching here is a genuine cross-flow taint. */
static void ctaint_record(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst taint) {
    void *po = JS_VALUE_GET_PTR(obj);
    for (int i = 0; i < g_n; i++)
        if (JS_VALUE_GET_PTR(g_tab[i].obj) == po && g_tab[i].atom == atom) return;   /* already tainted (monotonic) */
    if (g_n >= g_cap) {
        int nc = g_cap ? g_cap * 2 : 32;
        CTaint *n = (CTaint *)realloc(g_tab, (size_t)nc * sizeof(CTaint));
        CHECK(n, "concolic-taint-oom: taint realloc failed — a dropped taint silently loses a cross-flow fork (a gated endpoint the moat wants)");
        g_tab = n; g_cap = nc;
    }
    g_tab[g_n].obj = JS_DupValue(ctx, obj);       /* hold a ref so the pointer identity stays valid */
    g_tab[g_n].atom = JS_DupAtom(ctx, atom);
    g_tab[g_n].taint = JS_DupValue(ctx, taint);   /* keep the tainting concolic's provenance+example */
    if (g_n == 0) JS_SetConcolicTaintGetHook(ctaint_lookup);   /* set became non-empty -> enable the read-path lookup */
    g_n++;
}

void concolic_taint_init(JSContext *ctx) { g_ctx = ctx; JS_SetConcolicTaintAddHook(ctaint_record); }

void concolic_taint_reset(JSContext *ctx) {
    for (int i = 0; i < g_n; i++) { JS_FreeValue(ctx, g_tab[i].obj); JS_FreeAtom(ctx, g_tab[i].atom); JS_FreeValue(ctx, g_tab[i].taint); }
    g_n = 0;
    JS_SetConcolicTaintGetHook(NULL);   /* empty -> disable the read-path lookup (a normal property read pays only a NULL check) */
}
