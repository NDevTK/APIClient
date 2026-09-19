/* CSS 2.1 §9.9 "Layered presentation" — WHICH BOX PAINTS IN FRONT OF WHICH, and the first thing in this engine
 * that answers it.
 *
 * WHY CSS 2.1 §9.9 AND NOT APPENDIX E, WHICH IS THE ALGORITHM EVERYONE REACHES FOR. CSS 2.1 §9.9.1 is what
 * points at Appendix E rather than the other way round — "The order in which the rendering tree is painted
 * onto the canvas is described in terms of stacking contexts." — and Appendix E's own opening paragraph says
 * in as many words that it defines the CSS 2.1 painting order in more detail than the rest of the
 * specification does. THAT SENTENCE IS PARAPHRASED AND NOT QUOTED FOR A REASON WORTH THE LINE: it sits in
 * Appendix E's UN-NUMBERED preamble, whose only coordinate is a bare `§E`, and engine/citegen.mjs's
 * SECTION-SIGN reader does not lex one — measured with a paired control, `§E` reads 0 citations while `§E.1`,
 * `§9.9` and `§B.2` each read 1 — so a quotation anchored there is judged against whichever section is
 * lexable ABOVE it. The coordinate is right and the tool cannot see it; quoting under a
 * number that IS lexable would have been the actual defect.
 * IT IS THE SECTION-SIGN READER AND NOT THE BARE-NUMBER ONE, WHICH THIS COMMENT NAMED UNTIL THE TRADE WAS
 * PRICED: the bare-number reader lookbehind excludes any run a section sign introduces, so it never sees this
 * shape at all, and loosening the SECTION-SIGN reader letter branch — its numeric group from one-or-more to
 * zero-or-more — is what flips the control from zero to one. The clause is worth naming correctly because a
 * repair is written against a clause, and these two carry different letter RANGES as well as different
 * quantifiers, so a letter outside the section-sign reader range needs a second, separately-argued change.
 * AND THE RED IS SELF-REPAIRING, WHICH `reported as wrong` OVERSTATED: the corpus DOES index the preamble
 * under the lone letter, so the finding row NAMES it — the accusation carries its own repair rather than
 * sending a reader to a document nobody cited.
 * THE LOOSENING WAS MEASURED AND DECLINED, so the next reader re-derives the TRADE and not the proposal. It
 * admits seven-hundred-odd further runs over the audited set and buys ONE more resolved citation, because
 * nearly every lone capital after a section sign here opens the name of a HEADING THIS TREE CROSS-REFERENCES
 * — the design notes cite their own sections that way, in the hundreds — and each then becomes the nearest
 * preceding citation for the prose beneath it. The headline IMPROVES while the instrument goes blind: the
 * finding total falls, the quotations COMPARED fall further, and the unjudgeable band grows by far more than
 * the resolved count gains. DERIVATION rather than the figures, which move as sites are written: run the
 * auditor over its default set with that numeric group at one-or-more and again at zero-or-more, and read
 * COMPARED and UNJUDGEABLE beside the finding total — never the total alone, which falls in BOTH the repair
 * and the blinding direction. WHAT WOULD RETIRE THIS NOTE: a reader admitting a lone capital only where the
 * file names a standard whose index holds that letter as a section, which separates the appendix coordinate
 * from the heading cross-reference and makes the trade positive.
 * THE THING APPENDIX E IS A MORE DETAILED DESCRIPTION OF is §9.9.1's seven layers, and the two are not alternatives — §9.9.1 states the ORDER over
 * boxes, and §E.2 states, inside each of those layers, WHICH INK goes down in which sequence (a background
 * colour, then a background image, then a border; a table's six background levels; a line box's underline,
 * text and
 * line-through). So §9.9.1 is a total order over BOXES and §E.2 is a total order over MARKS, and every operand
 * of the first is a computed value while the second additionally needs backgrounds, borders, line boxes, table
 * grids, replaced content and outlines. THE ORDER IS THE PART THAT CAN BE WRONG WITHOUT A DEVICE, and it is the
 * part this component owns.
 *
 * CSS 2.1 §9.9.1's LAYERS, VERBATIM AND IN ITS OWN ORDER — "Within each stacking context, the following layers are
 * painted in back-to-front order":
 *   1. the background and borders of the element forming the stacking context.
 *   2. the child stacking contexts with negative stack levels (most negative first).
 *   3. "the in-flow, non-inline-level, non-positioned descendants."
 *   4. the non-positioned floats.
 *   5. "the in-flow, inline-level, non-positioned descendants, including inline tables and inline blocks."
 *   6. "the child stacking contexts with stack level 0 and the positioned descendants with stack level 0."
 *   7. the child stacking contexts with positive stack levels (least positive first).
 * Every operand of that partition is a computed `display`, `position`, `float` and `z-index` plus tree order,
 * and NOTHING ELSE — no geometry, no line box, no font, no device. That is why this is the first paint diff
 * that can be complete rather than partial: there is no box type it answers for and no box type it does not.
 *
 * THE STACKING CONTEXT IS NOT THE CONTAINING BLOCK AND CSS 2.1 §9.9.1 SAYS SO, which is the one structural mistake a
 * reader coming from core/layout/used_value.h will make. "Stacking contexts are not necessarily related to
 * containing blocks." So the walk that finds the context a box belongs to is an ELEMENT-TREE walk — the FLAT
 * tree, core/css/css_computed_value.h's `css_parent_element`, for css-transforms-1's reason as much as for
 * CSS Cascade §7.2's: a shadow host's stacking context contains the boxes its shadow tree generates. An absolutely
 * positioned box whose containing block is four ancestors up still belongs to the nearest ANCESTOR that forms
 * a stacking context, and a walk that followed containing blocks would put it in a different one.
 *
 * ATOMICITY IS WHAT MAKES A COMPARATOR POSSIBLE AT ALL. "A stacking context is atomic from the point of view of
 * its parent stacking context; boxes in other stacking contexts may not come between any of its boxes." So two
 * boxes in two different stacking contexts are ordered by whatever orders their CONTEXTS, recursively, and the
 * comparator below is that recursion: climb both boxes' stacking-context chains to the nearest common context,
 * then order the two members that sit directly in it. Without atomicity there would be no such reduction and
 * the only answer would be a full painted sequence.
 *
 * css-position-3 §2.2 "Painting Order and Stacking Contexts" IS PART OF THE RULE AND CSS 2.1 ALONE GETS `fixed`
 * WRONG. CSS 2.1 §9.9.1's stacking-context test is "any positioned element … having a computed value of 'z-index' other
 * than 'auto'", and css-position-3 amends exactly that: "Fixed and sticky positioned boxes nonetheless form a
 * stacking context." A `position: fixed` header with no `z-index` — which is most fixed headers — therefore
 * forms one, and an engine built to CSS 2.1's sentence alone puts every one of its positioned descendants into
 * the wrong context. `sticky` is not in CSS 2.1 at any section, so that half is not an amendment but the only
 * statement there is. The same section is also what keeps `relative` and `absolute` OUT: with `z-index: auto`
 * they do not form a context, which is why §E.2 step 8 has an arm for them, and it is why the chain walk below
 * SKIPS such an ancestor rather than stopping at it.
 *
 * WHAT IS NOT HERE, STATED AS A SCOPE AND NOT AS A HEDGE. CSS 2.1 §9.9.1's own sentence names the first
 * item: "In future levels of CSS, other properties may introduce stacking contexts, for example
 * 'opacity'". Those properties — `opacity`, `transform`, `filter`, `will-change`, `contain`, `isolation`,
 * `mix-blend-mode` — are properties
 * core/css/css_computed_value.h does not derive, and it CRASHES by name when asked for one rather than
 * answering a specified value under the word `computed`. So this component cannot silently get them wrong: a
 * page that declares one reaches that crash, which is the forcing function working at the property rather than
 * at the painter. See the residual at the foot of this header for what that costs and how it shows.
 *
 * NOTHING HERE IS STORED, FOR core/layout/used_value.h's REASON AND NOT A SEPARATE ONE. A stacking-context tree
 * is per-flow state — two flows with different DOMs and different cascades have different contexts — so a
 * cached one is shared state the COW delta does not swap. Every answer below is DERIVED PER READ from the
 * running flow's own tree and its own cascade, which makes it per-flow by construction with no capture to
 * write and no entry to unapply, and it is why every entry takes an element and returns by value. The day a
 * paint-layer tree exists for a reason a derivation cannot serve, it is per-flow state and needs
 * solver/cow.h's `cow_capture_host_record` at its accessor over the record's owned-value layout — never
 * solver/dom_cow.h, whose delta is the DOM tree and the attribute list and which has no host-record primitive
 * at all. HOW A MISSING CAPTURE WOULD SHOW: an order that does not move when the running flow's own tree or
 * cascade does — two flows that styled one document differently reading one paint order.
 *
 * A CASCADE VALUE IS THE PAGE'S AND IS NEVER ASSERTED ON. `z-index`, `position`, `float` and `display` are
 * whatever the document's own author wrote. The parse below therefore has no assert over a z-index's VALUE:
 * css-values-4 §5.1 "Range Restrictions and Range Definition Notation" already answers the only case a C
 * integer has trouble with — "CSS theoretically supports infinite precision and infinite ranges for all value
 * types; however in reality implementations have finite capacity" — so a magnitude beyond this UA's capacity
 * saturates at it, which preserves every comparison the order is made of and is the spec's own arm rather than
 * a clamp past a broken invariant. What IS asserted is this engine's own logic: that the value handed back is
 * one §9.9.1's grammar admits, because lexbor rejects a nonconforming declaration at parse time and CSS
 * Cascade §7 then supplies the initial `auto` — so a third spelling arriving here is this pipeline's defect
 * and not the page's. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_STACKING_ORDER_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_STACKING_ORDER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* §9.9.1's SEVEN LAYERS, numbered as §9.9.1 numbers them, so the enum's ORDER is the paint order and a
   comparison of two layers is a subtraction. Layer 1 is the context's OWN background and border and is
   therefore never a layer any DESCENDANT is in — `stacking_layer_of` never returns it, and it is in the enum
   because the enum is §9.9.1's list and a list missing a member is not that list. `stacking_order_compare` is
   where it does its work: an element that IS the stacking context another element lives in paints its layer 1
   before anything inside it. */
