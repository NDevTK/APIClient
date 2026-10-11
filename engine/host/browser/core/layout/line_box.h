/* CSS 2.2 §9.4.2 "Inline formatting contexts" and §10.8 "Line height calculations: the 'line-height' and
 * 'vertical-align' properties": the line box, the inline half of §10.6.3's content-based height.
 *
 * It is a separate component from core/layout/block_flow.h because the two formatting contexts are
 * alternatives over one box: "an inline formatting context is established by a block container box that
 * contains no block-level boxes", and CSS 2.2 §9.2.1.1 "Anonymous block boxes" makes that true of a mixed
 * container by wrapping each run of inline-level content ("if a block container box (such as that generated
 * for the DIV above) has a block-level box inside it (such as the P above), then we force it to have only
 * block-level boxes inside it"). So it is asked about a run of a block container's children, with the element
 * whose style the box has ("the properties of anonymous boxes are inherited from the enclosing non-anonymous
 * box … non-inherited properties have their initial value"). The algorithms share no step: §9.4.1 stacks
 * border boxes and collapses §8.3.1's margins, §9.4.2 flows boxes along a line and §10.8 takes two maxima. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BOX_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BOX_H

/* The two halves of the answer are measured by different things. §10.8's three steps are arithmetic over
   `line-height`, `A` and `D`, which core/css/css_computed_value.h answers per element, so one line box's height
   needs no outlines. How many line boxes there are needs the run's advances (core/css/font_metrics.h) and soft
   wrap opportunities ([UAX14], core/layout/line_break.h) against each line's available width: §9.4.2
   distributes boxes across "two or more vertically-stacked line boxes" when they "cannot fit horizontally
   within a single line box". That is `text_run_measure_fill`, beside css-sizing-3 §2.1's two partitions in
   core/layout/text_run.h because all three read one [UAX14] pass.
   So this component collects, fills, then measures each line: §10.8's step 3 is "the distance between the
   uppermost box top and the lowermost box bottom" of one line, and which boxes are on it only the fill knows
   (an inline box whose text spans three lines is on all three), the fact CSSOM View §6's `getClientRects()`
   step 3 reports as a fragment count. */

/* The available width is asked for only where it is an operand. §9.4.2: "if an inline box cannot be split
   (e.g., if the inline box contains a single character …), then the inline box overflows the line box", so a
   run with no break position inside it is one line box at every width (css-text-3 §5.5 makes a context of
   empty inline boxes such a run: "out-of-flow boxes and inline box boundaries do not introduce a forced line
   break or soft wrap opportunity in the flow"), and deriving a width would run CSS 2.1 §10.3 to discard it.
   `text_run_measure_splits` asks, and the fill asserts the theorem.
   §9.4.2's zero-height line box is reported separately from the height: "Line boxes that contain no text, no
   preserved white space, no inline elements with non-zero margins, padding, or borders, and no other in-flow
   content … and do not end with a preserved newline must be treated as zero-height line boxes for the purposes
   of determining the positions of any elements inside of them, and must be treated as not existing for any
   other purpose." §8.3.1 asks for the difference twice ("certain zero-height line boxes (see 9.4.2) are
   ignored for this purpose", and "it does not contain a line box"), so `<div><span></span></div>` has an
   in-flow child, no line box, and margins that collapse through it. */

/* What is on the line is what core/layout/box_tree.h's sequence holds. ::before and ::after are empty by
   construction: css-content-3 §1 "Inserting and Replacing Content: the content property" gives the initial
   `normal`, which "For ::before and ::after, this computes to none", and `content` does not parse (lexbor's
   source/lexbor/css/property/const.h has no `LXB_CSS_PROPERTY_CONTENT`). ::marker is a member of a list item's
   sequence; css-lists-3 §3.5 "Positioning Markers: The list-style-position property" puts it on the line only
   at `inside` ("The ::marker is an inline element at the start of the list item’s contents"). At the initial
   `outside` it "is placed outside the principal block box" and `lb_child` lays nothing for it; an `inside`
   marker aborts at core/layout/block_flow.c's classification. */

/* A context's fill and its per-line facts are held by the current layout job (`LineBoxRecord`), whose lifetime
   sees no DOM write (core/layout/layout_job.h), so each context is laid out once per job. With no job holding a
   record, every answer is derived per read. */

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/block_flow.h"

