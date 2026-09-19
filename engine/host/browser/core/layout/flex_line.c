/* css-flexbox-1 §9.3 "Main Size Determination" and §9.7 "Resolving Flexible Lengths" over one flex line. See
   flex_line.h for why the two sections are one component and for why nothing here is stored. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/replaced_element.h"
#include "core/layout/used_value.h"

/* ONE MEMBER of §6 "Flex Lines"' line, carrying every operand §9.7 reads and the two values it writes.
   `el` IS NULL FOR §4's ANONYMOUS CHILD TEXT SEQUENCE and that is a value rather than a gap: §4 makes that box
   unstyleable, so every property §9.7 asks of it answers its own `Initial:` line — `flex-grow` 0,
   `flex-shrink` 1, `flex-basis` `auto` over a `width` of `auto`, `min-width` `auto`, `max-width` `none`, and
   CSS 2.1 §8's four edges all zero. A reader who finds the arms below branching on NULL has found that
   sentence and not a missing case.
   EVERY SIZE HERE IS A CONTENT BOX. §9.7 floors "its content-box size at zero" in its own words and §7.2.3
   "The flex-basis property" makes the flex basis "the size of the content box, unless otherwise specified,
   such as by box-sizing", so css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"' conversion is
   applied ONCE as each operand is read and never again. `surround` is
   css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer step held apart from them, because §9.7 asks for an
   OUTER size in three places (its step 1, its initial free space and its remaining free space) and for an
   INNER one everywhere else. */
typedef struct {
    lxb_dom_element_t *el;
    /* THE ITEM'S OWN IDENTITY, WHICH `el` IS NOT FOR ALL OF THEM — the element's node for §4's element item,
       and the FIRST TEXT NODE of the child text sequence for §4's anonymous one. It is a DOM node this
       container's child list already holds and never a value this file invents, which is the whole of why an
       anonymous item can be named at all: a text node is not an element node, and two child text sequences
       begin at two different text nodes, so nothing here can collide with anything else on the line. */
    lxb_dom_node_t    *first;
    CssPx  base;          /* §9.2 "Line Length Determination"'s FLEX BASE SIZE */
    CssPx  hypothetical;  /* §9.2's HYPOTHETICAL MAIN SIZE */
    CssPx  surround;      /* the item's main-axis margin + border + padding */
    CssPx  min_main;      /* the used min main size, css-flexbox-1 §4.5's where `min-width` is `auto` */
    CssPx  max_main;      /* read only where `has_max` */
    bool   has_max;
    double grow;
    double shrink;
    CssPx  target;        /* §9.7's TARGET MAIN SIZE */
    double violation;     /* §9.7's step 4d ADJUSTMENT, `clamped size - unclamped size`; its sign is the kind */
    bool   frozen;
} FlItem;

/* THE MAGNITUDE OF ONE OF §9.7's FREE-SPACE VALUES. It is written here rather than taken from `<math.h>`'s
   `fabs` because the quantity §9.7 compares is a SIGNED FREE SPACE whose two candidates it then chooses
   between — "If the magnitude of this value is less than the magnitude of the remaining free space, use this
   as the remaining free space" — so the comparison is over magnitudes and the SELECTION keeps the sign, which
   an `fabs` at the assignment would silently discard. */
static double fl_magnitude(double v)
{
    return v < 0.0 ? -v : v;
}

static bool fl_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;
    char nbuf[160];

    DCHECKF(v != NULL,
            "%s, property `%s`: the cascade produced no computed value for a property this engine models — "
            "every one of them is in lexbor's registry with an initial value, so the last layer always answers",
            box_subject(el, nbuf, sizeof nbuf), name);
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* THE ITEM'S MAIN-AXIS BORDER AND PADDING, as USED values. They are used values and not the intrinsic ones
   core/layout/intrinsic_size.c composes, and the difference is the whole reason this component exists: a
   percentage padding resolves against the containing block's width, which for a flex item IS the flex
   container's content box and IS determined before §9.3 runs — while an INTRINSIC pass has no such number and
   css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" resolves the same percentage against
   zero. Two passes, two correct answers, and reaching for the other one here would report a box narrower than
   the page draws. */
static CssPx fl_main_border_padding(lxb_dom_element_t *el)
{
    CssPx b[4];

    if (el == NULL) return css_px(0.0);
    used_value_border_widths_px(el, b);
    return css_px_add(css_px_add(b[1], b[3]),
                      css_px_add(used_value_px(el, "padding-left"), used_value_px(el, "padding-right")));
}

/* THE ITEM'S MAIN-AXIS MARGINS. An `auto` one is NOT answered here and must not be: css-flexbox-1 §9.5
   "Main-Axis Alignment"' first step gives it the container's REMAINING free space — "If the remaining free
   space is positive and at least one main-axis margin on this line is auto, distribute the free space equally
   among these margins" — which is a number §9.7 has not produced yet at the point every caller below reads
   this. core/layout/used_value.c already refuses that value by name, so the refusal is ONE sentence in ONE
   place and this file does not restate it. */
static CssPx fl_main_margins(lxb_dom_element_t *el)
{
    if (el == NULL) return css_px(0.0);
    return css_px_add(used_value_px(el, "margin-left"), used_value_px(el, "margin-right"));
}

/* ONE DECLARED MAIN-AXIS SIZE of `el` as a CONTENT-box extent — true when the property states a size, false
   when it states its own initial keyword. `name` is `width`, `max-width` or `flex-basis` and `initial` is that
   property's initial value, which is the only keyword admitted.
   IT IS NOT core/layout/intrinsic_size.h's `intrinsic_declared_sizing_px` AND THE DIFFERENCE IS THE PERCENTAGE,
   which is the one thing that must not be shared between the two passes. That entry answers FALSE for a
   percentage, correctly, because css-sizing-3 §5.2.1 leaves a cyclic percentage out of an intrinsic
   contribution — and here the basis is `inner_main`, the container's own determined inner main size, so the
   same declaration IS definite and dropping it would report an item sized by a rule the author never wrote.
   §3.3's CONVERSION USES THE USED EDGES for the same reason `fl_main_border_padding` does. */
