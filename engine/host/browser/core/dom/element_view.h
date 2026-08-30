/* CSSOM VIEW §6 — EXTENSIONS TO THE ELEMENT INTERFACE, which is how a page reads an element's geometry, and
 * the file that has to say what geometry this engine HAS.
 *
 * THE BOX MODEL THIS ENGINE ACTUALLY HAS, stated once here because every member below is a branch over it and
 * because two of this engine's components had already answered it in opposite directions:
 *
 *   A BOX EXISTS wherever a user agent would generate one. HTML's "being rendered" is defined AS "has any
 *   associated CSS layout boxes", so §6's "associated box" and HTML's "being rendered" are ONE predicate under
 *   two names, and `element_view_has_box` below is that one predicate. It is decided from the element's
 *   connectedness, from whether its node document is some navigable's ACTIVE document and is being presented
 *   (viewport.h's `viewport_exists`), and from the COMPUTED `display` of the element and of its ancestors
 *   (core/css/css_computed_value.h) — `none` on any of them, or `contents` on the element itself, and there is
 *   no box. §15.3.1's UA-stylesheet rule for the `hidden` content attribute is one input to that value and is
 *   applied where every other UA rule is, in the cascade, rather than a second time here.
 *
 *   AN EXTENT AND A POSITION ARE TWO ANSWERS AND THEY COME FROM TWO COMPONENTS, and that split is what decides
 *   which §6 member below answers and which one crashes. It is not this component's distinction; it is the
 *   spec's own. An EXTENT is a distance between two parallel edges of ONE box, and core/layout/used_value.h
 *   computes those: CSS 2 §10's used value of every box-model length, over §10.1's containing block, and over
 *   them css-sizing §5's border box and CSS 2 §8.1's PADDING and BORDER EDGES. A POSITION is a coordinate in
 *   another box's space, so it needs §9.4.1's flow layout to PLACE a box inside the containing block §10.1
 *   gives it — the extent chain answers a rectangle's width and says nothing about where anything sits — and
 *   core/layout/flow_position.h is the component that owns it. WHAT STOOD HERE SAID THAT COMPONENT "ANSWERS
 *   EXACTLY ONE BOX TODAY, THE ROOT ELEMENT", and that has been false since the in-flow child walk landed: the
 *   root is §10.1's first case (its containing block is the initial containing block, "anchored at the canvas
 *   origin", reduced by §9.4.1's two placement rules and §8.3.1's "margins of the root element's box do not
 *   collapse" to the root's own two used margins) and it is the BASE CASE of an induction that now runs —
 *   every in-flow block-level box is placed at its containing block's origin plus §8.1's leading border and
 *   padding plus core/layout/block_flow.h's stack of the preceding siblings' used heights with §8.3.1's
 *   collapsing between them. That was the one subproblem §10.6.3's content-based height was waiting on too,
 *   and building it built both. What still crashes there is named per box and not per component: a float is
 *   §9.5's own positioning, an out-of-flow box is §9.3.2's offsets over a static position, and an inline box is
 *   §9.4.2's line boxes.
 *   SO §6 SPLITS EXACTLY THERE, and each member's own text says which side it is on. `clientTop`/`clientLeft`
 *   are neither — §6 defines them as a COMPUTED VALUE, core/css/css_computed_value.h's `border-*-width`, and
 *   not as a geometry at all. `clientWidth`/`clientHeight` ask for "the unscaled width of the PADDING EDGE",
 *   which is an extent, and are answered. `scrollWidth`/`scrollHeight` and the `scrollTop`/`scrollLeft` setter
 *   reach for the element's SCROLLING AREA, which §2 defines by four edges over this box AND every
 *   descendant's — a rectangle over a whole SUBTREE, which is core/layout/scrolling_area.h and is why
 *   `scrollWidth` is a member of this section that reaches a different component from `clientWidth`'s.
 *   `getClientRects()` asks for a
 *   BORDER AREA, which is one box's extent at one box's position, and is now written as exactly that: both
 *   operands are asked for, in that order, and whichever is unbuilt is the crash. An extent cannot stand in for
 *   a position, which is the one way this component could go wrong now that it has one.
 *   THE EXTENTS ARE NO LONGER BLOCKED ON THE SAME THING THE POSITIONS ARE, and that is what separated the two
 *   queues: a `width: auto` box is CSS 2.1 §10.3.3's constraint equation, which used_value.c now solves against
 *   §10.1's containing block down to the ICB at its base, so `clientWidth` answers for an ordinary box. THE
 *   POSITIONS HAVE SINCE CAUGHT UP for the ordinary box as well (§9.4.1's stacking, above), and the rectangle
 *   over a SUBTREE has followed them: §2's scrolling area is derived, so `scrollWidth` answers and the setter's
 *   step 10 decides its own overflow disjunct out of it. What still crashes here names a term the cascade never
 *   carried or an algorithm outside this section rather than "there is no layout": `getClientRects()` wants
 *   step 3's transforms APPLIED, and §6's own last step — the setter's and the three scroll members', one
 *   site — wants §6.1 Element Scrolling Members' SCROLL AN ELEMENT (which is where "to scroll an element … to
 *   x,y" is stated; §3.1 Scrolling is the PERFORM A SCROLL that one ends in) and a scroll
 *   position held as per-flow state. CSSOM VIEW §7's offset family is the measure of
 *   how far that is: core/html/html_element_view.c reports `offsetWidth` and `offsetTop` for an ordinary box
 *   out of these same two components, because §7's own text says UNSCALED and IGNORING TRANSFORMS where §6's
 *   step 3 says apply them.
 *
 * THOSE ARE TWO DIFFERENT ANSWERS AND THE SPEC ASKS THEM SEPARATELY, which is the whole reason this component
 * can answer anything at all. §6's algorithms use box EXISTENCE as a gate and then, in several branches, route
 * around the box entirely to the VIEWPORT: `clientWidth` on the root element returns the viewport width and
 * never looks at the root's box; `scrollWidth` on the root returns max(the viewport's scrolling area, the
 * viewport) and never looks at a descendant. Those branches are answered here, for real. A branch that reaches
 * the element's own box is answered when what it wants is an EXTENT — `clientWidth` on an ordinary box is CSS
 * 2.1 §8's padding edge over §10's used values — and DFAILs when what it wants is a POSITION, naming the layout
 * it needs. Neither ever answers a zero standing in for a number this engine does not have, which is the stub
 * §NO STUBS is about.
 *
 * WHAT WAS SAID HERE BEFORE, AND WHICH HALF OF IT WAS WRONG. viewport.c's §2 derivation said "this engine
 * generates no boxes — there is no layout", and core/html/focus.c's §6.6.2 row 1 said a connected element that
 * is not `hidden` IS being rendered. Both cannot be true. The second is the correct one — a UA generates a box
 * for the root element of a document it is presenting, and this engine presents every document it holds
 * (page_visibility.c and focus.c both already commit to that) — so it is the one that survives, and it is now
 * ONE function rather than a private static in the focus model. What survives of the first is the part that was
 * really about GEOMETRY and not about existence: no box in this model had a margin edge, so nothing extended the
 * ICB, and viewport.c's scrolling area was exactly the ICB.
 * THAT SECOND HALF IS NOW NARROWER THAN IT READS, AND THE NARROWING IS NAMED HERE RATHER THAN ASSUMED AWAY.
 * core/layout/flow_position.h places the ROOT ELEMENT's border box, and core/layout/used_value.h measures it,
 * so ONE box in this model now has a margin edge — and `html { height: 2000px }` puts that edge below the ICB's
 * bottom, which is exactly CSSOM VIEW §2's condition for the viewport's SCROLLING AREA to be taller than the
 * ICB ("the bottom-most edge of the bottom edge of the initial containing block and the bottom margin edge of
 * all of the viewport's descendants' boxes"). viewport.c still derives the two as equal, and with them the
 * viewport's one valid scroll position that every scroll member here reads. It is not wrong today, because
 * nothing else in this engine can measure the box either — `scrollHeight` on the root reaches viewport.h before
 * it reaches a layout — but it is a fact with a NEW writer, and the place to fix it is viewport.c's derivation,
 * over this component's predicate and that one's placement, the moment the root's used height is computable
 * for a page that did not declare one.
 *
 * SO THE VIEWPORT HAS ONE VALID SCROLL POSITION AND SO DOES EVERY ELEMENT. viewport.h derives the first: the
 * viewport's scrolling area is the ICB, and a scrolling box whose scrolling area is its own size can only sit
 * at its origin. The second is derived rather than assumed: the origin of a scrolling area is defined AT the
 * element's default scroll position, a scroll position moves only when §3.1's PERFORM A SCROLL runs, and every
 * route to that for an element ends at ONE crash — §6 writes the same two last steps for the
 * `scrollTop`/`scrollLeft` setter and for `scroll()`/`scrollTo()`/`scrollBy()`, and element_view.c states them
 * once (`ev_scroll_the_element_or_terminate`) rather than twice. So "no element has been scrolled" is true BY
 * CONSTRUCTION, which is what makes the getters' final step a derivation and not a shrug, and the DFAIL is what
 * keeps it true. IT MUST STAY ONE SITE: a second copy of that crash is a second description of one absence,
 * and the one nobody deletes is the one that goes on naming work that is already done.
 *
 * THE CONCOLIC POLICY IS INHERITED, NOT RE-DECIDED. A member that reports the viewport's size reports a UA
 * CHOICE, so it carries the modelled geometry as the EXAMPLE of a concolic minted through viewport.h's one seam
 * (`viewport_env_value`) — `document.documentElement.clientWidth` is the most common way a bundle asks how wide
 * the viewport is, and a bare number there deletes a responsive bundle's mobile world exactly as a bare
 * `innerWidth` would. It is its OWN source rather than `innerWidth`'s, by 2dbe86d8's own test: `innerWidth`
 * INCLUDES a rendered scroll bar and `clientWidth` EXCLUDES it, so `innerWidth - documentElement.clientWidth` is
 * a bundle measuring the scroll bar, a question with two answers. A member whose answer the model DERIVES —
 * every scroll position here — stays concrete, for the reason viewport.h gives: a fact with a writer is per-flow
 * state in the COW delta, not an environment source. Nothing in this file branches in C on a concolic.
 *
 * AND A RECTANGLE IS THE SAME SPLIT AGAIN, WHICH IS WHY `getClientRects` AND `getBoundingClientRect` ARE HERE
 * RATHER THAN ON THE ABSENT LIST. getClientRects' STEP 1 — "if the element does not have an associated box
 * return an empty DOMRectList" — is decided by the predicate above and by nothing else, and get-the-bounding-box
 * then answers an empty list with "a DOMRect whose x, y, width and height members are zero". That is a value
 * the SPEC computes, identically in every user agent, for every element that generates no box; it is not a zero
 * standing in for a number this engine does not have, and it is CONCRETE for viewport.h's reason — a domain of
 * one point has no arm to explore. That branch is most of what a lazy-loading bundle measures before it
 * inserts anything.
 * STEP 3 IS WRITTEN AS ITS OWN THREE OPERANDS AND CRASHES ON WHICHEVER IS UNBUILT, which is the difference
 * between naming a missing capability and shrugging at "there is no layout". The fragment COUNT is decided
 * first (an inline box is one fragment per line box, a `table` is step 3's own table-plus-caption pair, and
 * nothing else in this model is more than one); then the border area's two EXTENTS, which crash inside
 * used_value.h for a box §10 does not size; then its POSITION, which flow_position.h answers for an in-flow
 * block-level box and refuses for a float, an out-of-flow box, an inline box and a vertical writing mode, each
 * naming its own section; then step 3's FIRST CONSTRAINT, "apply the
 * transforms that apply to the element and its ancestors", which has no computed `transform` to apply and
 * crashes naming the computed-value rule css_computed_value.c's own transform crash already asks for. A
 * rectangle reported without that last one would be a WRONG rectangle rather than an absent one — an author's
 * declaration silently dropped — which is why it is a crash and not the zero-scroll-bar reading every other
 * member here makes: no scroll bar is a UA CHOICE this model makes, a dropped `transform` is not.
 *
 * A BOX'S GEOMETRY IS NOT A UA CHOICE, so it is not the kind of thing a concolic answers, and the padding edge
 * is now the case that DEMONSTRATES that rather than the case that postponed it. viewport.h states the test: a
 * member is an environment SOURCE when the model PICKED one point out of a range the environment leaves free
 * (the viewport's size, the device pixel ratio, the refresh rate), and CONCRETE when it DERIVED the only value
 * the rest of the model permits. A box's size and position are neither — they are what a LAYOUT determines from
 * this tree and this cascade — so `clientWidth` on a `width: 100px; padding: 10px` box reports 120 CONCRETELY,
 * a number computed from the author's own declarations through used_value.h and not one picked out of a range.
 * Where that layout is still unbuilt the member CRASHES, and the same reasoning is why: a concolic there would
 * put an engine's own missing component under the vocabulary reserved for what the environment does not tell
 * us, invent an example for `rect.top` that nothing computed, and silence the crash that is the only thing
 * asking for the layout to be built.
 * THAT EDGE IS CROSSED NOW, AND IT IS USED_VALUE.H'S CROSSING AND NOT THIS FILE'S. A `width: auto` box resolves
 * through CSS 2.1 §10.3.3's constraint equation against §10.1's containing block, whose chain bottoms out in
 * the ICB — so an ordinary `div`'s padding edge carries the viewport's domain and `clientWidth` reports a
 * concolic for it, while the `width: 100px; padding: 10px` box above still reports a concrete 120. Neither is a
 * decision made here: the used value carries the fact it derives from (css_length.h), `ev_length_long` rounds
 * the EXAMPLE and hands the pair to viewport.h's one seam, and a box whose size the author pinned arrives with
 * no fact to mint. Propagation from the operand, never a second policy.
 *
 * WHAT IS HONESTLY ABSENT: `checkVisibility`, `scrollIntoView` and `currentCSSZoom`. `checkVisibility` needs a
 * flat-tree walk over computed `content-visibility`, `visibility` and `opacity`. `scrollIntoView` is not the
 * three members beside it wearing another name: §6.1 Element Scrolling Members gives it two algorithms of its
 * own — DETERMINE THE SCROLL-INTO-VIEW POSITION, which aligns the target's bounding border box against a
 * scrolling box's four flow-relative edges under `block`/`inline` of "start"/"center"/"end"/"nearest", and
 * SCROLL A TARGET INTO VIEW, which walks "each ancestor element or viewport that establishes a scrolling box,
 * in order of innermost to outermost" and settles one Promise over the set. Neither is expressible while an
 * element has no ASSOCIATED SCROLLING BOX, which is the first of the two absences the crash in element_view.c
 * names; the ancestor walk is that same question asked once per ancestor. The IDL audit reports all three,
 * which is correct.
 *
 * WHAT WAS ON THAT LIST AND SHOULD NOT HAVE BEEN: `scroll`, `scrollTo` and `scrollBy`, which stood here as
 * "§6's Promise-returning form of the setter below" that "arrive with the Promise the perform-a-scroll steps
 * return". The first half is right and the second was over-broad by nine of §6's eleven steps. Only step 11
 * performs a scroll. Steps 3 through 9 are the four questions this component already answers plus the two
 * VIEWPORT routes the setter beside them was already taking — a root element scrolls the window, and so does a
 * quirks-mode body that is not potentially scrollable — and §6's own steps 4, 5, 7 and 10 RETURN A RESOLVED
 * PROMISE with no scroll at all. A member is absent when its ALGORITHM cannot run, never when its last step
 * cannot; the ones that reach step 11 crash there exactly as the setter does, and that crash is now one site
 * for both. Reading a member's hardest step as the member is how a buildable one stays on an absent list. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_VIEW_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_VIEW_H

#include <stdbool.h>
#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "core/css/css_length.h"

/* Declared once per AGENT — the two settable attributes' setter ids. Called from element_init, so no host has a
   line to remember. */
