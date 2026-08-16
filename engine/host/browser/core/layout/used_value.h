/* CSS 2.1 §10 — VISUAL FORMATTING MODEL DETAILS, which is where a box's USED VALUES come from, and the first
 * real box geometry in this engine.
 *
 * THE USED VALUE IS A THIRD ANSWER AND THE CASCADE PRODUCES NEITHER OF THE FIRST TWO. css_computed_value.h
 * states the split it owns — the cascade's SPECIFIED value, and the COMPUTED value a spec algorithm reads. CSS
 * Cascade's third stage is the USED value: "the values used are the same as the computed values, with 'auto'
 * replaced by some suitable value, and percentages calculated based on the containing block, but there are
 * exceptions" (§10.3's own opening). That sentence is this component's whole contract, and the exceptions are
 * §10.3.1 through §10.3.10 and §10.6.1 through §10.6.7, one per BOX TYPE.
 *
 * WHY THE BOX TYPE COMES FIRST AND IS NOT A DETAIL. §10.3 does not give `width` one algorithm; it gives ten,
 * selected by whether the box is inline or block-level, replaced or not, floating, absolutely positioned or in
 * normal flow. A used value computed without asking is not approximately right, it is an answer from the wrong
 * algorithm: a floated box's `width: auto` is a SHRINK-TO-FIT (§10.3.5) and a block-level one's follows from a
 * constraint equation (§10.3.3), and the two are not close. So `uv_box_kind` is the first thing every entry
 * below does, and a box type whose section is unbuilt CRASHES naming that section rather than borrowing the
 * neighbouring one's answer.
 *
 * WHAT THIS COMPONENT COMPUTES TODAY, AND WHY THAT SET AND NOT A LARGER ONE. Two of §10's arms need NO layout
 * at all, and they are not the small cases — and the last entry below is not one of §10's arms but a BOX EDGE
 * stated over them, which is here for the reason its own paragraph gives:
 *   - A MARGIN OR PADDING whose computed value is an absolute length. Nothing in CSS 2.1 alters it. §10.3.3's
 *     constraint equation solves for `auto` values and, when the box is OVER-CONSTRAINED, for one horizontal
 *     margin; it never touches a vertical margin, and §10.6.3 gives the vertical pair exactly one rule (`auto`
 *     becomes 0). Padding has no `auto` and appears in no equation as an unknown. So the used value IS the
 *     computed length, and saying so is a derivation rather than a shrug.
 *   - A `width` or `height` whose computed value is an absolute length, for every box type but a table box and
 *     a flex or grid item. §10.3.3's equation solves for `width` only when `width` is `auto`; §10.3.5,
 *     §10.3.7 and §10.3.9 each say the same for their own box type. A TABLE may be widened past it (§17.5.2)
 *     and a FLEX ITEM's size is its container's algorithm and not §10's at all, so both crash. One further
 *     conjunct is ASSERTED rather than assumed, because it is a case in which the declared length is not the
 *     used one and it is not visible as a crash otherwise: §10.4/§10.7's clamp by a declared `min-`/`max-`
 *     limit.
 *   - AND THE SAME SIZE UNDER `box-sizing: border-box`, which is a COMPUTATION and not a second assertion.
 *     What stood here called it one, on the ground that css-sizing §5 "makes the declared value the BORDER
 *     box's while §10.2 and CSSOM §9 both mean the content box's" — and §5's own next sentence says the
 *     opposite: "Used values, as exposed for instance through getComputedStyle(), also refer to the border
 *     box." So the exposed used value is the border box's size, which is the declared length except when the
 *     paddings and borders alone exceed it and the content box floors at zero (§5's own worked example: "the
 *     border-box size ends up at 120px, even though width: 100px is specified"). It is the LARGER of the two,
 *     and the four terms it needs are the two paddings and the two border widths.
 *   - AND THE PADDING EDGE'S EXTENT, which is not a property at all and is exactly why it is an entry HERE
 *     rather than arithmetic at the one caller that wants it. CSS 2.1 §8's box model makes the padding box the
 *     CONTENT box plus the two paddings on the axis, and that one sentence is the whole derivation in BOTH
 *     `box-sizing` modes — what differs between them is not the padding edge, it is which box the USED SIZE
 *     above is the size OF. Under `content-box` that size IS the content box and the two paddings are added;
 *     under `border-box` css-sizing §5 makes it the BORDER box, so the content box is that size minus the same
 *     four terms §5's own conversion names, and the paddings are added back to THAT. A CALLER CANNOT WRITE
 *     THIS: `used_value_px(el, "width")` plus the two paddings is the answer in one mode and DOUBLE-COUNTS
 *     them in the other, and nothing in the number it got back says which mode produced it. So the four terms
 *     are computed once, in one function, and both directions of §5's conversion are stated over that one
 *     result — the double-count is not a mistake to avoid, it is a sentence there is no longer anywhere to
 *     write.
 *
 * AND WHY THE ROOT ELEMENT'S `width: auto` IS STILL NOT AMONG THEM, WHICH IS THE ONE THING TO READ BEFORE
 * ADDING IT. §10.3.3's constraint equation has SEVEN terms,
 *     margin-left + border-left-width + padding-left + width + padding-right + border-right-width +
 *     margin-right = width of containing block
 * and EVERY ONE OF THEM IS READABLE. This paragraph used to say that two were not, because lexbor's property
 * registry carries no `border-*-width` longhand — it has the `border` and `border-<side>` SHORTHANDS and the
 * four `border-*-color` longhands and nothing else of the border, so `border: 1px solid red` set a width no
 * cascade read could see. That is built: core/css/css_shorthand.c expands `border`, `border-<side>`,
 * `border-width` and `border-style`, and core/css/css_computed_value.c derives the two longhands' computed
 * value (CSS 2.1 §8.5.1's rule that a `none`/`hidden` style makes it 0 included, and that file states why the
 * rule lives at the computed value rather than at the used one).
 * WHAT IS LEFT IS THE CONTAINING BLOCK, and it is two problems. §10.1's CHAIN makes every block-level box's
 * containing block the content edge of its nearest block container ancestor, so the equation is recursive; the
 * base case is real, because §10.1 makes the ROOT ELEMENT's the INITIAL CONTAINING BLOCK core/frame/viewport.c
 * models. And the ICB's width is not a number — see the paragraph below.
 *
 * THE GEOMETRY IS CONCRETE, AND A GEOMETRY DERIVED FROM THE VIEWPORT IS NOT. c35f1fed decided the first half
 * and it is right: viewport.h's test is whether the model PICKED one point out of a range the environment
 * leaves free or DERIVED the only value the model permits, and a box's size is neither — it is what a LAYOUT
 * determines from this tree and this cascade, so it is a computed number and a concolic there would invent an
 * example nothing computed. Every value this component returns today is that: an absolute length the author
 * wrote, or a zero §10.6.3 states. But the half that decision did not have to face is that the ICB's width is
 * a PICKED environment fact (viewport.h says so, and `viewport_env_value` is the seam that mints it), so a
 * used width DERIVED from it inherits its domain — `parseInt(getComputedStyle(el).width) < 768` is the same
 * responsive gate as `innerWidth < 768`, and answering it with a bare 1280 deletes the mobile arm exactly as
 * viewport.h warns. That is PROPAGATION and not a second policy: geometry stays concrete, and a value computed
 * FROM a concolic operand stays concolic because every operand's domain rides its result. It is also why the
 * viewport-unit arm of css_length.c crashes rather than resolving `50vw` — see its message: the resolved-value
 * path is a `char *`, and it stops being one on the day the first viewport-derived used value lands.
 *
 * NOTHING HERE IS STORED, SO NOTHING HERE TIME-TRAVELS — and that is a decision with a reason, not an omission.
 * A layout is per-flow state: two flows with different DOMs have different boxes, and a box tree cached across
 * a context switch would be exactly the shared state the COW delta does not swap. So there is no box tree.
 * Every used value is DERIVED PER READ from the running flow's own tree and its own cascade — the identical
 * decision css_style_declaration.c made for the cascade itself and for the identical reason — which makes it
 * per-flow by construction, with no capture to write and no entry to unapply. The day a box tree exists for a
 * reason a derivation cannot serve (an inline formatting context's line boxes cannot be re-derived per read
 * without re-running the whole flow's layout), it is per-flow state and it needs solver/dom_cow.h's capture at
 * its accessor, exactly as a browser component's own C record does. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_USED_VALUE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* THE USED VALUE of `name` on `el`, in CSS pixels. `name` is one of the physical box-model lengths CSSOM §9
   routes here — the four margins, the four paddings, `width` and `height` — and the caller has ALREADY
   established §9's two conjuncts (the property applies to the element, and the element generates a box), which
   is why nothing here re-asks them. A case CSS 2.1 §10 defines and this component does not compute crashes
   naming its own section; there is no fallback answer. */
double used_value_px(lxb_dom_element_t *el, const char *name);

/* THE USED EXTENT OF THE PADDING EDGE on one axis, in CSS pixels — the horizontal one for `vertical` false and
   the vertical one for true. CSSOM VIEW §6's `clientWidth` and `clientHeight` step 3 is its caller, and the
   header above derives it: the content box on that axis plus the two paddings, with css-sizing §5 deciding
   which box `used_value_px` handed back.
   IT IS AN EXTENT AND NOT AN EDGE POSITION, which is the whole reason these two §6 members can be answered
   while `getClientRects()` cannot: a position is a coordinate in the viewport's space and needs §10.1's
   containing-block chain, and a distance between two parallel edges of ONE box needs none of it.
   The caller has already established §6's step 1 — the element has an associated box and that box is not
   inline — which is what makes the size properties apply to it at all. Every arm CSS 2.1 §10 defines and this
   component does not compute crashes through `used_value_px` naming its own section, so an ordinary block-level
   box with `width: auto` reaches §10.3.3's constraint equation and the containing-block chain it is waiting
   on. */
double used_value_padding_edge_px(lxb_dom_element_t *el, bool vertical);

#endif
