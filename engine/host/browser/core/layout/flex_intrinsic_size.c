/* css-flexbox-1 §9.9 "Intrinsic Sizes" over a flex container's own items. See flex_intrinsic_size.h for why
   §5.1 decides which of §9.9's two sections answers an INLINE size, and for why neither of the two arms
   reachable through this entry needs a flex line. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_intrinsic_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/intrinsic_block_size.h"
#include "core/layout/intrinsic_size.h"

/* css-writing-modes-4 §7.3 "Orthogonal Flows"' PERPENDICULAR CASE, REFUSED. §7.3 states the alternative in two
   bullets — "The two writing modes are parallel to each other" against "The two writing modes are
   perpendicular to each other" — and names the second: "When a box has a writing mode that is perpendicular to
   its containing block it is said to be in, or establish, an orthogonal flow."
   IT IS THIS COMPONENT'S QUESTION BECAUSE EVERY OPERAND IT SUMS IS AN *INLINE* SIZE. §9.9's two sections are
   stated in the container's MAIN and CROSS axes, and this entry turns them into an inline size through §5.1's
   mapping — which is a fact about the CONTAINER's writing mode. An item's own intrinsic inline size is a fact
   about the ITEM's, so where the two modes are perpendicular the number core/layout/intrinsic_size.h returns
   for the item lies along the container's BLOCK axis and is the wrong operand entirely — not a narrower
   answer, a measurement of the other dimension.
   EQUAL COMPUTED VALUES IS A SUFFICIENT TEST AND NOT THE SECTION'S OWN, and that is deliberate: §7.3's
   "parallel" holds for `vertical-rl` against `vertical-lr` as well, so this refuses a pair that is in fact
   parallel. A refusal is the safe direction — it crashes naming what to build instead of measuring the wrong
   axis — and widening it to §6.2 "Flow-relative Directions"' real parallelism test is part of building
   §9.2 "Line Length Determination"'s own orthogonal arm, whose note names this exact document: "This case
   occurs, for example, in an English document (horizontal writing mode) containing a column flex container
   containing a vertical Japanese (vertical writing mode) flex item." */
static void fis_require_parallel(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    char *cw = css_computed_value(container, "writing-mode");
    char *iw = css_computed_value(item, "writing-mode");
    bool same;
    char nbuf[160], mbuf[160];

    DCHECK(cw != NULL && iw != NULL,
           "the cascade produced no computed `writing-mode` — css-writing-modes-4 §3.2 \"Block Flow Direction: "
           "the writing-mode property\" gives it an initial value of `horizontal-tb`, so the last layer of the "
           "cascade always answers");
    same = strcmp(cw, iw) == 0;
    if (!same)
        DFAILF("%s (computed `writing-mode` `%s`) inside %s (`%s`): this FLEX ITEM's writing mode differs from "
               "its flex container's, so css-writing-modes-4 §7.3 \"Orthogonal Flows\" is what decides whether "
               "the item's own intrinsic INLINE size is an operand of css-flexbox-1 §9.9 \"Intrinsic Sizes\" at "
               "all. Where the two are perpendicular it is not: §9.9's sums are along the container's main and "
               "cross axes, and a perpendicular item's inline size lies along the container's BLOCK axis, so "
               "summing it reports one dimension of a box as the other. WHAT TO BUILD IS §7.3's OWN "
               "CLASSIFICATION, not a case here — \"The two writing modes are parallel to each other\" against "
               "\"The two writing modes are perpendicular to each other\", over css-writing-modes-4 §6.2 "
               "\"Flow-relative Directions\"' axes rather than over the keyword — and then §9.2 \"Line Length "
               "Determination\"'s step 3 arm that is written for exactly this box, whose flex base size is "
               "\"the item's max-content main size\" after laying it out \"using the rules for a box in an "
               "orthogonal flow\". core/layout/flow_position.c refuses the same pair one question earlier, for "
               "a box's PLACEMENT rather than its size, and names the same mappings",
               box_subject(item, mbuf, sizeof mbuf), iw, box_subject(container, nbuf, sizeof nbuf), cw);
    free(cw);
    free(iw);
}

/* ONE ITEM'S css-sizing-3 §5.2 "Intrinsic Contributions" OUTER CONTRIBUTION IN THE CONTAINER'S INLINE AXIS —
   which is the CROSS axis here, and that is the whole reason this is not css-flexbox-1 §9.9.3 "Flex Item
   Intrinsic Size Contributions". §9.9.3's title names contributions in general and every sentence in it names
   the MAIN axis — "The main-size min-content contribution of a flex item", "the main-size max-content
   contribution" — and its cap, floor and clamp are all over the flex base size and the "min/max MAIN size". A
   cross-axis contribution has none of that: §9.9.2 "Flex Container Intrinsic Cross Sizes" states its operands
   as the items' plain "min-content contribution/max-content contribution", which is css-sizing-3 §5.2's term
   and is answered by the same entry every block-level child of a block container is answered by. */