/* CSS 2.2 §9.4.2's "The width of a line box is determined by a containing block and the presence of floats",
 * as an argument, because for one box the containing block is not the element whose style the lines use.
 * `style` answers whose properties the box has; this answers whose content box the lines fill. For §9.2.1.1's
 * anonymous block box they agree (its non-inherited properties "have their initial value", so CSS 2.1
 * §10.3.3's equation with six zero terms gives it the containing block's width). For css-flexbox-1 §4 "Flex
 * Items"' anonymous block container flex item they differ: its style is the flex container's and its width is
 * css-flexbox-1 §9.7 "Resolving Flexible Lengths"' used main size.
 *
 * `stated` false is not a zero width and `px` is not read on that arm (the presence-flag shape
 * core/layout/replaced_element.h uses). The derived arm is lazy because a run with no break position is one
 * line box at every width, and deriving one would run CSS 2.1 §10.3 over a box `used_value.c` may crash for;
 * whether the run splits is the fill's answer, so "derive it if needed" has to be a value. Built only by the
 * two entries below, so an unstated width has no `px` to read. */
typedef struct {
    bool  stated;
    CssPx px;   /* read only where `stated`; §9.4.2's line box width, a CONTENT-box extent in CSS pixels */
} LineBoxAvailableWidth;

/* "determined by a containing block", left to the walk, which reads `style`'s used content width only if the
   run has a break position in it. */
LineBoxAvailableWidth line_box_available_width_derived(void);

/* "determined by a containing block", stated by the caller, for a box whose containing rectangle is not
   `style`'s content box. `px` must be non-negative (css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing
   property" floors every inner size at zero, and css-flexbox-1 §9.7 "Resolving Flexible Lengths" floors "its
   content-box size at zero"); the walk asserts it. */
LineBoxAvailableWidth line_box_available_width_stated(CssPx px);

/* A layout job's line box record: one fill per (`style`, run, stated width), owned by the job and released
   with its memo. `release` asserts `asks == served + built` and one entry per fill, and frees every fill. */
struct LbEntry;
typedef struct {
    struct LbEntry *tab;     /* a power-of-two table, NULL with `cap` 0 before the first fill */
    size_t          cap, used;
    long long       asks, served, built;
} LineBoxRecord;
void line_box_record_release(LineBoxRecord *r);

/* CSS 2.2 §10.6.3's first bullet for the block container that establishes one inline formatting context, "the
   distance from its top content edge to the first applicable of the following", whose first bullet is "the
   bottom edge of the last line box", in CSS pixels (two fragments, quoted apart because the standard does not
   write them together).
   The context is one of §9.2.1.1's runs of `style`'s content, and `style` is whose properties the box has: an
   anonymous block box has no element and "inherit[s] from the enclosing non-anonymous box". A `BlockFlowRun`
   names the run by the two block-level boxes bracketing it (core/layout/block_flow.h), NULL at either end; a
   container with no block-level box is the run with both NULL. A run is not a sibling range: §9.2.1.1 breaks
   an inline box around a block-level box inside it, so a run can begin and end inside one, and the fill
   re-enters the open ancestors and withholds their opening edges ("the border would be drawn around C1 (open
   at the end of the line) and C2 (open at the start of the line)").
   The caller has established §9.4.2's condition (no block-level boxes in the run), which
   core/layout/block_flow.c decides once; a block-level box reaching this walk crashes. `avail` is §9.4.2's
   line box width (`LineBoxAvailableWidth`); a caller measuring a §9.2.1.1 run passes the derived arm. */
/* The out-parameters are the other answers of the same reduction, all written on every path, none NULL:
     `*any_line_box` is false when every line box is one §9.4.2 says "must be treated as not existing for any
       other purpose", the fact §8.3.1's collapse-through note asks for.
     `*last_baseline` is CSS 2.2 §10.8.1 "Leading and half-leading"'s offset from the same top content edge to
       the last line box's baseline, read at another point of the one loop, so it cannot fall on a line the
       height did not count.
     `*first_baseline` is the same offset to the first existing line box's baseline, read at a third point.
       css-inline-3 §4.2.1 "Alignment Baseline Source: the baseline-source longhand"'s `first` arm asks for it
       ("last-baseline alignment for inline-block, first-baseline alignment for everything else"), and so does
       CSS 2.1 §17.5.3 "Table height algorithms": "The baseline of a cell is the baseline of the first in-flow
       line box in the cell"; reading the last there makes a row too short when a taller cell's first line
       sits above its last.
   Both baselines are distances only when `*any_line_box` is true. */
