/* See realm.h. */
#include <stdlib.h>

#include "check.h"
#include "core/realm.h"

static RealmIntrinsic *g_list;
static int g_n, g_cap;

void realm_declare_intrinsic(RealmIntrinsic install)
{
    int i;

    DCHECK(install != NULL, "a component declared a per-realm intrinsic with no install to run");
    for (i = 0; i < g_n; i++)
        DCHECK(g_list[i] != install,
               "a per-realm intrinsic was declared twice — it would then build its prototype twice in every "
               "realm, and the second build leaves everything already chained to the first answering out of a "
               "discarded object");
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 8;
        RealmIntrinsic *g = realloc(g_list, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "realm: OOM recording a per-realm intrinsic — a realm missing one answers a member "
                         "out of another document");
        g_list = g;
        g_cap = cap;
    }
    g_list[g_n++] = install;
}

void realm_install_intrinsics(JSContext *ctx)
{
    int i;

    DCHECK(ctx != NULL, "the per-realm intrinsics were installed into no realm");
    for (i = 0; i < g_n; i++)
        g_list[i](ctx);
}

void realm_intrinsics_free(void)
{
    free(g_list);
    g_list = NULL;
    g_n = g_cap = 0;
}

/* A slot IS a class id whose per-context prototype slot holds something that is not a prototype. Nothing is
   ever constructed with the class — it exists for the SLOT, which is the per-realm store quickjs already keeps
   and already frees with the context. The `what` string is the class name, so a heap dump names the slot. */
int realm_value_declare(JSContext *ctx, const char *what)
{
    JSClassID id = 0;
    JSClassDef d;

    DCHECK(what != NULL && *what, "a per-realm value was declared with no name — a dump would not say whose");
    d = (JSClassDef){ what };
    JS_NewClassID(JS_GetRuntime(ctx), &id);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), id, &d) == 0, "realm: a per-realm value slot could not be declared");
    return (int)id;
}

void realm_value_set(JSContext *ctx, int slot, JSValue v)
{
    JSValue prev;

    DCHECK(slot > 0, "a per-realm value was set through a slot that was never declared");
    prev = JS_GetClassProto(ctx, (JSClassID)slot);
    DCHECK(JS_IsNull(prev), "a per-realm value was set twice in one realm — the first is what everything "
                            "already built in this realm is holding");
    JS_FreeValue(ctx, prev);
    JS_SetClassProto(ctx, (JSClassID)slot, v);
}

JSValue realm_value_get(JSContext *ctx, int slot)
{
    JSValue v;

    DCHECK(slot > 0, "a per-realm value was read through a slot that was never declared");
    v = JS_GetClassProto(ctx, (JSClassID)slot);
    DCHECK(!JS_IsNull(v), "a per-realm value was read in a realm that never ran the install that sets it");
    return v;
}
