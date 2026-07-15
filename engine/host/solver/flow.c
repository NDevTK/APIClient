/* Flow registry + WFQ — see flow.h. Pure host component (quickjs.h + check.h only). */
#include "solver/flow.h"
#include "check.h"
#include <stdlib.h>

static Flow **g_flows = NULL;
static int g_flows_n = 0, g_flows_cap = 0;

void flow_registry_init(void) { g_flows = NULL; g_flows_n = 0; g_flows_cap = 0; }

void flow_registry_free(JSContext *ctx) {
    for (int i = 0; i < g_flows_n; i++) {
        JS_FreeValue(ctx, g_flows[i]->fn);
        free(g_flows[i]->dec);
        free(g_flows[i]);
    }
    free(g_flows); g_flows = NULL; g_flows_n = g_flows_cap = 0;
}

Flow *flow_add(JSContext *ctx, JSValueConst fn, signed char *dec, int dec_n) {
    if (g_flows_n >= g_flows_cap) {
        g_flows_cap = g_flows_cap ? g_flows_cap * 2 : 32;
        g_flows = realloc(g_flows, (size_t)g_flows_cap * sizeof(Flow *));
        CHECK(g_flows, "flow_add: OOM growing the frontier — a dropped flow corrupts BFS exploration");
    }
    Flow *f = calloc(1, sizeof(Flow));
    CHECK(f, "flow_add: OOM allocating a flow — a dropped flow corrupts the frontier");
    f->fn = JS_DupValue(ctx, fn);
    f->dec = dec; f->dec_n = dec_n;
    f->val = 0.0; f->cpu = 0; f->visits = 0;
    g_flows[g_flows_n++] = f;
    return f;
}

/* Anytime-bandit priority: reward + UCB optimism − CPU aging. Additive (never a value/cpu ratio — the aging
   term already yields value-per-CPU behaviour without the 0/0 degeneracy on an unrun flow). */
double flow_weight(const Flow *f) {
    double reward = f->val;
    double ucb    = 1.0 / (double)(f->visits + 1);   /* a never-run flow (visits 0) gets the full bonus */
    double aging  = (double)f->cpu * 1e-6;            /* CPU burned without emitting sinks the flow */
    return reward + ucb - aging;
}

Flow *flow_best(void) {
    Flow *best = NULL; double bw = 0.0;
    for (int i = 0; i < g_flows_n; i++) {
        double w = flow_weight(g_flows[i]);
        if (!best || w > bw) { best = g_flows[i]; bw = w; }
    }
    return best;
}

void flow_remove(JSContext *ctx, Flow *f) {
    for (int i = 0; i < g_flows_n; i++) {
        if (g_flows[i] == f) {
            JS_FreeValue(ctx, f->fn);
            free(f->dec);
            free(f);
            g_flows[i] = g_flows[--g_flows_n];   /* swap-remove; order is by weight, not position */
            return;
        }
    }
    DFAIL("flow_remove: flow not in the registry — double remove or a dangling Flow*");
}

int flow_count(void) { return g_flows_n; }