void element_view_init(JSContext *ctx);
/* §6's `partial interface Element`, for ONE realm — installed on the prototype element.c has just built, the
   way §4.9's other partial interfaces are. It is per realm because its answers are: a child navigable's
   viewport is 300 CSS pixels wide and the top-level traversable's is 1280, and a C member runs in the realm
   that DEFINED it. */
void element_view_install(JSContext *ctx, JSValueConst proto);
void element_view_free(void);

/* HTML'S "BEING RENDERED" AND CSSOM VIEW'S "HAS AN ASSOCIATED BOX" — one predicate, one answer, for every
   standard in this engine that asks it. See the header above for what decides it. It reads the COMPUTED
   `display` of the element and of every ancestor, so an author rule, an inline style and the UA sheet all
   participate through the one cascade; what it still cannot see is a box that a LAYOUT would decline to
   generate for a reason other than `display` — which is the same narrowing this whole component states, never
   a wider answer than a laying-out browser's. `n` must be an element. */
bool element_view_has_box(const lxb_dom_node_t *n);

/* §6's `getClientRects()` AS THE INTERNAL ALGORITHM, which §2 is explicit is what a caller "said to call
   another method" invokes: "the method or attribute must be invoked, and not the algorithm the specification
   defines for it" is what it is NOT — the algorithm runs, so a page that overwrites
   `Element.prototype.getClientRects` cannot change what its callers measure. §6's own get-the-bounding-box
   step 1 is one caller and §9's Range members are the other, which is why this is exported rather than static.
   The DOMRectList and the rectangles in it are minted in the ELEMENT's relevant realm — Web IDL creates a
   `[NewObject]` in the relevant realm of `this`, which for an element is the realm of its node document, so a
   child navigable's rectangles are instances of the CHILD's DOMRect however the call was written. */
