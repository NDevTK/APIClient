/* CSS 2 §9.4.1 "Block formatting contexts" and CSS 2.1 §8.3.1 "Collapsing margins" — THE BOX TREE, which is
 * the one walk two members of this directory were each waiting on separately.
 *
 * ONE SUBPROBLEM, TWO ANSWERS, WHICH IS WHY THIS IS ONE COMPONENT AND NOT TWO. core/layout/flow_position.c's
 * non-root crash and core/layout/used_value.c's `height: auto` crash both named the SAME missing thing in the
 * same words — "the distance from its top content edge to the bottom margin edge of the last in-flow child"
 * (§10.6.3) and "boxes are laid out one after the other, vertically, beginning at the top of a containing
 * block" (§9.4.1) — because a box's y IS the running offset the height walk already computes and the height IS
 * where that offset ends up. Building them apart would be two walks over one child list that can disagree
 * about where a margin collapsed, so there is one walk and it answers both.
 *
 * §8.3.1 IS THE ALGORITHM, NOT A CORRECTION TO IT. "The vertical distance between two sibling boxes is
 * determined by the margin properties" is §9.4.1's whole placement rule, and the very next sentence is
 * "vertical margins between adjacent block-level boxes in a block formatting context collapse" — so a walk
 * that adds margins and then subtracts a collapse is a walk that had the rule wrong. What travels through the
 * walk is therefore not a number but §8.3.1's ADJOINING RUN: the set of margins that have met without a line
 * box, clearance, padding or border between them, whose collapsed width is "the maximum of the collapsing
 * margins' widths" with, "in the case of negative margins, the maximum of the absolute values of the negative
 * adjoining margins deducted from the maximum of the positive adjoining margins". That reduction is two
 * running maxima, so a run is two lengths and nothing else and it merges associatively — which is what lets the
 * SAME structure carry a run upward out of a child, downward out of a child, and through a child that
 * collapses through entirely.
 *
 * A RUN LEAVING THE BOX IS THE ONLY DIFFERENCE BETWEEN §10.6.3 AND §10.6.7, AND IT IS THE SPEC'S OWN.
 * §8.3.1's third and first adjoining pairs make a box's top margin adjoin its FIRST in-flow child's, and its
 * bottom margin adjoin its LAST in-flow child's — but only when nothing separates them, which is "no line
 * boxes, no clearance, no padding and no border", and only when the box does not establish a new block
 * formatting context (§8.3.1's own note: "margins of elements that establish new block formatting contexts …
 * do not collapse with their in-flow children"). When the run DOES escape, the child's top border edge is at
 * the box's own top content edge and the margin contributes nothing INSIDE the box — which is exactly
 * §10.6.3's "the bottom border edge of the last in-flow child whose top margin doesn't collapse with the
 * element's bottom margin" read from the other end. When it does NOT escape, the same walk measures from the
 * child's top MARGIN edge to the last child's bottom MARGIN edge, which is §10.6.7's rule verbatim. So the two
 * sections are one walk under two boolean flags, and the flags are a fact about the box (§9.4.1's list of
 * what establishes a formatting context, §8.3.1's list of what separates two margins), never a mode this
 * component picks.
 *
 * WHAT IS DELIBERATELY NOT BUILT, AND WHY EACH IS A CRASH AND NOT A ZERO. The smallest box model that answers
 * CSSOM VIEW §6 and §7 for the documents this engine parses is BLOCK FLOW: a block container whose in-flow
 * children are all block-level, over the used values core/layout/used_value.h already computes. Everything
 * else names its own section and aborts, because a zero from an unimplemented box passes a presence test and
 * is indistinguishable from a real one:
 *   - INLINE-LEVEL CONTENT is §9.4.2's inline formatting context and core/layout/line_box.h's, which this
 *     walk ROUTES TO rather than measures: §9.2.1 makes the two exclusive ("either contains only block-level
 *     boxes or establishes an inline formatting context"), so the choice is made once over the whole child
 *     list and each algorithm then sees only its own boxes. A container holding BOTH is not a third case and
 *     not an absence: §9.2.1.1 "Anonymous block boxes" says "if a block container box … has a block-level box
 *     inside it …, then we force it to have only block-level boxes inside it", so this walk iterates the BOX
 *     list that forcing produces — each maximal run of inline-level children wrapped in one anonymous block
 *     box whose height is §10.6.3's over line_box.h's line boxes and whose every other property is a constant
 *     the section fixes ("the margins will be 0"). That generation is a box-tree step and lives at the top of
 *     this walk, where the same list css-display-3 §2.5 "Box Generation: the none and contents keywords"'
 *     `contents` flattening will one day be spliced into.
 *   - A FLOAT in the formatting context is §9.5's own placement, and it is not enough to note that §10.6.3
 *     ignores floats: §9.5.2's `clear` on a LATER sibling introduces CLEARANCE, which §8.3.1 makes
 *     non-adjoining, so one float invalidates every collapse below it.
 *   - A TABLE box is §17.5's two algorithms and a FLEX or GRID container is its own spec's; neither's height
 *     is §10.6.3's walk, and a container whose children this walk placed would be a wrong number rather than
 *     an absent one.
 *   - An OUT-OF-FLOW child is not a gap at all: §10.6.3 states outright that "absolutely positioned boxes are
 *     ignored", so skipping one is the rule running, and the box's own position is §9.3.2's over a static
 *     position that this walk is what will one day provide.
 *
 * NOTHING IS STORED, FOR used_value.h's REASON, RESTATED BECAUSE THIS COMPONENT IS WHERE IT WOULD FIRST BE
 * TEMPTING TO BREAK IT. A layout is per-flow state — two flows with different DOMs have different boxes — so a
 * cached box tree is shared state the COW delta does not swap, and a stale one is a geometry from another
 * flow's document. Every answer here is DERIVED PER READ from the running flow's own tree and its own cascade,
 * which makes it per-flow by construction with no capture to write. The cost is that a subtree is walked once
 * per question asked of it; the day that is the bottleneck the cache is per-flow state and needs
 * solver/dom_cow.h's capture at its accessor, exactly as a browser component's own C record does.
 *
 * A HEIGHT IS A `CssPx` FOR used_value.h's REASON AND THE PROPAGATION IS FREE. Every operand of the walk is
 * already a used value carrying the set of environment facts it derives from — a percentage margin resolves
 * against the containing block's WIDTH (§8.3, and "note that this is true for 'margin-top' and 'margin-bottom'
 * as well"), which bottoms out in the initial containing block; a border width carries the device pixel ratio.
 * So an auto height computed here is a joint function of whatever its children's margins and borders were
 * functions of, and css_length.h's arithmetic unions those sets without this file deciding anything. A box
 * whose whole subtree is author-pinned comes out with the empty set and is CONCRETE, which is the correct
 * answer and not a lost domain: viewport.h's test is whether the model PICKED a value out of a range the
 * environment leaves free, and a stack of declared heights is not that. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BLOCK_FLOW_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BLOCK_FLOW_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* CSS 2.1 §9.2.1's BLOCK CONTAINER BOX, decided from a computed `display` and from nothing else — the box
   §9.4.1's formatting context is stated over, and the box §10.1's second case looks for when it walks for a
   containing block. It is exported because BOTH of those callers ask it and the list is not "everything
   block-level": an `inline-block` is a block container and is not block-level, and a TABLE box is block-level
   and is not a block container (§17.4 makes the CELL and the CAPTION the block containers inside one). */
