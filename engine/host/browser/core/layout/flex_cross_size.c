/* css-flexbox-1 §9.4 "Cross Size Determination"' steps 7 and 8 and §9.6 "Cross-Axis Alignment"' determination
   of the container's own used cross size. See flex_cross_size.h for why the three steps are one component and
   one call, and for why nothing here is stored. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/line_box.h"
#include "core/layout/used_value.h"

/* THE KEYWORD-VALUED PROPERTIES THIS COMPONENT READS, and only those. A LENGTH-valued one — a cross size or
   either cross-axis margin — is `css_computed_length_is` (core/css/css_computed_value.h), which is a different
   ENTRY and not a different spelling: a length's computed value is a `CssPx` carrying the environment fact a
   `50vh` cross size derives from, so the text entry refuses it rather than dropping it, and the abort names
   the cascade's own invariant in place of the §9.4 question the caller was asking. */
static bool fx_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* css-writing-modes-4 §7.2 "Dimensional Mapping"' PHYSICAL NAMES OF THE THREE CROSS-AXIS PROPERTIES THIS
   COMPONENT READS AS COMPUTED VALUES, for a cross axis that is the vertical one (`vertical`) or the
   horizontal one. It is the mirror of core/layout/flex_line.c's `fl_main_props` and its reasoning is that
   function's: §6.4 "Abstract-to-Physical Mappings" answers WHICH PHYSICAL AXIS an abstract one is and
   `flex_container_axis_is_vertical` (core/layout/flex_item.h) composes it with css-flexbox-1 §5.1 "Flex Flow
   Direction: the flex-direction property" to produce the `vertical` this takes, while §7.2 says which
   PROPERTY NAME states a size on that physical axis — "The height properties (height, min-height, and
   max-height) refer to the physical height, and the width properties (width, min-width, and max-width) refer
   to the physical width." So nothing is renamed: a `column` container's cross-axis size property IS `width`,
   in every writing mode, and what varies is which axis `width` is the size of.
   THE MARGINS ARE ON THIS STRUCT AND THE SIZE'S LIMITS ARE NOT, which is a fact about what this component
   reads rather than about §7.2: css-flexbox-1 §8.3 "Cross-axis Alignment: the align-items and align-self
   properties"' STRETCHED test names the cross size property and BOTH cross-axis margins and nothing else,
   and CSS 2.1 §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'"' clamp is the caller's —
   flex_cross_size.h states why. */
typedef struct {
    const char *size;          /* `height`        or `width`        */
    const char *margin_start;  /* `margin-top`    or `margin-left`  */
    const char *margin_end;    /* `margin-bottom` or `margin-right` */
} FxCrossProps;

static FxCrossProps fx_cross_props(bool vertical)
{
    FxCrossProps p;

    p.size         = vertical ? "height"        : "width";
    p.margin_start = vertical ? "margin-top"    : "margin-left";
    p.margin_end   = vertical ? "margin-bottom" : "margin-right";
    return p;
}

/* THE ITEM'S CROSS-AXIS BORDER AND PADDING, as USED values.
   THE AXIS PICKS THE PAIR AND css-writing-modes-4 §7.2 "Dimensional Mapping" IS THE SENTENCE, exactly as it
   is at core/layout/flex_line.c's `fl_main_border_padding`: CSS 2.1 §10.3's rules "apply to the inline size
   … and to the inline-start and inline-end margins, padding, and border", and the same sentence gives
   CSS 2.1 §10.6's "to the block size and to the block-start and block-end margins, padding, and border". A
   size and its edges are ONE dimension's pair, so a cross-axis size read beside the other axis's edges is
   the defect that sentence exists to prevent — and it is exactly the defect this function had while it could
   read only the vertical pair and the entry below refused every container whose cross axis was not vertical.
   `used_value_border_widths_px` WRITES CSS 2.1 §8.5.1's own top/right/bottom/left order, so the vertical
   pair is entries 0 and 2 and the horizontal one is 1 and 3. */
static CssPx fx_cross_border_padding(lxb_dom_element_t *el, bool vertical)
{
    CssPx b[4];

    used_value_border_widths_px(el, b);
    if (vertical)
        return css_px_add(css_px_add(b[0], b[2]),
                          css_px_add(used_value_px(el, "padding-top"), used_value_px(el, "padding-bottom")));
    return css_px_add(css_px_add(b[1], b[3]),
                      css_px_add(used_value_px(el, "padding-left"), used_value_px(el, "padding-right")));
}

/* THE ITEM'S CROSS-AXIS MARGINS. An `auto` one is NOT answered here and must not be: css-flexbox-1 §9.6
   "Cross-Axis Alignment"' first step gives it the difference between the item's outer cross size and the
   cross size of its flex line — "If its outer cross size (treating those auto margins as zero) is less than
   the cross size of its flex line, distribute the difference in those sizes equally to the auto margins" —
   which is a number the LINE's cross size is an operand of, so at step 8 it does not exist yet.
   core/layout/used_value.c already refuses that value by name and names §9.6 doing it, so the refusal is ONE
   sentence in ONE place and this file does not restate it — the same arrangement core/layout/flex_line.c has
   for the main axis. IT IS REFUSED ON WHICHEVER PHYSICAL AXIS IS THE CROSS ONE, which is why this reads the
   pair the axis names rather than only the vertical one: §9.6's sentence is about a CROSS-AXIS margin and
   names no physical side, so a `column` container's `margin-left: auto` is the same case — and that file's
   horizontal arm named §9.5 "Main-Axis Alignment" for every flex item until this diff asked the axis there
   too. */
static CssPx fx_cross_margins(lxb_dom_element_t *el, bool vertical)
{
    FxCrossProps prop = fx_cross_props(vertical);

    return css_px_add(used_value_px(el, prop.margin_start), used_value_px(el, prop.margin_end));
}

/* THE SAME PAIR FOR CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow"' STRETCH-FIT, WHERE
   AN `auto` MARGIN IS ZERO — which is that section's own sentence and not a fallback past the refusal above.
   CSS 2.1 §10.3.3: "If 'width' is set to 'auto', any other 'auto' values become '0' and 'width' follows from
   the resulting equality." css-flexbox-1 §9.4 "Cross Size Determination"' step 7 lays the item out AS an in-flow
   block-level box with its cross size treated as `auto`, so that antecedent holds wherever this is read and
   the margins it names resolve inside the hypothetical layout rather than waiting for §9.6 "Cross-Axis
   Alignment"' distribution.
   IT IS A SECOND FUNCTION AND NOT A FLAG ON THE FIRST, because the two answer DIFFERENT SECTIONS' questions
   about one property pair and the difference is invisible at the only inputs that make them agree. §8.3
   "Cross-axis Alignment: the align-items and align-self properties"' stretched value is reached only where
   NEITHER cross margin is `auto` — that is one of its three conditions — so `fx_cross_margins` above can
   never meet one and its refusal of one is the right behaviour; step 7 meets them routinely and must not
   crash on a page §10.3.3 answers in one sentence. A bool would have let either caller take the other's
   rule, which is one property pair with two used values free to disagree. */
static CssPx fx_cross_margins_stretch_fit(lxb_dom_element_t *el, bool vertical)
{
    FxCrossProps prop = fx_cross_props(vertical);
    CssPx m = css_px(0.0);

    if (!css_computed_length_is(el, prop.margin_start, "auto")) m = used_value_px(el, prop.margin_start);
    if (!css_computed_length_is(el, prop.margin_end, "auto"))
        m = css_px_add(m, used_value_px(el, prop.margin_end));
    return m;
}