/* Named residual. Not covered: `avail` is on this entry only; `line_box_content_span`, `line_box_glyphs` and
   `line_box_inline_fragments` reduce the same fill but derive their width from `style`, so a box whose
   containing rectangle is not `style`'s content box and which is painted or scrolled, not only measured,
   would be filled at two widths. Next: the same argument on those three entries, threaded to the same
   `lb_fill`. How its absence would show: a box's height taken over one set of line breaks and its glyph
   positions over another, so its painted text overflows or falls short of the rectangle its height claimed.
   Whether any caller both states a width and places the box is a grep of those entries' call sites. */
CssPx line_box_content_height(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail,
                              bool *any_line_box, CssPx *first_baseline, CssPx *last_baseline);

/* Where the boxes on this context's line boxes reach on one physical axis: `*lo` and `*hi` receive the lowest
 * and highest coordinates any of them occupies, as offsets from the establishing box's content box origin on
 * that axis (left edge horizontally, top edge vertically). `style` and `run` mean what they mean for
 * `line_box_content_height`. It is a second reduction over one fill, not a second layout, so a height and a
 * span cannot disagree about which line a box is on.
 *
 * It exists for CSSOM View §2 "Terminology"'s scrolling area, an extreme over "the … margin edge of all of the
 * element's descendants' boxes": the box holding a text run is the anonymous inline box CSS 2.2 §9.2.2.1
 * "Anonymous inline boxes" generates, which has no element for core/layout/flow_position.h to place. A run
 * wider than its container overflows the inline axis while adding nothing to §10.6.3's height, and a box with
 * a declared height never reaches the height walk (core/layout/block_flow.c's `bf_height_needs_content`). */
/* The inline axis is exact without a per-item position. §9.4.2 gives the line box the containing block's width
   and css-text-4 §7.1 "Text Alignment: the text-align shorthand" says "if (after justification, if any) the
   inline contents of a line box are too long to fit within it, then the contents are start-aligned: any
   content that doesn't fit overflows the line box's end edge." A line that overflows therefore reaches exactly
   `TextRunLine.size` from the content box's start edge. A line that fits is placed by `text-align` somewhere
   inside the line box, inside the padding box, so the content box's own edge reported for it is absorbed by
   the caller's extreme, whose other operand is that padding edge. The number is not a fragment position;
   `line_box_inline_fragments` computes those, and an extreme rebuilt from them would read a used content
   width for every context, including ones §10.3 crashes for. */
/* The block axis is a maximum over the boxes, not the stack's bottom: §10.8's line box includes the strut
   ("exactly as if each line box starts with a zero-width inline box with the element's font and line height
   properties. We call that imaginary box a 'strut'"), which is not a descendant's box, so
   `<div style="line-height:100px"><span style="line-height:10px">x</span></div>` overflows nothing.
   A context with nothing on its lines reports the content box's beginning corner on both outputs, which is
   inside the padding box (CSS 2 §8.1) and so invisible to the extreme. */
void line_box_content_span(lxb_dom_element_t *style, BlockFlowRun run,
                           bool vertical, CssPx *lo, CssPx *hi);

/* One placed character of this context's line boxes: the code point, its inline box, and the pen position
 * CSS 2.1 §E.2 "Painting order"'s step 7.2.1 draws it from, as offsets from the establishing box's content box
 * origin (the frame of `line_box_content_span` and of core/layout/block_flow.h's `BlockFlowAnonBox` origin).
 *
 * `baseline_y` is a baseline, not a top edge: §10.8's step 3 puts the line's baseline its maximum `A'` below
 * its top, the coordinate every box on the line hangs from, and `line_box_inline_fragments` derives its block
 * axis from the same number. A glyph's ink extent is the face's (core/css/font_metrics.h), not this
 * component's. Each `origin_x` is `text_run_measure_line_offset` over the same fill, so a painter has no pen
 * to advance and nothing to re-measure with, as core/paint/display_list.h states for a text mark. */
