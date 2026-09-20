/* WHAT A WHOLE-TREE RENDER KNOWS ABOUT THE CASCADE — css-cascade-5 §4.2 "Cascaded Values"' answer for one
 * (element, property), held for the span of one CSS 2.1 §E.2 "Painting order" walk and for no longer.
 *
 * THE DEFECT IT CLOSES IS NOT A SLOW FUNCTION, IT IS ONE ANSWER RE-DERIVED ONCE PER ASKER. css-cascade-5 §6
 * "Cascading" — "The cascade takes an unordered list of declared values for a given property on a given
 * element, sorts them by their declaration's precedence as determined below, and outputs a single cascaded
 * value" — is a function of the ELEMENT and the PROPERTY and of nothing about who is asking, and a render
 * asks for the same pair over and over for two reasons that compound:
 *   - css-cascade-5 §7.2 "Inheritance": "The inherited value of a property on an element is the computed
 *     value of the property on the element's parent element." So a read of an inherited property CLIMBS, and
 *     every element on that chain re-runs the whole cascade for every descendant that asks.
 *   - css-logical-1 §4 "Flow-Relative Box Model Properties" makes the pairing a PREREQUISITE of the cascade
 *     itself — "It also requires that writing-mode, direction, and text-orientation be computed as a
 *     prerequisite for cascading together the flow-relative and physical declarations of a logical property
 *     group" — and both of those are inherited, so every margin, padding, border and inset read on any
 *     element pays two climbs to the root before it looks at one declaration.
 *
 * THE COST IS A MULTIPLIER AND NOT AN INHERENT COST, WHICH IS A MEASUREMENT AND NOT AN ARGUMENT, AND THE
 * DERIVATION IS A COMMAND RATHER THAN A FIGURE: `node engine/layout_cost.mjs <native binary>` counts calls
 * with gdb breakpoints over two document SHAPES that hold the same number of elements, and the reading is
 * the pair of orders rather than either number. Before this record, `bp_visit` — CSS 2.1 §E.2's offer count,
 * which is the denominator every other row is a cost per — was LINEAR in both shapes while
 * `cssom_cascaded_value` was QUADRATIC over N siblings at constant depth and CUBIC over N nested boxes. An
 * order that RISES BY ONE when the depth grows names a per-ANCESTOR factor; one that is already superlinear
 * at constant depth names a second, per-SIBLING factor; and a LINEAR number of asks over superlinear work is
 * the signature core/layout/flow_placement.h names for a multiplier.
 *
 * WHICH SHAPE THE COST IS DECIDES THE INSTRUMENT, AND HERE IT IS NEITHER OF core/layout/flow_placement.h's
 * TWO. That component's CSS 2.1 §9.4.1 "Block formatting contexts" half is a WALK over a sibling list,
 * where a memo of the return value leaves N walks of O(N) and only the walk REPORTING removes the
 * multiplier; its CSS 2 §8.1 "Box dimensions" half is a RECURSION over
 * ancestors, where remembering the return value IS the collapse. This one is a THIRD shape and it is the
 * simplest of the three: ONE ANSWER ASKED BY MANY CALLERS. The per-sibling factor is N askers of N distinct
 * keys, and the per-ancestor factor is one key asked once per descendant — both collapse the moment the pair
 * is answered once, and neither needs a walk to report anything. That is why this record is keyed on the
 * QUESTION (element, property) rather than on the asker, and why it sits at the one function every asker
 * reaches rather than at any of them.
 *
 * ITS LIFETIME IS A PASS AND IT HAS NO INVALIDATION RULE AT ALL, which is the point and not an omission —
 * core/css/css_style_declaration.h states the standing objection to caching a cascade answer, that it would
 * be shared state the flow machinery does not swap, and that objection is exactly right about a record with
 * a LIFETIME. This one has none: it is EMPTY before a pass opens and EMPTY after it closes, so there is no
 * span in which one flow's answer can be served to another and nothing here decides whether an entry is
 * still good. A flow SWITCH inside the span is not reasoned about either; it is the case the tree version
 * below exists to crash on.
 *
 * THE SPAN IS ASSERTED AT THREE PLACES RATHER THAN DESCRIBED AT ONE, which is core/layout/flow_placement.h's
 * arrangement for the same reason — the premise is that the cascade's inputs do not move inside the walk,
 * and a premise stated in prose is a premise nobody can fail:
 *   - RE-ENTRY: a pass may not open inside a pass. Two passes would share one record with two lifetimes and
 *     the inner close would empty a record the outer walk is still reading.
 *   - THE TREE: `dom_cow_version()` is read at open and asserted unchanged at close AND at every ask this
 *     record answers. That number moves on an insert, a removal and — the case that matters — on the COW
 *     SWAP, so a flow switch during the walk crashes rather than serving one world's cascade into another.
 *   - EVERY OTHER CASCADE INPUT IS MADE IMPOSSIBLE AT ITS OWN WRITE, because no version number can see it.
 *     `dom_cow_version()` does not move for an attribute, and `class`, `id`, `style` and every
 *     presentational attribute reach the cascade without touching the tree's shape; nor does it move for a
 *     CSSOM write, and css-cascade-5 §6.2's author origin is the STYLE SHEET OBJECTS a page holds — whose
 *     rules it inserts and deletes, whose declarations it sets, and whose `disabled` flag it flips. Each of
 *     those writes asserts that no pass is open, at the write, which is the only place the two can be made
 *     exclusive by construction. If one fires, the write is real and the PASS is in the wrong place; the fix
 *     is where the pass opens and closes and never a re-check on the read side.
 *
 * WHAT THIS RECORD DOES NOT DO, STATED BECAUSE THE PRECEDENT DOES IT AND A READER WILL LOOK FOR IT: it does
 * NOT re-derive its answer at every hit. core/layout/flow_placement.h's border-box origin can afford that —
 * its check re-reads CSS 2 §10.1 "Definition of 'containing block'"'s equation out of the CASCADE and is
 * O(1) because it takes the containing block's
 * point out of the record rather than recursing — and the equivalent here is re-running the cascade, which
 * is the whole of the cost this component exists to remove. So the memo-integrity half is bought by the
 * probe (a slot is occupied by ONE (element, property) and answers only that pair) and the span half is
 * bought entirely by the tree version and by the write-side crashes above. An instrument trusted past its
 * evidence is worse than none: those three are what holds this record, and there is no fourth check here
 * quietly agreeing with them.
 *
 * AND ONE INPUT IS HELD BY THE SPAN ITSELF RATHER THAN BY ANY OF THE THREE, WHICH IS SAID HERE BECAUSE A
 * READER COUNTING CHOKEPOINTS WOULD OTHERWISE FIND IT MISSING. A sheet's media query is evaluated against
 * the ENVIRONMENT — HTML §4.2.4.1 "Processing the media attribute" makes that attribute prescriptive
 * against the environment — and the environment is the viewport this render was handed. It cannot move
 * inside a pass because the pass is inside ONE render, and a second render is a second pass over an
 * empty table; the MediaList a page can rewrite is a different question and is one of the writes above.
 *
 * NAMED RESIDUAL — THE RECORD HOLDS ONE OF THE FOUR SHAPES A COMPUTED VALUE COMES IN, AND THE CLIMB ITSELF
 * IS UNTOUCHED. WHAT IS NOT COVERED: this is the CASCADED value, so css-cascade-5 §7's defaulting still runs
 * per ask and §7.2's inheritance still walks to the root once per descendant — each of those frames is now a
 * table probe and a string copy instead of a sheet walk, a UA-table scan and a sort, but the FRAME COUNT is
 * unchanged. core/css/css_computed_value.c's other three entries — the absolute length, css-inline-3 §5.1's
 * `line-height` union and CSS 2.1 §17.6.1's `border-spacing` pair — each climb the same way and each reads
 * an environment fact this record deliberately does not hold, so none of them may be answered out of it.
 * WHAT THE NEXT DIFF BUILDS: the COMPUTED value beside the cascaded one, as a second fact under this same
 * span and this same key, which terminates css-cascade-5 §7.2 "Inheritance"'s climb at the first answered
 * ancestor exactly as core/layout/flow_placement.h's CSS 2 §8.1 "Box dimensions" memo terminates its own;
 * the length-shaped entry needs its own field
 * because a `CssPx` carries a domain a string cannot. HOW ITS ABSENCE WOULD SHOW, as an observation and not
 * as an instance: with this record in place, the command above reports a row that is LINEAR in the shape
 * whose depth is constant and superlinear in the shape whose depth grows — the residue being a per-ancestor
 * frame count that no longer carries a cascade with it.
 *
 * RETIREMENT: this component goes when the cascade PRODUCES a computed-style object that a render reads,
 * because a cascaded value is then a field of that object rather than a function re-derived per ask, and
 * there is nothing left to record. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* OPEN AND CLOSE THE PASS. `open` empties the record and takes the tree version it will be held to; `close`
   empties it again and releases every string it owns, so no allocation and no entry outlives the span. Both
   belong in ONE function of ONE component — the entry that performs the whole-tree walk — because a pass
   whose two ends are in two callers is a pass a returning arm can leave open. */