static bool fl_declared_main_px(lxb_dom_element_t *el, CssPx inner_main, const char *name, const char *initial,
                                CssPx *out)
{
    CssLength len;
    CssPx declared;
    char nbuf[160];

    if (el == NULL) return false;
    len = css_computed_length(el, name);
    if (len.kind == CSS_LENGTH_KEYWORD) {
        DCHECKF(strcmp(len.keyword, initial) == 0,
                "%s: `%s` computed to the keyword `%s` rather than to its initial value `%s`. "
                "css-sizing-3 §3.2 \"Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, "
                "min-content, max-content, and fit-content values\" adds keywords this engine records no "
                "computed-value "
                "rule for, and css-flexbox-1 §7.2.3 \"The flex-basis property\" gains every one of them by "
                "reference — \"The flex-basis property hereby also gains these new keywords, as its values are "
                "defined by reference to <'width'>\". BUILD "
                "css-sizing-3 §3.2's keyword arm where a `width` of the same "
                "shape is answered (core/layout/intrinsic_size.c and core/layout/used_value.c both refuse the "
                "identical keyword on the identical grammar) and this routes through it unchanged",
                box_subject(el, nbuf, sizeof nbuf), name, len.keyword, initial);
        return false;
    }
    DCHECKF(len.kind == CSS_LENGTH_ABSOLUTE || len.kind == CSS_LENGTH_PERCENTAGE ||
                len.kind == CSS_LENGTH_CALCULATED,
            "%s: `%s` computed to none of the shapes css-sizing-3 §3.2 \"Sizing Values: the "
            "<length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content "
            "values\" admits",
            box_subject(el, nbuf, sizeof nbuf), name);
    /* css-flexbox-1 §7.2.3's own sentence for the percentage half: "percentage values of flex-basis are
       resolved against the flex item's containing block (i.e. its flex container)". A `width` percentage
       resolves against the same rectangle by CSS 2.1 §10.2 "Content width: the 'width' property", so both
       spellings take one basis here. */
    declared = css_length_resolve_pct(len, inner_main);
    if (fl_computed_is(el, "box-sizing", "border-box"))
        declared = css_px_max(css_px_sub(declared, fl_main_border_padding(el)), css_px(0.0));
    *out = css_px_max(declared, css_px(0.0));
    return true;
}

/* css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items"' USED VALUE of a MAIN-AXIS automatic minimum size,
   which css-sizing-3 §3.2 hands to this module by name — "specifies an automatic minimum size. Unless
   otherwise defined by the relevant layout module, however, it resolves to a used value of 0."
   §4.5's own sentence is the whole rule: "the used value of a main axis automatic minimum size on a flex item
   whose computed overflow value is non-scrollable is its content-based minimum size; for main-axis scroll
   containers the automatic minimum size is zero, as usual." css-overflow-3 §3.1 "Managing Overflow: the
   overflow-x, overflow-y, and overflow properties" is where the two sets are named — "The scroll, auto, and
   hidden values are known as the scrollable values of overflow" and "The visible and clip values are known as
   the non-scrollable values" — so the gate is a MEMBERSHIP TEST over the MAIN axis's own property and not a
   compare against one keyword.
   THE NON-REPLACED ARM IS THE ONLY ONE REACHED AND THE OTHER IS REFUSED ABOVE, by name, at the walk: "For
   non-replaced elements: Use the larger of the content size suggestion and the transferred size suggestion (if
   one exists), capped by the specified size suggestion (if one exists)", and "In either case, the size is
   clamped by the maximum main size if it's definite." With no preferred aspect ratio the transferred size
   suggestion is undefined by its own definition, so the larger-of-one is the content size suggestion and this
   is that sentence read literally rather than simplified.
   IT MUST NOT BE READ BY §9.9.3 "Flex Item Intrinsic Size Contributions", and that is a MEASUREMENT rather
   than a preference: WPT's flex-container-min-content-001.html pairs a `flex: 0 1 0.2ch` item whose min-content
   size is 1ch against a reference of `width: 0.2ch`, and its rows 14 and 15 do the same over a declared
   `width` — which is the answer §9.9.3's clamp gives with NO §4.5 floor and is not the one it gives with one.
   core/layout/flex_intrinsic_size.c records that oracle at its own step four and reads the plain zero. So the
   two clamps are two different questions over one property name, and a reader who routes that one here will
   turn three passing rows red. */
static CssPx fl_automatic_minimum_main(lxb_dom_element_t *el, CssPx inner_main, IntrinsicInlineSizes measured)
{
    CssPx out = measured.min_content, v;

    /* §4's ANONYMOUS ITEM takes every initial value, and `overflow`'s is `visible` — a non-scrollable value —
       so it reaches the content-based minimum with no declaration to cap or clamp it. */
    if (el == NULL) return out;
    if (!fl_computed_is(el, "overflow-x", "visible") && !fl_computed_is(el, "overflow-x", "clip"))
        return css_px(0.0);
    /* "capped by the specified size suggestion (if one exists)", whose own definition is "If the item's
       preferred main size is definite and not automatic, then the specified size suggestion is that size." */
    if (fl_declared_main_px(el, inner_main, "width", "auto", &v)) out = css_px_min(out, v);
    /* "In either case, the size is clamped by the maximum main size if it's definite." */
    if (fl_declared_main_px(el, inner_main, "max-width", "none", &v)) out = css_px_min(out, v);
    return out;
}

/* THE TWO ARMS OF §9.2 "Line Length Determination"' STEP 3 THIS COMPONENT REACHES, which are NOT the two
   core/layout/flex_intrinsic_size.c reaches, and saying which is what stops the two being merged.
     - ARM A, "If the item has a definite used flex basis, that's the flex base size", shared with that file.
     - ARM E, the section's "Otherwise, size the item into the available space using its used flex basis in
       place of its main size, treating a value of content as max-content", which is the arm a container whose
       main size is DETERMINED takes.
   That file takes ARM C instead — "If the used flex basis is content or depends on its available space, and
   the flex container is being sized under a min-content or max-content constraint" — because it runs under
   exactly that constraint. THE TWO DIVERGE AT THE PERCENTAGE AND NOWHERE ELSE: a percentage `flex-basis` or
   `width` is INDEFINITE in an intrinsic pass, where the basis is the number being produced, and DEFINITE here,
   where `inner_main` is already determined. So this is §9.2's other arm and not a second copy of its neighbour,
   and a diff that shares one function between them would answer one of the two passes wrongly.
   `flex-basis: auto` IS ROUTED BY §7.1 "The flex Shorthand" AND NOT BY §7.2.3: §7.1 carries the value list and
   is where the keyword is defined — "When specified on a flex item, the auto keyword retrieves the value of
   the main size property as the used flex-basis. If that value is itself auto, then the used value is
   content." */
