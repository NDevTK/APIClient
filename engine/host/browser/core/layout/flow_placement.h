/* CSS 2.1 §9.4.1 "Block formatting contexts"' STACK POSITIONS, HELD FOR ONE WHOLE-TREE GEOMETRY PASS — the
 * component that exists because §9.4.1's walk answers a question about EVERY in-flow child and its caller
 * takes ONE of the answers.
 *
 * THE DEFECT IT CLOSES IS NOT A SLOW FUNCTION, IT IS AN ANSWER THROWN AWAY. §9.4.1 says "boxes are laid out
 * one after the other, vertically, beginning at the top of a containing block", so the running offset a walk
 * of one container carries IS the top border edge of each box it passes. core/layout/block_flow.h's
 * `block_flow_child_top` runs that walk to learn where ONE box sits and discards the other N-1 positions it
 * just computed; a consumer that asks for the position of every box in a container therefore runs N walks of
 * N children where ONE walk had already produced all N answers.
 *
 * WHICH IS WHY MEMOIZING THE FUNCTION WOULD NOT HAVE HELPED, and that is the whole reason this is a RECORD
 * WRITTEN BY THE WALK rather than a remembered return value. A memo keyed on the asked-about element still
 * runs one walk per distinct element, and each walk is still over the whole child list — N walks of O(N),
 * which is the same product. What removes the multiplier is that the walk REPORTS every position it
 * establishes, so the first ask about any child of a container answers every later ask about its siblings.
 *
 * ITS LIFETIME IS A PASS AND IT HAS NO INVALIDATION RULE AT ALL, WHICH IS THE POINT AND NOT AN OMISSION.
 * core/layout/block_flow.h states the standing objection to remembering anything here — a remembered answer
 * is state about a tree the cascade can change under it — and that objection is exactly right about a record
 * with a lifetime. This one has none: it is EMPTY before a pass opens and EMPTY after it
 * closes, and the only span in which it holds anything is a span in which the document cannot change. Nothing
 * here decides whether an entry is still good, so nothing here can decide it wrongly.
 *
 * THE SPAN IS A WHOLE-TREE GEOMETRY WALK, AND WHAT MAKES IT SAFE IS ASSERTED AT THREE PLACES RATHER THAN
 * DESCRIBED AT ONE.
 *   - RE-ENTRY: a pass may not open inside a pass. Two passes would share one record with two lifetimes, and
 *     the inner close would empty a record the outer walk is still reading.
 *   - THE TREE: `dom_cow_version()` is read at open and asserted unchanged at close AND at every ask a pass
 *     answers. That number moves on an insert, a removal and — the case that matters here — on the COW SWAP,
 *     so a flow switch inside the pass is caught rather than reasoned about. solver/dom_cow.h states that
 *     property as the thing that makes a C-side record sound without being per-flow.
 *   - A STYLE WRITE, which the tree version cannot see, is made IMPOSSIBLE at its own chokepoint instead of
 *     watched for here: solver/dom_cow.c's single attribute-capture funnel asserts that no pass is open. A
 *     write that would invalidate this record crashes at the WRITE, naming this pass, which is the root form
 *     of the guarantee rather than a check that runs after the damage.
 *
 * AND THE RECORD IS CHECKED AGAINST THE WALK AT EVERY FIRST ASK, WHICH IS WHAT KEEPS THE TWO SITES FROM
 * PARTING. `block_flow_child_top` asks this record first; on a miss it runs §9.4.1's walk exactly as it always
 * did, and then asserts that the walk's own answer for the box it was looking for is the value this record now
 * holds for that box. The two are written by DIFFERENT lines of one loop — the walk reports `want` and the
 * record takes every child — so they can disagree, and a disagreement is a position every box below it on the
 * stack was placed against. That assert arms on every miss, which is every first ask of every container.
 *
 * WHAT IT IS NOT: a layout cache, a box tree, or a substitute for either. A real engine hangs a used position
 * on a box object and marks it dirty; this holds nothing between passes and answers nothing outside one, so a
 * page reading `offsetTop` between two renders gets the same freshly-walked answer it always did.
 *
 * RETIREMENT: this component goes when layout PRODUCES a box tree that paint reads, because a position is then
 * a field of the box rather than a function re-derived per ask, and there is nothing left to record. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_PLACEMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_PLACEMENT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* OPEN AND CLOSE THE PASS. `open` empties the record and takes the tree version it will be held to; `close`
   empties it again and releases its storage, so no allocation and no entry outlives the span. Both are
   required in one function of one component — the entry that performs the whole-tree walk — because a pass
   whose two ends are in two callers is a pass a returning arm can leave open. */
