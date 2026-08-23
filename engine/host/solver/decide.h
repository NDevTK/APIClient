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

#include <stdint.h>   /* decide_seg_keys' column type is part of this interface, so it is named here */
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

/* `nonforking` — THE ARM THIS SITE'S ALGORITHM TAKES IN A SESSION THAT EXPLORES NOTHING, and it is a PARAMETER
 * because the answer belongs to the CALLER and to no two callers alike.
 *
 * A session declares whether it forks (solver/engine.h's engine_session_forks). The two callers that reach the
 * seam through the hook TABLE already answer this for themselves, by the table being empty: the interpreter's
 * `branch` is absent, so the arm is -1 and the ordinary ToBool decides the `if`; the step driver's `outcome` is
 * absent, so the machine takes outcome 0, which is why every step machine numbers its ordinary completion
 * there. A browser component asking BY SYMBOL has neither, and there is no answer the SEAM could pick for it:
 * "is this AbortSignal aborted" and "is this moment before that one" are two different algorithms and a single
 * choice made here would be right for at most one of them — and picking a timer order at random deletes a
 * program the page really runs. So the site says, at the ask, what its own algorithm does with one world.
 *
 * SOLVER_NO_NONFORKING_ARM IS A POSITIVE STATEMENT: "this question has NO answer with only one world in it."
 * A site returns it where its operands carry nothing to decide from — and reaching a NEW decision there in a
 * non-forking session then CRASHES naming the predicate, because the fix is that the site must not be
 * REACHABLE in such a session (the caller that put an undecidable operand in front of it), never a default the
 * seam invents. It is also what the interpreter's own hook passes, which costs nothing: that hook is installed
 * only in a session that forks, so the value is never read.
 *
 * IT IS NOT CONSULTED FOR A DECISION THIS FLOW ALREADY HAS. A refined arm (the constraint already answers this
 * predicate) and a replayed arm (the vector recorded it) are reached first and unchanged, so a candidate
 * re-fire replaying its recorded path never asks a site for this at all. */
#define SOLVER_NO_NONFORKING_ARM (-1)
int  solver_decide(JSContext *ctx, JSValueConst cond, int nonforking);

/* THE SAME DECISION, ASKED BY ENGINE CODE THAT IS RE-REACHED BY RE-RUNNING THE FLOW'S SCHEDULER STEP — and the
 * difference from the call above is not the QUESTION, it is where the sibling comes back.
 *
 * A prepared fork is only half a sibling: the other half is a RESUME POINT, and every consumer of
 * SOLVER_FORKED_BIT is an activation that owns one — the interpreter's branch fork clones the frame at the
 * `if`, a step machine's JS_STEP_FORK clones the machine at its ask. Engine code running BETWEEN a flow's
 * tasks has no activation at all: HTML §8.1.7.3 Processing model step 2.1 chooses the next task queue "in an
 * implementation-defined manner", so the engine's own choice is made with the flow switched in (its delta, its
 * DOM head and its decision state are the right ones) and nothing of it on any stack. That is not a missing
 * resume point, it is a resume point made of the flow's own recorded state: the sibling is assembled with NO
 * frame, and re-entering its scheduler step re-runs the same walk and REPLAYS the arm recorded for it here.
 *
 * SO THE CALLER DECLARES THE CONTRACT, which is that its whole computation is re-reached by re-running the
 * flow's step — no page code has run under it, nothing it has already done would be done twice. The seam
 * asserts the two halves it can see (the flow holds no program frame; the runtime holds no activation) and
 * builds the sibling before returning, so this NEVER returns SOLVER_FORKED_BIT: there is nothing left for the
 * caller to snapshot. A caller that cannot make that promise asks solver_decide, and a fork there crashes at
 * this seam naming the predicate rather than stranding a blob for some later fork to trip over.
 *
 * IT IS NOT decide_fork_same_path, WHICH IS A DIFFERENT MECHANISM AND WOULD LOSE THE ARM. That one is for a
 * fork over a VALUE THAT ARRIVED, where no question was asked and there is no slot to record; this one asks a
 * real predicate, so the sibling MUST carry the other arm at the cursor. A sibling given its parent's path
 * unchanged would re-reach this walk with nothing recorded, re-fork, take the same arm as its parent, and mint
 * another sibling exactly like itself — an unbounded chain at one site whose second arm never runs. */
int  solver_decide_restartable(JSContext *ctx, JSValueConst cond, int nonforking);

