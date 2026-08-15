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

/* JSFlowControlHooks.branch (installed by the scheduler, engine_run). For a concolic cond it returns the arm
   THIS flow takes, ORed with SOLVER_FORKED_BIT when a sibling was prepared for the other arm; -1 when the value
   is not concolic (the interpreter falls through to the normal ToBool).
   THE FORKED BIT IS PART OF THE RETURN VALUE AND THIS SAID IT WAS NOT. It documented "the arm (0/1)", and the
   ONLY caller outside this component believed it: core/dom/abort.c asked `arm == 1`, which is false for the 257
   a first-time fork returns — so the flow took the FALSE arm while its decision vector recorded TRUE, and the
   two disagreed for the rest of the run. A protocol whose one caller got it wrong is a protocol nobody should
   be asked to remember, so the two halves are named here and read through the accessors below. Never compare a
   raw return value against an arm. */
#define SOLVER_FORKED_BIT 0x100
/* The arm this flow takes, from a solver_decide/solver_outcome result known to be >= 0. */
#define SOLVER_ARM(r)     ((r) & (SOLVER_FORKED_BIT - 1))
/* Whether a sibling flow was prepared for the other arm — the interpreter's cue to snapshot-fork the frame. */
#define SOLVER_FORKED(r)  (((r) & SOLVER_FORKED_BIT) != 0)
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

/* WHICH PREDICATE IS GROWING THE FRONTIER — the most-forked constraint key, its hits, the total number of
   forks and how many distinct predicates have produced one. A frontier that grows without stopping is growing
   at ONE branch, and every counter in the progress stream says only that it is growing: `flows` climbing while
   `live` stays flat says the shape is a CHAIN and still not where the chain is. This is where.
   It is keyed by the CONSTRAINT key rather than a file:line because that is what a predicate IS here — two
   forks at one source and operation are one predicate however many call sites spell it, and a chain (a source
   whose operation string carries a position) shows as `distinct` climbing with `total`, which distinguishes
   the two shapes on its own. Walk `i` from 0 until it answers NULL; the key is borrowed and stable. */
const char *decide_fork_at(int i, long *hits);
long        decide_fork_total(void);

/* Swap the running decision state when the scheduler interleaves flows: suspend snapshots the evolving vector
   + cursor of the paused flow; resume restores them + re-binds the flow's fn. */
void *decide_suspend(void);
void  decide_resume(void *blob, JSValueConst fn);
void  decide_blob_free(void *blob);

/* THE DECISION CHAIN IS THE ONE PART OF A SNAPSHOT WITH A CROSS-TIER IDENTITY, and these six entries are how
 * the cold tier reads and rebuilds it. Everything else a parked flow holds names its target by a LIVE HEAP
 * POINTER — the COW delta's slots, the suspended frame chain's bytecode and closed cells, the constraint's
 * segments — so none of it can cross a park, a session or an instance. The vector can, because it is not a
 * pointer at anything: it is the sequence of ARMS this flow took, and flow.h defines a flow as exactly that
 * ("its state is replay(baseline, decision vector)"). Everything else is REGENERATED by replaying it, which is
 * also what §Time-travel-resume asks for — a resumed flow re-derives its example values from CURRENT sources
 * while its own path comes from the snapshot.
 *
 * THAT CHOICE DISSOLVES THE HAZARD `qjs_begin` USED TO NAME. A serialized snapshot would have to write down
 * where a flow suspended INSIDE a step machine, and a stage index means whatever this build's stage constants
 * say — so a recipe carrying one silently resumes at a different step of the same algorithm the first time
 * those constants move. A replayed flow names no rest point at all: it re-enters the machine by running the
 * same code again, so there is no index to go stale.
 *
 * THE SEGMENTS ARE WHAT CROSSES, NOT THE FLATTENED VECTOR. A fork shares its parent's whole prefix (see the
 * chain's own comment above), so writing each flow's path out in full would multiply that sharing back out —
 * 16046 flows standing on an 8000-deep chain is 128 MB of text for 658 KiB of actual chain, the same quadratic
 * this file deleted from RAM, re-created on the way to disk and allocated at the exact moment RAM pressure
 * asked for relief. The cold tier therefore writes each SEGMENT once, names it by a park-local ordinal, and
 * gives each flow the ordinal it stands on; the rebuild is the mirror. */
const void *decide_blob_seg(const void *blob);          /* the segment a parked flow's decision state stands on */
const void *decide_seg_base(const void *seg);           /* …and the one below it (NULL at the bottom) */
int         decide_seg_arms(const void *seg, const signed char **arms);   /* its own arms; returns how many */
/* Rebuild one frozen segment over `base`, holding ONE reference on it exactly as a freeze does. The returned
   segment carries the REBUILDER's reference; release it with decide_seg_release once every flow that stands on
   the park's chain has taken its own, so a segment nothing references is freed rather than leaked. */
void       *decide_seg_new(void *base, const signed char *arms, int n);
void        decide_seg_release(void *seg);
/* A resumed flow's decision state: the rebuilt chain, at CURSOR 0. It is a resume rather than an enter because
   the flow's arms are already recorded — it replays them from the top as it re-runs the document, and forks
   normally the moment the cursor reaches the end of what the recipe knew. */
void       *decide_blob_new(void *seg);

/* HOW BIG ONE FLOW'S DECISION STATE IS — the slots in it and the bytes it holds. The cold tier asks because the
   vector is PER FLOW and a fork COPIES the parent's prefix into it, so a chain of forks at one predicate costs
   O(n) per flow and O(n^2) over the frontier. A number nobody could read is how that stayed invisible. `blob`
   is a parked flow's (NULL for the running one, whose state is live here — decide_live_stats answers that). */
void decide_blob_stats(const void *blob, long *entries, long *bytes);
void decide_live_stats(long *entries, long *bytes);

/* WHAT THE FROZEN DECISION CHAIN IS HOLDING — the fourth of the four chains built on cow.c's refcounted
   immutable segment, reported beside the other three because a per-flow allocation nobody released looks the
   same from any one of them and only the one that CLIMBS names which. A flow's own decision state is a POINTER
   at a segment now, so what used to be the frontier's largest per-flow row is here, counted once. */
void decide_chain_stats(long *segs, long *entries, long *bytes);

/* The frontier's decision state going down with the frontier — called by flow_registry_free, which is the one
   teardown every host already runs. Releases the running flow's chain reference and the single reusable head
   buffer, and asserts that no frozen segment outlived the flows that referenced it. */
void decide_free(void);

#endif