bool block_flow_display_is_block_container(const char *display);

/* CSS 2.2 §9.2.2.1 "Anonymous inline boxes"'s WHITE-SPACE RULE for one TEXT child of a block container:
   "White space content that would subsequently be collapsed away according to the 'white-space' property does
   not generate any anonymous inline boxes." FALSE is that sentence — a run this element's computed
   `white-space` collapses away, which is most of the character data in a pretty-printed document — and TRUE
   is a run that generates an anonymous inline box.
   IT CLASSIFIES AND DOES NOT MEASURE, which is the contract each caller then has to satisfy for itself. Both
   of them go on to ask core/layout/line_box.h about the run and they ask it DIFFERENT questions: this file's
   walk wants §10.6.3's distance down the line boxes it flows into, while CSSOM VIEW §2's scrolling area wants
   where the boxes ON those line boxes REACH. Folding either measurement into this predicate would put one
   caller's question inside the other's classification, and the classification is right for both.
   IT IS EXPORTED BECAUSE EVERY WALK OVER A BLOCK CONTAINER'S CHILDREN MUST ASK IT, and the scrolling area
   asks for a reason the height walk cannot cover — a box with a DECLARED height never reaches the walk at all
   (`bf_height_needs_content`), so its own text would be invisible to a caller that only measured heights, and
   a text run that overflows a declared-height box is exactly what `scrollHeight` is asked about. A second
   copy of §9.2.2.1 would be one rule with two answers about whether a page's white space is content. */