static CssPx fl_flex_base_size(lxb_dom_element_t *el, CssPx inner_main, IntrinsicInlineSizes measured)
{
    CssLength basis;
    CssPx v;

    /* §4's ANONYMOUS ITEM: `flex-basis` is `auto` over a `width` of `auto`, which §7.1 makes `content`, which
       §9.2's last arm substitutes with max-content. */
    if (el == NULL) return measured.max_content;
    basis = css_computed_length(el, "flex-basis");
    if (basis.kind == CSS_LENGTH_KEYWORD) {
        if (strcmp(basis.keyword, "content") == 0) return measured.max_content;
        if (strcmp(basis.keyword, "auto") == 0) {
            if (fl_declared_main_px(el, inner_main, "width", "auto", &v)) return v;
            return measured.max_content;
        }
    }
    if (fl_declared_main_px(el, inner_main, "flex-basis", "auto", &v)) return v;
    return measured.max_content;
}

/* §9's ALGORITHM IS ONE ORDERED LIST CONTINUED ACROSS §9.1 TO §9.6, SO ITS STEP NUMBERS ARE GLOBAL AND NOT
   PER-SECTION, and both numbers above and below are stated in the standard's own numbering rather than in a
   count of the items under one heading. It is worth writing down because the local count is the intuitive one
   and this file had it: the two sentences that now read STEP 3 read STEP 2, which is the second item under
   §9.2 and the THIRD step of §9 "Flex Layout Algorithm". The standard settles it twice over — §9.1's list
   opens `<ol start="1">` and every later section's continues it, and §9.8 "Definite and Indefinite Sizes"
   points at the fifth item under §9.4 "Cross Size Determination" as "see step 11", which only the global
   reading makes true. Every other step number in this component was already global (§9.3's step 5 is the
   FIRST item under that heading), so the two that were not were an isolated slip and not a convention.
   RETIREMENT: this note goes when a citation channel in this tree reads §9's continued list, at which point a
   per-section count can no longer be written without something saying so.
   §9.2's STEP 3 CLOSING SENTENCE — "The hypothetical main size is the item's flex base size clamped according
   to its used min and max main sizes (and flooring the content box size at zero)" — in CSS 2.1 §10.4 "Minimum
   and maximum widths: 'min-width' and 'max-width'"' order, the maximum capping first and the minimum flooring
   last, so a `min-width` above a `max-width` wins.
   THE FLEX BASE SIZE ITSELF IS NOT CLAMPED and the section says so in the paragraph directly above: "When
   determining the flex base size, the item's min and max main sizes are ignored (no clamping occurs)." The two
   numbers are therefore both kept on `FlItem`, and §9.7 reads each of them where it names it. */
static CssPx fl_clamp_main(const FlItem *it, CssPx v)
{
    if (it->has_max) v = css_px_min(v, it->max_main);
    v = css_px_max(v, it->min_main);
    return css_px_max(v, css_px(0.0));
}

/* ONE ITEM'S OPERANDS, READ ONCE. §9.7 reads each of them several times and a re-read between two of its steps
   would be a second measurement of a document the first one could not have changed, so the whole of §9.2's
   step 2 happens here and the loop below reads nothing but this struct. */