/* §8.3's RESOLVED CROSS-AXIS ALIGNMENT FOR ONE ITEM, AS THE KEYWORD ITSELF — ONE FACT, WITH THE QUESTIONS
   ASKED OF IT BELOW. It was computed inline inside the baseline refusal while that was the only question this
   component asked of it; §9.4's step 11 asks a DIFFERENT one of the same value ("is this item STRETCHED"), and
   two resolutions of one keyword are two answers that are free to disagree about `align-self: auto`. So the
   resolution is the fact and each caller is a predicate over it, which is also why this returns the KEYWORD
   rather than a bool: a second bool would be a second resolution wearing a narrower name.
   §4's ANONYMOUS FLEX ITEM IS `item == NULL` AND IS NOT EXEMPT, which is §8.3's own sentence and the reason
   the question is asked of it at all: "align-items sets the default alignment for all of the flex container's
   items, INCLUDING ANONYMOUS FLEX ITEMS." §4 makes that box unstyleable, so it declares no `align-self` and
   takes the same arm an element item taking `auto` takes.
   §8.3's `Initial: auto` on `align-self` is what makes the container's `align-items` the value that decides an
   item that declares nothing, which is the whole of that keyword's meaning.
   THE CALLER FREES THE RESULT. */
static char *fx_resolved_align(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    char *items = css_computed_value(container, "align-items");
    char *self = NULL;

    DCHECK(items != NULL,
           "the cascade produced no computed value for `align-items` — it is in lexbor's registry with an "
           "initial value, so the last layer always answers");
    if (item != NULL) {
        self = css_computed_value(item, "align-self");
        DCHECK(self != NULL,
               "the cascade produced no computed value for `align-self` — it is in lexbor's registry with an "
               "initial value, so the last layer always answers");
    }
    if (self == NULL || strcmp(self, "auto") == 0) {
        free(self);
        return items;
    }
    free(items);
    return self;
}

/* §8.3's `stretch` VALUE'S OWN ANTECEDENT, WHICH IS THREE CONDITIONS AND NOT ONE: "If the cross size property
   of the flex item computes to `auto`, and neither of the cross-axis margins are `auto`, the flex item is
   STRETCHED." The keyword alone is not the test and reading it as one is the whole of what this predicate
   exists to prevent — a `stretch`-aligned item with a DECLARED cross size is not stretched, and neither is one
   with an `auto` cross margin, which §9.6 "Cross-Axis Alignment"' first step gives the spare cross space to
   instead.
   THE CROSS SIZE PROPERTY IS READ AS COMPUTED AND NOT AS THE PASS'S VALUE, which matters because the two come
   apart exactly once: CSS 2.1 §10.7's re-run substitutes a LIMIT for the size and runs the pass again, and
   that pass takes the DECLARED arm in core/layout/used_value.c rather than reaching this component at all. So
   `auto` here is §8.3's own word and never a restatement of which arm the caller is in — and a PERCENTAGE
   cross size that css-sizing-3 §3.2.1 "“Behaving as auto”" makes behave as auto is correctly NOT stretched,
   because §8.3 says COMPUTES TO `auto` and a percentage does not.
   THE MARGINS ARE READ AS COMPUTED FOR THE SAME REASON AND A SECOND ONE: core/layout/used_value.c REFUSES an
   `auto` cross margin on a flex item by name, naming §9.6, so asking for its used value here would crash on
   the very page this arm exists to answer NO for.
   ALL THREE NAMES COME FROM THE AXIS AND NONE OF THEM IS RENAMED BY IT — §8.3 states the test over the CROSS
   SIZE PROPERTY and the CROSS-AXIS MARGINS, which css-writing-modes-4 §7.2 "Dimensional Mapping" makes
   `width`/`margin-left`/`margin-right` for a `column` container and `height`/`margin-top`/`margin-bottom` for
   a `row` one. Reading `height` on either axis was this predicate answering a `column` container's item NO
   for a declared HEIGHT that is that item's MAIN size and that §8.3 does not mention. */
static bool fx_is_stretched(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    FxCrossProps prop = fx_cross_props(vertical);
    char *align;
    bool stretch;

    DCHECK(item != NULL,
           "css-flexbox-1 §8.3 \"Cross-axis Alignment: the align-items and align-self properties\"' STRETCHED "
           "test was asked with no item element. §4 \"Flex Items\"' anonymous flex item has no used cross size "
           "any caller asks for yet — §9.6 \"Cross-Axis Alignment\" is what would place it — so a NULL here is "
           "a caller that reached this predicate through a walk that should have stopped at step 8");
    align = fx_resolved_align(container, item);
    stretch = strcmp(align, "stretch") == 0;
    free(align);
    if (!stretch) return false;
    if (!css_computed_length_is(item, prop.size, "auto")) return false;
    return !css_computed_length_is(item, prop.margin_start, "auto") &&
           !css_computed_length_is(item, prop.margin_end, "auto");
}

/* css-flexbox-1 §9.4's STEP 8.1 — the BASELINE GROUP — which this component does not compute and therefore
   must not silently leave out of step 8.3's maximum. §8.3 "Cross-axis Alignment: the align-items and
   align-self properties" declares both properties over the closed value list
   `flex-start | flex-end | center | baseline | stretch`, with `align-self` adding `auto` and an `Initial:` of
   `auto`, and its own prose gives the resolution: "align-items sets the default alignment for all of the flex
   container's items, including anonymous flex items. align-self allows this default alignment to be
   overridden for individual flex items."
   THE TEST IS A SUBSTRING AND THAT IS DELIBERATE RATHER THAN LOOSE. §8.3's Level 1 grammar admits the bare
   keyword only, and css-align-3 spells the same alignment `first baseline` and `last baseline`; every one of
   those is a BASELINE alignment and puts the item in §9.4's step 8.1 collection, so a test that admitted only
   the Level 1 spelling would answer NO for a page this engine's cascade may well accept. Refusing MORE is the
   safe direction here because the refusal is a crash and not a number. */
static void fx_require_no_baseline_alignment(lxb_dom_element_t *container, lxb_dom_element_t *item,
                                             lxb_dom_node_t *subject)
{
    char *align = fx_resolved_align(container, item);
    bool baseline;
    char nbuf[160];

    baseline = strstr(align, "baseline") != NULL;
    free(align);
    if (!baseline) return;
    DFAILF("%s: this FLEX ITEM is BASELINE-ALIGNED in the cross axis (css-flexbox-1 §8.3 \"Cross-axis "
           "Alignment: the align-items and align-self properties\"), so css-flexbox-1 §9.4 \"Cross Size "
           "Determination\"' step 8 puts it in the FIRST of its two collections and not the second: "
           "\"Collect all the flex items whose inline-axis is parallel to the main-axis, whose align-self is "
           "baseline, and whose cross-axis margins are both non-auto. Find the largest of the distances "
           "between each item's baseline and its hypothetical outer cross-start edge, and the largest of the "
           "distances between each item's baseline and its hypothetical outer cross-end edge, and sum these "
           "two values.\" THAT SUM IS NOT THIS ITEM'S OUTER HYPOTHETICAL CROSS SIZE and it is not bounded by "
           "it either — two items whose baselines sit at different depths push the line TALLER than either of "
           "them is — so taking the walk below for it would report a line shorter than the page draws, which "
           "is the one answer a crash here is preferable to. "
           "FOR §4 \"Flex Items\"' ANONYMOUS FLEX ITEM THE KEYWORD CAME FROM THE CONTAINER, which §8.3 "
           "states outright — \"align-items sets the default alignment for all of the flex container's items, "
           "including anonymous flex items\" — so that box is in §9.4 \"Cross Size Determination\"' "
           "step 8.1's collection exactly as an element "
           "item declaring `align-self: auto` under the same container is. "
           "WHAT IT NEEDS IS A BASELINE PER ITEM, MEASURED IN THE SAME FRAME AS THE OUTER CROSS EDGES, and "
           "core/layout/block_flow.h has both halves: `block_flow_first_line_box_baseline` and "
           "`block_flow_last_line_box_baseline` each report a distance from the box's TOP CONTENT EDGE and "
           "each answer FALSE for a box with no in-flow line box, which is css-flexbox-1 §8.5 \"Flex "
           "Container Baselines\"' own fallback condition. So what is missing is §8.5's choice between them "
           "and the two distances' conversion from the content edge to the item's OUTER cross edges, not a "
           "measurement",
           box_subject_node(subject, nbuf, sizeof nbuf));
}