static IntrinsicInlineSizes fis_item_cross_contribution(lxb_dom_element_t *container, lxb_dom_node_t *child,
                                                        FlexItemChildKind kind, lxb_dom_node_t **next)
{
    /* THE KIND IS THE CALLER'S AND IS NOT RE-ASKED, which is not a saving but a correctness property: §4's
       classification of a TEXT node walks the whole sequence it is in, so asking twice is two readings of one
       child list, and two readings can only ever agree or be a bug nothing reports. One question, one answer,
       carried. */
    switch (kind) {
    case FLEX_ITEM_CHILD_TEXT: {
        /* §4's ANONYMOUS BLOCK CONTAINER FLEX ITEM. Its content is one CSS 2.2 §9.4.2 "Inline formatting
           contexts" over the sequence, which is core/layout/intrinsic_size.h's run entry, and its edges are
           zero because §4 makes the box unstyleable — the NULL that entry reads as an anonymous box. */
        lxb_dom_node_t *end = flex_item_text_sequence_end(container, child);
        /* §4's SEQUENCE IS A SIBLING RANGE AND `BlockFlowRun` IS HOW THAT IS SAID. The pair is read as
           `after`'s next sibling inside `after`'s parent, so `{ child->prev, end }` is exactly the half-open
           `[child, end)` — including when `child` is the container's first child, where a NULL `after` means
           the start of its content and that IS `child`. It cannot be anything else here: §4 blockifies every
           flex item ("if the computed display value of an element's nearest ancestor element (skipping
           display:contents ancestors) is flex or inline-flex, the element's own display value is blockified"),
           so a flex container has no inline box among its children for a run to begin inside — which is the
           whole of why CSS 2.2 §9.2.1.1's runs and these can share one entry. */
        BlockFlowRun seq;
        IntrinsicInlineSizes run;

        seq.after = child->prev;
        seq.end = end;
        run = intrinsic_inline_run_sizes(container, seq);
        *next = end;
        return intrinsic_outer_contribution(NULL, run);
    }
    case FLEX_ITEM_CHILD_ELEMENT: {
        lxb_dom_element_t *item = lxb_dom_interface_element(child);

        fis_require_parallel(container, item);
        *next = child->next;
        return intrinsic_outer_contribution(item, intrinsic_inline_sizes(item));
    }
    case FLEX_ITEM_CHILD_NONE:
        break;
    /* NO `default` ARM, DELIBERATELY: `-Wswitch` is the forcing function here and a default would switch it
       off. A fourth `FlexItemChildKind` is a fourth kind of member in every flex container's item list, and
       §9.9's arms are all over a LIST — a member this walk cannot classify is a member it would silently drop,
       so the day one is added the answer must be a COMPILE failure at every caller and not a crash at one. */
    }
    {
        IntrinsicInlineSizes none;

        DFAIL("css-flexbox-1 §4's classification answered FLEX_ITEM_CHILD_NONE for a child this walk had "
              "already decided to measure. The caller skips that value before it asks for a contribution, so "
              "two readings of one node disagreed — which means the child list changed under the walk");
        /* THE RELEASE FALL-THROUGH CONTRIBUTES NOTHING RATHER THAN MEASURING SOMETHING, which is what
           FLEX_ITEM_CHILD_NONE means: §9.9.2's arms are a MAXIMUM, so a zero pair leaves the answer exactly
           where the other items put it and cannot invent a width for a box no section generates. */
        none.min_content = css_px(0.0);
        none.max_content = css_px(0.0);
        *next = child->next;
        return none;
    }
}

/* THE MAIN AXIS'S THREE SIZING PROPERTY NAMES AND ITS PHYSICAL AXIS, composed once. css-flexbox-1 §9.9.3
   "Flex Item Intrinsic Size Contributions" and §9.2 "Line Length Determination"' step 3 are stated in the
   container's MAIN axis and name no physical property; css-writing-modes-4 §7.2 "Dimensional Mapping" pins each
   triple to a physical extent — "The height properties (height, min-height, and max-height) refer to the
   physical height, and the width properties (width, min-width, and max-width) refer to the physical width" — so
   a physical name is only ever reached by composing the two, and this is where that composition is made.
   `vertical` IS THE CALLER'S ANSWER AND NOT A QUESTION ASKED HERE, for the reason the entry at the bottom of
   this file states: which section owns a PHYSICAL axis is a fact about the container's `flex-direction` and
   core/layout/flex_item.h's `flex_container_axis_is_vertical` is the composition that answers it.
   `flex-basis` IS NOT ON THIS TABLE AND MUST NOT BE, which is core/layout/flex_line.c's finding at its own
   copy: css-flexbox-1 §7.2.3 "The flex-basis property" gives it ONE spelling on either axis, so a mapping that
   renamed it would rename a property the axis does not move. */
typedef struct {
    const char *size;       /* §9.9.3's "outer preferred size" — §7.1's "main size property" */
    const char *min_size;
    const char *max_size;
    IntrinsicAxis axis;     /* which pair of edges §2.2's outer size and §3.3's conversion read */
} FisMainProps;

static FisMainProps fis_main_props(bool vertical)
{
    FisMainProps p;

    p.size     = vertical ? "height"     : "width";
    p.min_size = vertical ? "min-height" : "min-width";
    p.max_size = vertical ? "max-height" : "max-width";
    p.axis     = vertical ? INTRINSIC_AXIS_VERTICAL : INTRINSIC_AXIS_HORIZONTAL;
    return p;
}

/* css-sizing-3 §5.1 "Intrinsic Sizes"' TWO SIZES IN THE CONTAINER'S MAIN AXIS, which is a DIFFERENT FACT from
   core/layout/intrinsic_size.h's `IntrinsicInlineSizes` and not a second copy of it — that type is named for the
   INLINE axis because that is the only axis its walk measures, and a `column` container's main axis is the BLOCK
   one. The two coincide for a `row` container in a `horizontal-tb` mode and for nothing else, so a caller that
   read one as the other would report a box's height as its width. core/layout/flex_line.c declares the same type
   for the same reason and states it at length; this is that decision applied to the INTRINSIC pass.
   THE CROSS WALK IN THIS FILE KEEPS `IntrinsicInlineSizes` AND THAT IS NOT AN INCONSISTENCY: §9.9.2 "Flex
   Container Intrinsic Cross Sizes" is reached here only for a container whose CROSS axis IS its inline axis, so
   every number in that walk really is an inline size and the type's name is a true statement about it. */
