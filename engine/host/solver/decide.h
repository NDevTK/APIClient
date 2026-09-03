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
   ONLY caller outside this component believed it: core/dom/abort.c asked `arm == 1`, which is false of every
   value a forking branch returns — so the flow took the FALSE arm while its decision vector recorded TRUE, and
   the two disagreed for the rest of the run. A protocol whose one caller got it wrong is a protocol nobody
   should be asked to remember, so the two halves are named here and read through the accessors below. Never
   compare a raw return value against an arm.
   AND THE ARM A FORK KEEPS IS NO LONGER ALWAYS TRUE, which is why that defect can no longer be described by
   ONE number. It used to be: a first-time fork returned 257 and nothing else, so `arm == 1` was wrong in a way
   a reader could memorise. A fork now keeps the arm the run OBSERVED — §Learning-from-replies' "at a branch
   the example marks the real arm PRIMARY" — so it returns 256 or 257 depending on what the value's concrete
   example was, and a caller comparing the raw value against an arm is wrong on a schedule nothing can predict
   from the source. The accessors are the whole of the contract. */
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
   completions the machine declares feasible. Returns the COMPLETION this flow takes, ORed with 0x100 when a
   sibling was prepared to carry the rest — the same protocol solver_decide uses, because it is the same fork.
   N IS NOT TWO, AND IT COSTS NOTHING TO SAY SO. "Which of N completions" is asked as the elimination sequence
   "is it c0? is it c1? …" — N-1 two-armed questions, each keyed by the COMPLETION it is about, each recorded
   as one ordinary boolean slot, each forking ONE sibling. So an N-way outcome needs no N-way vector slot, no
   sibling queue and no second frame from the step driver's one clone; the chain is drawn lazily, one link per
   time the scheduler picks the sibling that carries the remainder. The walk is at solver_outcome and states
   why the order it asks in is forced rather than chosen.
   `real` IS THE ASKING MACHINE'S DECLARATION, AND IT IS THE OUTCOME SEAM'S ANSWER TO WHAT decide_real_arm
   COMPUTES FOR A BRANCH — "which completion does a session carrying real values reach". It is a PARAMETER for
   the same reason `nonforking` above is: the answer belongs to the site and to no two sites alike, and this
   seam holds the operand and the operation's NAME but never its semantics, so anything it derived would be a
   recognizer over that name. JS_OUTCOME_REAL_UNSTATED (quickjs.h) is the machine's positive "I cannot say" and
   is what every ask site passes until it states one; the seam maps it onto the same "nothing observed says"
   a branch over an example-free value produces, so both arms run and neither is marked forced.
   THE FORKED BIT IS FOR THE STEP DRIVER, WHICH IS THE ONLY THING THAT CAN CONSUME IT FOR A C BUILTIN. A plain
   C body is already inside its activation when it asks and has no machine state for the other arm to be
   snapshotted at, so a fork from one crashes at the seam naming the operation — and what that names is the
   DECLARATION to build: JS_CFUNC_STEP_DEF, with the ask moved into the machine's own step_fork_run. */
