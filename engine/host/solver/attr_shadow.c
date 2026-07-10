/* DOM attribute taint shadow — see attr_shadow.h. */
#include <string.h>
#include <stdlib.h>
#include "check.h"        /* CHECK — dropping a taint shadow on OOM would silently corrupt @S isolation */
#include "solver/attr_shadow.h"

typedef struct { lxb_dom_element_t *el; char *name; JSValue opaque; } AttrShadow;
static AttrShadow *g_attr_shadow = NULL; static int g_attr_shadow_n = 0, g_attr_shadow_cap = 0;

int attr_shadow_find(lxb_dom_element_t *el, const char *name) {
    for (int i = 0; i < g_attr_shadow_n; i++) if (g_attr_shadow[i].el == el && strcmp(g_attr_shadow[i].name, name) == 0) return i;
    return -1;
}
void attr_shadow_set(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque) {
    int i = attr_shadow_find(el, name);
    if (JS_IsUndefined(opaque)) {   /* concrete overwrite -> clear any stale taint */
        if (i >= 0) { JS_FreeValue(ctx, g_attr_shadow[i].opaque); free(g_attr_shadow[i].name); g_attr_shadow[i] = g_attr_shadow[--g_attr_shadow_n]; }
        return;
    }
    if (i >= 0) { JS_FreeValue(ctx, g_attr_shadow[i].opaque); g_attr_shadow[i].opaque = JS_DupValue(ctx, opaque); return; }
    if (g_attr_shadow_n >= g_attr_shadow_cap) { int nc = g_attr_shadow_cap ? g_attr_shadow_cap * 2 : 16;
        AttrShadow *n = realloc(g_attr_shadow, (size_t)nc * sizeof(AttrShadow)); CHECK(n, "attr-shadow-oom: realloc failed — silently dropping a taint shadow corrupts @S isolation"); g_attr_shadow = n; g_attr_shadow_cap = nc; }
    g_attr_shadow[g_attr_shadow_n].el = el; g_attr_shadow[g_attr_shadow_n].name = strdup(name);
    g_attr_shadow[g_attr_shadow_n].opaque = JS_DupValue(ctx, opaque); g_attr_shadow_n++;
}
JSValue attr_shadow_opaque(int i) { return g_attr_shadow[i].opaque; }   /* borrowed */
void attr_shadow_free(JSContext *ctx) {
    for (int i = 0; i < g_attr_shadow_n; i++) { JS_FreeValue(ctx, g_attr_shadow[i].opaque); free(g_attr_shadow[i].name); }
    free(g_attr_shadow); g_attr_shadow = NULL; g_attr_shadow_n = g_attr_shadow_cap = 0;
}