/* css-flexbox-1 §9.4 "Cross Size Determination"' STEP 8's ANSWER FOR THE ONE LINE OF A SINGLE-LINE CONTAINER
 * — ITS TWO ARMS COINCIDE HERE AND THAT IS A DERIVATION RATHER THAN A SHORTCUT, so it is spelled out: the
 * obvious implementation is a dispatch on whether the container's cross size is definite, and a reader who
 * finds one call instead will otherwise write the dispatch back.
 *   - THE DEFINITE ARM IS THE CALL LITERALLY. "If the flex container is single-line and has a definite cross
 *     size, the cross size of the flex line is the flex container's inner cross size", and
 *     `used_value_content_px` on the cross axis IS the container's inner cross size.
 *   - THE INDEFINITE ARM REACHES THE SAME NUMBER THROUGH §9.6. Its "Otherwise" branch makes the line's cross
 *     size step 8.3's maximum, and §9.6 "Cross-Axis Alignment"' own step makes an auto-sized container's cross
 *     size "the sum of the flex lines' cross sizes" — one term for a single-line container. So the container's
 *     inner cross size IS that maximum, and core/layout/block_flow.c's `row` arm is the route that produces
 *     it: `used_value_content_px` reaches `block_flow_auto_height`, which reaches `flex_cross_size_content_
 *     based` below.
 *     ON THE INLINE CROSS AXIS THE INDEFINITE ARM REACHES SOMETHING ELSE, AND THAT IS THE SAME SENTENCE
 *     RATHER THAN AN EXCEPTION: §9.6's step says to use "the rules of the formatting context in which it
 *     participates" FIRST and a content-based size only "if" one is needed, and for a `column` container the
 *     cross size is a WIDTH whose rule is CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal
 *     flow"' constraint equation — which needs no content. `used_value_content_px` answers exactly that,
 *     so this call is right on either axis and the walk below is reached on only one of them.
 *   - STEP 8.3's SINGLE-LINE CLAMP FALLS OUT AND IS NOT WRITTEN TWICE. "If the flex container is single-line,
 *     then clamp the line's cross-size to be within the container's computed min and max cross sizes" —
 *     which is CSS 2.1 §10.7's three steps over the CONTAINER, and `used_value_content_px` answers a USED
 *     value, so those steps have already run. The standard says as much in the note that follows: "Note that
 *     if CSS 2.1's definition of min/max-width/height applied more generally, this behavior would fall out
 *     automatically." THE CLAMP IS INSIDE THE `Otherwise` ARM AND NOT AFTER BOTH, which is a fact about the
 *     source and not about the prose: its `<p>` is nested in step 8.3's own `<li>`, inside the `<ol>` the
 *     "Otherwise, for each flex line:" paragraph opens. Reading it as applying to the definite arm too would
 *     clamp a number §10.7 had already clamped.
 * A STRETCHED CONTAINER IS COVERED BY THE FIRST ARM AND NOT BY AN EXCEPTION, which is the case that looks
 * like a counterexample: a flex container that is ITSELF a stretched flex item has no declaration of its own
 * and still has a definite cross size, because §9.8 "Definite and Indefinite Sizes" says so in its own words
 * — "If a single-line flex container has a definite cross size, the automatic preferred outer cross size of
 * any stretched flex items is the flex container's inner cross size (clamped to the flex item's min and max
 * cross size) and IS CONSIDERED DEFINITE." `used_value_content_px` answers that box's used cross size, which
 * is the stretched one.
 * IT TERMINATES, AND THE REASON IS STEP 7's OWN RECLASSIFICATION rather than anything this file does. The
 * only way back into this component from here is the indefinite arm's walk, and every per-item measurement
 * that walk makes is `used_value_block_level_content_px`, which reads the item AS A BLOCK-LEVEL BOX — so it
 * takes CSS 2.1 §10.6.3's stack and never re-enters §9.4's step 11. What DOES recur is the ancestor chain:
 * an item's container may itself be an item, and each level asks its own container, which terminates at the
 * first box that is not a flex item.
 * STEP 7's INLINE ARM IS A SECOND CALLER OF THIS AND TERMINATES FOR A DIFFERENT REASON, which is stated
 * because the first one does not cover it: that arm asks for the CONTAINER's size while standing inside an
 * ITEM's, and the container's is CSS 2.1 §10.3.3's equation over the container's OWN containing block, which
 * is strictly further up the tree. Its other operand, core/layout/intrinsic_size.h's pair, walks the item's
 * DESCENDANTS. Neither reaches the item this component was asked about. */
static CssPx fx_line_cross_size(lxb_dom_element_t *container, bool vertical)
{
    return used_value_content_px(container, vertical);
}

/* css-flexbox-1 §9.4's STEP 7 — ONE ITEM'S HYPOTHETICAL CROSS SIZE, as a CONTENT-box extent on `container`'s
   cross axis. The step's whole sentence is "Determine the hypothetical cross size of each item by performing
   layout as if it were an in-flow block-level box WITH THE USED MAIN SIZE AND THE GIVEN AVAILABLE SPACE,
   TREATING AUTO AS FIT-CONTENT", and the last clause is what makes this ONE function with TWO arms rather
   than one measurement with a property name swapped: css-sizing-3 §3.2 "Sizing Values: the
   <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content values" spells
   the fit-content size as "min(max-content, max(min-content, stretch))", and that expression COLLAPSES in one
   dimension and not in the other.
     - THE BLOCK ARM IS ONE CALL AND THE COLLAPSE IS WHY. §3.2 says of `min-content` and again of
       `max-content` that "for a box's block size, unless otherwise specified, this is equivalent to its
       automatic size", so both intrinsic terms of the formula are ONE number A and
       `min(A, max(A, stretch))` is A for every value of the stretch term. The available space therefore does
       not enter the block arm at all, which is why this function reads it only on the other one, and A is
       core/layout/used_value.h's `used_value_block_level_content_px` — CSS 2.1 §10.6 "Calculating heights and
       margins"' and §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'"' rules run over a box
       read as CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow"', which is what
       css-flexbox-1 §9.4 "Cross Size Determination"' step 7 means by "as if it were an in-flow block-level
       box" and which this file must not copy, §10.7's percentage rule and its non-commutative min/max order
       included.
     - THE INLINE ARM IS THE FORMULA ITSELF, because the two intrinsic inline sizes are genuinely two numbers:
       css-sizing-3 §5.1 "Intrinsic Sizes" is the pair and core/layout/intrinsic_size.h measures it, and
       css-sizing-3 §2.1 "Auto Box Sizes" names the third term — the STRETCH-FIT inline size, "the size a box
       would take if its outer size filled the available space in the given axis", with its own Note that
       "for the inline axis, this is called the 'available width' in CSS2.1§10.3.5 and computed by the rules
       in CSS2.1§10.3.3". §10.3.3's equation over an item whose containing block IS the flex container is the
       container's inner cross size less that item's own cross-axis margins, border and padding, which is
       `avail` below. The formula is written in CSS 2.2 §10.3.5 "Floating, non-replaced elements"' order, as
       core/layout/used_value.c's `uv_shrink_to_fit_width` is and for its reason: §2.1's `clamp` spelling is
       the same function only while `min-content <= max-content`, which core/layout/intrinsic_size.c asserts
       at the read.
   THE AVAILABLE SPACE IS COMPUTED HERE AND NOT HANDED IN, WHICH IS WHY THE CONTAINER IS A PARAMETER — and
   the reason is one property pair rather than tidiness. Step 11's STRETCHED arm below subtracts the same two
   operands, so the obvious shape is to compute the subtraction once and give both arms the number; it is
   WRONG, because the two sections disagree about an `auto` cross margin. §8.3's stretched value is reached
   only where neither is `auto`, and CSS 2.1 §10.3.3 makes an `auto` margin ZERO under an `auto` size — so a
   shared expression would either refuse a page §10.3.3 answers or hand §8.3 a margin it has already excluded.
   `fx_cross_margins_stretch_fit` above is §10.3.3's rule and `fx_cross_margins` is the other.
   IT IS THE CONTAINER'S INNER CROSS SIZE AND NOT THE LINE'S, which is step 7's own operand — css-flexbox-1
   §9.4 "Cross Size Determination"' step 11's first arm says to "recalculate its cross size using the flex
   line’s cross size (rather than the flex container’s) as the available space", so the container's is
   what step 7 used. For the SINGLE-LINE container this
   component admits they are one number, which `fx_line_cross_size` below derives at length, and that is why
   one function answers both.
   A REPLACED ITEM NEEDS NO ARM OF ITS OWN HERE AND THAT IS THE FORMULA'S ARITHMETIC RATHER THAN AN OMISSION:
   css-sizing-3 §5.1 ends by making such a box's max-content size CSS 2.1 §10.3.2 "Inline, replaced elements"'
   answer — "a block-level or inline-level replaced element whose height or width behaves as auto is
   effectively defined to use its max-content size" — and core/layout/intrinsic_size.c returns that number as
   BOTH terms, so `min(max(A, avail), A)` is A whatever the available space is. The BLOCK arm refuses the same
   box by name inside `used_value_block_level_content_px`, because §10.6.2's ratio arm would divide by a width
   that is not the item's used main size. */
