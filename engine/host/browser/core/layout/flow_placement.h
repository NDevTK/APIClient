/* WHAT A WHOLE-TREE GEOMETRY PASS KNOWS ABOUT A BOX — three facts under one span, each of them an answer a
 * walk already computed and a caller already threw away.
 *
 * THE NAME STAYS `flow_placement` AND THAT IS A DECISION RATHER THAN AN OMISSION. This opened on CSS 2.1
 * §9.4.1 "Block formatting contexts"' stack positions when that was the only fact here, and a banner that
 * still opened there would be describing a third of its own subject — which is the defect a reader meets
 * first and the one nothing mechanical reports. What the three have in common is PLACEMENT in the sense the
 * directory uses it: where a box sits (§9.4.1's stack position), where its border box is (CSS 2 §8.1's
 * origin, placed through §10.1's cases), and what it contributes to the stack it sits on (§10.6.3's content
 * height with §8.3.1's two adjoining runs). Renaming the file would move three components' includes for a
 * word, and the word is not wrong.
 *
 * THE ONE SPAN IS THE COMPONENT AND THE THREE RECORDS ARE ITS TENANTS. Every one of them is a pure function
 * of the same two inputs — the tree and the cascade — so all three go stale on exactly the same events, and
 * that is what makes ONE span correct for all three rather than one span stretched over three questions. One
 * open, one close, one tree-version check, one table: three tables would be three probes and three growth
 * policies over one key space, and each record's own check reads more than one of the facts.
 *
 * THE FIRST OF THE THREE, AND THE ONE THE COMPONENT WAS BUILT FOR: §9.4.1's walk answers a question about
 * EVERY in-flow child and its caller takes ONE of the answers.
 *
 * THE DEFECT IT CLOSES IS NOT A SLOW FUNCTION, IT IS AN ANSWER THROWN AWAY. §9.4.1 says "boxes are laid out
 * one after the other, vertically, beginning at the top of a containing block", so the running offset a walk
 * of one container carries IS the top border edge of each box it passes. core/layout/block_flow.h's
 * `block_flow_child_top` runs that walk to learn where ONE box sits and discards the other N-1 positions it
 * just computed; a consumer that asks for the position of every box in a container therefore runs N walks of
 * N children where ONE walk had already produced all N answers.
 *
 * WHICH IS WHY MEMOIZING *THAT* FUNCTION WOULD NOT HAVE HELPED, and that is the whole reason §9.4.1's half
 * of this component is a RECORD WRITTEN BY THE WALK rather than a remembered return value. A memo keyed on
 * the asked-about element still runs one walk per distinct element, and each walk is still over the whole
 * child list — N walks of O(N), which is the same product. What removes the multiplier is that the walk
 * REPORTS every position it establishes, so the first ask about any child of a container answers every later
 * ask about its siblings.
 * READ NARROWLY: IT IS A STATEMENT ABOUT A WALK AND NOT ABOUT MEMOS. This file's OTHER half is a memo of a
 * return value, deliberately, because CSS 2 §8.1 "Box dimensions"' border-box origin is a RECURSION over
 * ancestors rather
 * than a walk over siblings — and for a recursion, remembering the return value IS the collapse. The
 * §10.1 section below states the difference; a reader who takes this paragraph as a rule against memos will
 * refuse the one instrument the other question needs.
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
#include "core/layout/flow_position.h"

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

/* THE SAME LOOKUP WITH NO CENSUS ON IT — the entry an ASSERTION takes. §Offensive-programming requires an
   assertion's condition to be side-effect-free, and an ask counted from inside a `DCHECK` would make
   `asks == served + walks` false in a dev build and true in a release one, so the census would be a fact
   about which build took the measurement rather than about the render. */
bool flow_placement_peek(const lxb_dom_element_t *el, CssPx *out);

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