typedef struct {
    CssPx min_content;
    CssPx max_content;
} FisMainSizes;

/* ONE ITEM'S OWN css-sizing-3 §5.1 PAIR IN THE MAIN AXIS — the measurement §9.9.3's first step takes "the larger
   of" against a declared size, before any cap, floor or clamp.
   THE BLOCK ARM IS ONE NUMBER TWICE AND css-sizing-3 §3.2 IS WHY, in its own words at BOTH intrinsic keywords:
   "for a box's block size, unless otherwise specified, this is equivalent to its automatic size". So the block
   axis has ONE intrinsic size and this pair is the inline axis's shape carried across rather than a measurement
   made twice — core/layout/intrinsic_block_size.h is where that derivation and the walk it reaches live, shared
   with §9.2's step 3 in the USED pass so the two passes cannot come to disagree about how tall a box's content
   is. */
static FisMainSizes fis_measure_item(lxb_dom_element_t *item, bool vertical)
{
    FisMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes m = intrinsic_inline_sizes(item);

        out.min_content = m.min_content;
        out.max_content = m.max_content;
        return out;
    }
    out.min_content = out.max_content = intrinsic_block_size(item);
    return out;
}

/* THE SAME PAIR FOR §4's ANONYMOUS CHILD TEXT SEQUENCE, whose box has no element. Both arms are the entry that
   owns an anonymous box's measurement on that axis, and neither adds an edge — see `fis_child_main_contribution`
   for why §4 makes every one of them zero. */
static FisMainSizes fis_measure_run(lxb_dom_element_t *container, BlockFlowRun run, bool vertical)
{
    FisMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes m = intrinsic_inline_run_sizes(container, run);

        out.min_content = m.min_content;
        out.max_content = m.max_content;
        return out;
    }
    out.min_content = out.max_content = intrinsic_block_run_size(container, run);
    return out;
}

/* css-sizing-3 §2.2 "Intrinsic Size Contributions"' OUTER STEP IN THE MAIN AXIS.
   IT IS TWO ENTRIES BEHIND ONE ARM AND NOT AN AXIS PARAMETER, which is core/layout/intrinsic_size.h's own split
   and is worth restating at the site that consumes both: §2.2's FLOOR — "if the ideal max-content contribution
   would be smaller than the min-content contribution (e.g. due to the use of negative margins), the effective
   max-content contribution is floored by the min-content contribution" — is a rule about a pair that can INVERT,
   and the block axis has one intrinsic size for it to have nothing to compare. The horizontal arm therefore
   takes the pair entry, which applies that floor, and the vertical arm takes the single-extent entry, which has
   no floor to apply and no second member to apply it to. */
static FisMainSizes fis_outer_main(lxb_dom_element_t *el, bool vertical, FisMainSizes inner)
{
    FisMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes p;

        p.min_content = inner.min_content;
        p.max_content = inner.max_content;
        p = intrinsic_outer_contribution(el, p);
        out.min_content = p.min_content;
        out.max_content = p.max_content;
        return out;
    }
    out.min_content = intrinsic_outer_block_contribution(el, inner.min_content);
    out.max_content = intrinsic_outer_block_contribution(el, inner.max_content);
    return out;
}

/* css-flexbox-1 §9.2 "Line Length Determination"'s FLEX BASE SIZE of `item`, in the container's MAIN axis —
   which for every caller here is the INLINE axis, by §5.1's mapping. `measured` is the item's own §5.1 pair.
   TWO OF §9.2's FIVE ARMS ARE REACHABLE AND THE ROUTING IS §7.1's, NOT §7.2.3's. §7.1 "The flex Shorthand"
   carries the <'flex-basis'> value list and is where `auto` is defined: "When specified on a flex item, the
   auto keyword retrieves the value of the main size property as the used flex-basis. If that value is itself
   auto, then the used value is content." §7.2.3 "The flex-basis property" defines the property and refers back
   to those values as "defined above" — a citation aimed there is aimed one heading past the sentence.
     - A DEFINITE USED FLEX BASIS is §9.2's first arm, "If the item has a definite used flex basis, that's the
       flex base size" — a `flex-basis` length, or `flex-basis: auto` over a `width` that is one.
     - `content` IS THE ITEM'S MAX-CONTENT MAIN SIZE, in BOTH halves of the pair. §7.1 says so of the keyword —
       "content: Indicates an automatic size based on the flex item's content. This is typically equivalent to
       the max-content size" — and §9.2's own last arm says it again as a substitution, "treating a value of
       content as max-content".
   §9.2's ARM C SAYS SOMETHING ELSE AND THE SPEC EDITOR'S OWN REFERENCE CONTRADICTS IT, which is recorded here
   because a reader who re-derives the arm will otherwise "fix" this line. Arm C is conditioned on exactly the
   situation every call here is inside — "If the used flex basis is content or depends on its available space,
   and the flex container is being sized under a min-content or max-content constraint" — and its answer is
   "size the item under that constraint. The flex base size is the item's resulting main size", which under a
   MIN-content constraint is the item's MIN-content size. WPT's flex-container-min-content-001.html, authored
   by the section's own editor, disagrees at its sixteenth row: a `flex: 1 0 auto` item whose content measures
   1ch min-content and 3ch max-content is expected at 3ch, and only a flex base size of the MAX-content size
   produces that through §9.9.3's floor. The whole 24-row `row` half of that file and of its max-content
   sibling agree with the rule above, 48 of 48. WHAT WOULD RETIRE THIS NOTE is a case that separates the two
   readings the other way; until one exists, the oracle outranks the arm and this cites the two sentences that
   agree with the oracle. */