static CssPx fx_hypothetical_cross(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    CssPx inner;

    if (vertical) {
        inner = used_value_block_level_content_px(item, true);
    } else {
        IntrinsicInlineSizes in = intrinsic_inline_sizes(item);
        CssPx surround = css_px_add(fx_cross_border_padding(item, false),
                                    fx_cross_margins_stretch_fit(item, false));
        CssPx avail = css_px_max(css_px_sub(fx_line_cross_size(container, false), surround), css_px(0.0));

        inner = css_px_min(css_px_max(in.min_content, avail), in.max_content);
    }
    DCHECK(inner.px >= 0.0,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 7 produced a NEGATIVE hypothetical cross "
           "size. Every arm it can take floors at zero — css-sizing-3 §3.3 \"Box Edges for Sizing: the "
           "box-sizing property\" says of the `border-box` conversion that \"the content box width and height "
           "are calculated by subtracting the border and padding in the corresponding axis from the specified "
           "<length-percentage>, and flooring the result at zero (as the inner size of a box cannot be "
           "negative)\", and CSS 2.1 §10.6.3's walk is a running sum of used heights — so a negative here is "
           "a derivation that lost an operand rather than a page. "
           "THE INLINE ARM NEEDS NO FLOOR OF ITS OWN AND THAT IS THE FORMULA'S ARITHMETIC RATHER THAN A "
           "CLAMP LEFT OUT, the same argument core/layout/used_value.c's `uv_shrink_to_fit_width` makes: "
           "`max(min-content, available)` is at least the min-content size, which core/layout/intrinsic_size.c "
           "asserts non-negative, and the `min` with the max-content size cannot take it below that same "
           "floor while core/layout/intrinsic_size.c's own `min-content <= max-content` holds. So a negative "
           "on THIS arm is one of those two asserts having been bypassed rather than a narrow container. "
           "THE WORDING IS THE STANDARD'S CURRENT ONE, WHICH IS WORTH A CLAUSE BECAUSE THIS TREE SPENT EIGHT "
           "SITES ON A SUPERSEDED ONE: they rendered the same floor in css-ui-3 §3.1 \"Changing the Box "
           "Model: the box-sizing property\"'s words under a css-sizing-3 §3.3 citation. THAT IS A MIS-AIMED "
           "QUOTATION AND NOT A FABRICATION — css-ui-3 defined `box-sizing` before css-sizing-3 took it "
           "over, css-ui-4 records the handover in its own prose, and css-sizing-3 REWROTE the sentence, so "
           "the words were a real standard's and were owed to no citation here. All eight are repaired and "
           "core/layout/used_value.h holds the argument; what a reader re-deriving this needs is that §3.3's "
           "floor is a step of the `border-box` conversion while §3.1's is unconditional, so a floor that "
           "converts nothing cites §3.1");
    return inner;
}

/* THE SAME STEP PLUS css-sizing-3 §2.2 "Intrinsic Size Contributions"' OUTER STEP, which is what §9.4's step
   8.2 takes the largest of: "Among all the items not collected by the previous step, find the largest outer
   hypothetical cross size." It is a second function rather than a flag because §9.4's two consumers of step 7
   want DIFFERENT boxes — step 8.2 the outer one and step 11's second arm the INNER one — and a caller that
   took the wrong one would report an item's margins as part of its own cross size. */
static CssPx fx_outer_hypothetical_cross(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    return css_px_add(fx_hypothetical_cross(container, item, vertical),
                      css_px_add(fx_cross_border_padding(item, vertical), fx_cross_margins(item, vertical)));
}

/* §4 "Flex Items"' ANONYMOUS FLEX ITEM'S OUTER HYPOTHETICAL CROSS SIZE — §9.4's step 7 for the one box a
   flex container's item list holds that is not an element, so `fx_outer_hypothetical_cross` cannot be asked
   for it and this is not a second copy of that function but the SAME step over a different box.
   THE TWO HALVES THE ELEMENT ARM GETS FROM core/layout/used_value.h ARE BOTH ANSWERED HERE, and each is a
   sentence rather than a simplification.
     - THE INNER HALF. Step 7's own words are "performing layout as if it were an in-flow block-level box WITH
       THE USED MAIN SIZE and the given available space, treating auto as fit-content" — and §4 makes this box
       an "anonymous block container", whose content is a CHILD TEXT SEQUENCE and therefore holds no
       block-level box at all. So CSS 2.2 §9.4.2 "Inline formatting contexts" is the formatting context by
       §9.4.1's own alternative, CSS 2.1 §10.6.3's first bullet is the whole height rule — "the distance
       from its top content edge to the first applicable of the following", whose first bullet is "the bottom
       edge of the last line box" — and core/layout/line_box.h answers it.
       THE WIDTH IS STATED AND THAT IS THE WHOLE POINT OF THE ARGUMENT: `style` is the CONTAINER, because §4's
       box is unstyleable and therefore has the container's properties, and the container's content box is the
       WHOLE LINE rather than this item's share of it. Handing the walk `style` alone would lay the run out at
       the container's inner main size, which equals this item's used main size only where §9.7 "Resolving
       Flexible Lengths" happened to flex it to the entire line.
     - THE OUTER HALF IS ZERO AND IS NOT READ. css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer step
       adds the box's margin, border and padding, and §4's box has none to add: it is anonymous, so
       CSS 2.2 §9.2.1.1 "Anonymous block boxes"' rule applies — "non-inherited properties have their initial
       value" — and every one of CSS 2.1 §8's four edges is initially zero on all four sides. Reading them off
       the CONTAINER would report the container's own padding as this item's, which is the same mistake in the
       cross axis that core/layout/flex_line.c refuses in the main one by answering zero for a NULL element. */