static void fl_fill(FlItem *it, CssPx inner_main, IntrinsicInlineSizes measured)
{
    CssPx v;
    char nbuf[160];

    if (it->el != NULL) {
        ReplacedElement rep = replaced_element_of(it->el);

        /* css-flexbox-1 §9.2's ARM B AND §4.5's TRANSFERRED SIZE SUGGESTION ARE THE SAME ABSENT CAPABILITY and
           are refused together, because both of them turn on a PREFERRED ASPECT RATIO with a DEFINITE cross
           size and neither has anything to say without one. §9.2's arm B is "If the flex item has … a
           preferred aspect ratio, a used flex basis of content, and a definite cross size, then the flex base
           size is calculated from its used cross size and the flex item's aspect ratio", and §4.5's is "If the
           item has a preferred aspect ratio and its preferred cross size is definite, then the transferred
           size suggestion is that size (clamped by its minimum and maximum cross sizes if they are definite),
           converted through the aspect ratio."
           A REPLACED ITEM WITH NO DEFINITE CROSS SIZE IS NOT REFUSED, and that is the reading and not a
           leniency: both sentences are conditioned on the cross size being definite, so with `height` at its
           initial `auto` the transferred size suggestion is undefined by its own definition and §4.5's
           replaced arm — "Use the smaller of the content size suggestion and the transferred size suggestion
           (if one exists)" — is a smaller-of-ONE, which is the same number the non-replaced arm's larger-of-one
           gives. The two arms coincide exactly where the ratio has nothing to transfer.
           WHAT TO BUILD is §9.2's arm B over
           css-images-3 §4.1 "Object-Sizing Terminology"'s ratio, which core/layout/replaced_element.h already
           answers, together with the CROSS-axis definiteness test §9.4 "Cross Size Determination" owns.
           THAT SECOND HALF NOW HAS A COMPONENT AND THE SENTENCE THAT STOOD HERE SENT ITS READER TO THE WRONG
           FILE: it named core/layout/block_flow.c as naming the same pair, and block_flow.c CALLS
           core/layout/flex_cross_size.h for a `row` container's own auto cross size rather than refusing it.
           What is absent there, and is this arm's other half, is §9.4's step 11 — the used cross size of one
           ITEM, which is what "a definite cross size" would have to be read from. */
        if (rep.replaced && rep.has_ratio && !fl_computed_is(it->el, "height", "auto"))
            DFAILF("%s: this REPLACED flex item has a preferred aspect ratio and a declared cross size, so "
                   "css-flexbox-1 §9.2 \"Line Length Determination\"' arm B is its flex base size and §4.5 "
                   "\"Automatic Minimum Size of Flex Items\"' TRANSFERRED SIZE SUGGESTION is part of its "
                   "automatic minimum — and neither is built. Both are stated over the same two facts: \"If "
                   "the flex item has … a preferred aspect ratio, a used flex basis of content, and a definite "
                   "cross size, then the flex base size is calculated from its used cross size and the flex "
                   "item's aspect ratio\", and \"If the item has a preferred aspect ratio and its preferred "
                   "cross size is definite, then the transferred size suggestion is that size (clamped by its "
                   "minimum and maximum cross sizes if they are definite), converted through the aspect "
                   "ratio\". THE RATIO IS NOT WHAT IS MISSING — core/layout/replaced_element.h answers "
                   "css-images-3 §4.1 \"Object-Sizing Terminology\"'s natural aspect ratio for this element "
                   "already. WHAT IS MISSING IS NOT THE USED CROSS SIZE EITHER, WHICH IS THE THIRD REMEDY "
                   "THIS ARM HAS HAD AND THE SECOND TO BE RETIRED — each was correct when written and each is "
                   "left here because a reader re-derives them in the same order. The first said to build "
                   "§9.4 whole; its steps 7 and 8 are built (core/layout/flex_cross_size.h) and "
                   "core/layout/block_flow.c CALLS them. The second said to build §9.4's STEP 11, and that is "
                   "built too (`flex_cross_size_used_item_cross`). WHAT BOTH SENTENCES ASKED FOR IS A USED "
                   "SIZE AND WHAT THESE TWO RULES ASK FOR IS A DEFINITE ONE, which is a different question "
                   "that css-flexbox-1 §9.8 \"Definite and Indefinite Sizes\" owns: step 11 answers for an "
                   "INDEFINITE cross size as readily as for a definite one — its second arm is the item's "
                   "hypothetical cross size — so calling it here would turn this arm's own `definite cross "
                   "size` condition into a test that is always true and would run §9.2's arm B on every "
                   "replaced item with a "
                   "ratio. BUILD §9.8's DEFINITENESS TEST FOR THE CROSS AXIS, whose own sentences are the "
                   "algorithm (\"If a single-line flex container has a definite cross size, the automatic "
                   "preferred outer cross size of any stretched flex items is the flex container's inner "
                   "cross size … and is considered definite\"), and then both rules become a call",
                   box_subject(it->el, nbuf, sizeof nbuf));
        it->grow = flex_item_flexibility_factor(it->el, "flex-grow");
        it->shrink = flex_item_flexibility_factor(it->el, "flex-shrink");
    } else {
        /* §4's ANONYMOUS ITEM again: css-flexbox-1 §7.2.1 "The flex-grow property" and §7.2.2 "The flex-shrink
           property" give `Initial:` values of `0` and `1`, and §4 leaves that box nothing that could override
           them. */
        it->grow = 0.0;
        it->shrink = 1.0;
    }
    it->surround = css_px_add(fl_main_border_padding(it->el), fl_main_margins(it->el));
    it->base = fl_flex_base_size(it->el, inner_main, measured);
    it->has_max = fl_declared_main_px(it->el, inner_main, "max-width", "none", &v);
    it->max_main = it->has_max ? v : css_px(0.0);
    /* css-sizing-3 §3.1.2 "Minimum Size Properties: the min-width and min-height properties" gives `min-width`
       the initial `auto`, which css-sizing-3 §3.2 hands to
       css-flexbox-1 §4.5 for this box — so the FALSE arm of the read below is that keyword and never an
       absence. */
    if (!fl_declared_main_px(it->el, inner_main, "min-width", "auto", &v))
        v = fl_automatic_minimum_main(it->el, inner_main, measured);
    it->min_main = v;
    it->hypothetical = fl_clamp_main(it, it->base);
    it->target = it->base;
    it->frozen = false;
}

/* THE INLINE DIMENSION, REQUIRED RATHER THAN MAPPED. Every edge this component reads is PHYSICAL —
   `margin-left`, `padding-right`, the left and right border widths — and every size it reads is `width`,
   `min-width` or `max-width`, which css-writing-modes-4 §7.2 "Dimensional Mapping" pins to the physical width
   in every writing mode ("The height properties (height, min-height, and max-height) refer to the physical
   height, and the width properties (width, min-width, and max-width) refer to the physical width"). §5.1
   "Flex Flow Direction: the flex-direction property" states the main axis in FLOW-RELATIVE terms, so the two
   coincide only where the inline dimension is the horizontal one.
   THIS BANNER NAMED css-writing-modes-4 §7.4 "Flow-Relative Mappings" AS THE ABSENT CAPABILITY AND THAT WAS
   MIS-AIMED, which is written out rather than quietly corrected because the mis-aim is invisible: §7.4 is
   real, its title is exactly as this file wrote it, and it is about the algorithms this file states. It is
   not a mapping table — it is the rule deciding WHOSE writing mode a flow-relative question is read against
   ("Flow-relative directions are calculated with respect to the writing mode of the containing block of the
   box"). The TABLE is §6.4 "Abstract-to-Physical Mappings" and core/css/css_logical.c has held it all along;
   its dimension rows are exported as `css_logical_axis_is_vertical` and composed with §5.1 by
   `flex_container_axis_is_vertical` (core/layout/flex_item.h), which core/layout/used_value.c routes on.
   SO WHAT THIS COMPONENT IS WAITING ON IS NOT A MAPPING. css-writing-modes-4 §7.2 says "the rules used to
   calculate box dimensions and positions are logical" and then names which rules those are: "the calculation
   rules in CSS2.1 Section 10.3 are used for the inline dimension measurements", and "the calculation rules in
   CSS2.1 Section 10.6 are used in the block dimension". A `column` container's main axis IS the block
   dimension, so §9.2 "Line Length Determination"'
   step 3 would have to size its items with a BLOCK-axis automatic size where `fl_fill` passes an
   `intrinsic_inline_sizes`, and §4.5's content-based minimum would have to be the block-axis one. BUILD THAT
   MEASUREMENT — css-sizing-3 §3.2 sends both intrinsic keywords in the block axis to the automatic size,
   which CSS 2.1 §10.6.3 is and which `used_value_block_level_content_px` already computes — and this
   component becomes an axis parameter rather than a refusal.
   IT IS ASKED OF THE ITEM AS WELL AS OF THE CONTAINER, and that is not redundancy: the two are different
   elements with different cascades, and an item in a perpendicular mode is css-writing-modes-4 §7.3
   "Orthogonal Flows"' box whose own inline size lies along the CONTAINER's block axis — so summing it would
   report one dimension of a box as the other, which is the refusal core/layout/flex_intrinsic_size.c makes at
   the same boundary for the same operand. */