JSValue element_view_client_rects(lxb_dom_element_t *el);

/* §6's GET THE BOUNDING BOX AS THE INTERNAL ALGORITHM, answering in css_length.h's vocabulary rather than as a
 * DOMRect — `out` is filled with x, y, width and height in CLIENT COORDINATES, in that order.
 *
 * IT IS A SECOND ENTRY AND NOT A SECOND ANSWER, and the reason it exists is the reason `CssPx` exists. A caller
 * that must COMPARE two rectangles has to do it on the EXAMPLES, in C, with the environment facts still
 * attached: Intersection Observer §3.2.10 takes a rectangle's area (step 9), intersects two of them (its step 8
 * calls §3.2.7) and divides one area by another (step 12), and every one of those is a C comparison that must
 * not fork and must not lose the domain either. A rectangle handed over as a DOMRect has already crossed
 * viewport.h's seam — its numbers are the page's, and asking C for `rect.width` back would either branch on
 * unknown external input or throw the fact set away. So the rectangle stops here for an engine-internal caller
 * and is minted only where it becomes an entry's `boundingClientRect`.
 *
 * The FOUR NUMBERS are the same derivation `getBoundingClientRect` reports; see element_view.c for which of §6's
 * steps each road takes and which of them crash. `el` must be an element. */