typedef enum {
    STACKING_LAYER_SELF = 1,        /* the background and borders of the element forming the stacking context */
    STACKING_LAYER_NEGATIVE = 2,    /* child stacking contexts with negative stack levels (most negative first) */
    STACKING_LAYER_BLOCK = 3,       /* the in-flow, non-inline-level, non-positioned descendants */
    STACKING_LAYER_FLOAT = 4,       /* the non-positioned floats */
    STACKING_LAYER_INLINE = 5,      /* the in-flow, inline-level, non-positioned descendants */
    STACKING_LAYER_LEVEL_ZERO = 6,  /* child stacking contexts at level 0 AND positioned descendants at level 0 */
    STACKING_LAYER_POSITIVE = 7     /* child stacking contexts with positive stack levels (least positive first) */
} StackingLayer;

/* DOES `el` GENERATE A STACKING CONTEXT — CSS 2.1 §9.9.1 as amended by css-position-3 §2.2. CSS 2.1 §9.9.1: "The
   root element forms the root stacking context. Other stacking contexts are generated by any positioned element
   (including relatively positioned elements) having a computed value of 'z-index' other than 'auto'."
   css-position-3 §2.2 adds the `auto` arm this engine would otherwise get wrong: "Fixed and sticky positioned
   boxes nonetheless form a stacking context."
   IT IS A QUESTION ABOUT A BOX, so an element whose computed `display` generates none is not a caller's to ask
   and crashes rather than answering false — an absent box and a box that forms no context are different facts
   and a single `false` for both is the shape that reads as an answer. */