static CssPx fx_anonymous_outer_hypothetical_cross(lxb_dom_element_t *container, lxb_dom_node_t *first,
                                                   lxb_dom_node_t *end, CssPx used_main)
{
    bool any_line_box;
    CssPx first_baseline, last_baseline, inner;
    BlockFlowRun seq;
    char nbuf[160];

    /* §4's CHILD TEXT SEQUENCE AS core/layout/block_flow.h's HALF-OPEN RUN, built in ONE place and from the
       two nodes the caller's own walk already holds: `after` is the sibling BEFORE the sequence (NULL where it
       opens the container's content) and `end` is one past its last text node, which is exactly the form
       core/layout/flex_line.c hands the same sequence to the intrinsic pass. */
    seq.after = first->prev;
    seq.end = end;
    inner = line_box_content_height(container, seq, line_box_available_width_stated(used_main),
                                    &any_line_box, &first_baseline, &last_baseline);

    DCHECKF(any_line_box,
            "%s: css-flexbox-1 §4 \"Flex Items\"' ANONYMOUS FLEX ITEM was measured and CSS 2.2 §9.4.2 "
            "\"Inline formatting contexts\" reports that it contains NO LINE BOX. Two of this engine's own "
            "components have then disagreed about one child text sequence: §4's classification already "
            "answered FLEX_ITEM_CHILD_NONE for a sequence that \"contains only document white space "
            "characters\", so a sequence that reached this measurement holds at least one character that is "
            "not one — and CSS 2.2 §9.4.2's zero-height rule begins \"line boxes that contain NO TEXT\", "
            "which such a line does not satisfy. So this is core/layout/flex_item.c's white-space test and "
            "core/layout/line_box.c's existence test reading one run differently, not a page",
            box_subject_node(first, nbuf, sizeof nbuf));
    DCHECK(inner.px >= 0.0,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 7 produced a NEGATIVE hypothetical cross "
           "size for §4 \"Flex Items\"' anonymous flex item. CSS 2.1 §10.6.3's first bullet is a running sum "
           "of line box heights and CSS 2.2 §10.8's step 3 makes each of those \"the distance between the "
           "uppermost box top and the lowermost box bottom\" — which core/layout/line_box.c asserts "
           "non-negative at its own origin — so a negative here is a derivation that lost an operand rather "
           "than a page");
    /* §4's box has no margin, border or padding to add, so the INNER size IS the outer one — see the banner. */
    return inner;
}

/* THE THREE PRECONDITIONS BOTH ENTRIES OF THIS COMPONENT STATE, ASKED IN ONE PLACE — that it IS a flex
   container, that its block axis is the VERTICAL one, and that it is SINGLE-LINE. They were written inside
   `flex_cross_size_content_based` while that was the only entry, and they are asked here rather than copied
   because they are one FACT about the container and not one entry's question about it: css-flexbox-1 §9.4
   "Cross Size Determination"' step 8 and its step 11 are two steps of ONE algorithm over ONE container, so a
   second copy is two lists that can come apart and the one that drifts is whichever entry a later reader does
   not open.
   THERE WERE FOUR AND THE ONE THAT LEFT IS THE MAIN AXIS — a refusal of every container whose main axis was
   not its INLINE one, which is to say of every `column` container. It was never a fact about the CONTAINER at
   all: it was this component reading `margin-top`, `padding-top` and the top border width as the cross-axis
   edges unconditionally, so it held exactly while the cross axis was the vertical one. Those reads now come
   from css-writing-modes-4 §7.2 "Dimensional Mapping" through `fx_cross_props` above, so §9.4's steps 7 and
   11 answer WHICHEVER axis §5.1 "Flex Flow Direction: the flex-direction property" makes the cross one —
   which is what that section says, since it states one mapping and not two algorithms.
   WHAT DID NOT MOVE IS THE ENTRY-SPECIFIC HALF, and `flex_cross_size_content_based` states its own below:
   step 8's walk is reached on the BLOCK axis and on no other, because its consumer is.
   THE MESSAGES NAME THE SECTION AND NOT THE CALLER, WHICH IS WHAT ASKING HERE COSTS AND IS WORTH IT: a
   refusal reading "the number THIS ENTRY answers" was true while there was one entry, and a step 11 caller
   meeting it would read it as a claim about step 8. */
