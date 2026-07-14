/* Forced-execution FORK decisions — see fork.h. Split out of scheduler.c: the exploration/fork policy. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "solver/fork.h"
#include "solver/scheduler.h"      /* the scheduler state branch_decide reads (g_dec/g_c/g_cur_fn/…) + reg_add/reg_readd/spawn_async_sibling */
#include "solver/boot_flow.h"      /* reg_add_boot / reg_add_session — a boot/session flow forks its own kind */
#include "solver/constraints.h"    /* cons_set / cons_arm_feasible / opcmp_neg / OPCMP_NONE — the per-flow value-domain */
#include "solver/heap_cow.h"       /* heap_cow_fork — freeze the heap delta into a shared base for the snapshot sibling */
#include "solver/dom_cow.h"        /* dom_cow_fork — freeze the DOM delta likewise */

/* SNAPSHOT-FORK hand-off: branch_decide stashes the FALSE sibling's decision vector here + sets JS_SetForkPending;
   fork_spawn_sibling (called from the scheduler's st==1 suspend handler) consumes it — snapshots the just-
   suspended frame for the sibling, which CONTINUES from the snapshot with this vector (the opposite arm). */
static signed char *g_fork_sib_dec = NULL; static int g_fork_sib_dec_n = 0;

int branch_decide(JSContext *ctx, JSValueConst cond)
{
    if (g_boot_replay) return 1;                            /* boot-replay: fixed arm (re-establishing shared state for an @S candidate, no vector to replay) */
    if (!g_running || (JS_IsUndefined(g_cur_fn) && !g_in_boot_flow && !g_in_session)) return 0;   /* meaningful inside a starter flow, a boot flow, OR a session flow (all re-run without a single fn handle) */
    /* value-domain provenance of the condition: cond TRUE means `src <op> tok`; false arm holds the negation. */
    const char *src = NULL, *tok = NULL; int op = JS_ConcolicCmp(cond, &src, &tok);
    const char *jk = JS_ConcolicJKey(cond);   /* method-clean JSON field path of the compared value (for the @S envelope gate-merge) */
    int has = (op != OPCMP_NONE) && src;
    int true_op = op, false_op = opcmp_neg(op);

    if (g_c < g_dec_n) {                                    /* forced replay: take the recorded arm; RE-RECORD its constraint */
        int arm = g_dec[g_c] ? 1 : 0;
        cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? (arm ? true_op : false_op) : OPCMP_NONE, has ? jk : NULL);
        g_c++; return arm;
    }
    g_dec_ensure(g_c + 1);                                   /* only RAM/disk (the platform floor) bounds depth — g_dec_ensure CHECK-crashes at the hard wall, never fabricates an arm */

    /* A CANDIDATE session (verifying an @S breakout) takes a fixed arm for a NEW branch — it replays the
       detecting session's vector (above) then follows through with the candidate, it does not re-explore. */
    if (g_in_session && g_candidate) { cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE, has ? jk : NULL); g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }

    /* LOOP-BACK over an OPAQUE COLLECTION (for-of/for-in `done`, tagged "{@iterdone}"): iteration is
       UNBOUNDED-PARKABLE-PAGED, never run-to-completion in one flow. Take the EXIT arm (done=true) as the
       PRIMARY — this flow STOPS iterating (breadth first) — and PARK the CONTINUE arm (done=false) as its OWN
       sibling flow, so every additional iteration is a separate preemptible/pageable flow. A parked continue
       sibling runs at mark=1, so its per-iteration transients (Request/Promise/…) are flow_local-skipped and
       no single flow accumulates an unbounded delta. This is the ONE design replacing BOTH the drive-once
       terminating iterator (a banned bound that dropped shared-state-gated deep endpoints) and the
       loop-forever-in-one-flow cow-oom. The WFQ starves the identical-input continue tail (paged, resumable). */
    if (JS_IsConcolic(cond)) {
        const char *cs = JS_ConcolicSrcC(cond);
        if (cs && !strcmp(cs, "{@iterdone}")) {
            signed char *sib = (signed char *)malloc((size_t)(g_c + 1));
            CHECK(sib, "iterdone: cannot park the CONTINUE arm — dropping it truncates iteration (loses every deeper element/endpoint); OOM is a physical floor, never a silent drop");
            for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
            sib[g_c] = 0;   /* CONTINUE (done=false): the per-iteration sibling does one more iteration */
            /* SNAPSHOT-FORK the loop (same mechanism as the branch fork — the {@iterdone} done-check IS an OP_if):
               the CONTINUE sibling CONTINUES from a snapshot of the loop-back state (advances one more element)
               instead of RE-RUNNING the whole flow. So a loop that mutates SHARED state per iteration
               (state.items.push, an accumulator) is preserved across iterations, not replayed from boot. */
            if (!g_in_boot_flow && !(g_cur_flow && g_cur_flow->is_async) && JS_AtFlowBase()) {   /* sessions included: snapshot-continue avoids re-firing side-effecting handlers */
                heap_cow_fork(ctx); dom_cow_fork();
                g_fork_sib_dec = sib; g_fork_sib_dec_n = g_c + 1;
                JS_SetForkPending(1);
                cons_set(g_c, NULL, NULL, OPCMP_NONE, NULL);
                g_dec[g_c] = 1; g_dec_n = g_c + 1;   /* EXIT primary; no g_c++ — OP_if rewinds and replays this arm */
                return 1;
            }
            if (g_in_session) reg_add_session(ctx, sib, g_c + 1);
            else if (g_in_boot_flow) reg_add_boot(ctx, sib, g_c + 1);
            else if (g_cur_flow && g_cur_flow->is_async) spawn_async_sibling(ctx, g_cur_flow, sib, g_c + 1);
            else reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, sib, g_c + 1)->orphan_idx = g_cur_orphan_idx;   /* C-reentry: re-run the per-iteration sibling */
            cons_set(g_c, NULL, NULL, OPCMP_NONE, NULL);
            g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;   /* EXIT (done=true): this flow stops iterating here */
            return 1;
        }
    }

    /* NEW decision: ask the substrate which arms the per-flow domain permits (pinned -> one arm via forced-exec
       predicate eval; unpinned -> prune only a provably-contradicted arm). The scheduler holds ZERO constraint
       logic — it only INTERPRETS the two booleans to take the one feasible arm, or fork when both are open. */
    int tf = 1, ff = 1;
    if (has) cons_arm_feasible(src, tok, true_op, false_op, g_c, &tf, &ff);
    if (has && !tf && ff) { cons_set(g_c, src, tok, false_op, jk); g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++; return 0; }   /* TRUE arm impossible */
    if (has && !ff && tf) { cons_set(g_c, src, tok, true_op, jk);  g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }   /* FALSE arm impossible */

    if (g_initial_boot) {
        /* INITIAL boot flow: KEEP the all-false PRIMARY (so g_boot_delta stays the logged-out baseline the whole
           frontier layers over) but FORK the TRUE arm as a sibling boot flow — one run explores boot's gates,
           replacing the monolithic (false-only, no exploration) pass AND its separate reg_add_boot re-run. */
        signed char *bsib = (signed char *)malloc((size_t)(g_c + 1));
        if (bsib) { for (int i = 0; i < g_c; i++) bsib[i] = g_dec[i]; bsib[g_c] = 1; reg_add_boot(ctx, bsib, g_c + 1); }
        cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? false_op : OPCMP_NONE, has ? jk : NULL);
        g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++;
        return 0;
    }
    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));   /* both arms feasible: FALSE sibling, take TRUE */
    CHECK(sib, "branch_fork_oom: cannot record the sibling arm — dropping it truncates BFS exploration; OOM is a physical floor");
    for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
    sib[g_c] = 0;
    /* SNAPSHOT-FORK (replaces the orphan replay re-run): an ORPHAN flow at the flow-base activation freezes its
       heap+DOM deltas into shared bases and SIGNALS a suspend — the scheduler snapshots the frame for the FALSE
       sibling, which CONTINUES from the snapshot (never re-runs from boot, so the cross-flow shared state its
       path depends on is preserved). Session/boot/async flows re-fork by RE-EXECUTION (they model a re-fired
       handler sequence / a re-run boot / replayed awaits — not a single-function replay); an orphan gate in a
       nested C-reentry (not the flow base, so the interpreter cannot unwind to suspend here) also re-runs. */
    if (!g_in_boot_flow && !(g_cur_flow && g_cur_flow->is_async) && JS_AtFlowBase()) {   /* sessions included: snapshot-continue avoids re-firing side-effecting handlers */
        heap_cow_fork(ctx); dom_cow_fork();            /* freeze this flow's heap+DOM deltas into shared bases (refcount 2: primary + sibling) */
        g_fork_sib_dec = sib; g_fork_sib_dec_n = g_c + 1;   /* the scheduler builds the sibling from this vector + the frame snapshot */
        JS_SetForkPending(1);                          /* OP_if rewinds to the gate opcode + suspends; the scheduler snapshots the frame */
        cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE, has ? jk : NULL);
        g_dec[g_c] = 1; g_dec_n = g_c + 1;             /* PRIMARY = TRUE; do NOT g_c++ — OP_if rewinds and re-runs the gate, replaying this recorded arm */
        return 1;                                      /* return value is IGNORED (OP_if rewinds on fork-pending) */
    }
    if (g_in_session) reg_add_session(ctx, sib, g_c + 1);      /* an exploratory session forks ANOTHER session (re-fire handlers with the sibling vector) */
    else if (g_in_boot_flow) reg_add_boot(ctx, sib, g_c + 1);  /* a boot flow forks ANOTHER boot flow (re-run boot with the sibling vector) */
    else if (g_cur_flow && g_cur_flow->is_async)               /* an ASYNC flow: re-run via the recipe (func+args) so the awaits — recorded in this SAME g_dec vector — replay too */
        spawn_async_sibling(ctx, g_cur_flow, sib, g_c + 1);
    else reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, sib, g_c + 1)->orphan_idx = g_cur_orphan_idx;   /* C-reentry orphan: re-run fallback (cannot suspend to snapshot) */
    cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE, has ? jk : NULL);
    g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;
    return 1;
}

