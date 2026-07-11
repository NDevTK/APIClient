/* Parked async delivery — see parked.h. */
#include "solver/parked.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char *url; JSValue resolve; int tag; } ParkEntry;
struct ParkTable { ParkEntry *e; int n, cap; };

ParkTable *park_new(void) { ParkTable *t = calloc(1, sizeof *t); CHECK(t, "park_new: table alloc"); return t; }

void park_add(JSContext *ctx, ParkTable *t, const char *url, JSValueConst resolve, int tag) {
    DCHECK(t && url, "park_add: null table/url");
    if (t->n >= t->cap) { int nc = t->cap ? t->cap * 2 : 8; ParkEntry *n = realloc(t->e, (size_t)nc * sizeof(ParkEntry)); CHECK(n, "park_add: a dropped parked consumer never resolves, losing the endpoint"); t->e = n; t->cap = nc; }
    t->e[t->n].url = strdup(url); t->e[t->n].resolve = JS_DupValue(ctx, resolve); t->e[t->n].tag = tag; t->n++;
}

/* Fire one entry: compute its value, resolve the promise, free the entry's held refs. */
static void park_fire(JSContext *ctx, ParkEntry *e, ParkCompute f, void *ud) {
    JSValue v = f(ctx, e->url, e->tag, ud);
    JSValue r = JS_Call(ctx, e->resolve, JS_UNDEFINED, 1, (JSValueConst *)&v); JS_FreeValue(ctx, r); JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, e->resolve); free(e->url);
}

void park_resolve_url(JSContext *ctx, ParkTable *t, const char *url, ParkCompute f, void *ud) {
    if (!t) return;
    for (int i = 0; i < t->n; ) {
        if (strcmp(t->e[i].url, url) == 0) { park_fire(ctx, &t->e[i], f, ud); t->e[i] = t->e[--t->n]; }   /* swap-remove */
        else i++;
    }
}

void park_drain(JSContext *ctx, ParkTable *t, ParkCompute f, void *ud) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) park_fire(ctx, &t->e[i], f, ud);
    t->n = 0;
}

void park_free(JSContext *ctx, ParkTable *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) { JS_FreeValue(ctx, t->e[i].resolve); free(t->e[i].url); }   /* never-fired: free without resolving */
    free(t->e); free(t);
}