/* Only characters are placed. css-text-3 §4.1.1's Phase I has already removed what the line does not hold; an
   edge draws nothing, a forced break's U+000A is supplied by HTML §15.3.4 "Phrasing content"'s declaration for
   [UAX14], and an atomic inline's U+FFFC expresses css-text-3 §5.5 "Line Breaking Details"' opportunities (CSS
   2.1 §E.2 paints an atomic inline through its own item). `text_run_measure_item_cp` refuses the last two. A
   line §9.4.2 treats as zero-height holds no text, so it contributes no character and the stack still
   advances past it.
   Answers the count and stores a newly allocated array at `*out`, which the caller owns and frees; a count of
   zero (an empty inline box, or text §4.1.1 collapsed away) stores NULL and is a positive answer. `style` must
   be `horizontal-tb`, asserted, because `origin_x` and `baseline_y` are physical left and top. `style` and
   `run` mean what they mean for `line_box_content_height`. */
typedef struct {
    uint32_t cp;                /* the code point that draws — always a css-text-3 §4.1.1 Phase I character */
    lxb_dom_element_t *style;   /* the inline box it is in, whose `font-size` and `color` it has */
    CssPx origin_x;             /* the pen position, from the content box's left edge (horizontal-tb, asserted) */
    CssPx baseline_y;           /* its baseline, from the content box's top edge (horizontal-tb, asserted) */
} LineBoxGlyph;

size_t line_box_glyphs(lxb_dom_element_t *style, BlockFlowRun run, LineBoxGlyph **out);

/* One box fragment of an inline box: CSSOM View §6 "Extensions to the Element Interface"'s getClientRects()
 * step 3's "one for each box fragment", one per line box it spans. The four numbers are the fragment's border
 * area ("describing its border area") as offsets from the content box origin of the establishing block
 * container, the frame core/layout/flow_position.h turns into an ICB coordinate.
 *
 * Inline axis: core/layout/text_run.h's two edge items for the box are css-sizing-3 §2.2's outer size at each
 * boundary (CSS 2.2 §9.4.2: "horizontal margins, borders, and padding are respected between these boxes"), so
 * the border area is that span less the box's two horizontal margins, the one subtraction made here. */
/* Block axis: the box's own content area, not the line box's. CSS 2.2 §10.6.1 "Inline, non-replaced elements":
   "the vertical padding, border and margin of an inline, non-replaced box start at the top and bottom of the
   content area, and has nothing to do with the 'line-height'. But only the 'line-height' is used when
   calculating the height of the line box." The content area sits on the line's baseline (its maximum `A'`
   below the line's top), `A` above and `D` below, with §8.1's padding and border outside it. §10.6.1 leaves
   its height to the UA ("A UA may, e.g., use the em-box or the maximum ascender and descender of the font");
   the second is taken, the same `A` and `D` §10.8.1's strut reads, so a fragment and its line describe one
   font. A line §9.4.2 treats as zero-height still carries a fragment at the position the stack reached: §6's
   step 3 asks for zero-extent rectangles ("including those with a height or width of zero"), and "not
   existing for any other purpose" concerns §8.3.1 and §10.6.3, which this entry does not answer. */
/* An atomic inline-level box hangs from the same baseline by CSS 2.2 §10.8's `vertical-align` definition: "for
   inline non-replaced elements, the box used for alignment is the box whose height is the 'line-height' … for
   all other elements, the box used for alignment is the margin box", and `baseline` says "If the box does not
   have a baseline, align the bottom margin edge with the parent's baseline." §10.8 gives a baseline to an
   `inline-table` (its first row's) and an `inline-block` ("the baseline of its last line box in the normal
   flow, unless it has either no in-flow line boxes or if its 'overflow' property has a computed value other
   than 'visible', in which case the baseline is the bottom margin edge"), and to nothing else. The
   `inline-block` rule is answered for every block container §9.4.1's stack can place
   (core/layout/block_flow.h); a `flex` or `grid` container on that stack crashes naming css-flexbox-1 §8.5
   "Flex Container Baselines" and css-grid-1 §10.6 "Grid Container Baselines". A replaced element's bottom
   margin edge sits on the baseline (so a text descender shows below an image): its border area is the baseline
   less `margin-bottom`, one used border-edge extent tall (§10.6.2), the pair `lb_atomic_extent` gives §10.8's
   step 1. */