static CssPx fis_flex_base_size(lxb_dom_element_t *item, bool vertical, FisMainSizes measured)
{
    FisMainProps prop = fis_main_props(vertical);
    CssLength basis = css_computed_length(item, "flex-basis");
    CssPx v;

    if (basis.kind == CSS_LENGTH_ABSOLUTE) {
        /* §3.3's conversion is the same one a declared main size needs, so it is the same entry: §7.2.3's own
           closing sentence is that "flex-basis determines the size of the content box, unless otherwise
           specified, such as by box-sizing". THE PROPERTY NAME IS `flex-basis` ITSELF ON EITHER AXIS — §7.2.3
           gives it one spelling, which is why `fis_main_props` does not carry it — and `prop.axis` is what
           tells that entry WHICH pair of paddings and border widths to subtract, which is the half a bare
           property name could never have carried. Its initial keyword is `auto` exactly as a main size
           property's is. */
        if (intrinsic_declared_sizing_px(item, prop.axis, "flex-basis", "auto", &v)) return v;
        DFAIL("css-flexbox-1 §7.2.3 \"The flex-basis property\"' computed value was an absolute length and "
              "core/layout/intrinsic_size.h's §3.3 conversion declined it. The two read the same cascade entry "
              "one call apart, so they cannot disagree about its shape");
        return measured.max_content;
    }
    if (basis.kind == CSS_LENGTH_PERCENTAGE || basis.kind == CSS_LENGTH_CALCULATED) {
        /* §7.2.3 STATES THIS CASE AND IT IS NOT A REFUSAL: "percentage values of flex-basis are resolved
           against the flex item's containing block (i.e. its flex container) and if that containing block's
           size is indefinite, the used value for flex-basis is content." The containing block's main size here
           IS the number this component is being run to produce, so css-sizing-3 §2 "Terminology"' definite
           size — "a size that can be determined without performing layout" — is false of it by construction
           and the used value is `content` for every call that reaches this line. */
        return measured.max_content;
    }
    DCHECK(basis.kind == CSS_LENGTH_KEYWORD,
           "`flex-basis` computed to none of the shapes css-flexbox-1 §7.2.3 \"The flex-basis property\"' "
           "`Value:` line of `content | <'width'>` admits");
    if (strcmp(basis.keyword, "content") == 0) return measured.max_content;
    if (strcmp(basis.keyword, "auto") == 0) {
        /* §7.1's `auto` reads "the value of the main size property", which is the one the axis names and
           never `width` — the sentence is stated flow-relatively and `fis_main_props` is where it is made
           physical. core/layout/flex_line.c's own §9.2 arm states the identical correction. */
        if (intrinsic_declared_sizing_px(item, prop.axis, prop.size, "auto", &v)) return v;
        return measured.max_content;
    }
    DFAILF("`flex-basis` computed to the keyword `%s`. css-flexbox-1 §7.2.3 \"The flex-basis property\" gives "
           "it `content | <'width'>`, and css-sizing-3 §3.2 \"Sizing Values: the <length-percentage [0,∞]>, "
           "auto | none, stretch, min-content, max-content, and fit-content values\" adds the rest by "
           "reference — \"The flex-basis property hereby also gains these new keywords, as its values are "
           "defined by reference to <'width'>\". This engine records a computed-value rule for none of those, "
           "so BUILD §3.2's keyword arm where a `width` of the same shape is answered "
           "(core/layout/intrinsic_size.c refuses the identical keyword on the identical grammar) and this "
           "routes through it unchanged",
           basis.keyword);
    return measured.max_content;
}