int ctx_forks(void) {
    if (g_boot_replay) return 1;                                                   /* fixed-arm replay (exit after 1) */
    if (!g_running) return 0;
    /* A running flow ALWAYS carries a fork context: g_cur_fn is a real fn (sync/async/orphan dispatch), or it
       is a boot flow (g_in_boot_flow — incl. the initial forking boot), or a session (g_in_session). The old
       "monolithic boot -> drive-once" state (running, no fn, not boot, not session) is GONE — boot is now the
       initial FORKING boot flow — so assert the invariant instead of silently returning drive-once past it. */
    DCHECK(!JS_IsUndefined(g_cur_fn) || g_in_boot_flow || g_in_session,
           "ctx_forks: running flow has no fn handle and is neither boot nor session — the deleted monolithic-boot state should be unreachable");
    return 1;
}

/* SNAPSHOT-FORK sibling build — called from the scheduler's st==1 suspend handler once the flow has cleanly
   suspended at the gate (branch_decide froze its heap+DOM deltas into shared bases + set fork-pending). The
   sibling SHARES the frozen bases (refcount 2 from the fork) and CONTINUES from a SNAPSHOT of the frame — never
   a re-run from boot, so the cross-flow shared state its path depends on is preserved. Both resume at the gate
   and replay their recorded arm (primary TRUE via f->dec, sibling FALSE via g_fork_sib_dec). No-op if no fork
   is pending. NOTE: the base var_refs must already be detached (JS_FlowCloseVarRefs, done while applied). */