void flow_placement_pass_open(void);
void flow_placement_pass_close(void);

/* IS A PASS OPEN — read by solver/dom_cow.c's attribute chokepoint, which asserts it is NOT, and by nothing
   that branches on it to decide what to compute. It is a fact about the instrument, never an operand. */
bool flow_placement_pass_is_open(void);

/* ONE BOX PLACED BY §9.4.1's WALK, at its TOP BORDER EDGE in the frame core/layout/block_flow.h's
   `block_flow_child_top` answers in — the distance from the containing block's top CONTENT edge. Called for
   every in-flow child a walk passes, with no test of whether anybody will ask: the walk has the number in hand
   and deciding here who might want it is the throw-away this component exists to end.
   A NO-OP WHEN NO PASS IS OPEN, which is what keeps the walk's own code free of a mode.
   `el`'s CONTAINING BLOCK MUST BE ITS PARENT ELEMENT, which is core/layout/block_flow.h's
   `block_flow_child_top` precondition and is therefore this record's: that entry CRASHES for a box whose
   containing block is an ancestor the containing-block walk steps over, and a position stored under such a
   key would be a number returned where a refusal belongs on a build with the crash compiled out. §9.4.1's
   walk really can place one — it descends into an in-flow inline box §9.2.1.1 breaks — so the caller tests
   it at the write, where the walked container is in hand. */
void flow_placement_record(const lxb_dom_element_t *el, CssPx top);

/* THE ASK, COUNTED WHETHER OR NOT A PASS IS OPEN — the recording point is the QUESTION and never the outcome,
   so a run that opens no pass at all still reports how many positions it was asked for. Answers TRUE and
   writes `*out` when this record holds the box; FALSE means the caller owes §9.4.1's walk and must report it
   with `flow_placement_walked` so the census closes. */
bool flow_placement_ask(const lxb_dom_element_t *el, CssPx *out);

/* DOES THIS RECORD ALREADY HOLD `el` AT `top` — the entry a DCHECK takes, and the reason it is not
   `flow_placement_ask` with its census suppressed. It COUNTS NOTHING and MUTATES NOTHING, which is what
   §Offensive-programming requires of an assertion's condition: an ask counted from inside a check would make
   `asks == served + walks` false in a dev build and true in a release one, so the census would be a fact
   about which build took the measurement. It answers FALSE when no pass is open, when the record has no entry
   for the box, and when the two EXAMPLES differ — three states its one caller does not need apart, because
   each of them is the same finding: the walk's `want_top` write and the walk's record write have parted.
   THE COMPARISON LIVES HERE AND NOWHERE ELSE, and core/layout/flow_placement.c says why it is not an entry of
   core/css/css_length.h. */
bool flow_placement_agrees(const lxb_dom_element_t *el, CssPx top);

/* …AND THE WALK THAT ASK COST. Called by the one caller of `flow_placement_ask` on its FALSE arm, immediately,
   so that `asks == served + walks` holds at every instant and not merely at the end. */
void flow_placement_walked(void);

/* THE CENSUS, in solver/result.c's vocabulary. Every field is a LIFETIME counter of this agent — it states
   what has happened, never what stands now — so every one may be differenced across two samples.
   `asks` is the number of times §9.4.1's stack position of a box was asked for; `served` is how many of those
   this record answered; `walks` is how many ran the walk. `asks == served + walks` is asserted here rather
   than left to a reader's arithmetic, because it is the one property of the pair that says the two outcomes
   were counted at the same event. `placements` is how many positions the walks reported into the record and
   `passes` is how many whole-tree spans have opened; `placements / walks` is the amortization this component
   buys and `walks` alone is the number that must stop being quadratic in a container's child count. */
typedef struct {
    long long asks;
    long long served;
    long long walks;
    long long placements;
    long long passes;
} FlowPlacementCensus;
void flow_placement_census(FlowPlacementCensus *out);

#endif /* ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_PLACEMENT_H */