/* css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions" for ONE ELEMENT FLEX ITEM, as the OUTER pair
   §9.9.1's arms sum and maximize over, IN THE CONTAINER'S MAIN AXIS — `vertical` says which physical axis that
   is and `fis_main_props` turns it into the three property names and the edge pair every step below reads. The
   section is stated flow-relatively throughout and names no physical property, so one walk answers both arms of
   §5.1's mapping and a second copy of it would be two readings of one section.
   THE SECTION IS FOUR STEPS AND THEIR ORDER IS ITS OWN. "The main-size min-content contribution of a flex item
   is the larger of its outer min-content size and outer preferred size if that is not an automatic size", the
   same sentence with max-content for the other half, and then: "each contribution is capped by the item's flex
   base size if the item is not growable, floored by the item's flex base size if the item is not shrinkable,
   and then further clamped by the item's min/max main size."
   THE CAP AND THE FLOOR ARE NOT EXCLUSIVE AND THE TEXT DOES NOT MAKE THEM SO: an item that is neither growable
   nor shrinkable (`flex: 0 0 <length>`) takes BOTH, which pins its contribution to the flex base size exactly.
   Writing them as two `if`s rather than an `if/else` is that sentence and not a convenience.
   EVERY TERM IS TAKEN ON THE CONTENT BOX AND §2.2's OUTER STEP RUNS LAST, which is arithmetically the section
   read literally rather than a simplification of it: §9.9.3 caps an OUTER contribution by a flex base size the
   section does not call outer, and BOTH operands belong to the SAME item, so its margins, borders and paddings
   are common to the two sides of every comparison here and add out of all of them. §9.9.1.1 is what proves the
   spec has the other phrase when it wants it — "subtract its outer flex base size from its max-content
   contribution size" — so the omission in §9.9.3 is deliberate and this ordering is what makes the two
   readings agree.
   NAMED RESIDUAL — §4.5 "Automatic Minimum Size of Flex Items" IS NOT BUILT, and the code above is right for
   what the conformance corpus expects rather than unfinished. WHAT IS NOT COVERED: an AUTOMATIC MINIMUM MAIN
   SIZE on a flex item — the MIN SIZE PROPERTY the main axis names computing to `auto`, which css-sizing-3 §3.2's
   "unless otherwise defined by the relevant layout module" hands to §4.5 — "the used value of a main-axis
   automatic minimum size on a flex item whose computed overflow value is non-scrollable is its content-based
   minimum size" — and which this clamp reads as the plain zero §3.2 gives every other box. IT IS STATED AS THE
   PROPERTY THE AXIS NAMES AND NOT AS `min-width`, WHICH IS WHAT IT USED TO SAY: the enumeration was exact while
   this walk ran on one axis, and naming a physical property in a clause about a flow-relative step is the
   §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS shape in a residual — a reader building to it would have covered a
   `row` container and left a `column` one exactly as uncovered while the clause read as spent. WHAT THE NEXT
   DIFF BUILDS: §4.5's three suggestions over this item, of which the content size suggestion ("the min-content
   size in the main axis") is the pair's own min-content half and is already in hand here, with the specified
   size suggestion taken from the same main size read above; the transferred size suggestion needs a preferred
   aspect ratio, which core/layout/replaced_element.h mints nowhere.
   HOW ITS ABSENCE WOULD SHOW: a flex container whose reported intrinsic MAIN size is SMALLER than a real
   browser's, for a document holding an item whose flex base size or max main size sits below that item's own
   min-content size — read the container's measured extent against the same document in Chrome, since the number
   is a size on both axes and neither is a crash. The oracle is silent in the OTHER direction and that is why
   this is a residual and not a defect: WPT's flex-container-min-content-001.html expects 0.2ch for a
   `flex: 0 1 0.2ch` item whose min-content size is 1ch, which is the answer a zero floor gives and not the one
   §4.5 gives. */
static FisMainSizes fis_item_main_contribution(lxb_dom_element_t *container, lxb_dom_element_t *item,
                                               bool vertical)
{
    FisMainProps prop = fis_main_props(vertical);
    FisMainSizes measured, out;
    CssPx base, v;

    fis_require_parallel(container, item);
    measured = fis_measure_item(item, vertical);
    out = measured;

    /* STEP ONE — "the larger of its outer min-content size and outer preferred size if that is not an
       automatic size". The preferred size is the MAIN SIZE PROPERTY, which §5.1's mapping makes `width` for a
       container whose main axis is its INLINE one and `height` for a `column` container in a horizontal writing
       mode — `fis_main_props` is that composition — and its automatic value is the one css-sizing-3 §3.2 "Sizing
       Values: the <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content
       values" describes for `auto`: "specifies an automatic size". So the FALSE arm of the read is that
       condition of §9.9.3 being met rather than a gap in this walk. */
    if (intrinsic_declared_sizing_px(item, prop.axis, prop.size, "auto", &v)) {
        out.min_content = css_px_max(out.min_content, v);
        out.max_content = css_px_max(out.max_content, v);
    }
    /* STEPS TWO AND THREE. */
    base = fis_flex_base_size(item, vertical, measured);
    if (flex_item_flexibility_factor(item, "flex-grow") <= 0.0) {
        out.min_content = css_px_min(out.min_content, base);
        out.max_content = css_px_min(out.max_content, base);
    }
    if (flex_item_flexibility_factor(item, "flex-shrink") <= 0.0) {
        out.min_content = css_px_max(out.min_content, base);
        out.max_content = css_px_max(out.max_content, base);
    }
    /* STEP FOUR — "and then further clamped by the item's min/max main size", in CSS 2.1's own order: the
       maximum caps first and the minimum floors last, so a `min-width` above a `max-width` wins.
       WHICH SECTION STATES THAT ORDER DEPENDS ON THE AXIS AND BOTH STATE IT THE SAME WAY, which is why one
       ordering serves both: CSS 2.1 §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'" for the
       horizontal axis and CSS 2.1 §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'" for the
       vertical one. The `auto` arm is the residual above. */
    if (intrinsic_declared_sizing_px(item, prop.axis, prop.max_size, "none", &v)) {
        out.min_content = css_px_min(out.min_content, v);
        out.max_content = css_px_min(out.max_content, v);
    }
    if (intrinsic_declared_sizing_px(item, prop.axis, prop.min_size, "auto", &v)) {
        out.min_content = css_px_max(out.min_content, v);
        out.max_content = css_px_max(out.max_content, v);
    }
    return fis_outer_main(item, vertical, out);
}