void fork_spawn_sibling(JSContext *ctx, Flow *f)
{
    if (!JS_ForkPending()) return;
    JS_SetForkPending(0);
    DCHECK(JS_FlowSnapshottable(f->fs), "snapshot-fork: frame not snapshottable (a still-open var_ref?) — JS_FlowCloseVarRefs must run first; never silently fall back to replay");
    Flow sib = *f;                                     /* inherit scalars (val, saved_c, orphan_idx, visits, cpu) */
    sib.fs = JS_FlowSnapshot(ctx, f->fs);              /* the sibling's OWN frame copy — continue from snapshot */
    CHECK(sib.fs, "snapshot-fork: frame snapshot alloc failed — OOM is a physical floor");
    sib.cow = NULL; sib.cow_n = sib.cow_cap = 0; sib.cow_base = f->cow_base;   /* SHARE the heap base (refcount 2) */
    sib.dom = NULL; sib.dom_n = sib.dom_cap = 0; sib.dom_base = f->dom_base;   /* SHARE the DOM base */
    sib.dec = g_fork_sib_dec; sib.dec_n = g_fork_sib_dec_n;   /* the FALSE arm at the fork point */
    g_fork_sib_dec = NULL; g_fork_sib_dec_n = 0;
    sib.handle = JS_DupValue(ctx, f->handle);
    sib.candidate = NULL; sib.vtarget = NULL; sib.drive_src = NULL;   /* owned strings not shared with an orphan sibling */
    sib.aresolve = JS_UNDEFINED; sib.areject = JS_UNDEFINED; sib.await_promise = JS_UNDEFINED;
    sib.rthis = JS_UNDEFINED; sib.rargs = NULL; sib.rargc = 0;
    reg_readd(ctx, sib);
}
