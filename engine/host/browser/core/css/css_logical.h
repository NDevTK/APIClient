/* @LOGICAL — css-writing-modes-4 §6 "Abstract Box Terminology"'s ABSTRACT-TO-PHYSICAL MAPPING, over the
 * css-logical-1 §4 "Flow-Relative Box Model Properties" LOGICAL PROPERTY GROUPS it is applied to.
 *
 * WHAT A FLOW-RELATIVE PROPERTY IS: `margin-block-start` is not a second spelling of `margin-top` and it is
 * not an alias. css-logical-1 §4 pairs it with ONE of the four physical margins using the element's own
 * computed writing mode, and the pair then SHARES A COMPUTED VALUE — "Although the specified value of each
 * property remains distinct, paired properties share a computed value. This shared value is determined by
 * cascading the declarations of both properties together as one; in other words, the computed value of both
 * properties in the pair is derived from the specified value of the property declared with higher priority in
 * the CSS cascade."
 *
 * SO THE MAPPING IS AT THE CASCADE, AND A NAMED RESIDUAL IN THIS TREE SAID IT WAS AT THE USED VALUE. Three
 * sites — core/layout/used_value.c's routing crash, core/css/css_property_applies.c's, and
 * core/css/css_style_declaration.c's UA-margin residual — each named "css-writing-modes §6's mapping to a
 * physical property before §10 can be asked anything", and the last of them named the next diff as that
 * mapping AT `used_value_px`. The spec half of all three is right and the LAYER is wrong, which is worth
 * stating rather than quietly fixing, because the wrong layer produces a wrong ANSWER and not merely a late
 * one. Two of them:
 *   - css-logical-1 §4 ends "[CSSOM] APIs that return computed values (such as getComputedStyle()) must
 *     return the same value for each individual property in such a pair." A mapping that runs at the used
 *     value leaves the COMPUTED value of `margin-top` at its initial `0` on an element whose only declaration
 *     is `margin-block-start: 1em`, so every reader of a computed margin that is not CSSOM §9's used-value
 *     path answers zero — and a `getComputedStyle(p).marginTop` of `0px` beside a `marginBlockStart` of `1em`
 *     is the pair NOT sharing a value, which is the one thing §4 states twice.
 *   - core/layout/block_flow.c asks `used_value_px(el, "margin-top")`. That call would answer the physical
 *     property's own cascade, which no logical declaration ever reached — so the box would lay out with no
 *     margin while an instrument one component over reported the logical one correctly.
 * WHAT IS STILL TRUE OF `used_value_px` is the redirect: CSSOM §9 puts `margin-block-start` in its
 * used-if-rendered list, so that entry must answer for a flow-relative name too. That is a SEPARATE diff and
 * it is second, because a redirect landing before this one would answer the PHYSICAL property's unpaired
 * cascade — a plausible wrong datum where today there is a crash.
 *
 * TWO QUESTIONS, AND THEY ARE ASKED OF DIFFERENT THINGS. `css_logical_group_of` is PURE DATA — which
 * css-logical-1 §4 group a longhand is in, and whether its mapping logic is physical or flow-relative — and it
 * needs no element because a property's group is a fact about the property. `css_logical_partner_of` is the
 * MAPPING and needs the element, because css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings" is a
 * function of the computed `writing-mode` and the used `direction`.
 *
 * THE BLOCK AXIS NEEDS NO `direction` AND THE INLINE AXIS DOES, WHICH IS THE SPEC'S OWN SPLIT AND NOT A
 * SHORTCUT. css-writing-modes-4 §6.2 "Flow-relative Directions" ends: "Note that while determining the
 * block-start and block-end sides of a box depends only on the writing-mode property, determining the
 * inline-start and inline-end sides of a box depends not only on the writing-mode property but also the
 * direction property." So a `margin-top` query in a vertical writing mode resolves through the block axis and
 * never reads a direction, and only an inline-axis query reaches the used direction — which is where the one
 * capability this component does not have crashes by name.
 *
 * WHAT IS NOT COVERED — css-logical-1 §4.6 "Flow-Relative Corner Rounding"'s `border-start-start-radius` and
 * its three siblings are absent from the table below, and they are absent because they are a different SHAPE
 * rather than because nobody transcribed them: §4.6's own sentence gives the composition — "with the first
 * start/end giving the block axis side, and the second the inline-axis side" — so a corner is a PAIR of
 * abstract sides where every row here is one side or one dimension, and the physical name it composes to is a
 * pair of physical sides (`border-top-left-radius`). WHAT THE NEXT DIFF BUILDS: a corner role beside the side
 * and dimension roles, mapping each of its two halves through the same §6.4 table and joining them into the
 * physical corner name. HOW ITS ABSENCE WOULD SHOW: a declaration of a flow-relative corner radius is carried
 * by the cascade and read by nothing, so an element whose only rounding is declared that way renders with
 * square corners while `getComputedStyle` reports the physical radius as the initial `0`.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_LOGICAL_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_LOGICAL_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* css-logical-1 §4's LOGICAL PROPERTY GROUPS, each named by the "Logical property group:" line of the
   property definition that declares it — §4.1 gives `size`, `min-size` and `max-size`, §4.2 `margin`, §4.3
   `inset`, §4.4 `padding`, §4.5.1 `border-width`, §4.5.2 `border-style`, §4.5.3 `border-color`, and
   css-overflow-3 §3.1 "Managing Overflow: the overflow-x, overflow-y, and overflow properties" gives
   `overflow` on all four of its longhands — its `Name:` line is the four together. `NONE` is a
   property in no group, which is every property whose name encodes no axis or side at all. */
typedef enum {
    CSS_LOGICAL_GROUP_NONE = 0,
    CSS_LOGICAL_GROUP_MARGIN,
    CSS_LOGICAL_GROUP_PADDING,
    CSS_LOGICAL_GROUP_BORDER_WIDTH,
    CSS_LOGICAL_GROUP_BORDER_STYLE,
    CSS_LOGICAL_GROUP_BORDER_COLOR,
    CSS_LOGICAL_GROUP_INSET,
    CSS_LOGICAL_GROUP_SIZE,
    CSS_LOGICAL_GROUP_MIN_SIZE,
    CSS_LOGICAL_GROUP_MAX_SIZE,
    CSS_LOGICAL_GROUP_OVERFLOW,
} CssLogicalGroup;

/* The css-logical-1 §4 group `longhand` belongs to, and its MAPPING LOGIC — "The type of directional or axis
   mapping (flow-relative or physical) of each such property is called its mapping logic." `*pphysical` is
   written on EVERY path, including the one that answers no group, because a caller that compares two
   properties' mapping logic would otherwise read whatever the last question left there. A SHORTHAND is never
   in a group: §4 says "(ignoring shorthand properties)" in the sentence that defines the term. */
CssLogicalGroup css_logical_group_of(const char *longhand, bool *pphysical);

/* THE OTHER MEMBER OF `longhand`'s PAIR on this element — the flow-relative longhand that a physical one is
   paired with, or the physical longhand a flow-relative one maps to, resolved through
   css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings". A STATIC name, never owned; NULL exactly when
   `longhand` is in no group, which is the answer for every property whose cascade is a single property's.
   The pairing is an INVOLUTION and `css_logical_init` asserts it: the partner of the partner is the property
   you started from, which is what makes "cascade the two together" the same set whichever member was
   asked. */
const char *css_logical_partner_of(lxb_dom_element_t *el, const char *longhand);

/* The table's own invariants, asserted once per instance beside the shorthand table's. */
void css_logical_init(void);

#endif
