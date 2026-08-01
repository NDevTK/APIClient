/* Flow registry + WFQ — see flow.h. Pure host component (quickjs.h + check.h only). */
#include "solver/flow.h"
#include "check.h"
#include <stdlib.h>

static Flow **g_flows = NULL;
static int g_flows_n = 0, g_flows_cap = 0;
static unsigned g_gen = 0;   /* bumped whenever the frontier's membership changes (add/remove) — lets the
                                value-yield recompute the rival only on a change, never per-opcode */
static Flow *g_running = NULL;   /* the flow currently holding the worker (the scheduler sets it) */

/* THE FRONTIER'S MEMBERS, in registry order. A walk, not a rank: the pending-fetch register lives on the flows,
   so whoever asks what the host owes has to visit all of them. flow_best answers a different question. */
Flow *flow_at(int i) { return (i >= 0 && i < g_flows_n) ? g_flows[i] : NULL; }

void flow_registry_init(void) { g_flows = NULL; g_flows_n = 0; g_flows_cap = 0; g_gen = 0; g_running = NULL; }

unsigned flow_frontier_gen(void) { return g_gen; }

void  flow_set_running(Flow *f) { g_running = f; }
Flow *flow_running(void) { return g_running; }

/* Credit the running flow with newly EMITTED value (a new @H endpoint / @S PoC): its WFQ reward rises and its
   CPU-since-emit aging resets, so a productive flow outranks fresh + starved flows. Called by the detectors
   (endpoint.c / solve.c) when they record something NEW — this is what makes the WFQ value-of-information
   ordered rather than merely breadth-first by visit count. The frontier gen bumps so the value-yield re-ranks. */
void flow_credit_emit(double v) {
    if (!g_running) return;
    g_running->val += v;
    g_running->cpu = 0;   /* emitted -> no longer a CPU-burning monopolizer */
    g_gen++;              /* rank changed: let the value-yield recompute */
}

/* Age the running flow: CPU burned since its last emit. A monopolizer that runs without emitting sinks below
   productive + unrun flows. Called once per scheduler step. */
void flow_age_running(long units) { if (g_running) g_running->cpu += units; }

void flow_registry_free(JSContext *ctx) {
    for (int i = 0; i < g_flows_n; i++) {
        JS_FreeValue(ctx, g_flows[i]->fn);
        free(g_flows[i]->dec);
        for (int k = 0; k < g_flows[i]->njob; k++) {   /* free any undrained microtask jobs */
            for (int a = 0; a < g_flows[i]->jobs[k].argc; a++) JS_FreeValue(ctx, g_flows[i]->jobs[k].argv[a]);
            free(g_flows[i]->jobs[k].argv);
        }
        free(g_flows[i]->jobs);
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
    g_gen++;   /* frontier changed */
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

/* The highest-weight flow OTHER than `exclude` — the running flow's rival for the value-driven yield. */
Flow *flow_best_other(const Flow *exclude) {
    Flow *best = NULL; double bw = 0.0;
    for (int i = 0; i < g_flows_n; i++) {
        if (g_flows[i] == exclude) continue;
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
            g_gen++;   /* frontier changed */
            return;
        }
    }
    DFAIL("flow_remove: flow not in the registry — double remove or a dangling Flow*");
}

int flow_count(void) { return g_flows_n; }