bool stacking_context_forms(lxb_dom_element_t *el);

/* CSS 2.1 §9.9.1's STACK LEVEL of `el`'s box, within the stacking context it belongs to. For `z-index: auto`, §9.9.1
   itself: "The stack level of the generated box in the current stacking context is 0." For an `<integer>`:
   "This integer is the stack level of the generated box in the current stacking context."
   ONLY A BOX HAS ONE AT ALL, and that is the PRIOR question rather than a restatement of the next one:
   §9.9.1 says "Each positioned box in a given stacking context has an integer stack level", so an element
   css-display-3 §2.5 "Box Generation: the none and contents keywords" generates no box for crashes BEFORE
   the positioned test below it — CSS 2.1 §9.7 "Relationships between 'display', 'position', and 'float'"'s
   own order, `display` first — because `display: none; position: absolute` satisfies that test.
   ONLY A POSITIONED BOX HAS ONE. `z-index`'s own "Applies to:" line is "positioned elements", so a stack level
   is not a property every box has a value of, and asking for one of a static box is asking a question §9.9.1
   does not answer — it crashes. §9.9.1's layers 2, 6 and 7 are the only ones a stack level orders, and every
   member of those three is positioned. */
int stacking_level(lxb_dom_element_t *el);

/* THE STACKING CONTEXT `el`'s BOX BELONGS TO — the nearest FLAT-TREE ancestor for which
   `stacking_context_forms` is true, or NULL when `el` is itself the root element and there is none above it.
   `el` ITSELF MUST GENERATE A BOX and crashes otherwise, which is a DIFFERENT question from the boxless
   ANCESTOR the walk steps over two sentences down: §9.9.1 states the membership over a box — "Each box
   belongs to one stacking context" — so an `el` with none contributes no member for a context to hold,
   while such an ANCESTOR is a real flat-tree parent of boxes that do exist.
   THE WALK SKIPS A POSITIONED ANCESTOR THAT FORMS NO CONTEXT, which is not an optimisation but the rule:
   css-position-3 §2.2 says a relative or absolute box at `z-index: auto` is painted "as if those elements did
   generated new stacking contexts, except that their positioned descendants and any would-be child stacking
   contexts take part in the current stacking context" — so such an ancestor is exactly the one a descendant
   must look PAST. IT ALSO STEPS OVER AN ANCESTOR WITH NO BOX: css-display-3 §2.5's `display: contents` leaves a
   real flat-tree parent whose own box does not exist, and §9.9.1's every sentence is about a box, so there is
   nothing there for the test to answer. It terminates because the root element always forms one. */