/* ---- CSS 2 §8.1 "Box dimensions"' BORDER-BOX ORIGIN, PLACED THROUGH §10.1's CASES --------------------
 * THE SECOND FACT A PASS HOLDS ABOUT A BOX, AND IT IS A MEMO OF A RETURN VALUE — which the argument above
 * says would not have helped for the FIRST one, so the difference has to be stated or this record reads as
 * the thing that paragraph forbids.
 * THE TWO COSTS HAVE DIFFERENT SHAPES AND THAT IS THE WHOLE OF IT. `block_flow_child_top`'s cost is a WALK
 * OVER A SIBLING LIST, so remembering its answer per element leaves one walk per element and N walks of O(N)
 * is the same product — the only thing that removes it is the walk REPORTING what it already computed.
 * `flow_border_box_origin`'s cost is a RECURSION OVER ANCESTORS: CSS 2 §10.1 "Definition of 'containing
 * block'"' second case derives a box's
 * point from its containing block's, so an element whose ancestors are already answered costs ONE equation.
 * Remembering the return value is therefore exactly the collapse — the recursion terminates at the first
 * answered ancestor, and a whole pass over a tree of N boxes derives N points instead of one chain per ask.
 * A memo is the wrong instrument for a walk and the right one for a recursion, and which of the two a
 * function is costs one reading of it.
 *
 * ITS CHECK IS WHAT THE TREE VERSION CANNOT BE, WHICH IS WHY IT IS WORTH ITS COST. The span's own number
 * moves for an insert, a removal and the COW swap and does NOT move for an attribute or a stylesheet, so the
 * style axis is held by a crash at solver/dom_cow.c's write chokepoint and by nothing on the read side.
 * core/layout/flow_position.c re-derives §10.1's second-case equation AT EVERY ANSWER this record serves,
 * reading `used_value_leading_edge_px` and `fp_left_offset` out of the CASCADE — so a computed value that
 * moved inside the span makes the recomputed point disagree with the recorded one and the assert fires,
 * whatever route the change arrived by. That is a span check from the inside, at every hit, over the one axis
 * the version number is blind to; the memo-integrity it also buys is the smaller half.
 * IT DOES NOT RE-ENTER, which is what keeps it O(1): the recomputation reads the CONTAINING BLOCK'S point out
 * of this record rather than calling the entry again, so a check cannot trigger a check. An assert that
 * recomputed through the entry would be exponential in depth, and that is the shape to look for if this is
 * ever widened.
 *
 * `derived_from` IS THE CONTAINING BLOCK when §10.1's second case produced the point and NULL on every other
 * arm — the root, the out-of-flow arms and CSS 2.1 §17.5 "Visual layout of table contents"' arms each reach
 * their point some other way, so there is no second route for a check to take and the record says so rather
 * than the caller guessing. It is stored because the write path HAS it: re-deriving which arm a recorded
 * point came from would mean re-asking the cascade the arm predicates it already asked. */
bool flow_placement_origin_ask(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out);
bool flow_placement_origin_peek(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out);
void flow_placement_origin_record(const lxb_dom_element_t *el, FlowPoint origin,
                                  const lxb_dom_element_t *derived_from);

/* ---- WHAT A BOX CONTRIBUTES TO ITS PARENT'S STACK — CSS 2 §8.1 "Box dimensions"' CONTENT HEIGHT AS
 * §10.6.3 COMPUTES IT, WITH §8.3.1's TWO ADJOINING RUNS ---------------------------------------------------
 * THE THIRD FACT, AND IT IS THE SAME DEFECT ONE LEVEL DOWN FROM THE FIRST. §9.4.1's walk asks each child what
 * it contributes, and answering that for a box whose `height` behaves as auto means walking ITS children, so
 * one walk of a container computes the contribution of every box in its subtree and reports one. A container
 * asked about N times re-descends N times.
 * IT IS THE RECURSION SHAPE AND NOT THE WALK SHAPE, which is why a memo of the return value is the collapse
 * here as it is for the border-box origin: the cost is the descent INTO DESCENDANTS, and a remembered answer
 * terminates it at the first recorded one. MEASURED at the revision this record was built against, on a
 * document N boxes deep: the two entries that ask were already LINEAR — `block_flow_child_top` at 2N+2 and
 * `block_flow_auto_height` at 4N+8 — while the work beneath them was QUADRATIC, `bf_box` at 2(N+1)^2. A
 * linear number of asks over quadratic work is the signature that separates a MULTIPLIER from an inherent
 * cost, and it is the reason this is a record rather than a thing to accept.
 *
 * SIX FACTS AND NOT THREE, WHICH IS A CORRECTION TO THE FIRST ANALYSIS OF THIS RECORD AND IS WRITTEN DOWN
 * BECAUSE THE MISSING PAIR IS THE LOAD-BEARING ONE. It was first stated as `border_h`, `content_h` and
 * `collapse_through`. The walk ALSO reads a child's two §8.3.1 ADJOINING RUNS on every iteration, and a
 * record that replayed those as zero would silently drop every margin collapse in the document — a wrong
 * stack with every number in it real. They are here as four lengths rather than as that component's own run
 * type, because a run IS its two extrema and exporting the type to share this table would widen a
 * deliberately private one for a storage reason.
 *
 * WHAT IS DELIBERATELY NOT HELD: CSS 2.1 §10.8.1's BASELINE and whether the box holds a line box. Those are
 * the only two of the eight that depend on WHICH baseline pass asked, so they are the only two this record
 * cannot answer — and it therefore serves no pass but the one that asks for neither. NOT COVERED: a
 * first-baseline or last-baseline walk re-descends exactly as it did. WHAT THE NEXT DIFF BUILDS: the same
 * record keyed on (box, pass) for those two fields, which is a bigger key and not a bigger idea. HOW ITS
 * ABSENCE WOULD SHOW: a document whose cost falls with this record and does not fall further when the boxes
 * on its stack are ones a caller asks a baseline of.
 *
 * `border_from_content` IS HOW `border_h` WAS DERIVED and is the one thing its check needs — the same role
 * `derived_from` plays above, for the same reason: the write path knows which arm it took and re-deriving
 * that at the check would mean re-asking the cascade a question it already asked. */