bool block_flow_text_child_generates_box(lxb_dom_element_t *parent, const lxb_dom_node_t *text);

/* CSS 2.2 §9.4.2's OWN CONDITION over `el`'s WHOLE CHILD LIST: "an inline formatting context is established by
   a block container box that contains no block-level boxes."
   IT IS THE QUESTION "IS THIS ELEMENT WHERE core/layout/line_box.h's RUN IS THE WHOLE CHILD LIST", which is the
   one shape of §9.4.2's context that has an ELEMENT to name it. §9.2.1.1's anonymous block box is the other,
   and it is deliberately NOT reachable through this entry: a MIXED container answers FALSE here even though
   its inline-level children do sit in inline formatting contexts, because those contexts belong to boxes the
   element tree does not contain and a caller that wants them wants a RUN and not an element.
   IT IS EXPORTED BECAUSE §9.2.1's ALTERNATIVE IS ASKED FROM OUTSIDE THIS WALK AS WELL AS INSIDE IT. CSSOM VIEW
   §2's scrolling area needs the boxes on this context's line boxes PLACED, and reaching them means knowing
   which element establishes the context they are on — the same classification this file's own walk makes to
   choose between §9.4.1 and §9.4.2, and a second copy of it would be one document with two box trees, free to
   disagree about whether a run of white space is content (§9.2.2.1, the predicate above). */
bool block_flow_establishes_inline_context(lxb_dom_element_t *el);

/* CSS 2.2 §9.2.1.1 "Anonymous block boxes"' OTHER SHAPE OF §9.4.2's CONTEXT — the one with no element to name
   it. §9.2.1.1: "if a block container box (such as that generated for the DIV above) has a block-level box
   inside it (such as the P above), then we force it to have only block-level boxes inside it", and the boxes
   that forcing generates are one per MAXIMAL RUN of inline-level children, each holding an inline formatting
   context of its own.
   ONE BOX, AND EVERY FIELD OF IT IS THE SECTION'S OWN CONSTANT OR THE STACK'S OWN NUMBER. "The properties of
   anonymous boxes are inherited from the enclosing non-anonymous box …. Non-inherited properties have their
   initial value. For example, the font of the anonymous box is inherited from the DIV, but the margins will be
   0." So there is no margin, no border and no padding on it: its content box, its border box and its MARGIN
   box are ONE rectangle, which is why a single origin and a single extent describe all three.
   ITS INLINE-AXIS EDGES ARE NOT REPORTED, and that is a derivation rather than an omission. `width` has the
   initial value `auto` and both margins are zero, so CSS 2.1 §10.3.3's constraint equation leaves the whole of
   the containing block's content width to `width` — the anonymous box's two inline margin edges are exactly its
   container's two CONTENT edges, and CSS 2 §8.1 "Box dimensions" nests those inside the padding edge every
   caller of this entry has already folded. A number for them would be one an extreme cannot see. */