/* css-flexbox-1 §9.9.1 "Flex Container Intrinsic Main Sizes", in the container's MAIN axis — `vertical` says
   which physical axis that is, so a `row` container reaches this walk for an INLINE size and a `column`
   container for a BLOCK one. The section names no physical axis anywhere in it.
   WHICH ALGORITHM, AND WHY THE SECTION LETS THIS CHOOSE: §9.9.1 says outright that "an implementation is
   conformant to CSS Flexible Box Layout if it conforms to either the Ideal Algorithm or the Web-compatible
   Algorithm", of max-content sizes and single-line min-content sizes. This is the SECOND, §9.9.1.2
   "Web-compatible Intrinsic Sizing Algorithm: Max-content Size and Min-content Single-line Size", whose whole
   text is two sentences of arithmetic over the items: "For the max-content size of a flex container, take the
   sum of the max-content contributions of all the non-collapsed flex items in the flex container", and the
   same sentence with min-content for a single-line container. §9.9.1.1's own note is why the FIRST is not
   built: "because it was not implemented correctly initially and existing content became dependent on the
   unfortunately consistent incorrect implemented behavior it is not web compatible".
   THE MULTI-LINE MIN-CONTENT SIZE IS NOT PART OF THAT CHOICE. §9.9.1 hands it to §9.9.1.3 "Multi-line
   Min-content Algorithm" unconditionally — "For the min-content size of a multi-line flex container, see
   §9.9.1.3" — and §9.9.1.3 is a MAXIMUM rather than a sum: "For a multi-line container, the min-content main
   size is simply the largest min-content contribution of all the non-collapsed flex items in the flex
   container." So the two halves of this pair run different operations on one walk, and NEITHER needs a flex
   line: §9.3 "Main Size Determination"'s line breaking is not on this path at all.
   A COLLAPSED ITEM IS SKIPPED HERE AND IS NOT SKIPPED BY §9.9.2, which is the one place the two sections part
   over their operands. Both of these arms say "non-collapsed" in their own words; §9.9.2 says it in neither of
   its, because §4.4 "Collapsed Items" gives a collapsed item a CROSS-axis effect it does not have in the main
   one — "the collapsed flex item is removed from rendering entirely, but leaves behind a strut that keeps the
   flex line's cross-size stable". So this walk asks css-display-3 §4 "Invisibility: the visibility property"
   and the walk beside it must not.
   AN ANONYMOUS ITEM IS NEVER COLLAPSED AND ITS CONTRIBUTION IS ITS MEASUREMENT, and both fall out of §4 rather
   than being special cases. §4 makes the box unstyleable, so every property it could be asked about holds its
   INITIAL value: `visibility` is `visible`, so §4.4's keyword cannot be on it; and `flex: 0 1 auto` over a
   `width: auto` makes its flex base size its own max-content size, which caps a max-content contribution at
   itself and leaves a min-content one alone because the pair is ordered. Reading those properties off the
   container — the only element there is — would answer about the wrong box. */
/* ONE CHILD of the container as a MAIN-axis contribution, with `*counts` false where §9.9.1.2's and
   §9.9.1.3's "non-collapsed" excludes it. The pair is returned even then and is never read: a ZERO would be
   wrong in §9.9.1.3's MAXIMUM, because css-sizing-3 §2.2 "Intrinsic Size Contributions" permits a negative
   outer contribution (CSS 2.1 §8.3 "Margin properties": "negative values for margin properties are allowed")
   and a zero would floor the answer above every one of them. A skipped item is skipped, not zeroed. */
static FisMainSizes fis_child_main_contribution(lxb_dom_element_t *container, lxb_dom_node_t *child,
                                                FlexItemChildKind kind, bool vertical,
                                                lxb_dom_node_t **next, bool *counts)
{
    /* THE KIND IS THE CALLER'S AND IS NOT RE-ASKED, for the reason the cross walk states in full: §4's
       classification of a TEXT node walks the whole sequence it is in, so asking twice is two readings of one
       child list. */
    *counts = true;
    switch (kind) {
    case FLEX_ITEM_CHILD_TEXT: {
        /* §4's ANONYMOUS BLOCK CONTAINER FLEX ITEM, whose §9.9.3 contribution IS its measurement — see
           `fis_main_sizes` for why every step of that section collapses on a box §4 makes unstyleable. Its
           edges are zero because §4 gives it no declaration to compute one from, which is the NULL that entry
           reads as an anonymous box. */
        lxb_dom_node_t *end = flex_item_text_sequence_end(container, child);
        BlockFlowRun seq;

        seq.after = child->prev;
        seq.end = end;
        *next = end;
        return fis_outer_main(NULL, vertical, fis_measure_run(container, seq, vertical));
    }
    case FLEX_ITEM_CHILD_ELEMENT: {
        lxb_dom_element_t *item = lxb_dom_interface_element(child);

        *next = child->next;
        if (flex_item_is_collapsed(item)) {
            FisMainSizes none;

            *counts = false;
            none.min_content = css_px(0.0);
            none.max_content = css_px(0.0);
            return none;
        }
        return fis_item_main_contribution(container, item, vertical);
    }
    case FLEX_ITEM_CHILD_NONE:
        break;
    /* NO `default` ARM, DELIBERATELY, and for the reason the cross walk states: `-Wswitch` is the forcing
       function here and a default would switch it off. A fourth `FlexItemChildKind` is a fourth kind of member
       in every flex container's item list, and §9.9's arms are all over a LIST — a member this walk cannot
       classify is a member it would silently drop, so the day one is added the answer must be a COMPILE
       failure at every caller and not a crash at one. */
    }
    {
        FisMainSizes none;

        DFAIL("css-flexbox-1 §4's classification answered FLEX_ITEM_CHILD_NONE for a child this walk had "
              "already decided to measure. The caller skips that value before it asks for a contribution, so "
              "two readings of one child list disagreed — which means the child list changed under the walk");
        /* THE RELEASE FALL-THROUGH CONTRIBUTES NOTHING AND IS EXCLUDED RATHER THAN ZEROED, which is what
           FLEX_ITEM_CHILD_NONE means and is also what keeps §9.9.1.3's maximum honest. */
        *counts = false;
        none.min_content = css_px(0.0);
        none.max_content = css_px(0.0);
        *next = child->next;
        return none;
    }
}