static void fx_require_supported_container(lxb_dom_element_t *container)
{
    char nbuf[160], wbuf[64];
    char *display;
    bool is_container;

    DCHECK(container != NULL,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' cross sizing was asked for with no CONTAINER "
           "element");
    display = css_computed_value(container, "display");
    DCHECK(display != NULL, "the cascade produced no computed `display` for a box a layout is walking");
    is_container = flex_item_display_is_flex_container(display);
    free(display);
    DCHECK(is_container,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' cross sizing was asked of a box that is "
           "not a FLEX CONTAINER. §3 \"Flex Containers: the flex and inline-flex display values\" is the two "
           "spellings and core/layout/flex_item.h decides them; every property this component reads has a "
           "flex container or a flex item on its `Applies to:` line, so on any other box the walk below would "
           "be reading the cascade's initial keywords and reporting them as a layout");
    /* css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property" is what makes the CROSS axis
       of a `row` container the VERTICAL one, and it is asked FIRST and refused rather than assumed. It is
       re-asked here rather than taken on trust from the caller because this header states it as this
       component's own precondition and every physical edge below depends on it.
       THE CLAUSE THAT STOOD HERE NAMED css-writing-modes-4 §7.4 "Flow-Relative Mappings" AS THE ONE ABSENT
       CAPABILITY THIS AND TWO OTHER COMPONENTS WERE WAITING ON, AND THAT CITATION WAS MIS-AIMED. §7.4 is not
       a mapping table: it is the rule deciding WHOSE writing mode a flow-relative question is read against —
       "Flow-relative directions are calculated with respect to the writing mode of the CONTAINING BLOCK of
       the box" for a box's layout within it, and the box's own for rules about its contents. The TABLE is
       §6.4 "Abstract-to-Physical Mappings", and core/css/css_logical.c has held it, transcribed cell by cell
       and asserted by `css_logical_init`, for longer than these refusals have stood; its DIMENSION rows are
       now exported as `css_logical_axis_is_vertical` and composed with §5.1 by
       `flex_container_axis_is_vertical` (core/layout/flex_item.h), which core/layout/used_value.c routes on.
       THE CLAUSE THAT FOLLOWED SAID THE TWO QUESTIONS BELOW WERE TWO AND STAYED TWO — that the FIRST was
       that the CROSS axis is the vertical one, deciding whether `margin-top` and `padding-top` are the edges
       this walk adds, and the SECOND that the vertical axis is the BLOCK dimension, deciding whether
       `used_value_block_level_content_px` is the right measurement at all. IT IS ONE QUESTION NOW AND THE
       FIRST HALF IS GONE: the edges come from css-writing-modes-4 §7.2 "Dimensional Mapping" through
       `fx_cross_props` above, so this component reads whichever pair the cross axis names and asks nothing
       about which axis that is. WHAT IS LEFT IS THE SECOND HALF ALONE, and it is why this refusal stays: a
       `horizontal-tb` container is one whose BLOCK dimension is the vertical axis and whose INLINE dimension
       is the horizontal one, so the single `vertical` bit this component threads names a PHYSICAL axis and a
       DIMENSION at the same time — which is what lets `fx_hypothetical_cross` above pick §10.6's rules on one
       arm and §10.3's on the other from one operand. In a `vertical-rl` mode those two facts come apart and
       one bit would answer for both, which is exactly the state this refusal keeps out. */
    if (!fx_computed_is(container, "writing-mode", "horizontal-tb"))
        DFAILF("%s, computed `writing-mode` `%s`: this FLEX CONTAINER's BLOCK dimension is not the vertical "
               "axis, so the ONE bit this component threads cannot name both — css-writing-modes-4 §7.2 "
               "\"Dimensional Mapping\" reads it as `which physical pair of edges is the cross axis's` and "
               "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 7 reads it as `which of CSS 2.1's two "
               "sizing chapters measures this item`, and in a `horizontal-tb` mode those are the same bit. A `vertical-rl` `column` container has its CROSS axis vertical and "
               "its INLINE dimension vertical too, so step 7 owes it css-sizing-3 §5.1 \"Intrinsic Sizes\"' "
               "pair measured in the VERTICAL axis while the edges it adds are `margin-top` and "
               "`padding-top` — one arm of this component's own dispatch taken from each answer. "
               "TWO REMEDIES HAVE BEEN RETIRED HERE AND BOTH ARE WRITTEN OUT BECAUSE A READER RE-DERIVES "
               "THEM. The first said to BUILD css-writing-modes-4 §7.4 \"Flow-Relative Mappings\", and that "
               "citation was MIS-AIMED: §7.4 is the rule for WHOSE writing mode a flow-relative question is "
               "read against, and the TABLE is §6.4 \"Abstract-to-Physical Mappings\", which "
               "core/css/css_logical.c holds and whose dimension rows are exported as "
               "`css_logical_axis_is_vertical`. The second said to BUILD §9.4's STEPS 7, 8 AND 11 READ IN "
               "THE OTHER DIMENSION, naming css-sizing-3 §5.1's two intrinsic sizes as what CSS 2.1 §10.3 "
               "\"Calculating widths and margins\" is to §10.6 \"Calculating heights and margins\" — and "
               "step 7's INLINE arm and step 11 ARE BUILT, over exactly that pair, so a reader who follows "
               "it builds them a second time. WHAT IS ACTUALLY MISSING IS THAT `fx_cross_props` AND "
               "`fx_hypothetical_cross` TAKE ONE `vertical` WHERE THIS BOX NEEDS TWO: split it into the "
               "PHYSICAL axis §6.4 answers and the DIMENSION §7.2 answers, hand each to the half that reads "
               "it, and measure the intrinsic pair in the dimension rather than in the inline axis — "
               "core/layout/intrinsic_size.h is named for the INLINE one and says so",
               box_subject(container, nbuf, sizeof nbuf),
               box_subject_computed(container, "writing-mode", wbuf, sizeof wbuf));
    /* §5.2 "Flex Line Wrapping: the flex-wrap property"' other arm, refused for TWO reasons that would each
       be enough on its own — which is why it is refused here as well as at core/layout/flex_line.c, where
       only the first of them applies. */
    if (flex_container_is_multi_line(container))
        DFAILF("%s: this FLEX CONTAINER is MULTI-LINE (css-flexbox-1 §5.2 \"Flex Line Wrapping: the flex-wrap "
               "property\"; §6 \"Flex Lines\" makes `wrap` and `wrap-reverse` one case against `nowrap`), so "
               "§9.6 \"Cross-Axis Alignment\"' \"use the sum of the flex lines' cross sizes\" is a sum over "
               "SEVERAL lines and the walk below computes ONE. TWO THINGS ARE MISSING AND ONLY THE FIRST OF "
               "THEM IS core/layout/flex_line.c's: §9.3 \"Main Size Determination\"' step 5 must BREAK the "
               "items into lines before any of them has a used main size, and §9.4 \"Cross Size "
               "Determination\"' step 8 must then be run PER LINE, because its own step 8.3 takes the largest "
               "outer hypothetical cross size among the items OF ONE LINE — a single maximum over every item "
               "in the container would report the tallest item's height as the height of every line. §9.4's "
               "step 9 becomes reachable at the same moment and is unreachable now: \"If the flex container "
               "has a definite cross size, align-content is stretch, and the sum of the flex lines' cross "
               "sizes is less than the flex container's inner cross size, increase the cross size of each "
               "flex line by equal amounts\" — a condition on a DEFINITE cross size, which is the one thing "
               "a content-based cross size is asked for exactly because the container does not have",
               box_subject(container, nbuf, sizeof nbuf));
}