lxb_dom_element_t *stacking_context_of(lxb_dom_element_t *el);

/* WHICH OF §9.9.1's LAYERS `el`'s box is painted in, within `stacking_context_of(el)`. Asked of the root
   element it crashes: the root has no parent stacking context, so there is no list for it to be a member of.
   THE THREE TESTS RUN IN CSS 2.1 §9.7 "Relationships between 'display', 'position', and 'float'"'s OWN ORDER —
   display, then position, then float — because that order is the rule and not a style: `display: none;
   float: left` generates no box at all, and a walk that asked `float` first would report a float for it. */
StackingLayer stacking_layer_of(lxb_dom_element_t *el);

/* THE ANSWER THIS COMPONENT EXISTS FOR, and the consumer of everything above it: does `a`'s box paint BEFORE
   `b`'s? Negative when `a` is painted first (further from the user), positive when `b` is, zero only when they
   are the same element. It is a strict weak ordering over the boxes of one document, which is what makes it
   usable as a comparator by whatever sorts, hit-tests or occludes.
   IT TAKES A `ctx` FOR ONE REASON AND IT IS THE TREE-ORDER TIE-BREAK. CSS 2.1 §9.9.1: "Boxes with the same stack level
   in a stacking context are stacked back-to-front according to document tree order", and CSS 2.1 §E.1 says
   which tree that is — "Preorder depth-first traversal of the rendering tree, in logical (not visual) order for
   bidirectional content, after taking into account properties that move boxes around". The RENDERING tree
   flattens shadow trees, so the order is DOM §4.8 "Interface ShadowRoot"'s shadow-including tree order — "In
   shadow-including tree order is shadow-including preorder, depth-first traversal of a node tree" — and
   core/dom/shadow_root.h's one-step walker for it needs a realm because the element -> shadow root association
   is a per-flow fact kept on the element's wrapper. THE NUMBER IS §4.8 AND NOT §4.2, which
   core/dom/shadow_root.h states at three of its own sites and which this file copied before an audit caught
   it: DOM §4.2 is "Node tree" and defines neither the order nor the traversal, and both definitions sit in
   §4.8 beside the interface. Recorded rather than repaired there, because a wrong number copied from a
   neighbouring component is exactly how it arrived here. Every other entry here reads only upward, where the shadow
   root already names its host, which is why only this one takes a realm.
   BOTH ELEMENTS MUST BE IN ONE DOCUMENT and must each generate a box; a caller mixing two documents is asking
   a question CSS 2.1 does not define, and it crashes rather than inventing an order between two canvases. */
int stacking_order_compare(JSContext *ctx, lxb_dom_element_t *a, lxb_dom_element_t *b);

/* NAMED RESIDUAL — WHAT IS NOT COVERED: a box whose stacking context is created by a property CSS 2.1 does not
   state one over. CSS 2.1 §9.9.1 names the first of them itself ("In future levels of CSS, other properties may
   introduce stacking contexts, for example 'opacity'"), and the live list is css-color-4's `opacity`,
   css-transforms-1's `transform` and `perspective`, css-filter-effects-1's `filter`, css-will-change-1's
   `will-change`, css-contain-2's `contain`, css-compositing-1's `isolation` and `mix-blend-mode`, and
   css-position-4 §3's top layer. This component answers as though none of them creates one.
   WHAT THE NEXT DIFF BUILDS: `opacity`'s computed value in core/css/css_computed_value.c. css-color-4 §3.3
   "Transparency: the opacity property" gives it a computed value of "specified number, clamped to the range
   [0,1]", so it is a `<number>` — the one shape that file's TEXT entry carries with NOTHING LOST, which is
   why its `flex-grow` row already exists and is the same row this one would be — and then one arm in
   `stacking_context_forms` over it. The others each need their own property first, and `transform` needs
   css-transforms-1 §7 "The Transform Functions"' whole `<transform-list>` grammar, which
   core/css/css_computed_value.c crashes for by name today.
   HOW ITS ABSENCE WOULD SHOW: a comparison that puts a positioned descendant of an `opacity: .5` box in the
   wrong context — the descendant ordered against the outer context's members instead of being confined inside
   its parent's — which is observable as two boxes swapping order with no declaration between them naming
   either. It is NOT shown by a crash: every property in that list other than `opacity` reaches
   core/css/css_computed_value.c's own refusal first, so for those the engine stops before it can be wrong,
   and `opacity` is the one that would silently answer. RETIREMENT: this record goes when
   `stacking_context_forms` reads a property outside §9.9.1's own sentence. */

#endif
