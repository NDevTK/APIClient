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