CssPx flex_cross_size_content_based(lxb_dom_element_t *container)
{
    lxb_dom_node_t *c;
    CssPx largest = css_px(0.0);
    char nbuf[160];

    fx_require_supported_container(container);
    /* THIS ENTRY'S OWN PRECONDITION, WHICH ITS SIBLING DOES NOT SHARE: the CROSS axis is the BLOCK one, so
       this walk is a `row` container's and a `column` container is refused. It is NOT a narrowing of §9.4's
       step 8, which is stated over whichever axis is the cross one exactly as step 11 is — it is a statement
       about who ASKS. §9.6 "Cross-Axis Alignment"' step reaches this entry only where a CONTENT-BASED cross
       size is needed, and for a `column` container the cross size is a WIDTH whose rules of the formatting
       context are CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow"' constraint equation —
       a number that needs no content, so `block_flow_auto_height`'s dispatch never reaches here with one and
       core/layout/used_value.c's own route for it returns before this component is entered.
       SO BUILDING THE WALK IN THE INLINE DIMENSION WOULD BE BUILDING A WRITE WITH NO READER, and the one
       thing that would create a reader is named rather than left to be found: a `column` container that is a
       FLOAT, an INLINE-BLOCK or absolutely positioned has a SHRINK-TO-FIT width, whose two terms are
       css-flexbox-1 §9.9.2 "Flex Container Intrinsic Cross Sizes" and not this step — so what a reader
       arriving here should build is that section, in core/layout/flex_intrinsic_size.c beside §9.9.1's
       inline arm, and this walk becomes its operand rather than its replacement.
       HOW ITS ABSENCE SHOWS: a `column` flex container that is a float or an inline-block, asked for its
       width, refuses inside core/layout/intrinsic_size.c — which measures line boxes and says in its own
       words that a flex container is not a block container — rather than anywhere in this file. */
    if (!flex_container_axis_is_vertical(container, FLEX_AXIS_CROSS))
        DFAILF("%s: this FLEX CONTAINER's CROSS axis is its INLINE axis (css-flexbox-1 §5.1 \"Flex Flow "
               "Direction: the flex-direction property\": a `column` container's main axis \"has the same "
               "orientation as the block axis of the current writing mode\"), so a CONTENT-BASED cross size "
               "for it is a WIDTH — and §9.6 \"Cross-Axis Alignment\"' step asks for one only where the "
               "rules of the formatting context need it, which for a block-level container is CSS 2.1 "
               "§10.3.3 \"Block-level, non-replaced elements in normal flow\"' constraint equation and "
               "therefore never. THE STEP 8 WALK BELOW IS NOT WHAT IS MISSING FOR THIS BOX: §9.4 \"Cross "
               "Size Determination\"' step 7 reads either dimension now and step 11 answers this "
               "container's items, so a reader who builds this walk in the inline dimension has built a "
               "measurement with no consumer. BUILD css-flexbox-1 §9.9.2 \"Flex Container Intrinsic Cross "
               "Sizes\" INSTEAD — \"The min-content/max-content cross size of a single-line flex container "
               "is the largest min-content contribution/max-content contribution (respectively) of its flex "
               "items.\" — in "
               "core/layout/flex_intrinsic_size.c beside §9.9.1 \"Flex Container Intrinsic Main Sizes\"' "
               "inline arm, because a SHRINK-TO-FIT `column` container is the one box that asks this "
               "question and it asks it as an intrinsic size rather than as §9.6's sum",
               box_subject(container, nbuf, sizeof nbuf));
    /* §9.4's step 8 HAS AN ARM FOR THE DEFINITE CASE — "If the flex container is single-line and has a
       definite cross size, the cross size of the flex line is the flex container's inner cross size" — AND
       THERE IS NO PRECONDITION HERE THAT RULES IT OUT, which is worth writing down because the obvious
       assert is wrong and was written and removed rather than merely not written. `used_value_height_behaves_
       as_auto` looks like the predicate that says this entry is the right one, and it is not: this entry's
       caller is core/layout/block_flow.h's AUTOMATIC block size, which core/layout/used_value.c asks for at
       TWO places, and only one of them is the `auto` arm. The other is css-sizing-3 §3.2 "Sizing Values: the
       <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content values"'
       `min-content`/`max-content` keyword on a `min-height` or a `max-height`, whose own sentence is "for a
       box's block size, unless otherwise specified, this is equivalent to its automatic size" — so
       `min-height: max-content` on a flex container with a DECLARED `height` reaches here with a definite
       cross size in hand and wants this walk anyway, because what it asked for is the AUTOMATIC size and the
       automatic size is the one computed with the cross axis treated as indefinite. An assert on the
       declaration would have fired on that page and named a defect that is not there.
       WHAT DOES DECIDE IT IS §9.6's OWN CONDITIONAL and it is the caller's to read: "If a content-based cross
       size is needed, use the sum of the flex lines' cross sizes." Both callers need one. */
    for (c = lxb_dom_interface_node(container)->first_child; c != NULL; ) {
        lxb_dom_node_t *next = c->next;
        FlexItemChildKind kind = flex_item_child_kind(container, c);

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        switch (kind) {
        case FLEX_ITEM_CHILD_TEXT: {
            /* css-flexbox-1 §4 "Flex Items"' ANONYMOUS FLEX ITEM — "each child text sequence is wrapped in an
               anonymous block container flex item" — which §9.4's step 8 needs an OUTER HYPOTHETICAL CROSS
               SIZE for exactly like every other item on the line, and which used to crash here for two
               reasons that were BOTH about naming rather than about measuring.
               (1) ITS USED MAIN SIZE COULD NOT BE ASKED FOR. core/layout/flex_line.h had always COLLECTED
               this box and flexed it — §9.7 "Resolving Flexible Lengths" gives it a target main size like any
               other item — and only its LOOKUP was keyed on an element, so the number existed and had no
               name. It is now named by the item's FIRST NODE, which is this text node: §4's box is anonymous,
               so a DOM node the container's own child list holds is the only identity available that cannot
               collide with another item's.
               (2) ITS CROSS SIZE IS A RUN OF LINE BOXES AT THAT WIDTH, and core/layout/line_box.h derived the
               width from the element it was GIVEN — which here is the CONTAINER, whose content box is the
               whole line. That entry now takes §9.4.2's line box width as an argument, and this is the caller
               that states one.
               THE RUN IS §4's CHILD TEXT SEQUENCE AND IS DELIMITED THE SAME WAY core/layout/flex_line.c
               DELIMITS IT, through the same entry over the same child list, because it is the same sequence —
               and `next` IS that delimiter, so the node this walk advances to and the node the measurement
               stops at are ONE value rather than two reads of one question. */
            CssPx used_main;

            next = flex_item_text_sequence_end(container, c);
            /* §9.4's step 8.1 IS ASKED OF THIS BOX TOO — §8.3 "Cross-axis Alignment: the align-items and
               align-self properties" says `align-items` sets the default "for all of the flex container's
               items, including anonymous flex items", so the container's keyword reaches it and a baseline
               one puts it in the first collection rather than in the maximum below. */
            fx_require_no_baseline_alignment(container, NULL, c);
            used_main = flex_line_used_main_size(container, c);
            largest = css_px_max(largest,
                                 fx_anonymous_outer_hypothetical_cross(container, c, next, used_main));
            break;
        }
        case FLEX_ITEM_CHILD_ELEMENT: {
            lxb_dom_element_t *item = lxb_dom_interface_element(c);

            next = c->next;
            /* §9.4's step 10 is this component's own and it is unbuilt, which is why the refusal is here and
               not a second copy of core/layout/flex_line.c's: that file refuses a collapsed item because the
               used main size it would answer is the FIRST round's, and this one refuses it because step 8
               owes the line a STRUT SIZE that only the second round produces. */
            if (flex_item_is_collapsed(item))
                DFAILF("%s: this flex item's computed `visibility` is `collapse`, so css-flexbox-1 §4.4 "
                       "\"Collapsed Items\" makes it a COLLAPSED FLEX ITEM and css-flexbox-1 §9.4 \"Cross "
                       "Size Determination\"' step 10 owns it — \"note the cross size of the line they're in "
                       "as the item's strut size, and restart layout from the beginning\". THE STRUT IS WHY "
                       "THIS IS STEP 8's PROBLEM AND NOT ONLY §9.3's: step 10's second round ignores the "
                       "collapsed items \"entirely (as if they were display:none) except that after "
                       "calculating the cross size of the lines, if any line's cross size is less than the "
                       "largest strut size among all the collapsed items in the line, set its cross size to "
                       "that strut size\" — so the line's cross size is a FLOOR taken from a number this "
                       "walk's own first round produces, and answering from one round drops it. BUILD STEP "
                       "10's TWO ROUNDS HERE: this walk is the first of them, what it owes is to record each "
                       "collapsed item's line cross size and run itself again with those items at zero main "
                       "size and out of the maximum below",
                       box_subject(item, nbuf, sizeof nbuf));
            fx_require_no_baseline_alignment(container, item, c);
            /* THE AXIS IS A LITERAL `true`, which is the refusal at the top of this entry spent rather
               than a shortcut: this walk runs on the BLOCK cross axis only. */
            largest = css_px_max(largest, fx_outer_hypothetical_cross(container, item, true));
            break;
        }
        case FLEX_ITEM_CHILD_NONE:
            /* UNREACHABLE BY THE GUARD ABOVE, and asserted rather than left to fall through: the tail counts
               a member, and an item this walk cannot classify is one whose cross size would silently not be
               in the maximum — which is the line reported SHORTER than the page draws. The arm exists
               because `-Wswitch` is the forcing function this walk relies on, exactly as
               core/layout/flex_line.c's does. */
            DFAIL("css-flexbox-1 §4 \"Flex Items\"' classification answered FLEX_ITEM_CHILD_NONE for a child "
                  "this walk had already decided to measure. The loop skips that value before it reaches this "
                  "switch, so two readings of one child list disagreed — which means the child list changed "
                  "under the walk");
            break;
        /* NO `default` ARM, DELIBERATELY, for core/layout/flex_line.c's reason: `-Wswitch` is the forcing
           function, and a fourth `FlexItemChildKind` is a fourth kind of member of every flex container's
           item list — §9.4's step 8 is a MAXIMUM over a line, so a member this walk cannot classify is one it
           would silently leave out of it. */
        }
        DCHECK(next != c,
               "css-flexbox-1 §4 \"Flex Items\"' item walk did not advance past the child it had just "
               "measured, so this walk would add the same flex item to the maximum for ever. A text sequence "
               "always contains at least the node it was delimited from, and an element item always advances "
               "by one sibling");
        c = next;
    }
    /* §9.4's STEP 8.3 — "The used cross size of the flex line is the largest of the numbers found in the
       previous two steps and zero" — where the first of those two steps is the baseline collection the walk
       above refuses and the second is the maximum it accumulated. The zero is the section's own third
       operand and is therefore also the answer for a container with no items at all, which §6 "Flex Lines"
       admits by name: "Every line contains at least one flex item, unless the flex container itself is
       completely empty."
       §9.6's SUM IS THIS ONE NUMBER because §5.2's other arm is refused above, and it is stated rather than
       written as a loop of one so that a reader adding line breaking finds the sum missing rather than
       finding a loop that happens to run once.
       THERE IS NO ASSERT OVER THE RESULT AND THAT IS A DECISION. The obvious one — that the answer is
       non-negative — has no value of this program that could fail it: `largest` starts at §9.4's step 8.3 zero
       and the only thing done to it is a maximum WITH that zero, so the two sides of such a check cannot
       disagree and it would certify the walk without examining it. The invariant that is NOT vacuous is one
       step down and is asserted there, over the per-item hypothetical cross size this walk did not floor. */
    return largest;
}