/* JSFlowControlHooks.outcome — the same decision, asked by a C BUILTIN that has no OP_if to ask it at. `over`
   is the unknown operand its completion depends on, `op` names the operation ("JSON.parse"), `n` is how many
   completions the machine declares feasible. Returns the arm this flow takes, ORed with 0x100 when a sibling
   was prepared for the other — the same protocol solver_decide uses, because it is the same fork.
   THE FORKED BIT IS FOR THE STEP DRIVER, WHICH IS THE ONLY THING THAT CAN CONSUME IT FOR A C BUILTIN. A plain
   C body is already inside its activation when it asks and has no machine state for the other arm to be
   snapshotted at, so a fork from one crashes at the seam naming the operation — and what that names is the
   DECLARATION to build: JS_CFUNC_STEP_DEF, with the ask moved into the machine's own step_fork_run. */
int  solver_outcome(JSContext *ctx, JSValueConst over, const char *op, int n);

/* Take the decision vector out of a fork blob (ownership transfers; blob struct freed). For the replay fork. */

/* The scheduler brackets each flow run: enter loads the flow's decision vector as the replay source (cursor 0);
   leave clears the running-flow state. */
void decide_enter(JSContext *ctx, Flow *f);
void decide_leave(JSContext *ctx);

/* WHAT THIS FLOW HAS ALREADY DECIDED ABOUT THIS PREDICATE — 1 true, 0 false, -1 not decided.
 *
 * ASKED BY VALUE, and that is the whole point of the seam. The path constraint is keyed by a string this file
 * builds from the value (decide_key: the IDENTITY of the value the branch tests — and a comparison result's
 * identity already carries its operator and both its operands, so there is no second rule for comparisons).
 * A component that wanted the same answer could rebuild that string — and then the key FORMAT would live in
 * two places, which is the second copy that is always subtly wrong the first time one of them changes. It
 * hands over the VALUE instead and this file keys it, so there is exactly one speller of the key.
 * -1 also answers for a value whose identity this engine cannot spell, which is the sound answer: nothing is
 * decided from it and every branch over it keeps both arms.
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

/* WHAT IS GROWING THE FRONTIER — the most-forked key, its hits, the total number of forks and how many
   distinct sites have produced one. A frontier that grows without stopping is growing at ONE place, and every
   counter in the progress stream says only that it is growing: `flows` climbing while `live` stays flat says
   the shape is a CHAIN and still not where the chain is. This is where.
   IT IS EVERY FORK AND NOT EVERY PREDICATE, which is a correction rather than a widening: a sibling minted
   over a VALUE THAT ARRIVED (decide_fork_same_path) is a member of the frontier and asked no question, so it
   was in neither the total nor the rows while the total was the number a reader subtracts to account for where
   the frontier's members came from. Each such mechanism has its OWN row, named for what it is — a peer's
   answer, a message from one arm of a sender's branch, a drive of a function the page never called are three
   different things and one row for all three describes none of them — and never a fabricated predicate key,
   which would merge with a real one the moment a predicate spelled the same way.
   READ THE COUNTS AS ARRIVALS, NOT AS PRODUCTION. Each row is how many flows REACHED that site, and every
   fork upstream doubles what reaches everything below it, so a program with k independent gates in sequence
   produces rows in a geometric series and the LAST site in program order is always the largest. Measured on
   the full-document smoke: 32, 64, 128, 256, 512 across five distinct gates, with the biggest row 44% of all
   forks — read as one hot predicate that is a hot predicate, and read correctly it is the bottom of a cascade
   that would be untouched by anything done to it.
   It is keyed by the CONSTRAINT key rather than a file:line because that is what a predicate IS here — two
   forks at one source and operation are one predicate however many call sites spell it, and a chain (a source
   whose operation string carries a position) shows as `distinct` climbing with `total`, which distinguishes
   the two shapes on its own. Walk `i` from 0 until it answers NULL; the key is borrowed and stable. */
const char *decide_fork_at(int i, long *hits);
long        decide_fork_total(void);

