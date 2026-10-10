/* What a layout job knows about a box's placement: three facts in one record, each an answer a walk already
 * computed. Where a box sits on CSS 2.1 §9.4.1 "Block formatting contexts"' stack, where its border box is
 * (CSS 2 §8.1's origin, placed through CSS 2.1 §10.1's cases), and CSS 2.1 §10.1's containing-block width. A
 * box's contribution to its parent's stack is not here: it is a layout question (core/layout/layout_job.h).
 * The three are functions of the same inputs, the tree and the cascade, so they share one table keyed by
 * element, one probe and one lifetime. */

/* The stack position is a record the walk writes, not a remembered return value. §9.4.1 lays boxes out "one
 * after the other, vertically, beginning at the top of a containing block", so a walk of one container
 * establishes every child's top; `block_flow_child_top` asks about one child, and a memo of its return value
 * would still run one walk of N children per child asked. The walk reports every position it passes, so the
 * first ask about any child answers its siblings'. The origin and the width are recursions over ancestors,
 * for which remembering the return value is the collapse; their sections below state that. */

/* The record's lifetime is its job's (layout_job_placement): a driven job's for all its activations, and the
 * synchronous job's while a drive is open. Nothing here decides whether an entry is still good: the job's
 * validity argument (no DOM write while a job's activation holds the thread; a driven job rests only inside
 * its own flow's member call) is what makes every entry an answer about the current tree. Each served answer
 * is still checked against its own derivation where that is cheap: `block_flow_child_top` asserts that the
 * walk's answer for the box it named is the recorded one, and core/layout/flow_position.c re-derives a
 * recorded origin's equation at each hit.
 * RETIREMENT: this component goes when layout produces a box tree that paint reads, because a position is then
 * a field of the box rather than a function re-derived per ask. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_PLACEMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_PLACEMENT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/flow_position.h"

/* The record's storage, owned by a layout job. `tab` is a power-of-two table, NULL with `cap` 0 before the
   first entry; `release` frees it and leaves the record empty. */
struct FpEntry;
typedef struct {
    struct FpEntry *tab;
    size_t          cap, used;
} FlowPlacementRecord;
void flow_placement_record_release(FlowPlacementRecord *r);

/* Whether the current job holds a record. Read by assertions and by nothing that decides what to compute. */
bool flow_placement_is_held(void);

/* ONE BOX PLACED BY §9.4.1's WALK, at its TOP BORDER EDGE in the frame core/layout/block_flow.h's
   `block_flow_child_top` answers in — the distance from the containing block's top CONTENT edge. Called for
   every in-flow child a walk passes, with no test of whether anybody will ask: the walk has the number in hand
   and deciding here who might want it is the throw-away this component exists to end.
   A no-op when no record is held, which keeps the walk's own code free of a mode.
   `el` is a box on the walked container's CSS 2.2 §9.2.1.1 "Anonymous block boxes" box list, which is not
   the same thing as a child of it: §9.2.1.1 makes a block-level box inside a broken inline box "a sibling of
   those anonymous boxes". The position is a distance down the stack of the box's own containing block, which
   §10.1's second case makes the container that recorded it; the caller asserts membership at the write. */
void flow_placement_record(const lxb_dom_element_t *el, CssPx top);

/* The ask, counted whether or not a record is held — the recording point is the question and never the
   outcome. Answers TRUE and
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
   about which build took the measurement. It answers FALSE when no record is held, when the record has no entry
   for the box, and when the two EXAMPLES differ — three states its one caller does not need apart, because
   each of them is the same finding: the walk's `want_top` write and the walk's record write have parted.
   THE COMPARISON LIVES HERE AND NOWHERE ELSE, and core/layout/flow_placement.c says why it is not an entry of
   core/css/css_length.h. */
bool flow_placement_agrees(const lxb_dom_element_t *el, CssPx top);

/* …AND THE WALK THAT ASK COST. Called by the one caller of `flow_placement_ask` on its FALSE arm, immediately,
   so that `asks == served + walks` holds at every instant and not merely at the end. */
void flow_placement_walked(void);

/* ---- CSS 2 §8.1 "Box dimensions"' border-box origin, placed through CSS 2.1 §10.1's cases ------------
 * A memo of a return value, because the origin is a recursion over ancestors: §10.1's second case derives a
 * box's point from its containing block's, so an element whose ancestors are answered costs one equation,
 * and a tree of N boxes derives N points rather than one chain per ask.
 * core/layout/flow_position.c re-derives the second-case equation at every answer this record serves, out of
 * the cascade, reading the containing block's point from this record rather than through the entry, so the
 * check is O(1) and cannot trigger a check. `derived_from` is the containing block when the second case
 * produced the point and NULL on every other arm (the root, the out-of-flow arms, CSS 2.1 §17.5 "Visual
 * layout of table contents"'), which have no second route to check. */
bool flow_placement_origin_ask(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out);
bool flow_placement_origin_peek(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out);
void flow_placement_origin_record(const lxb_dom_element_t *el, FlowPoint origin,
                                  const lxb_dom_element_t *derived_from);

/* ---- CSS 2.1 §10.1 "Definition of 'containing block'"'s width ------------------------------------------
 * The recursion shape again: §10.1's fourth case answers out of the nearest block container ancestor's used
 * width, which CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow"' constraint equation
 * derives from its own containing block's, so the climb terminates at the first answered ancestor.
 * A hit asserts the arm dispatch and not the number: re-deriving the number would re-enter this entry, while
 * which ancestor §10.1 chose is computed `display`, `position` and the tree. §10.1's first case has no
 * ancestor, so its number (the initial containing block's, one read) is what is compared. `cb_out` is the
 * ancestor the second, third and fourth cases chose, NULL for the first.
 * NAMED RESIDUAL. Not covered: §10.1's height, so percentage heights against definite ancestors pay the
 * climb. Next: the same pair over the same key for that quantity, with definiteness as its extra field. How
 * its absence shows: engine/layout_cost.mjs's `deep` column for the height chain staying superlinear. */
bool flow_placement_cb_width_ask(const lxb_dom_element_t *el, CssPx *out, const lxb_dom_element_t **cb_out);
bool flow_placement_cb_width_peek(const lxb_dom_element_t *el, CssPx *out, const lxb_dom_element_t **cb_out);
void flow_placement_cb_width_record(const lxb_dom_element_t *el, CssPx width, const lxb_dom_element_t *cb);

/* The census, in solver/result.c's vocabulary. Every field is a lifetime counter of this agent, so each
   may be differenced across two samples. `asks` counts §9.4.1 stack positions asked, `served` those this
   record answered, `walks` those that ran the walk; `asks == served + walks` is asserted. `placements`
   counts positions the walks reported; `placements / walks` is the amortization the record buys.
   `origin_*` and `cb_width_*` are the two recursions' asks, answers and derivations, each closing the same
   way; the shortfall is `derived` because a missed ask runs one equation over an answered ancestor. A
   derivation made with no record held is still counted, so a run that holds none has `*_asks ==
   *_derived` and nothing served. */
typedef struct {
    long long asks;
    long long served;
    long long walks;
    long long placements;
    long long origin_asks;
    long long origin_served;
    long long origin_derived;
    long long cb_width_asks;
    long long cb_width_served;
    long long cb_width_derived;
} FlowPlacementCensus;
void flow_placement_census(FlowPlacementCensus *out);

#endif /* ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_PLACEMENT_H */