static void fl_require_horizontal_tb(lxb_dom_element_t *el, const char *what)
{
    char nbuf[160], wbuf[64];

    if (fl_computed_is(el, "writing-mode", "horizontal-tb")) return;
    DFAILF("%s, computed `writing-mode` `%s`: this %s's inline axis is not the horizontal one, so the "
           "css-flexbox-1 §5.1 \"Flex Flow Direction: the flex-direction property\" mapping that made this "
           "container's MAIN axis its inline axis does not make it the axis `margin-left`, `padding-left` and "
           "the left border width lie along — and every operand of §9.7 \"Resolving Flexible Lengths\" below "
           "is one of those. THE REMEDY THAT STOOD HERE SAID TO BUILD css-writing-modes-4 §7.4 "
           "\"Flow-Relative Mappings\" AND THE CITATION WAS MIS-AIMED: §7.4 is the rule for WHOSE writing "
           "mode a flow-relative question is read against, the TABLE is §6.4 \"Abstract-to-Physical "
           "Mappings\", and core/css/css_logical.c holds it with its dimension rows exported as "
           "`css_logical_axis_is_vertical`. WHAT IS ABSENT IS THE BLOCK-DIMENSION READING OF §9.2 \"Line "
           "Length Determination\"' step 3 and §4.5 \"Automatic Minimum Size of Flex Items\": "
           "css-writing-modes-4 §7.2 \"Dimensional Mapping\" puts CSS 2.1 §10.3's rules on the inline "
           "size and §10.6's on the block size, and every measurement below is §10.3's. BUILD the block "
           "one and this becomes an axis parameter",
           box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "writing-mode", wbuf, sizeof wbuf),
           what);
}

/* §9.3's STEP 5 — "Collect flex items into flex lines" — for the ONE arm that needs no line breaking: "If the
   flex container is single-line, collect all the flex items into a single flex line." §6 "Flex Lines" is where
   `flex-wrap: nowrap` and single-line are made the same thing, and core/layout/flex_item.h answers which this
   container is.
   THE ITEM LIST IS §4 "Flex Items"' AND IS ENUMERATED THE SAME WAY core/layout/flex_intrinsic_size.c
   enumerates it, through the same two entries, because it is the same list: §4's own "Each in-flow child of a
   flex container becomes a flex item" with its child text sequences wrapped in one anonymous box each. `out`
   may be NULL, which counts without filling — the two passes are one walk written twice rather than a list
   built and then measured, because §4's classification of a TEXT node walks the whole sequence it is in and a
   half-built list would have to carry that delimiting decision across. */
static size_t fl_collect(lxb_dom_element_t *container, CssPx inner_main, FlItem *out)
{
    lxb_dom_node_t *c = lxb_dom_interface_node(container)->first_child;
    size_t n = 0;
    char nbuf[160];

    while (c != NULL) {
        lxb_dom_node_t *next = c->next;
        FlexItemChildKind kind = flex_item_child_kind(container, c);
        FlItem it;

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        memset(&it, 0, sizeof it);
        switch (kind) {
        case FLEX_ITEM_CHILD_TEXT: {
            lxb_dom_node_t *end = flex_item_text_sequence_end(container, c);
            BlockFlowRun seq;

            seq.after = c->prev;
            seq.end = end;
            next = end;
            it.el = NULL;
            it.first = c;
            if (out != NULL) fl_fill(&it, inner_main, intrinsic_inline_run_sizes(container, seq));
            break;
        }
        case FLEX_ITEM_CHILD_ELEMENT:
            next = c->next;
            it.el = lxb_dom_interface_element(c);
            it.first = c;
            /* css-flexbox-1 §4.4 "Collapsed Items" gives this item a SECOND layout round and this component
               runs the first one only, so its answer for the container would be the round a browser discards.
               §9.4 "Cross Size Determination"' step 10 is the sentence: "If any flex items have visibility:
               collapse, note the cross size of the line they're in as the item's strut size, and restart
               layout from the beginning. In this second layout round, when collecting items into lines, treat
               the collapsed items as having zero main size." BUILD §9.4's STEP 10, which is what supplies
               the strut size the second round needs. THE CLAUSE THAT STOOD HERE SAID TO BUILD §9.4 AND NAMED
               core/layout/block_flow.c AS THE SAME ABSENCE, and both halves have moved: §9.4's steps 7 and 8
               are built (core/layout/flex_cross_size.h), block_flow.c CALLS them, and that component refuses
               a collapsed item at its own walk for the OTHER half of this same sentence — the strut its step
               8 owes the line. */
            if (flex_item_is_collapsed(it.el))
                DFAILF("%s: this flex item's computed `visibility` is `collapse`, so css-flexbox-1 §4.4 "
                       "\"Collapsed Items\" makes it a COLLAPSED FLEX ITEM and §9.4 \"Cross Size "
                       "Determination\"' step 10 restarts the whole of "
                       "css-flexbox-1 §9 \"Flex Layout Algorithm\" for its container with every "
                       "collapsed item at ZERO main size. This component runs §9.3 \"Main Size "
                       "Determination\" once, so the used main size it would answer is the FIRST round's — "
                       "the one a browser throws away — and every other item on the line would be flexed "
                       "against free space this item is still occupying. BUILD §9.4's STEP 10, WHICH OWNS "
                       "BOTH ROUNDS — its step 8 is built (core/layout/flex_cross_size.h) and supplies "
                       "the STRUT SIZE the second one carries, and refuses this same item at its own walk "
                       "for that reason, so what is absent is the two ROUNDS and neither measurement",
                       box_subject(it.el, nbuf, sizeof nbuf));
            if (out != NULL) {
                fl_require_horizontal_tb(it.el, "FLEX ITEM");
                fl_fill(&it, inner_main, intrinsic_inline_sizes(it.el));
            }
            break;
        case FLEX_ITEM_CHILD_NONE:
            /* UNREACHABLE BY THE GUARD ABOVE, and asserted rather than left to fall through into the tail:
               the tail COUNTS a member and the zeroed record it would count is an item with no size, no
               factor and no edges, which §9.7 would then give a share of the free space. The arm exists
               because `-Wswitch` is the forcing function this walk relies on. */
            DFAIL("css-flexbox-1 §4's classification answered FLEX_ITEM_CHILD_NONE for a child this walk had "
                  "already decided to collect. The loop skips that value before it reaches this switch, so "
                  "two readings of one child list disagreed — which means the child list changed under the "
                  "walk");
            break;
        /* NO `default` ARM, DELIBERATELY, for core/layout/flex_intrinsic_size.c's reason: `-Wswitch` is the
           forcing function, and a fourth `FlexItemChildKind` is a fourth kind of member of every flex
           container's item list — §9.7's sums are over a LINE, so a member this walk cannot classify is one it
           would silently drop out of every free-space total on that line. */
        }
        DCHECK(next != c,
               "css-flexbox-1 §4's item walk did not advance past the child it had just collected, so this "
               "walk would put the same flex item on the line for ever. A text sequence always contains at "
               "least the node it was delimited from, and an element item always advances by one sibling");
        if (out != NULL) out[n] = it;
        n++;
        c = next;
    }
    return n;
}

