/* DOM attribute taint shadow — see attr_shadow.h. */
#include <string.h>
#include <stdlib.h>
#include "check.h"        /* CHECK — dropping a taint shadow on OOM would silently corrupt @S isolation */
#include "solver/attr_shadow.h"

typedef struct { const void *owner; int kind; char *ns; char *name; JSValue opaque; } AttrShadow;
static AttrShadow *g_attr_shadow = NULL; static int g_attr_shadow_n = 0, g_attr_shadow_cap = 0;

/* Two namespaces match when both are the null namespace or both are the same URI. NULL is a VALUE here, not a
   missing argument — the null namespace is where every attribute an HTML page sets lives. */
static int ns_same(const char *a, const char *b) { return (!a || !b) ? (a == b) : strcmp(a, b) == 0; }

static void shadow_drop(JSContext *ctx, int i) {
    JS_FreeValue(ctx, g_attr_shadow[i].opaque);
    free(g_attr_shadow[i].name); free(g_attr_shadow[i].ns);
    g_attr_shadow[i] = g_attr_shadow[--g_attr_shadow_n];
}

int attr_shadow_find(const void *owner, int kind, const char *ns, const char *name) {
    for (int i = 0; i < g_attr_shadow_n; i++)
        if (g_attr_shadow[i].owner == owner && g_attr_shadow[i].kind == kind
            && ns_same(g_attr_shadow[i].ns, ns) && strcmp(g_attr_shadow[i].name, name) == 0) return i;
    return -1;
}
void attr_shadow_set(JSContext *ctx, const void *owner, int kind, const char *ns, const char *name,
                     JSValueConst opaque) {
    int i;
    DCHECK(kind != ATTR_SLOT_PROPERTY || ns == NULL,
           "a DOM PROPERTY slot was given a namespace — a property has none, and a key that carries one would "
           "never be found by the read, which passes NULL");
    i = attr_shadow_find(owner, kind, ns, name);
    if (JS_IsUndefined(opaque)) {   /* concrete overwrite -> clear any stale taint */
        if (i >= 0) shadow_drop(ctx, i);
        return;
    }
    if (i >= 0) { JS_FreeValue(ctx, g_attr_shadow[i].opaque); g_attr_shadow[i].opaque = JS_DupValue(ctx, opaque); return; }
    if (g_attr_shadow_n >= g_attr_shadow_cap) { int nc = g_attr_shadow_cap ? g_attr_shadow_cap * 2 : 16;
        AttrShadow *n = realloc(g_attr_shadow, (size_t)nc * sizeof(AttrShadow)); CHECK(n, "attr-shadow-oom: realloc failed — silently dropping a taint shadow corrupts @S isolation"); g_attr_shadow = n; g_attr_shadow_cap = nc; }
    g_attr_shadow[g_attr_shadow_n].owner = owner; g_attr_shadow[g_attr_shadow_n].kind = kind;
    g_attr_shadow[g_attr_shadow_n].ns = ns ? strdup(ns) : NULL;
    CHECK(!ns || g_attr_shadow[g_attr_shadow_n].ns, "attr-shadow-oom: a namespace key could not be copied");
    g_attr_shadow[g_attr_shadow_n].name = strdup(name);
    CHECK(g_attr_shadow[g_attr_shadow_n].name, "attr-shadow-oom: a slot name could not be copied");
    g_attr_shadow[g_attr_shadow_n].opaque = JS_DupValue(ctx, opaque); g_attr_shadow_n++;
}
JSValue attr_shadow_opaque(int i) { return g_attr_shadow[i].opaque; }   /* borrowed */
void attr_shadow_forget(JSContext *ctx, const void *owner) {
    /* BACKWARDS, because shadow_drop fills the hole from the END: walking forwards would step over the entry it
       just moved into `i` and leave one of this owner's behind. */
    for (int i = g_attr_shadow_n - 1; i >= 0; i--)
        if (g_attr_shadow[i].owner == owner) shadow_drop(ctx, i);
}
void attr_shadow_free(JSContext *ctx) {
    for (int i = 0; i < g_attr_shadow_n; i++) { JS_FreeValue(ctx, g_attr_shadow[i].opaque);
        free(g_attr_shadow[i].name); free(g_attr_shadow[i].ns); }
    free(g_attr_shadow); g_attr_shadow = NULL; g_attr_shadow_n = g_attr_shadow_cap = 0;
}