void element_view_bounding_box_px(lxb_dom_element_t *el, CssPx out[4]);

/* §6's CONVERSION OF A REAL LENGTH TO THE `long` ITS IDL DECLARES — the rounding derived in element_view.c and
   the mint through viewport.h's one seam, in one function because CSSOM VIEW §7's `offsetTop`, `offsetLeft`,
   `offsetWidth` and `offsetHeight` report the same kind of value through the same type. The rule is
   ROUND-TO-NEAREST with ties going UP (`floor(px + 0.5)`), for the reason element_view.c derives at length: the
   members are MEASUREMENTS, and nearest is the only one of the three conversions whose error is bounded by half
   a CSS pixel in both directions.
   THE TIE-BREAK IS OBSERVABLE HERE AND IS NOT IN §6, which is why it is stated rather than asserted away. §6's
   six extents are distances between parallel edges and cannot be negative, so round-half-up and
   round-half-away-from-zero are the same function on every value they can take — `ev_length_long` asserts
   exactly that. §7's `offsetTop` CAN be negative (a box above its offsetParent's top padding edge, which a
   negative margin puts there), so the two separate at −2.5 and one of them has to be chosen: it is this one,
   because it is the rule this engine already had and a second rounding rule for the same question is the
   defect §per-realm names — one fact answered from two places.
   `px` carries the environment fact it derives from (css_length.h) and the domain rides the answer. */
JSValue element_view_length_long(JSContext *ctx, CssPx px);

/* §6's step-3 FRAGMENT COUNT — how many box fragments an element's principal box generates, which is one
   question and two crashes. §6's `getClientRects()` returns one DOMRect per fragment; §7's `offsetWidth` and
   `offsetHeight` return "the unscaled width of the axis-aligned bounding box of the border boxes of ALL
   FRAGMENTS generated by the element's principal box". Both are blocked by the same two absences and each
   names its own section's text, so the DERIVATION is stated once here and the DFAILs stay with their callers. */
typedef enum {
    ELEMENT_VIEW_FRAGMENTS_ONE = 0,   /* the principal box is not split: one border box, one rectangle */
    ELEMENT_VIEW_FRAGMENTS_LINE_BOXES,/* an inline box: one fragment per line box of §9.4.2's inline context */
    ELEMENT_VIEW_FRAGMENTS_TABLE      /* `table`/`inline-table`: §6's own table-box-plus-caption-box pair */
} ElementViewFragments;
ElementViewFragments element_view_fragment_kind(lxb_dom_element_t *el);

#endif
