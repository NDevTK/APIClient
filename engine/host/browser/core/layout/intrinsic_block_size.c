/* css-sizing-3 §3.2's intrinsic size in a box's BLOCK axis. See intrinsic_block_size.h for why it is one number
   rather than a pair, why the entry it reaches is `block_flow_auto_height` rather than the used-value one, and
   why it is a component rather than a function inside either of its two consumers. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/intrinsic_block_size.h"
#include "core/layout/line_box.h"
#include "core/layout/replaced_element.h"

/* THE BLOCK→PHYSICAL SUBSTITUTION THIS COMPONENT MAKES, ASSERTED WHERE IT IS MADE. Both entries reach a walk
   that stacks children DOWNWARD, which is CSS 2.1 §10.6's rules on the VERTICAL axis — and
   css-writing-modes-4 §7.2 "Dimensional Mapping" puts them on the BLOCK dimension, not on the vertical one:
   "the calculation rules in CSS2.1 Section 10.6 are used in the block dimension". The two coincide in a
   `horizontal-tb` mode and in no other, css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode
   property" being what makes the block flow direction a property at all.
   WHAT IS MISSING IS THE WALK AND NOT THE MAPPING, which is worth naming because the mapping is the thing a
   reader reaches for: core/css/css_logical.c holds css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings" and
   exports `css_logical_axis_is_vertical`, so composing the axis question is a call. What no component here can
   do is run CSS 2.1 §10.6.3's stack ALONG A LINE — core/layout/block_flow.c stacks downward and
   core/layout/intrinsic_size.c measures across, so each is one physical direction's algorithm wearing a logical
   name. BUILD either one over the other direction and this refusal becomes a third value of an axis parameter,
   which is the same sentence core/layout/flex_line.c's own refusal makes about the same two walks. */
static void ibs_require_horizontal_tb(lxb_dom_element_t *el)
{
    char *wm;
    bool horizontal;
    char nbuf[160], wbuf[64];

    wm = css_computed_value(el, "writing-mode");
    DCHECK(wm != NULL,
           "the cascade produced no computed `writing-mode` — css-writing-modes-4 §3.2 \"Block Flow Direction: "
           "the writing-mode property\" gives it an initial value of `horizontal-tb`, so the last layer of the "
           "cascade always answers");
    horizontal = strcmp(wm, "horizontal-tb") == 0;
    free(wm);
    if (horizontal) return;
    DFAILF("%s, computed `writing-mode` `%s`: this box's BLOCK axis is not the vertical one, so the automatic "
           "size css-sizing-3 §3.2 \"Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, "
           "min-content, max-content, and fit-content values\" sends both intrinsic keywords to in the BLOCK "
           "axis is not the number this component measures. css-writing-modes-4 §7.2 \"Dimensional Mapping\" "
           "puts CSS 2.1 §10.6's calculation rules on the BLOCK dimension — \"the calculation rules in CSS2.1 "
           "Section 10.6 are used in the block dimension\" — and the walk this component reaches for stacks "
           "children DOWNWARD, so in a vertical writing mode it would report this box's INLINE size under its "
           "block size's name. THE MAPPING IS NOT WHAT IS ABSENT: core/css/css_logical.c holds "
           "css-writing-modes-4 §6.4 \"Abstract-to-Physical Mappings\" and exports "
           "`css_logical_axis_is_vertical`. WHAT IS ABSENT IS A DIMENSION-PARAMETERISED READING OF THE TWO "
           "WALKS THEMSELVES — core/layout/block_flow.c stacks children downward and "
           "core/layout/intrinsic_size.c measures a pair by laying line boxes out horizontally — so BUILD "
           "either one over the other direction and this refusal becomes a third value of an axis parameter. "
           "core/layout/flex_line.c refuses the same box for the same two walks one question later",
           box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "writing-mode", wbuf, sizeof wbuf));
}