typedef struct {
    CssPx content_h;        /* §10.6.3's distance from the box's own top content edge */
    CssPx border_h;         /* CSS 2 §8.1's border-box height — meaningless when the box collapses through */
    /* §8.3.1's two ADJOINING RUNS, each as its two extrema: the largest positive margin in the run and the
       largest ABSOLUTE value among its negative ones, which is the whole of what that section's reduction
       carries and is what makes a run merge associatively. */
    CssPx top_pos, top_neg;
    CssPx bottom_pos, bottom_neg;
    bool  collapse_through;
    bool  is_table_wrapper;
    bool  border_from_content;
} FlowPlacementBox;

bool flow_placement_box_ask(const lxb_dom_element_t *el, FlowPlacementBox *out);
bool flow_placement_box_peek(const lxb_dom_element_t *el, FlowPlacementBox *out);
void flow_placement_box_record(const lxb_dom_element_t *el, const FlowPlacementBox *box);

/* ---- CSS 2.1 §10.1 "Definition of 'containing block'"' WIDTH ------------------------------------------
 * THE FOURTH FACT A PASS HOLDS ABOUT A BOX, AND IT IS THE RECURSION SHAPE AGAIN — the same argument the
 * border-box origin's section above makes, about the same section of the same standard, one quantity over.
 * §10.1's fourth case makes a box's containing block "the content edge of the nearest block container
 * ancestor box", so `used_value_containing_block_width` answers out of that ancestor's own used width —
 * which CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow"' constraint equation derives
 * from ITS containing block's, and so on up to §10.1's first case. An ask at depth d costs d derivations,
 * N boxes asked a constant number of times each cost the sum of their depths, and the total is quadratic in
 * DEPTH. Remembering the return value is therefore the collapse, exactly as it is for the origin: the climb
 * terminates at the first answered ancestor and a whole pass derives one width per box.
 * IT WAS MEASURED BESIDE ITS OWN CONTROL RATHER THAN ARGUED, AND THE CONTROL IS THE SECTION ABOVE IT.
 * engine/layout_cost.mjs counts both chains in ONE run of ONE binary over ONE document: CSS 2 §8.1's
 * border-box origin, which goes through this record, is LINEAR in a document's depth, and §10.1's width,
 * which did not, is QUADRATIC. Two §10.1 recursions in one walk inside one span, one routed here and one
 * not — which is a comparison no artifact of the hour, the machine or the revision can produce.
 * ITS CHECK IS THE ARM DISPATCH AND NOT THE NUMBER, WHICH IS NARROWER THAN THE ORIGIN'S CHECK AND IS
 * NARROWER FOR A REASON RATHER THAN FOR CONVENIENCE. Re-deriving the NUMBER means the ancestor's content
 * size, which is the ancestor's used `width`, which is this entry again — the shape the origin's section
 * names as exponential in depth and tells its reader to look for. What re-reads with no re-entry at all is
 * WHICH ancestor §10.1 chose, because that dispatch is computed `display` and `position` and the tree and
 * nothing else. So a hit asserts that the cascade still names the ancestor this answer was derived from,
 * which is the one axis the pass's tree version is blind to — and it is stated here that this does NOT
 * assert the number, because a check whose scope is not written down is read as checking everything.
 * AND §10.1's FIRST CASE IS CHECKED BY ITS NUMBER, because that arm has no ancestor at all: the initial
 * containing block is the viewport, which is one read that re-enters nothing, so the arm that records a
 * NULL ancestor is the one arm whose VALUE is compared.
 * `cb_out` IS THE ANCESTOR §10.1's second, third and fourth cases chose and NULL for its FIRST — the same
 * role `derived_from` plays for the origin, and stored for the same reason: the write path has it, and
 * re-deriving which arm a recorded width came from at the check would mean re-asking the cascade a question
 * the arm dispatch already asked.
 * NOT COVERED: §10.1's HEIGHT. The same recursion one axis over is not held here, so a document whose boxes
 * resolve percentage heights against definite ancestors pays the climb it always did. WHAT THE NEXT DIFF
 * BUILDS: the same pair over the same key for that quantity, whose extra field is DEFINITENESS rather than
 * a second idea. HOW ITS ABSENCE WOULD SHOW: engine/layout_cost.mjs's `deep` column for the height chain
 * staying superlinear after the width one has fallen. */