static FisMainSizes fis_main_sizes(lxb_dom_element_t *el, bool vertical, bool multi_line)
{
    lxb_dom_node_t *c = lxb_dom_interface_node(el)->first_child;
    FisMainSizes out;
    bool any = false;

    /* THE EMPTY CONTAINER IS A REAL ANSWER: a sum over no items is zero and so is a maximum over none, which
       is §6 "Flex Lines"' one container with no line at all — "Every line contains at least one flex item,
       unless the flex container itself is completely empty." IT IS ALSO WHY THE MAXIMUM CARRIES `any`: a
       container whose only items contribute NEGATIVE outer sizes has a min-content main size below zero under
       §9.9.1.3, and seeding a maximum at zero would report it as zero — while a container with no item at all
       is zero because there is nothing to take a maximum over. Two states, two answers, one accumulator. */
    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    while (c != NULL) {
        lxb_dom_node_t *next = c->next;
        FlexItemChildKind kind = flex_item_child_kind(el, c);
        FisMainSizes one;
        bool counts;

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        one = fis_child_main_contribution(el, c, kind, vertical, &next, &counts);
        DCHECK(next != c,
               "css-flexbox-1 §4's item walk did not advance past the child it had just measured, so this walk "
               "would measure the same flex item for ever. A text sequence always contains at least the node "
               "it was delimited from, and an element item always advances by one sibling");
        if (counts) {
            out.max_content = css_px_add(out.max_content, one.max_content);
            if (!multi_line) out.min_content = css_px_add(out.min_content, one.min_content);
            else if (!any) out.min_content = one.min_content;
            else out.min_content = css_px_max(out.min_content, one.min_content);
            any = true;
        }
        c = next;
    }
    return out;
}

/* §9.9.2 "Flex Container Intrinsic Cross Sizes", for the container whose CROSS axis is its inline axis — a
   `column` or `column-reverse` container, by §5.1's mapping.
   BOTH REACHABLE ARMS ARE A MAXIMUM OVER THE ITEMS, and the section states them as two sentences that agree
   on the min-content size and part on the max-content one. Single-line: "The min-content/max-content cross
   size of a single-line flex container is the largest min-content contribution/max-content contribution
   (respectively) of its flex items." Multi-line column: "The min-content cross size is the largest min-content
   contribution among all of its flex items", and the section's own note says why that is not the same shape as
   the max-content one — "This heuristic effectively assumes a single flex line, in order to guarantee that the
   min-content size is smaller than the max-content size."
   A COLLAPSED ITEM IS *NOT* SKIPPED HERE and that is the section rather than an oversight. §9.9.1.2 and
   §9.9.1.3 both state their operand as the "non-collapsed" flex items and §9.9.2 states neither of its as
   that, because §4.4 "Collapsed Items" gives a collapsed item a cross-axis effect the main axis does not have:
   "the collapsed flex item is removed from rendering entirely, but leaves behind a strut that keeps the flex
   line's cross-size stable". So this walk asks css-display-3 §4 "Invisibility: the visibility property"
   nothing, and a walk over §9.9.1 will have to. */
static IntrinsicInlineSizes fis_cross_sizes(lxb_dom_element_t *el, bool multi_line)
{
    lxb_dom_node_t *c = lxb_dom_interface_node(el)->first_child;
    IntrinsicInlineSizes out;
    bool any = false;
    char nbuf[160];

    /* THE EMPTY CONTAINER IS A REAL ANSWER AND NOT A FLOOR: a maximum over no items is zero, and §6 "Flex
       Lines" says that is the one container with no line at all — "Every line contains at least one flex item,
       unless the flex container itself is completely empty." */
    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    while (c != NULL) {
        lxb_dom_node_t *next = c->next;
        FlexItemChildKind kind = flex_item_child_kind(el, c);
        IntrinsicInlineSizes one;

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        one = fis_item_cross_contribution(el, c, kind, &next);
        DCHECK(next != c,
               "css-flexbox-1 §4's item walk did not advance past the child it had just measured, so this walk "
               "would measure the same flex item for ever. A text sequence always contains at least the node "
               "it was delimited from, and an element item always advances by one sibling");
        out.min_content = css_px_max(out.min_content, one.min_content);
        out.max_content = css_px_max(out.max_content, one.max_content);
        any = true;
        c = next;
    }
    /* §9.9.2's MULTI-LINE COLUMN MAX-CONTENT ARM IS THE ONE THING IN THIS COMPONENT THAT NEEDS FLEX LINES, and
       it is refused HERE rather than at the dispatch so the min-content half — which is a maximum and IS
       built — still runs and so the crash names the container it is about. An EMPTY multi-line container is
       not refused: the sum of no line cross sizes is zero, which is the number a maximum over no items already
       produced, so §6's completely-empty container has one answer and not two. */
    if (multi_line && any)
        DFAILF("%s: css-flexbox-1 §9.9.2 \"Flex Container Intrinsic Cross Sizes\" gives a MULTI-LINE COLUMN "
               "flex container a max-content cross size this component cannot compute, and it is the only arm "
               "of §9.9 reachable from an intrinsic INLINE size that is stated over flex lines rather than "
               "over items: \"The max-content cross size is the sum of the flex line cross sizes resulting "
               "from sizing the flex container under a cross-axis max-content constraint, using the largest "
               "max-content cross-size contribution among the flex items as the available space in the cross "
               "axis for each of the flex items during layout.\" THE MIN-CONTENT HALF IS ALREADY THE ANSWER "
               "ABOVE — §9.9.2 makes it \"the largest min-content contribution among all of its flex items\", "
               "a maximum over items with no line in it — so what is missing is exactly the LINES: §9.3 "
               "\"Main Size Determination\"'s step 5 collects items into flex lines, and §9.4 \"Cross Size "
               "Determination\" is what gives each line the cross size this sum is over. ONE OF THOSE TWO IS "
               "BUILT AND THE OTHER IS EXACTLY WHAT THIS ARM STILL NEEDS: core/layout/flex_cross_size.h runs "
               "§9.4's steps 7 and 8 and answers §9.6 \"Cross-Axis Alignment\"' sum, and it REFUSES a "
               "multi-line container by name because §9.3's step 5 has no line BREAKING behind it — which is "
               "the same sentence this crash is making, one axis over. So the component that answers this "
               "arm exists and what it is waiting on is §9.3's breaking, not §9.4. THE CLAUSE THAT STOOD "
               "HERE NAMED core/layout/block_flow.c AS THE SAME ABSENCE and that file now CALLS that "
               "component, so a reader who follows the old sentence finds a call",
               box_subject(el, nbuf, sizeof nbuf));
    return out;
}