/* css-text-4 §7.1 "Text Alignment: the text-align shorthand"'s alignment is applied here, so the coordinate is
   complete. §7.1's `Computed value:` is "see individual properties", so this reads §7.3 "Default Text
   Alignment: the text-align-all property" and, for the block's last line and a line a forced break ends, §7.4
   "Last Line Alignment: the text-align-last property". Two callers applying it would be two answers. */
typedef struct {
    CssPx inline_start, inline_end;   /* from the content box's left edge (horizontal-tb, asserted) */
    CssPx block_start, block_end;     /* from the content box's top edge */
} LineBoxFragment;

/* `el`'s fragments, in content order, with `*establishing` receiving the block container whose content box
 * origin they are measured from. Answers the count and stores a newly allocated array at `*out`, which the
 * caller owns and frees; the count is never zero ("line boxes are created as needed to hold inline-level
 * content", and this box's items are content the fill partitions).
 *
 * `el` must be a box on a line: `display: inline` or `inline-block`, in flow, `horizontal-tb`. A non-replaced
 * inline box is delimited by its two edge items and split across the line boxes it spans; an atomic
 * inline-level box is the one item `lb_child` collects and always one fragment (CSS 2.2 §9.2.2
 * "Inline-level elements and inline boxes": it participates "as a single opaque box"). Both are asserted.
 * Atomic here is two facts: a replaced element (HTML §15.4 "Replaced elements", whose `display` stays
 * `inline`) and an `inline-block` of either kind (CSS 2.2 §10.3.10 "'Inline-block', replaced elements in
 * normal flow": "Exactly as inline replaced elements."). The block axis reads §10.8.1's split of the margin
 * box at the box's baseline (`lb_atomic_extent`), whose `below` is zero for a box with no baseline. */
/* An `inline-table`, `inline-flex` or `inline-grid` crashes in the walk, for different reasons. An
   `inline-flex` or `inline-grid` needs the used main size its module owns (css-flexbox-1 §9.9.1 "Flex
   Container Intrinsic Main Sizes", css-grid-1 §5.2 "Sizing Grid Containers"), and its baseline comes from the
   same layout. An `inline-table` has its inline size (CSS 2.1 §17.5.2 Table width algorithms: the
   'table-layout' property, core/layout/table_width.h); what is missing is its baseline, CSS 2.2 §10.8.1
   "Leading and half-leading"'s "the baseline of the first row of the table", which CSS 2.1 §17.5.3 Table
   height algorithms establishes on the way to a row's height. core/layout/table_height.h answers the heights
   and reports no baseline, and computes none for a row with at most one baseline-aligned cell, so the export
   is a derivation, not a field to plumb. */
/* It finds the formatting context itself by walking past every inline ancestor to the nearest block
   container. §10.1's second case steps over inline ancestors the same way (its worked example gives `em1` and
   `strong1` inside `<P id="p2">` the block established by `p2`), but §10.1's other cases answer something
   else (the ICB, the viewport, a positioned ancestor's padding edge, where its inline-ancestor exception
   lives), while "which context is this box on a line of" is one block container whatever `position` is. So
   the walks agree in one case of four and are separate entries; this one serves CSSOM View §6's and §7's
   consumers. */
/* In a mixed container the line boxes are inside CSS 2.2 §9.2.1.1 "Anonymous block boxes"' anonymous block
   boxes, one per maximal run of inline-level children; filling the container's whole child list would be a
   partition on line boxes that do not exist. So this asks core/layout/block_flow.h which of those boxes hold
   the child it descended through and fills each, taking the runs and their positions from block_flow.h's
   §9.4.1 stack, so a run's boundaries and its box's position stay one derivation.
   "Which boxes", because of §9.2.1.1's second sentence: "When an inline box contains an in-flow block-level
   box, the inline box (and its inline ancestors within the same line box) is broken around the block-level box
   …, splitting the inline box into two boxes (even if either side is empty), one on each side of the
   block-level box(es). The line boxes before the break and after the break are enclosed in anonymous block
   boxes." A broken box has fragments on several, so `<span>` around a `display: block` child reports two.
   `*establishing` is still the container and the frame its content box: an anonymous box has no element,
   margin, border or padding, and its origin inside the container (from block_flow.h) is added to each
   fragment, so a mixed container's fragments are in the same frame as an unmixed one's. */