/* §9.7's STEP 3 AND ITS "as for initial free space, above" — "Sum the outer sizes of all items on the line, and
   subtract this from the flex container's inner main size. For frozen items, use their outer target main size;
   for other items, use their outer flex base size." ONE function because the section states the second as the
   first: a remaining free space computed any other way would be a second reading of a sentence that has one. */
static CssPx fl_free_space(const FlItem *items, size_t n, CssPx inner_main)
{
    CssPx used = css_px(0.0);
    size_t i;

    for (i = 0; i < n; i++)
        used = css_px_add(used, css_px_add(items[i].frozen ? items[i].target : items[i].base,
                                           items[i].surround));
    return css_px_sub(inner_main, used);
}

/* css-flexbox-1 §9.7 "Resolving Flexible Lengths" over ONE LINE, ending on its own last sentence — "Set each
   item's used main size to its target main size", which is `target` on return.
   THE LOOP IS BOUNDED BY THE SECTION AND NEVER BY A COUNT, which is why the assert below is the shape it is:
   §9.7's own note says "This freezes at least one item, ensuring that the loop makes progress and eventually
   terminates", and that sentence is a CLAIM about the three arms of its freeze step rather than a promise the
   caller has to take on trust. A total violation of zero freezes every item; a positive one freezes every MIN
   violation, of which a positive sum guarantees at least one; a negative one freezes every MAX violation, for
   the mirror reason. So the assert is the section's own note made falsifiable, and an iteration cap in its
   place would be the bound §NO BOUNDS forbids wearing the costume of a safety net.
   THE USED FLEX FACTOR IS CHOSEN ONCE, FOR THE WHOLE LINE, which is step 1 and not a per-item question: "Sum
   the outer hypothetical main sizes of all items on the line. If the sum is less than the flex container's
   inner main size, use the flex grow factor for the rest of this algorithm; otherwise, use the flex shrink
   factor." An implementation that re-asked it per pass would flip direction half way through a line whose
   items had been clamped. */
static void fl_resolve(FlItem *items, size_t n, CssPx inner_main)
{
    CssPx sum_hypothetical = css_px(0.0), initial_free;
    bool grow;
    size_t i;

    for (i = 0; i < n; i++)
        sum_hypothetical = css_px_add(sum_hypothetical,
                                      css_px_add(items[i].hypothetical, items[i].surround));
    grow = sum_hypothetical.px < inner_main.px;

    /* STEP 2 — "Each item in the flex line has a target main size, initially set to its flex base size. Each
       item is initially unfrozen" (done in `fl_fill`), then SIZE INFLEXIBLE ITEMS: "Freeze, setting its target
       main size to its hypothetical main size… any item that has a flex factor of zero; if using the flex grow
       factor: any item that has a flex base size greater than its hypothetical main size; if using the flex
       shrink factor: any item that has a flex base size smaller than its hypothetical main size."
       THE FIRST CONDITION IS THE *USED* FACTOR'S AND NOT BOTH FACTORS', which is the one place a careless
       reading gives a different line: "a flex factor of zero" is the factor step 1 just chose, so a
       `flex: 0 1 auto` item is inflexible on a GROWING line and fully shrinkable on a shrinking one. */
    for (i = 0; i < n; i++) {
        double factor = grow ? items[i].grow : items[i].shrink;
        bool inflexible = factor == 0.0 ||
                          (grow && items[i].base.px > items[i].hypothetical.px) ||
                          (!grow && items[i].base.px < items[i].hypothetical.px);

        if (inflexible) {
            items[i].target = items[i].hypothetical;
            items[i].frozen = true;
        }
    }

    /* STEP 3 — "Calculate initial free space." It is computed ONCE and kept, because step 4's own
       less-than-one arm multiplies THE INITIAL free space and not the remaining one. */
    initial_free = fl_free_space(items, n, inner_main);

    for (;;) {
        CssPx remaining;
        double sum_factors = 0.0, denom = 0.0, total_violation = 0.0;
        size_t unfrozen = 0, froze = 0;

        /* STEP 4a — "Check for flexible items. If all the flex items on the line are frozen, exit this loop." */
        for (i = 0; i < n; i++)
            if (!items[i].frozen) { unfrozen++; sum_factors += grow ? items[i].grow : items[i].shrink; }
        if (unfrozen == 0) break;

        /* STEP 4b — "Calculate the remaining free space as for initial free space, above. If the sum of the
           unfrozen flex items' flex factors is less than one, multiply the initial free space by this sum. If
           the magnitude of this value is less than the magnitude of the remaining free space, use this as the
           remaining free space."
           BOTH HALVES ARE CONDITIONS AND THE SECOND IS NOT A CLAMP: a MAGNITUDE comparison keeps the sign of
           the scaled value, so a line whose factors sum to 0.8 distributes 0.8 of the initial free space and
           leaves the rest unused, which is exactly what WPT's max-width-violation.html expects of its
           `flex: 0.8 0 0` column (520px of an 800px container, and 640px of the same container in its second
           part where an absolutely positioned sibling is not a flex item at all). */
        remaining = fl_free_space(items, n, inner_main);
        if (sum_factors < 1.0) {
            CssPx scaled = css_px_scale(initial_free, sum_factors);

            if (fl_magnitude(scaled.px) < fl_magnitude(remaining.px)) remaining = scaled;
        }

        /* STEP 4c — "If the remaining free space is non-zero, distribute it proportional to the flex factors."
           THE DENOMINATOR IS OVER THE UNFROZEN ITEMS ONLY, in both arms, and it is the SCALED factor in the
           shrink arm: "multiply its flex shrink factor by its inner flex base size, and note this as its
           scaled flex shrink factor." The word INNER is load-bearing and is why `base` is the content box
           throughout this file — a scaled factor taken over an outer size would give an item with padding a
           larger share of the shrinkage than the section allows. */
        if (remaining.px != 0.0) {
            for (i = 0; i < n; i++) {
                if (items[i].frozen) continue;
                denom += grow ? items[i].grow : (items[i].shrink * items[i].base.px);
            }
            if (denom != 0.0) {
                for (i = 0; i < n; i++) {
                    double share;

                    if (items[i].frozen) continue;
                    if (grow) {
                        share = items[i].grow / denom;
                        items[i].target = css_px_add(items[i].base, css_px_scale(remaining, share));
                    } else {
                        share = (items[i].shrink * items[i].base.px) / denom;
                        /* "Set the item's target main size to its flex base size minus a fraction of the
                           ABSOLUTE VALUE of the remaining free space proportional to the ratio. Note this may
                           result in a negative inner main size; it will be corrected in the next step." */
                        items[i].target = css_px_sub(items[i].base,
                                                     css_px_scale(remaining,
                                                                  remaining.px < 0.0 ? -share : share));
                    }
                }
            }
        }

        /* STEP 4d — "Fix min/max violations. Clamp each non-frozen item's target main size by its used min and
           max main sizes and floor its content-box size at zero. If the item's target main size was made
           smaller by this, it's a max violation. If the item's target main size was made larger by this, it's
           a min violation." THE CLAMPED VALUE IS KEPT, which is the step that decides WPT's
           total-min-max-violation-zero.html: two items clamped in opposite directions sum to a total violation
           of zero, which freezes every item — and the sizes they freeze AT are the clamped ones, so that
           document's own assertion is "the min/max constraints still need to apply. So the final main sizes
           are 250px and 50px". An implementation that restored the unclamped value on a zero total would
           answer 150px for both. */
        for (i = 0; i < n; i++) {
            CssPx clamped;

            if (items[i].frozen) continue;
            clamped = fl_clamp_main(&items[i], items[i].target);
            items[i].violation = clamped.px - items[i].target.px;
            total_violation += items[i].violation;
            items[i].target = clamped;
        }

        /* STEP 4e — "Freeze over-flexed items. The total violation is the sum of the adjustments from the
           previous step ∑(clamped size - unclamped size). If the total violation is: Zero — Freeze all items.
           Positive — Freeze all the items with min violations. Negative — Freeze all the items with max
           violations."
           THE PER-ITEM ADJUSTMENT IS THE SECTION'S OWN QUANTITY and is carried rather than re-derived: step 4d
           names it ("the adjustments from the previous step") and step 4e both SUMS it and reads its SIGN, so
           one value answers both. Re-deriving the sign here from a second comparison against the limits would
           be a second spelling of a number this loop already computed, free to disagree with the total it was
           summed into. */
        for (i = 0; i < n; i++) {
            bool freeze;

            if (items[i].frozen) continue;
            if (total_violation == 0.0) freeze = true;
            else if (total_violation > 0.0) freeze = items[i].violation > 0.0;   /* a MIN violation */
            else freeze = items[i].violation < 0.0;                              /* a MAX violation */
            if (freeze) { items[i].frozen = true; froze++; }
        }
        DCHECK(froze > 0,
               "css-flexbox-1 §9.7 \"Resolving Flexible Lengths\"' freeze step froze NO item, so this loop "
               "would run for ever over a line it can never finish. The section's own note is the invariant "
               "this asserts — \"This freezes at least one item, ensuring that the loop makes progress and "
               "eventually terminates\" — and it holds because its three arms are exhaustive over the sign of "
               "the total violation: zero freezes every item, a POSITIVE total is a sum of adjustments of "
               "which at least one must be positive (a min violation), and a NEGATIVE one the mirror. A pass "
               "that freezes nothing therefore means the violation this loop measured and the violation this "
               "step tested for were computed from different values");
        if (froze == 0) break;
    }
}