IntrinsicInlineSizes flex_intrinsic_inline_sizes(lxb_dom_element_t *el)
{
    IntrinsicInlineSizes out;
    FisMainSizes main;

    DCHECK(el != NULL, "css-flexbox-1 §9.9's intrinsic sizes were asked for with no element");
    /* §5.1's MAPPING RUN BACKWARDS — see flex_intrinsic_size.h. Asked first and over the whole container,
       because §9.9.1 and §9.9.2 share no step: one sums along an axis and the other takes a maximum across
       the other one, so which section this box gets is a fact about its `flex-direction` and never a case
       discovered part-way through an accumulation. */
    if (flex_container_main_axis(el) == FLEX_MAIN_AXIS_BLOCK)
        return fis_cross_sizes(el, flex_container_is_multi_line(el));

    /* §9.9.1 IN THE HORIZONTAL AXIS, WHICH FOR THIS CONTAINER IS BOTH ITS MAIN AND ITS INLINE ONE — that is
       what makes the conversion between the two types SOUND here rather than a cast. `FisMainSizes` is a pair
       in the MAIN axis and `IntrinsicInlineSizes` is a pair in the INLINE axis; §5.1 has just established that
       this container's main axis "has the same orientation as the inline axis of the current writing mode", so
       for this box and no other the two types describe one number each. A `column` container never reaches
       this line, which is why the conversion cannot be reached with the two axes apart. */
    main = fis_main_sizes(el, false, flex_container_is_multi_line(el));
    out.min_content = main.min_content;
    out.max_content = main.max_content;
    return out;
}

CssPx flex_intrinsic_max_content_main_block_size(lxb_dom_element_t *el)
{
    char nbuf[160];

    DCHECK(el != NULL,
           "css-flexbox-1 §9.9.1's max-content main size was asked for with no element");
    /* §5.1's MAPPING, ASSERTED AND NOT ASKED, which is the difference between this entry and the one above.
       That entry answers an INLINE size and so must DISPATCH on which of §9.9.1 and §9.9.2 owns it; this one
       answers the MAIN size outright, so the only thing §5.1 decides is whether the caller has come to the
       right axis at all. A `row` container's main size is its INLINE size and `flex_intrinsic_inline_sizes` is
       where that is answered. */
    DCHECKF(flex_container_main_axis(el) == FLEX_MAIN_AXIS_BLOCK,
            "%s: this FLEX CONTAINER's main axis is its INLINE axis (css-flexbox-1 §5.1 \"Flex Flow Direction: "
            "the flex-direction property\": a `row` container's main axis \"has the same orientation as the "
            "inline axis of the current writing mode\"), so the max-content MAIN size asked for here is not a "
            "block size and this entry would measure it along the wrong dimension. "
            "`flex_intrinsic_inline_sizes` is the entry for it, and it reaches the same §9.9.1 walk with its "
            "other axis",
            box_subject(el, nbuf, sizeof nbuf));
    /* THERE IS NO MULTI-LINE ARM AND THAT IS §9.9.1's OWN SCOPING RATHER THAN A GAP — which is worth stating
       because the walk above carries `multi_line` and `fis_cross_sizes` REFUSES a multi-line container by name,
       so a reader arriving here expects a third refusal. §9.9.1 hands only the MIN-content size of a multi-line
       container to §9.9.1.3 "Multi-line Min-content Algorithm" — "For the min-content size of a multi-line flex
       container, see §9.9.1.3" — and §9.9.1.3's own title says min-content. §9.9.1.2's max-content sentence is
       stated over "a flex container" with no line-count condition on it at all, and its conformance sentence is
       scoped to "max-content sizes, and … single-line min-content sizes". So the MAX-CONTENT main size is one
       algorithm for every container, the `multi_line` argument below can only reach the half this entry does not
       read, and passing the container's real answer is what keeps that true if a min-content reader is ever
       built rather than a literal that would then be a lie.
       THE MIN-CONTENT HALF IS NOT RETURNED BECAUSE IT HAS NO CALLER, and that is §Do-subproblems-IN-ORDER
       rather than an omission: the only thing that could ask for it is css-sizing-3 §3.2's `min-content`
       keyword on a `min-height`, and this engine records no computed-value rule for that keyword — every entry
       that reads the grammar asserts so by name. A pair here would be one half nothing exercises. */
    return fis_main_sizes(el, true, flex_container_is_multi_line(el)).max_content;
}