int  solver_outcome(JSContext *ctx, JSValueConst over, const char *op, int n, int real);

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
   THE TWO ARE COUNTED IN SEPARATE TABLES AND RENDERED IN ONE OBJECT, because only one of them is a page's to
   enlarge: a mechanism row is prose written at a call site in this tree, so that population is enumerable
   where it is authored and is counted EXACTLY, while a constraint key is the document's and is what a fixed
   census must choose among. Sharing one table would spend its scarce slots on rows that were never at risk and
   would let the heaviest mechanism — one that fires on every orphan drive outweighs every individual branch
   beneath it — evict the predicates the census exists to name. A consumer that wants "which PREDICATE" rather
   than "which fork site" partitions the object by the same rule fork_key_count asserts: a constraint key opens
   on concolic_ident_compose's decimal length prefix, a mechanism row on `(`, and the census's own bound member
   on `_`. THE OVERFLOW ROW IS MATCHED BY NAME BEFORE THAT TEST AND NEVER BY IT — it is prose, so it opens on
   `(` exactly as a mechanism does, and it is the opposite kind of thing: a mechanism row NAMES a site this
   tree wrote, the overflow row is the mass of the sites it CANNOT name. Splitting on the byte first and
   subtracting after files the largest thing in the object under the population that is exact by construction,
   which is the argmax reading inverted rather than approximated.
   READ THE COUNTS AS ARRIVALS, NOT AS PRODUCTION. Each row is how many flows REACHED that site, and every
   fork upstream doubles what reaches everything below it, so a program with k independent gates in sequence
   produces rows in a geometric series and the LAST site in program order is always the largest. A biggest row
   is therefore read as the BOTTOM OF A CASCADE and not as one hot predicate — anything done to that site
   leaves the series that feeds it untouched. (A percentage measured on some past run used to stand here as the
   worked example. It was status: it named a fixture and a revision it did not carry, so it stayed quotable
   long after it stopped being true of any tree, and the shape above is the whole of what it was teaching.)

   AND THAT SENTENCE IS WHY ADMISSION CANNOT BE BY ARRIVAL, which is a property of the table and not of any
   run. The largest row is the LAST site in program order; latest and largest are therefore the same end of the
   program, so a fixed table that claims its rows FIRST-COME is structurally likeliest to exclude exactly the
   row it exists to name — and it excludes it into a bucket rendered in the same object under the same shape as
   a real key, so a consumer taking the argmax hands back a NON-SITE as its answer to "which predicate is
   growing the frontier". Which keys a fixed census RETAINS is the question, and it is a known one outside this
   file: the table is Space-Saving, the argument against Misra-Gries is at the rows' declaration, and growing
   the table until some fixture stops overflowing is not a candidate at all because it fixes one document and
   no other.
   A ROW PUBLISHES A FLOOR, SO THE CENSUS UNDERSTATES AND NEVER INVENTS. A predicate row's count is the hits it
   can prove are its own; the mass it inherited from whatever it displaced is published once, summed, as a row
   of its own. Those still partition the counters, so the rows still sum to the total, and each of the three
   readings a consumer can take off them is exact rather than approximate: that row ABSENT means no eviction
   ever happened and no site was excluded; that row LEADING means more mass is unattributable than the
   best-proven site can claim, so the table has not answered and the argmax is not worth quoting; and below
   that, it is the bound on how far any one named row understates.
   WHAT THE CENSUS CANNOT SAY IS WHICH KEYS ARE MISSING, SO IT SAYS HOW BIG A MISSING ONE CAN BE. An evicted
   site and a site the document never reached are both ABSENT from this object and no arrangement of these rows
   tells them apart. A count of ZERO is not a third state and cannot occur: a row is claimed BY an arrival and
   only grows, so absence is this table's only way of saying "not seen". What separates the two absences is the
   LIGHTEST resident count — Space-Saving's guarantee is that a key the table is NOT holding has taken at most
   that many, so a key below it may have been evicted while a key above it cannot have been — and it is emitted
   beside the rows as a MEMBER of the object rather than a row of it, under a name opening on `_` that neither
   namespace can spell, because it is a bound on hits and not a count of them and must never join the total.
   IT RIDES EVERY OBJECT THAT HAS ROWS, AND ITS VALUE CARRIES THE STATE RATHER THAN ITS PRESENCE. Nothing is
   excluded until an eviction happens and an eviction leaves an error no later eviction can clear, so a spill
   of zero IS "every key ever seen is resident" and the bound is then exactly ZERO — a reading, not a sentinel,
   and the tight answer rather than the algorithm's loose one. A member that came and went could not be told
   from one a producer PREDATING IT never wrote, and a stale census would be partitioned happily with its old
   overflow bucket — prose, opening on `(` — filed among the mechanism rows as the largest site this tree
   names. Above zero it answers the question the summed spill structurally cannot: whether the excluded mass is ONE hot predicate
   that fell out of the table or a long tail, which decide_fork_json's own note calls two readings with
   opposite fixes. When the bound is below the largest named predicate's published floor, no excluded site can
   outrank that row and the argmax is safe to quote WHATEVER the spill is — which is a stronger statement than
   "the named rows are a floor", and it is not derivable from the rows.
   WHAT IS STILL NOT EMITTED is each row's own inherited error, which would bound one row's understatement
   tightly instead of by the whole spill. It has no reader and would need a shape change (`key -> [floor,
   err]`) that extension/bridge.js's finite-number DCHECK and popup.js's generic renderer both refuse, so it
   lands with those two consumers or not at all; a field written for nobody is the same broken contract as a
   field read from nobody.
   It is keyed by the CONSTRAINT key rather than a file:line because that is what a predicate IS here — two
   forks at one source and operation are one predicate however many call sites spell it, and a chain (a source
   whose operation string carries a position) shows as `distinct` climbing with `total`, which distinguishes
   the two shapes on its own.
   THE TABLE RENDERS ITSELF, as one JSON object of key→hits plus the one `_`-named bound member above (caller
   frees) — see the composer for why the
   escaping belongs here and for why it rides solver/result.c's document as `_forkAt` rather than a host's
   printf. The indexed accessor this replaces had exactly ONE caller, in `run_scheduler`, which the production
   ABI never enters. */
