/* The FRAME-AGNOSTIC decision — the core of the rebuilt solver, and the fix for what the old fork got wrong.
 *
 * solver_decide is JSFlowControlHooks.branch: the interpreter (a bytecode OP_if) OR a native builtin loop-back calls it
 * with a condition value. If the value is CONCOLIC, it decides which arm THIS flow takes and PARKS the other
 * arm as a sibling flow (append one arm to the decision vector + flow_add). It does NOT rewind an OP_if and it
 * does NOT snapshot a bytecode frame — a fork is purely "a new decision vector to replay from the flow's fn."
 * That is why it is frame-agnostic: nothing about it assumes the caller is bytecode, so a native builtin forks
 * by the exact same call, which is precisely where the old bytecode-gate-coupled design crashed. */
#ifndef ENGINE_HOST_SOLVER_DECIDE_H
#define ENGINE_HOST_SOLVER_DECIDE_H

#include "quickjs.h"
#include "solver/flow.h"

/* JSFlowControlHooks.branch (installed by the scheduler, engine_run). Returns the arm (0/1) for a concolic cond,
   or -1 when the value is not concolic (interpreter falls through to the normal ToBool). */
int  solver_decide(JSContext *ctx, JSValueConst cond);

/* JSFlowControlHooks.outcome — the same decision, asked by a C BUILTIN that has no OP_if to ask it at. `over`
   is the unknown operand its completion depends on, `op` names the operation ("JSON.parse"), `n` is how many
   completions the machine declares feasible. Returns the arm this flow takes, ORed with 0x100 when a sibling
   was prepared for the other — the same protocol solver_decide uses, because it is the same fork. */
int  solver_outcome(JSContext *ctx, JSValueConst over, const char *op, int n);

/* Take the decision vector out of a fork blob (ownership transfers; blob struct freed). For the replay fork. */

/* The scheduler brackets each flow run: enter loads the flow's decision vector as the replay source (cursor 0);
   leave clears the running-flow state. */
void decide_enter(JSContext *ctx, Flow *f);
void decide_leave(JSContext *ctx);

/* WHAT THIS FLOW HAS ALREADY DECIDED ABOUT THIS PREDICATE — 1 true, 0 false, -1 not decided.
 *
 * ASKED BY VALUE, and that is the whole point of the seam. The path constraint is keyed by a string this file
 * builds from the value (decide_key: the source path for a bare truthiness test, the source plus its operator
 * and token for a comparison, separated by a control character no field path contains). A component that
 * wanted the same answer could rebuild that string — and then the key FORMAT would live in two places, which
 * is the second copy that is always subtly wrong the first time one of them changes. It hands over the VALUE
 * instead and this file keys it, so there is exactly one speller of the key.
 *
 * WHY A BROWSER COMPONENT NEEDS IT AT ALL: a C read cannot fork. HTML §8.1.7.3 step 3 asks whether a document
 * is hidden while the page's `if (document.hidden)` over the SAME source is a fork — so the engine's read must
 * CONCRETIZE ON THE PIN (CLAUDE.md), taking the arm this flow already committed to rather than answering with
 * a modelled default the forked arm contradicts. -1 means the flow has committed to neither, and the caller
 * falls back to the concolic's own example, which is the modelled UA answer. */
int  decide_value_arm(JSValueConst cond);

/* Decisions taken on THIS run so far (the replay/extend cursor) — the scheduler reads it to know how far a
   re-run got before forking. */
int  decide_cursor(void);

/* Swap the running decision state when the scheduler interleaves flows: suspend snapshots the evolving vector
   + cursor of the paused flow; resume restores them + re-binds the flow's fn. */
void *decide_suspend(void);
void  decide_resume(void *blob, JSValueConst fn);
void  decide_blob_free(void *blob);

#endif
