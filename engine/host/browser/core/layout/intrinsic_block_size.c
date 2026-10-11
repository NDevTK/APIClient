/* css-sizing-3 §3.2's intrinsic size in a box's block axis. See intrinsic_block_size.h. */
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

/* The block-to-physical substitution this component makes, asserted where it is made. Both entries reach a
   walk that stacks children downward, and css-writing-modes-4 §7.2 "Dimensional Mapping" puts CSS 2.1 §10.6
   on the block dimension ("the calculation rules in CSS2.1 Section 10.6 are used in the block dimension");
   the two coincide only in `horizontal-tb` (css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode
   property"). The mapping exists (core/css/css_logical.c's `css_logical_axis_is_vertical`); what is missing is
   a block_flow.c or intrinsic_size.c walk over the other physical direction. */
static void ibs_require_horizontal_tb(lxb_dom_element_t *el)
{
    char nbuf[160], wbuf[64];

    if (css_computed_keyword_is(el, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB)) return;
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
    /* A replaced box is CSS 2.1 §10.6.2, and `block_flow_auto_height` cannot refuse it: css-flexbox-1 §4
       blockifies an `img` item to `display: block`, which passes that walk's block-container guard and would
       answer zero for a box sized by its natural height. The walk leaves classification to its caller. */
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
    /* The three out-parameters are discarded: §10.6.3's first bullet is a distance, and baselines are
       CSS 2.1 §10.8's question. A run with no line box answers the zero that entry returns. */
    return line_box_content_height(style, run, line_box_available_width_derived(), &any_line_box, &first, &last);
}