void css_cascade_pass_open(void);
void css_cascade_pass_close(void);

/* IS A PASS OPEN — read by the write chokepoints of every cascade input, which assert it is NOT, and by
   nothing that branches on it to decide what to compute. It is a fact about the instrument, never an
   operand. */
bool css_cascade_pass_is_open(void);

/* THE ASK, COUNTED WHETHER OR NOT A PASS IS OPEN — the recording point is the QUESTION and never the
   outcome, so a run that opens no pass at all still reports how many cascaded values it was asked for.
   Answers TRUE and writes `*out` when this record holds the pair; the value is an OWNED copy the caller
   frees exactly as it frees the cascade's own answer, and NULL is one of the values it can be — css-cascade-5
   §4.2 "Cascaded Values"' "if the output of the cascade is an empty list, there is no cascaded value" is a
   real answer and not a miss. FALSE means the caller owes the cascade and must report the answer with
   `css_cascade_pass_record`, so the census closes. */
bool css_cascade_pass_ask(lxb_dom_element_t *el, const char *name, char **out);

/* …AND THE CASCADE THAT ASK COST. Called by the one caller of `css_cascade_pass_ask` on its FALSE arm,
   immediately, so `asks == served + resolved` holds at every instant and not merely at the end. `value` is
   BORROWED — the record takes its own copy — and NULL is stored as the answer it is. A no-op for the storage
   when no pass is open, which is what keeps the cascade's own code free of a mode; the count is taken
   either way. */
void css_cascade_pass_record(lxb_dom_element_t *el, const char *name, const char *value);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H */