size_t line_box_inline_fragments(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 LineBoxFragment **out);

/* The same fragments without a drive of their own: false ("not yet") when the establishing context's §9.4.1
   walk deferred a contribution, with nothing stored; true with the array at `*out` and its count at `*count`.
   `line_box_inline_fragments` is this ask inside a drive (core/layout/layout_job.h). */
/* The walks the fill of `style`'s run reads for its atomic inline-level boxes (CSS 2.2 §10.8 step 1's margin box
   of an `inline-block` and its §10.8.1 baseline), asked without a drive before the line boxes are measured:
   false ("not yet") when one was deferred, with the question suspended on the current job. The fill itself
   is held by the job's line box record, so asking costs no second fill. */
bool line_box_atomics_ask(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail);

bool line_box_inline_fragments_ask(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                   LineBoxFragment **out, size_t *count);

/* Where a non-replaced inline box's own margin edges reach on one physical axis: `*lo` and `*hi` receive the
 * extreme over all of its fragments, in the frame `line_box_content_span` and `LineBoxFragment` use (offsets
 * from `*establishing`'s content box origin), with `*establishing` receiving that block container.
 *
 * Both halves of the name are asserted here; `line_box_inline_fragments` admits `inline-block` and replaced
 * elements. CSS 2.2 §8.3 "Margin properties"' exception is written over exactly one box: "these properties
 * apply to all elements, but vertical margins will not have any effect on non-replaced inline elements". A
 * replaced element's and an `inline-block`'s vertical margins do have an effect (CSS 2.2 §10.6.6 "Complicated
 * cases": "for 'inline-block' elements, the margin box is used when calculating the height of the line box"),
 * and neither needs this entry: §10.3.2 and §10.6.2, or §10.3.9 "'Inline-block', non-replaced elements in
 * normal flow" and §10.6.6, give each both extents, and core/layout/flow_position.h one origin. */
/* An inline box has no `width` and no `height` (CSS 2.2 §10.3.1 "Inline, non-replaced elements": "The 'width'
   property does not apply"; §10.6.1: "The 'height' property does not apply"), so core/layout/used_value.h's
   border-edge extent asserts against it. §9.4.2 says what it is instead: "when an inline box exceeds the width
   of a line box, it is split into several boxes and these boxes are distributed across several line boxes",
   so its margin edge is an extreme over a set of border areas. Its caller is CSSOM View §2 "Terminology"'s
   scrolling area ("the right margin edge of all of the element's descendants' boxes"); CSSOM View §6's
   `scrollWidth` steps have no inline exclusion, so this is every paragraph's shape. */
/* The two inline margins go on the first and last fragments, which carry the box's boundaries: §9.4.2 says
   "when an inline box is split, margins, borders, and padding have no visual effect where the split occurs
   (or at any split, when there are several)". `line_box_inline_fragments` emits one fragment per line its
   `[open, close]` item range intersects, in line order, over lines that partition the items, so the first
   holds `open` and the last `close`, exactly where it took the margins off for §6's border area.
   The block axis takes no margin (§8.3: "vertical margins will not have any effect on non-replaced inline
   elements"; the `margin-top`/`margin-bottom` definition: "These properties have no effect on non-replaced
   inline elements."), so on that axis the margin edge is §10.6.1's border area.
   The extreme is over every fragment, not the two ends, because "negative values for margin properties are
   allowed": `margin-left: -20px` puts the first fragment's margin edge inside its border edge, and a middle
   fragment's split edge, which carries no margin, can then be outermost. */
/* Answers false ("not yet"), with nothing written, where `line_box_inline_fragments_ask` does. */
bool line_box_inline_margin_span_ask(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                     bool vertical, CssPx *lo, CssPx *hi);

#endif