/* THE SIBLING'S DECISION STATE AT A FORK THAT TOOK NO ARM — and the arm-taking fork above is the special case,
 * not this one. A flow forks over a VALUE as well as over a predicate: a peer document's state IS its flows, so
 * one cross-instance read has N true answers and the asking flow explores one arm per DISTINCT ANSWER
 * (engine.c's flow_answer_fork). Nothing about that is a branch — the asking program asked no question it could
 * have answered two ways, so there is no slot to record and the sibling's path is its parent's, unchanged.
 * It freezes the running head exactly as the branch fork does, so both flows stand on ONE shared immutable
 * prefix and neither pays for the other's, and it hands back a blob at the parent's CURRENT cursor rather than
 * at the end of the vector: a flow may fork here mid-replay (a cold-resumed flow re-reaching the read), and a
 * sibling given the end would skip every arm its parent still had to consume.
 * WHAT IT CANNOT CARRY IS THE ANSWER, and that is the honest limit of a decision vector: the arms are what the
 * flow DECIDED, and which of a peer's timelines answered it is not one of them. A parked arm therefore resumes
 * by re-running, re-asking, and taking the first answer of whatever the peer's timelines say today — so the SET
 * of arms is regenerated while the mapping from arm to peer timeline is not. Recording that mapping is the
 * N-way outcome slot solver_outcome's own DCHECK already names.
 *
 * `why` NAMES WHAT ARRIVED, AND IT IS A PARAMETER BECAUSE THE ANSWER FORK IS NOT THE ONLY CALLER. The census
 * row this mints is the frontier's PROVENANCE (decide_fork_at), and it stood as one fixed string — "a peer's
 * answer arrived" — while three unrelated things were counted under it: the answer fork, the ORPHAN DRIVE of a
 * function the page never called, and the delivery-time fork over two sender arms. Two of those three had
 * nothing to do with an answer, so the row said something false about most of what it counted, and the reader
 * it is written for is one asking WHICH mechanism is growing the frontier. It is still not a PREDICATE key and
 * must not be spelled like one — there is no question here for a replay to re-ask — so it is prose naming the
 * mechanism, and every caller passes a string that says what it is rather than what it forked over. Borrowed:
 * the row copies it. */
void *decide_fork_same_path(const char *why);

/* THE SAME FREEZE WITH NO FRONTIER MEMBER BEHIND IT — for a caller that wants the running flow's PATH and is
 * not minting a flow to stand on it.
 * IT IS NOT A VARIANT OF THE CALL ABOVE, IT IS THE OTHER HALF OF WHAT THAT CALL DOES. decide_fork_same_path
 * freezes the head AND counts a fork, because §census defines the frontier's provenance as
 * `created = 1 + forks + candidates + joined documents + cold resumes` and every sibling it mints is a member
 * of that sum. A caller that takes the blob and creates NO flow makes that identity false by one — and the
 * comment on the count says exactly what a wrong term costs a reader: "a sum labelled with one of its terms is
 * a number that means whichever term the reader had in mind". The @S re-injection point is that caller: it
 * holds a path so that candidates SEEDED LATER can replay it, and those candidates are already counted as
 * candidates. Counting them again as forks would double them in the one identity that says where the frontier
 * came from.
 * Refcounting is identical — the returned blob carries the caller's reference on the frozen segment, released
 * with decide_blob_free — and so is the cursor. */
void *decide_freeze_path(void);

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
/* …AND WHICH QUESTION EACH OF THOSE ARMS ANSWERS — the same count, in the same order. IT CROSSES THE PARK, and
   that is the whole point rather than a completeness detail: the divergence this column exists to catch is the
   CROSS-SESSION one. A resumed flow replays at cursor 0 against today's code and today's replies (decide_blob_new
   below), so its question sequence is the one most likely to differ from the recorded run's, and a key that
   died at the park boundary could see everything except that. RAM-only, it would check a hot sibling's single
   replayed slot and nothing else — and nothing else would check the case the column is for. See decide.c's
   DecSeg for the width and for what a hash can and cannot promise. */
const uint32_t *decide_seg_keys(const void *seg);
/* Rebuild one frozen segment over `base`, holding ONE reference on it exactly as a freeze does. The returned
   segment carries the REBUILDER's reference; release it with decide_seg_release once every flow that stands on
   the park's chain has taken its own, so a segment nothing references is freed rather than leaked.
   `keys` IS REQUIRED AND IS NOT DEFAULTABLE: a segment rebuilt without one would hand every flow standing on it
   arms whose questions are unknown, and the replay check would have nothing to compare against — the exact
   §Offensive-programming shape where a consumer's default turns "the producer does not produce this" into a
   plausible datum. A document that cannot supply it is residue from a writer that predates the column. */
void       *decide_seg_new(void *base, const signed char *arms, const uint32_t *keys, int n);
void        decide_seg_release(void *seg);
/* THE SEGMENT'S NAME IN THE PARK DOCUMENT BEING WRITTEN — -1 until the pager assigns one, and the ordinal
   thereafter. It lives on the SEGMENT rather than in a table the pager keys by address because a park now runs
   more than once per session (a PARTIAL self-park writes a low-value tail and releases it, then writes another
   later), so between two of those runs a segment can be freed and its address handed to a segment on a
   different path — and a table would answer that one with the dead segment's ordinal, resuming a flow onto a
   path nothing ever took. The name dies with the segment instead. Set once per segment per document; the
   setter asserts both halves. */
long        decide_seg_park_id(const void *seg);
void        decide_seg_set_park_id(const void *seg, long id);
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