CssPx intrinsic_block_size(lxb_dom_element_t *el)
{
    char nbuf[160];

    DCHECK(el != NULL, "css-sizing-3 §3.2's intrinsic block size was asked for with no element");
    ibs_require_horizontal_tb(el);
    /* A REPLACED BOX IS A DIFFERENT SECTION AND `block_flow_auto_height` CANNOT REFUSE IT, which is why the
       classification is this entry's and not that walk's. That walk's own guard is over the box being a BLOCK
       CONTAINER, and css-flexbox-1 §4 "Flex Items" blockifies every flex item — so an `img` item computes
       `display: block`, passes that guard, and is walked for BLOCK-LEVEL CHILDREN IT HAS NONE OF, which
       answers ZERO for a box whose automatic block size is its natural height. That is the one shape this
       measurement can get wrong while returning a number, and that entry's header says the classification is
       the caller's: it walks `el`'s CHILDREN and asks nothing about what kind of box they belong to.
       THE OTHER AXIS REFUSES THE SAME BOX AT THE SAME POINT OF THE SAME WALK — core/layout/intrinsic_size.c
       crashes for a replaced element by name — so this is the two axes of ONE absence and not a new
       restriction. */
    if (replaced_element_of(el).replaced)
        DFAILF("%s: this box is a REPLACED element, so the automatic block size css-sizing-3 §3.2 \"Sizing "
               "Values: the <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and "
               "fit-content values\" sends both intrinsic keywords to is NOT CSS 2.1 §10.6.3 \"Block-level "
               "non-replaced elements in normal flow when 'overflow' computes to 'visible'\"' stack of "
               "block-level children — that section says non-replaced in its own title. It is CSS 2.1 §10.6.2 "
               "\"Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' "
               "replaced elements in normal flow and floating replaced elements\", whose arms derive the "
               "height from the box's natural dimensions and its intrinsic ratio. THE ARITHMETIC IS BUILT AND "
               "WHAT IS MISSING IS THE WIRING: core/layout/replaced_element.h answers css-images-3 §4.1 "
               "\"Object-Sizing Terminology\"'s natural dimensions and core/layout/used_value.c runs CSS 2.1 "
               "§10.6.2 over them, so BUILD that section's arms behind THIS entry — css-sizing-3 §5.1 "
               "\"Intrinsic Sizes\"' own closing sentence is the argument for it, \"a block-level or "
               "inline-level replaced element whose height or width behaves as auto is effectively defined to "
               "use its max-content size\" — and every consumer of this component gets the answer without one "
               "of them classifying the box. THE INLINE AXIS REFUSES THE SAME BOX AT THE SAME POINT and names "
               "its own half, so the two are one absence read twice",
               box_subject(el, nbuf, sizeof nbuf));
    return block_flow_auto_height(el);
}

CssPx intrinsic_block_run_size(lxb_dom_element_t *style, BlockFlowRun run)
{
    bool any_line_box = false;
    CssPx first = css_px(0.0), last = css_px(0.0);

    DCHECK(style != NULL,
           "css-sizing-3 §3.2's intrinsic block size of an ANONYMOUS box was asked for with no styling "
           "element. CSS 2.2 §9.2.1.1 \"Anonymous block boxes\" and css-flexbox-1 §4 \"Flex Items\" both give "
           "such a box its properties from the enclosing non-anonymous box, so the run's content is styled by "
           "an element even where the box is not");
    ibs_require_horizontal_tb(style);
    /* THE THREE OUT-PARAMETERS ARE READ AND DISCARDED, and that is this entry's contract rather than waste:
       CSS 2.1 §10.6.3's first bullet is the DISTANCE to the bottom edge of the last line box, and a baseline
       is CSS 2.1 §10.8's question about where text sits inside that distance. A caller that wants one asks
       `line_box_content_height` itself. `any_line_box` false is a run with no line box at all, whose distance
       is the zero that entry returns — §10.6.3's own "if it has block-level children" list reaching none of
       its bullets is the same number and not a refusal. */
    return line_box_content_height(style, run, line_box_available_width_derived(), &any_line_box, &first, &last);
}