bool flow_placement_cb_width_ask(const lxb_dom_element_t *el, CssPx *out, const lxb_dom_element_t **cb_out);
bool flow_placement_cb_width_peek(const lxb_dom_element_t *el, CssPx *out, const lxb_dom_element_t **cb_out);
void flow_placement_cb_width_record(const lxb_dom_element_t *el, CssPx width, const lxb_dom_element_t *cb);

/* THE CENSUS, in solver/result.c's vocabulary. Every field is a LIFETIME counter of this agent — it states
   what has happened, never what stands now — so every one may be differenced across two samples.
   `asks` is the number of times §9.4.1's stack position of a box was asked for; `served` is how many of those
   this record answered; `walks` is how many ran the walk. `asks == served + walks` is asserted here rather
   than left to a reader's arithmetic, because it is the one property of the pair that says the two outcomes
   were counted at the same event. `placements` is how many positions the walks reported into the record and
   `passes` is how many whole-tree spans have opened; `placements / walks` is the amortization this component
   buys and `walks` alone is the number that must stop being quadratic in a container's child count.
   `origin_asks`, `origin_served` and `origin_derived` are the border-box origin's pair and close the same
   way. The
   shortfall is named `derived` rather than `walks` because a missed origin ask runs ONE equation over an
   already-answered ancestor, never a walk: `origin_derived` is the count of points this AGENT derived, which
   over a rendered tree of N boxes is O(N) and which USED TO BE the sum of every ask's ancestor chain — the
   row that must stop growing with the square of a document's DEPTH.
   IT IS THIS AGENT'S AND NOT THIS PASS'S, AND THE DIFFERENCE IS NOT PEDANTRY. A derivation made with no pass
   open — CSSOM VIEW §6's members answer one for every `getBoundingClientRect` a page makes between two
   renders — stores nothing and is still a derivation, and counting only the stored ones put the ask in the
   numerator and in neither denominator. That is what broke this identity the first time a fixture read
   geometry through the members rather than through a paint; core/layout/flow_placement.c holds the reason
   at the line whose ORDER decides it. So a run with no render at all has `origin_asks == origin_derived`
   and `origin_served` of zero, which is the correct reading and not an instrument that failed to see a
   pass.
   `cb_width_asks`, `cb_width_served` and `cb_width_derived` are CSS 2.1 §10.1's WIDTH and close the
   same way, with the shortfall named `derived` for the origin's reason — a missed ask runs ONE
   equation over an already-answered ancestor and never a walk. It is this AGENT's and not this
   pass's, for the same reason the origin's is: a width derived with no pass open stores nothing and
   is still a derivation, and counting only the stored ones would put the ask in the numerator and in
   neither denominator. `cb_width_derived` is the row that must stop growing with the square of a
   document's DEPTH. */
typedef struct {
    long long asks;
    long long served;
    long long walks;
    long long placements;
    long long passes;
    long long origin_asks;
    long long origin_served;
    long long origin_derived;
    long long box_asks;
    long long box_served;
    long long box_derived;
    long long cb_width_asks;
    long long cb_width_served;
    long long cb_width_derived;
} FlowPlacementCensus;
void flow_placement_census(FlowPlacementCensus *out);

#endif /* ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_PLACEMENT_H */