CssPx flex_cross_size_used_item_cross(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    bool vertical;
    CssPx line, surround;

    fx_require_supported_container(container);
    /* WHICH PHYSICAL AXIS THE CROSS AXIS IS, ASKED ONCE AND THREADED WHOLE — css-flexbox-1 §5.1 "Flex Flow
       Direction: the flex-direction property"' mapping composed with css-writing-modes-4 §6.4
       "Abstract-to-Physical Mappings"' dimension rows, which is what `flex_container_axis_is_vertical`
       (core/layout/flex_item.h) is. It is asked of the CONTAINER and not of the item, which is §7.4
       "Flow-Relative Mappings"' own rule — a box's layout within its containing block is read against the
       containing block's writing mode, and §4 "Flex Items" makes the flex container the containing block of
       its items. `fx_require_supported_container` above has already refused every writing mode in which this
       one bit does not also name the DIMENSION. */
    vertical = flex_container_axis_is_vertical(container, FLEX_AXIS_CROSS);
    DCHECK(item != NULL,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for with no item element");
    DCHECK(lxb_dom_interface_node(item)->parent == lxb_dom_interface_node(container),
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for an item that is not a "
           "child of the container it was asked about. Step 11's operand is \"the flex line's cross size\", "
           "so a subject drawn from one container and a line drawn from another is an item sized against a "
           "line it is not on — the same precondition core/layout/flex_line.h states for the main axis, and "
           "the one that makes `css-display-3 §2.5 \"Box Generation: the none and contents keywords\"' "
           "`contents` splice a case this component has not been handed rather than one it answers wrongly");
    /* css-sizing-3 §3.2.1 "“Behaving as auto”" IS ONE QUESTION WITH TWO SPELLINGS AND THE AXIS PICKS THE
       SPELLING, which is a fact about §3.2.1 rather than a narrowing here: it exists "to have a common term
       for both when width/height computes to auto and when it is defined to behave as if auto were specified
       (as in the case of BLOCK PERCENTAGE HEIGHTS resolving against an indefinite size, see CSS2§10.5)", and
       the parenthesis is the ONLY case it names. A percentage INLINE size has no such case — CSS 2.1 §10.2
       "Content width: the 'width' property" resolves it against the containing block's width, which is
       definite wherever this component runs — so on the inline axis behaving as auto IS computing to `auto`
       and the computed-length keyword test is the whole of it. Asking
       `used_value_height_behaves_as_auto` on either axis would have read the item's HEIGHT, which for a
       `column` container's item is its MAIN size. */
    DCHECK(vertical ? used_value_height_behaves_as_auto(item)
                    : css_computed_length_is(item, fx_cross_props(false).size, "auto"),
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for an item whose cross size "
           "property DOES NOT behave as `auto`. This entry answers one arm of step 11 and its header says "
           "which: the other arm — \"Otherwise, the used cross size is the item's hypothetical cross size\" — "
           "is, for a declared cross size, step 7's own layout of that declaration \"as if it were an in-flow "
           "block-level box\", which CSS 2.1 §10.6.2 and §10.6.3 make the declared value itself in the BLOCK "
           "dimension and CSS 2.1 §10.3.3 \"Block-level, non-replaced elements in normal flow\" makes it in "
           "the INLINE one, every rule either of them states being conditioned on the property being `auto`. "
           "core/layout/used_value.c computes that for every other box already and FALLS THROUGH to it, so an "
           "item arriving here with a declaration is that fall-through having been lost and the number this "
           "entry would answer is the LINE's cross size reported as the item's");
    /* STEP 11's FIRST ARM — "If a flex item's cross size depends on the available space in the cross axis,
       recalculate its cross size using the flex line's cross size (rather than the flex container's) as the
       available space." §8.3 "Cross-axis Alignment: the align-items and align-self properties" is what makes
       an item's cross size depend on that space, and it names the state: "the flex item is STRETCHED. Its
       used value is the length necessary to make the cross size of the item's MARGIN BOX as close to the
       same size as the line as possible."
       SO THE SUBTRACTION IS THE MARGIN BOX'S AND NOT THE BORDER BOX'S, which is the whole of the arithmetic:
       the margins are subtracted alongside the border and padding because §8.3 equalises the MARGIN box, and
       an item with a 10px cross margin under a 100px line has an 80px content box rather than a 90px one.
       THE FLOOR IS §8.3's OWN "AS CLOSE … AS POSSIBLE" and not a defensive clamp: an item whose surround
       alone exceeds the line cannot reach it, and the nearest cross size it can take is zero — which
       css-sizing-3 §3.1 "Sizing Properties" states unconditionally in the same words, "the inner size is
       always floored at zero". THE CITATION USED TO READ §3.3 "Box Edges for Sizing: the box-sizing
       property" AND QUOTE ITS `border-box` FLOOR, and that is the mis-aim this file's own step 7 assert
       already records at length: §3.3's floor is a STEP of the `border-box` conversion and this subtraction
       converts nothing, so an unconditional floor cites §3.1.
       THE MIN/MAX CLAMP §8.3's NEXT CLAUSE NAMES IS NOT HERE AND THE HEADER SAYS WHY — CSS 2.1 §10.7's three
       steps are that clamp and they run over this number at the caller.
       THE TWO OPERANDS ARE READ INSIDE THIS ARM AND NOT ABOVE THE `if`, which is the shape a reader will
       want to change and must not: `fx_cross_margins` REFUSES an `auto` cross margin by name, and an `auto`
       cross margin is one of the three things §8.3 tests for — so hoisting the read would crash on exactly
       the page this branch answers NO for. Step 7's own subtraction is inside `fx_hypothetical_cross`, over
       §10.3.3's auto-as-zero rule instead. */
    if (fx_is_stretched(container, item, vertical)) {
        line = fx_line_cross_size(container, vertical);
        surround = css_px_add(fx_cross_border_padding(item, vertical), fx_cross_margins(item, vertical));
        return css_px_max(css_px_sub(line, surround), css_px(0.0));
    }
    /* STEP 11's SECOND ARM — "Otherwise, the used cross size is the item's HYPOTHETICAL CROSS SIZE" — which is
       step 7. This is the INNER half of what `fx_outer_hypothetical_cross` above returns rather than a second
       reading of it: that function adds css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer step because
       step 8.2 takes the largest OUTER hypothetical cross size, and step 11 gives the item its own cross
       size, which is the inner one. Two callers of one function, each adding what its own step asks for.
       THE CONTAINER IS HANDED OVER BECAUSE STEP 7 READS IT — its own "given available space", which on the
       inline axis is an operand of css-sizing-3 §3.2's fit-content formula and on the block axis is not read
       at all. */
    return fx_hypothetical_cross(container, item, vertical);
}
