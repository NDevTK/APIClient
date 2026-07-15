/* The dispatch loop — see engine.h. */
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "check.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Snapshot-fork handoff: solver_decide stashes the sibling's hot decision + pins here at a forking branch;
   the interpreter then clones the frame and calls engine_fork_finalize, which assembles the sibling flow. */
static void *g_fork_dec = NULL, *g_fork_pins = NULL;
void engine_prepare_fork(void *dec_blob, void *pin_blob) { g_fork_dec = dec_blob; g_fork_pins = pin_blob; }

static void engine_fork_finalize(JSContext *ctx, JSValue *clone) {
    Flow *parent = flow_running();
    DCHECK(parent != NULL && g_fork_dec != NULL, "engine_fork_finalize: fork without a running flow / prepared state");
    Flow *sib = flow_add(ctx, parent->fn, NULL, 0);
    sib->started = 1;                 /* HOT: resume from the cloned frame + blobs, never a fresh re-run */
    sib->frame = clone;               /* the frame snapshot taken AT the branch */
    sib->script_i = parent->script_i; /* same position in the script sequence */
    sib->delta = cow_delta_clone(ctx, (CowDelta *)parent->delta);   /* branch-point globals, then diverges */
    sib->dec_blob = g_fork_dec; g_fork_dec = NULL;
    sib->pin_blob = g_fork_pins; g_fork_pins = NULL;
    if (parent->dyn_n) {              /* inherit the lazy chunks loaded up to the branch */
        sib->dyn = malloc((size_t)parent->dyn_n * sizeof(char *)); CHECK(sib->dyn, "engine: OOM fork dyn");
        for (int i = 0; i < parent->dyn_n; i++) { sib->dyn[i] = strdup(parent->dyn[i]); CHECK(sib->dyn[i], "engine: OOM fork dyn body"); }
        sib->dyn_n = sib->dyn_cap = parent->dyn_n;
    }
}

void engine_queue_script(const char *body) {
    Flow *f = flow_running();   /* the running flow owns the lazy chunk it loads */
    if (!body || !f) return;
    if (f->dyn_n >= f->dyn_cap) { f->dyn_cap = f->dyn_cap ? f->dyn_cap * 2 : 8; f->dyn = realloc(f->dyn, (size_t)f->dyn_cap * sizeof(char *)); CHECK(f->dyn, "engine: OOM dynamic-script queue"); }
    f->dyn[f->dyn_n] = strdup(body); CHECK(f->dyn[f->dyn_n], "engine: OOM dynamic-script body"); f->dyn_n++;
}

/* Preempt hook, two orthogonal yield decisions at the one per-back-edge check:
   (1) VALUE yield — suspend the running flow the MOMENT a parked flow outranks it (the WFQ, not a clock,
       decides which flow runs). The rival is recomputed only when the frontier membership changes (a fork
       adds a flow) or the running flow switches — cached by (gen, cur) so this is O(1) per back-edge, never
       an O(flows) scan per opcode.
   (2) COOPERATIVE-QUANTUM yield — a thread-sharing floor: even a top-ranked flow breathes every Q back-edges
       so the host loop can interleave / pump / snapshot. NOT a step cap: it drops/reorders no flow and the
       flow resumes byte-identically. */
static unsigned g_qtick = 0;
#define FLOW_QUANTUM 64
static unsigned g_seen_gen = 0; static Flow *g_seen_cur = NULL; static int g_outranked = 0;
static int preempt_hook(void) {
    Flow *cur = flow_running();
    if (flow_frontier_gen() != g_seen_gen || cur != g_seen_cur) {   /* (1) recompute rival only on change */
        g_seen_gen = flow_frontier_gen(); g_seen_cur = cur;
        Flow *rival = cur ? flow_best_other(cur) : NULL;
        g_outranked = (rival && cur && flow_weight(rival) > flow_weight(cur));
    }
    if (g_outranked) return 1;                        /* value yield */
    return (++g_qtick % FLOW_QUANTUM) == 0;           /* (2) quantum floor */
}

/* Advance flow `f` by up to one quantum. Returns 1 when the flow has FINISHED all its scripts + lazy chunks,
   0 when it yielded mid-execution (resume it later). Each <script>/chunk is its OWN program (JS_FlowNew) run
   in document order in the shared context, under f's COW delta (set by the caller). */