char *decide_fork_json(void);
long  decide_fork_total(void);

/* THE REPLAY LEDGER — WHAT A REPLAY DID WITH THE PATH IT WAS HANDED. §Time-travel-resume's claim is that a
 * parked flow resumes as "the same execution continued, byte-identical", and its razor is that a resume which
 * "drops, starves, skips, reorders, or forgets ANY flow" is a CAP. This engine ASSERTS that at the branch —
 * decide.c's dec_replay consumes a recorded arm only when the branch's constraint key equals the key recorded
 * beside it — and until these three rows it could state NOTHING about the OUTCOME. dec_leave_path had one call
 * site and no counter, so a session that rebuilt N flows and abandoned its recorded path at the first branch of
 * every one of them published `resumed`/`resumedSegs`/`resumedFlows`/`resumedCands` IDENTICAL to a session that
 * replayed every arm. Those four say what the DOCUMENT HELD; these three say what the REPLAY DID WITH IT.
 *
 * KIND: all three are LIFETIME COUNTS over the SESSION, released with it (decide_free), monotone within one,
 * and therefore differenceable between two samples of ONE session — unlike the `stepUnits` GAUGE beside them
 * in the same census, which states who is standing in an arm at the instant it is taken.
 * UNIT: `hits` and `left_arms` are ARMS (decision-vector slots). `left` is EVENTS — one per divergence,
 * whatever it abandoned. THE MIDDLE ONE IS THE TRAP and it is the `svcMax` shape: a name built from a verb
 * reads as a count of arms, and the accessor increments once per CALL of dec_leave_path. Read the accessor.
 * ACCESSOR: plain reads of three statics; no division, no derivation, nothing behind the names.
 * IDENTITY, checkable off the emitted numbers and asserted where all three are in one hand
 * (solver/result.c's composer): `left_arms >= left` AND `(left == 0) == (left_arms == 0)`. Both come from
 * dec_leave_path's own precondition `g_c < dec_total()`, which makes every divergence abandon at least one arm;
 * they are two clauses and not one because `left_arms >= left` alone permits `left == 0` beside a non-zero sum.
 *
 * NAMED RESIDUAL — THE POPULATION IS EVERY REPLAY THIS SESSION PERFORMED, AND THESE ROWS DO NOT SAY WHICH.
 * WHAT IS NOT COVERED: three mechanisms replay recorded arms and all three land in one number here — a
 * COLD-RESUMED flow replaying its whole path from cursor 0 (decide_blob_new); a BRANCH-FORK sibling replaying
 * the single arm dec_seg_arm appended above its cursor (decide_fork_blob asserts `cursor == dec_total()`, so
 * there is exactly one arm above it); and a VALUE-FORK sibling of a MID-REPLAY parent, which
 * decide_fork_same_path hands the parent's whole remaining tail and which this header's own contract says may
 * legitimately diverge when a peer answers differently today. The first and the third can abandon tails of the
 * SAME MAGNITUDE, so reading the numbers harder does not separate them and this residual is not closable by a
 * consumer. What the rows CAN say on their own is one thing and it is exact: `resumed: 0` beside `left > 0`
 * is siblings, because that row states this session was handed no residue at all.
 * WHAT THE NEXT DIFF BUILDS: an ORIGIN field on decide.c's DecideBlob, written at the one site that mints a
 * cold rebuild (decide_blob_new), carried across decide_suspend so a flow that parks mid-replay does not
 * launder it, and inherited by decide_fork_same_path (whose sibling really is replaying the cold flow's tail)
 * but NOT by decide_fork_blob (whose sibling stands at the end of the vector) — with these three rows split
 * per origin. It is plumbing across four blob sites, which is why it is named here rather than guessed at.
 * HOW ITS ABSENCE WOULD SHOW: a session reporting `resumed: 1` with a large `replayLeftArms` that is in fact a
 * peer's answer having moved — a correct and expected divergence — read as the cold tier having lost a path,
 * or the same numbers read the other way and a real loss excused as a moved peer. */
void decide_replay_stats(long *hits, long *left, long *left_arms);

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
 * of arms is regenerated while the mapping from arm to peer timeline is not. Recording it needs a column of
 * its own — an ANSWER beside the arm, naming the peer WORLD the answer came from — and nothing records that
 * yet. This line used to point at solver_outcome's `n == 2` DCHECK as the same missing slot, which was wrong
 * in both directions and is corrected at decide_fork_same_path's own site: an outcome's completions are
 * NUMBERED by the machine at its definition, so they mean the same thing in every session and are recorded as
 * N-1 ordinary boolean arms; a peer's answers are a set only the peer knows, so they have no index a slot
 * could hold.
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