CssPx flex_line_used_main_size(lxb_dom_element_t *container, lxb_dom_node_t *item)
{
    FlItem *items;
    size_t n, i;
    CssPx inner_main, out;
    char nbuf[160];
    bool found = false;

    DCHECK(container != NULL && item != NULL,
           "css-flexbox-1 §9.3 \"Main Size Determination\"'s used main size was asked for with no container "
           "or no item");
    DCHECK(item->parent == lxb_dom_interface_node(container),
           "css-flexbox-1 §9.7 \"Resolving Flexible Lengths\"' used main size was asked for an item that is "
           "not a child of the container it was asked about. §9.7's every sum is over \"all items on the "
           "line\", so a subject drawn from one container and a free space drawn from another is an item "
           "flexed against a rectangle it is not in");
    /* §5.1's mapping, asked FIRST and over the whole container, for core/layout/flex_intrinsic_size.c's
       reason: §9.3 and §9.4 "Cross Size Determination" share no step, so which of the two owns a PHYSICAL
       axis is a fact about this container's `flex-direction` and never a case discovered part way through. */
    if (flex_container_main_axis(container) != FLEX_MAIN_AXIS_INLINE)
        DFAILF("%s: this FLEX CONTAINER's main axis is its BLOCK axis (css-flexbox-1 §5.1 \"Flex Flow "
               "Direction: the flex-direction property\": a `column` container's main axis \"has the same "
               "orientation as the block axis of the current writing mode\"), so the INLINE size this entry "
               "was reached for is one of its items' CROSS sizes and §9.3 \"Main Size Determination\" does "
               "not answer it. §9.4 \"Cross Size Determination\"' step 11 does — \"Determine the used cross "
               "size of each flex item. If a flex item's cross size depends on the available space in the "
               "cross axis, recalculate its cross size using the flex line's cross size (rather than the flex "
               "container's) as the available space. Otherwise, the used cross size is the item's "
               "hypothetical cross size.\" THE TWO REMEDIES THAT STOOD HERE ARE BOTH RETIRED AND ARE "
               "WRITTEN OUT BECAUSE EACH IS A READING A READER RE-DERIVES: the first said to build §9.4 and "
               "named core/layout/block_flow.c as the same absence, and that file now CALLS "
               "core/layout/flex_cross_size.h for a `row` container's own auto cross size; the second said to "
               "build §9.4's STEP 11, and step 11 is built (`flex_cross_size_used_item_cross`). NEITHER "
               "REACHES THIS BOX, and the reason is the one this refusal opens with: both of those components "
               "refuse a `column` container at their own entry, because every physical edge they add is "
               "chosen by §5.1's mapping and css-writing-modes-4 §3.2 \"Block Flow Direction: the "
               "writing-mode property\". A THIRD REMEDY IS RETIRED HERE AND IS WRITTEN OUT WITH THE OTHER "
               "TWO: it said to BUILD css-writing-modes-4 §7.4 \"Flow-Relative Mappings\", naming this "
               "component, core/layout/flex_cross_size.c and core/layout/block_flow.c as waiting on one "
               "absent capability. §7.4 is not a mapping table — it decides WHOSE writing mode a "
               "flow-relative question is read against — and the table, §6.4 \"Abstract-to-Physical "
               "Mappings\", is in core/css/css_logical.c with its dimension rows now exported. "
               "core/layout/used_value.c routes on that mapping today, which is why a `column` container's "
               "item asked for its DECLARED cross size no longer reaches any of these three. WHAT IS LEFT "
               "for this component is the BLOCK-DIMENSION measurement css-writing-modes-4 §7.2 "
               "\"Dimensional Mapping\" names — CSS 2.1 §10.6's rules where every operand below is "
               "§10.3's — and for core/layout/flex_cross_size.c the INLINE one",
               box_subject(container, nbuf, sizeof nbuf));
    fl_require_horizontal_tb(container, "FLEX CONTAINER");
    /* §5.2 "Flex Line Wrapping: the flex-wrap property"' other arm. §9.3's step 5 has a SECOND sentence this
       component does not run — "Otherwise, starting from the first uncollected item, collect consecutive
       items one by one until the first time that the next collected item would not fit into the flex
       container's inner main size" — and it is a real algorithm rather than a special case: it reads each
       item's OUTER HYPOTHETICAL MAIN SIZE, which this file computes, and §10 "Fragmenting Flex Layout"'s
       forced breaks, which nothing here models. */
    if (flex_container_is_multi_line(container))
        DFAILF("%s: this FLEX CONTAINER is MULTI-LINE (css-flexbox-1 §5.2 \"Flex Line Wrapping: the flex-wrap "
               "property\"; §6 \"Flex Lines\" makes `wrap` and `wrap-reverse` one case against `nowrap`), "
               "so css-flexbox-1 §9.3 \"Main Size Determination\"' step 5 must BREAK its items into lines "
               "before §9.7 \"Resolving Flexible Lengths\" can run over any of them — and every free space "
               "§9.7 distributes is per LINE, so answering from one line holding every item would flex each "
               "of them against space the other lines are using. §9.3's own sentence is the algorithm: "
               "\"Otherwise, starting from the first uncollected item, collect consecutive items one by one "
               "until the first time that the next collected item would not fit into the flex container's "
               "inner main size (or until a forced break is encountered, see § 10 Fragmenting Flex Layout). "
               "If the very first uncollected item wouldn't fit, collect just it into the line. For this "
               "step, the size of a flex item is its outer hypothetical main size.\" THE SIZE IT BREAKS ON "
               "IS ALREADY COMPUTED HERE — `fl_fill` produces exactly that outer hypothetical main size for "
               "every item — so what is missing is the BREAKING and §10's forced break, not the measurement",
               box_subject(container, nbuf, sizeof nbuf));

    /* §9.2 "Line Length Determination"' STEP 1 for the MAIN axis, taking its FIRST arm: "if that dimension of
       the flex container's content box is a definite size, use that". A `row` container's main size is its
       inline size, which CSS 2.1 §10.3.3's constraint equation determines for a block-level box and which
       core/layout/used_value.h answers — so this is the arm and not the third one, whose "space available to
       the flex container" this component would have no way to read.
       IT IS THE CONTENT BOX AND `used_value_content_px` IS WHY THAT IS NOT ASSUMED: css-sizing-3 §3.3 "Box
       Edges for Sizing: the box-sizing property" makes the used value `used_value_px` exposes the BORDER
       box's under `border-box`, and §9.2 says "content box" in its own words. */
    inner_main = used_value_content_px(container, false);

    n = fl_collect(container, inner_main, NULL);
    DCHECK(n > 0,
           "css-flexbox-1 §9.7 \"Resolving Flexible Lengths\" was asked for an item of a container whose §4 "
           "\"Flex Items\" walk found NO item at all, and the subject of the ask is one of them. §6 \"Flex "
           "Lines\" is what makes that impossible — \"Every line contains at least one flex item, unless the "
           "flex container itself is completely empty\" — so two readings of one child list have disagreed, "
           "which means the list changed under the walk");
    /* THE RELEASE ARM OF THAT ASSERT, stated rather than left to `calloc(0, …)`: a zero-length allocation may
       answer NULL, and the always-fatal CHECK below would then report the physical RAM floor for a state that
       is a disagreement between two walks. Zero is visibly not a used main size, which is the direction
       core/layout/flex_intrinsic_size.c chooses for the same reason. */
    if (n == 0) return css_px(0.0);
    items = (FlItem *)calloc(n, sizeof *items);
    CHECK(items != NULL,
          "allocating css-flexbox-1 §6 \"Flex Lines\"' line for §9.7 \"Resolving Flexible Lengths\" failed. "
          "The line is one record per flex item of ONE container and it is freed before this entry returns, so "
          "this is the physical RAM floor and not a leak: §9.7's sums are over every item on the line at once "
          "and there is no smaller unit of it to resolve");
    (void)fl_collect(container, inner_main, items);
    fl_resolve(items, n, inner_main);

    out = css_px(0.0);
    for (i = 0; i < n; i++)
        if (items[i].first == item) { out = items[i].target; found = true; break; }
    free(items);
    DCHECKF(found,
            "%s: css-flexbox-1 §4 \"Flex Items\"' walk over this item's own flex container did not produce "
            "the item the used main size was asked for. Every caller establishes that it IS one before asking "
            "— core/layout/used_value.c classifies the box as a flex item from its box parent's `display`, and "
            "core/layout/flex_cross_size.c reaches §4's anonymous item through the same "
            "`flex_item_child_kind` walk this one runs — so the two readings of one child list have "
            "disagreed, and the number this entry would otherwise return is another item's. THE SUBJECT IS A "
            "NODE AND NOT AN ELEMENT for the reason §4 gives: its anonymous block container flex item is \"the "
            "one box a flex container's item list holds that is not an element\", so a TEXT node here is the "
            "FIRST node of a child text sequence and an ELEMENT node is an item in its own right",
            box_subject_node(item, nbuf, sizeof nbuf));
    return out;
}