static int flow_step(JSContext *ctx, Flow *f, char *const *bodies, int n) {
    for (;;) {
        if (!f->frame) {
            const char *body;
            if (f->script_i < n) body = bodies[f->script_i];
            else if (f->script_i - n < f->dyn_n) body = f->dyn[f->script_i - n];
            else return 1;   /* all static scripts + lazily-loaded chunks done */
            f->frame = JS_FlowNew(ctx, body, strlen(body));
            DCHECK(f->frame != NULL, "flow_step: a page <script>/chunk did not compile");
        }
        if (JS_FlowResume(ctx, (JSValue *)f->frame)) return 0;   /* quantum yield — more work, resume later */
        JS_FlowFree(ctx, (JSValue *)f->frame); f->frame = NULL; f->script_i++;   /* this script done -> next */
    }
}

static void flow_switch_out(JSContext *ctx, Flow *f) {   /* pause f: snapshot its solver state, restore baseline */
    f->dec_blob = decide_suspend();
    f->pin_blob = concolic_pins_suspend();
    cow_unapply(ctx, (CowDelta *)f->delta);
    cow_set_current(NULL);
    flow_set_running(NULL);
}

static void flow_switch_in(JSContext *ctx, Flow *f) {   /* resume/start f: apply its delta + solver state */
    if (!f->delta) f->delta = cow_delta_new();
    cow_set_current((CowDelta *)f->delta);
    cow_apply(ctx, (CowDelta *)f->delta);
    if (!f->started) { f->started = 1; decide_enter(ctx, f); }   /* fresh flow: replay from cursor 0 */
    else {                                                        /* paused flow: restore where it left off */
        decide_resume(f->dec_blob, f->fn);   decide_blob_free(f->dec_blob); f->dec_blob = NULL;
        concolic_pins_resume(f->pin_blob);   concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    }
    flow_set_running(f);
}

static void flow_finish(JSContext *ctx, Flow *f) {   /* f completed: tear down its interleaving state + remove */
    decide_leave(ctx);
    cow_unapply(ctx, (CowDelta *)f->delta); cow_set_current(NULL);
    cow_delta_free(ctx, (CowDelta *)f->delta); f->delta = NULL;
    for (int i = 0; i < f->dyn_n; i++) free(f->dyn[i]);
    free(f->dyn); f->dyn = NULL; f->dyn_n = f->dyn_cap = 0;
    flow_set_running(NULL);
    flow_remove(ctx, f);
}

/* Run ONE flow start-to-finish (no interleaving) — the @S candidate re-run path, where a single candidate must
   execute fully to observe whether it fires. Uses the flow machinery + a transient COW delta. */
void flow_exec_once(JSContext *ctx, char *const *bodies, int n) {
    CowDelta *d = cow_delta_new(); cow_set_current(d);
    for (int i = 0; i < n; i++) {
        JSValue *frame = JS_FlowNew(ctx, bodies[i], strlen(bodies[i]));
        DCHECK(frame != NULL, "flow_exec_once: a page <script> did not compile");
        while (JS_FlowResume(ctx, frame)) { }
        JS_FlowFree(ctx, frame);
    }
    cow_set_current(NULL); cow_unapply(ctx, d); cow_delta_free(ctx, d);
}

void engine_run(JSContext *ctx, char *const *bodies, int n) {
    flow_add(ctx, JS_UNDEFINED, NULL, 0);   /* the first flow: the page's scripts, empty decision vector */
    JS_SetFlowLocalMark(1);                 /* objects created while a flow runs are flow-local (discarded) */
    JS_SetPreemptHook(preempt_hook);        /* flows never run to completion in one go: they yield + interleave */
    JS_SetForkHook(engine_fork_finalize);   /* a concolic branch snapshot-forks the frame (no re-run sibling) */
    Flow *cur = NULL;
    int switches = 0, yields = 0, finishes = 0;
    for (;;) {
        Flow *best = flow_best();           /* WFQ: highest value-of-information — a fresh fork (UCB) can preempt */
        if (!best) break;
        if (best != cur) {                  /* context switch: swap COW delta + decision + pins */
            if (cur) flow_switch_out(ctx, cur);
            flow_switch_in(ctx, best);
            best->visits++;
            cur = best;
            switches++;
        }
        flow_age_running(1);   /* this step burned CPU; flow_credit_emit resets it when the flow emits value */
        if (flow_step(ctx, cur, bodies, n)) { flow_finish(ctx, cur); cur = NULL; finishes++; }
        else yields++;
    }
    fprintf(stderr, "[scheduler] switches=%d yields=%d flows_finished=%d\n", switches, yields, finishes);
    JS_SetPreemptHook(NULL);
    JS_SetForkHook(NULL);
    JS_SetFlowLocalMark(0);
}