typedef struct {
    lxb_dom_node_t *first;   /* the run's first child, INCLUSIVE — core/layout/line_box.h's `first` */
    lxb_dom_node_t *end;     /* one past its last, EXCLUSIVE; NULL is "to the end of the child list" */
    CssPx content_x;         /* its content box origin as an OFFSET from the container's own content box
                                origin — zero on the inline axis, by the derivation above */
    CssPx content_y;         /* … and on the block axis, which is where §9.4.1's stack put it */
    CssPx height;            /* its border-box height — §10.6.3's first bullet over its own line boxes */
} BlockFlowAnonBox;

/* `el`'s ANONYMOUS BLOCK BOXES, in tree order. Answers the count and stores a newly allocated array of that
   many at `*out`, WHICH THE CALLER OWNS AND MUST FREE; a count of zero stores NULL.
   ZERO IS A POSITIVE ANSWER AT EVERY ELEMENT THAT GETS IT, never a shrug. §9.2.1.1 generates a box only where
   a BLOCK CONTAINER holds a block-level box AND inline-level content, so: a container with no block-level box
   establishes ONE inline formatting context with its OWN element to name it (the predicate above, and the run
   is then the whole child list); a container with no inline-level content has nothing to wrap; and an element
   that is not a block container at all is outside the section's sentence — an inline box's inline content is
   on its ANCESTOR's lines, and a flex or grid container's is css-flexbox §4's anonymous flex ITEM, which is a
   different box this engine does not build. A caller that needs the difference asks the predicate above too.
   IT IS A SECOND ENTRY BESIDE THAT PREDICATE BECAUSE §9.2.1's TWO SHAPES OF ONE CONTEXT ARE REACHED
   DIFFERENTLY, and the header states why the predicate deliberately does not answer for this one: a MIXED
   container answers FALSE there, because the contexts inside it belong to boxes the ELEMENT TREE DOES NOT
   CONTAIN. So a caller that wants to reach every inline formatting context under an element — CSSOM VIEW §2
   "Terminology"'s scrolling area is the one that must, since §9.2.2.1's anonymous inline boxes around a text
   run are "descendants' boxes" wherever they sit — asks the predicate for the shape that has an element and
   this entry for the shape that does not.
   THE POSITION COMES OUT OF §9.4.1's OWN STACK AND IS NOT RE-DERIVED, which is the whole reason this lives
   here: the offset reported is the same running position the walk reads out for an ELEMENT that asks
   `block_flow_child_top`, taken at the same point, so an anonymous box and its block-level siblings cannot
   come to disagree about where a margin collapsed. */
size_t block_flow_anonymous_boxes(lxb_dom_element_t *el, BlockFlowAnonBox **out);

/* CSS 2.1 §10.6.3's (and, for a box that establishes a block formatting context, §10.6.7's) CONTENT-BASED
   HEIGHT of `el`'s box, in CSS pixels — the used value of a `height` that BEHAVES AS AUTO (css-sizing-3
   §3.2.1, `used_value_height_behaves_as_auto`), which is a computed `auto` and also a percentage whose
   containing block's height is indefinite.
   THE CALLER HAS ALREADY ESTABLISHED that the height behaves as auto and that the box is one §10.6.3 or
   §10.6.6 covers; core/layout/used_value.c's `uv_size` is that caller and it has classified the box type
   first. Every
   case this component does not lay out crashes naming its own section — see the header. */
CssPx block_flow_auto_height(lxb_dom_element_t *el);

/* CSS 2 §9.4.1's VERTICAL PLACEMENT of `el`: the distance from the TOP CONTENT EDGE of `el`'s containing block
   to `el`'s TOP BORDER EDGE, in CSS pixels. It is stated against the containing block's CONTENT edge because
   that is the rectangle §10.1 gives the box and the one §10.6.3 measures its height from, so the caller adds
   exactly the containing block's own origin plus its top border and padding and nothing else.
   `el` MUST BE AN IN-FLOW BLOCK-LEVEL BOX whose containing block is §10.1's second case — core/layout/
   flow_position.c is the caller and it has taken every other positioning scheme out through its own section
   first. An element the walk over its containing block's children never places crashes rather than answering
   a coordinate no box has. */
CssPx block_flow_child_top(lxb_dom_element_t *el);

#endif
