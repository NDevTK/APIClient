/* CSS 2.2 §9.4.2 "Inline formatting contexts" and §10.8 "Line height calculations: the 'line-height' and
 * 'vertical-align' properties" — THE LINE BOX, which is the other half of §10.6.3's content-based height and
 * the box core/layout/block_flow.h has been crashing for.
 *
 * WHY THIS IS A SEPARATE COMPONENT FROM block_flow.h AND NOT ANOTHER ARM OF ITS WALK. §9.4.2 opens by saying
 * which box establishes this formatting context — "an inline formatting context is established by a block
 * container box that CONTAINS NO BLOCK-LEVEL BOXES" — and §9.4.1 says the same of the other one. They are
 * ALTERNATIVES over one BOX, decided by its content and by nothing else — and CSS 2.2 §9.2.1.1 "Anonymous
 * block boxes" is what makes that sentence true of a container holding both, by putting a box there that the
 * element tree does not: "if a block container box (such as that generated for the DIV above) has a
 * block-level box inside it (such as the P above), then we force it to have only block-level boxes inside it",
 * each run of inline-level content wrapped in an anonymous block box. So this component is asked about a RUN
 * of a block container's children rather than about an element, and the ELEMENT it is handed alongside is the
 * one whose style the box has — §9.2.1.1: "the properties of anonymous boxes are inherited from the enclosing
 * non-anonymous box … non-inherited properties have their initial value." The two algorithms share no step:
 * §9.4.1 stacks border boxes down a column and reduces §8.3.1's adjoining margin runs, while §9.4.2 flows
 * boxes ALONG a line and then §10.8 takes two maxima across it. A single walk carrying both would be two
 * algorithms behind one `if`, and the `if` would be re-asked per child instead of once per box.
 *
 * THE TWO HALVES OF THE ANSWER ARE MEASURED BY DIFFERENT THINGS, and keeping them apart is what makes this
 * component readable. §10.8's three steps are arithmetic over `line-height`, `A` and `D`, all of which
 * core/css/css_computed_value.h answers per element — so the HEIGHT of one line box needs no font outlines at
 * all. HOW MANY line boxes there are is the other half and it needs everything: §9.4.2 distributes
 * inline-level boxes across "two or more vertically-stacked line boxes" when they "cannot fit horizontally
 * within a single line box", which is the run's own advances (core/css/font_metrics.h, per Unicode scalar
 * value off the first available face's 'cmap' and 'hmtx') and its soft wrap opportunities ([UAX14], through
 * core/layout/line_break.h) measured against the AVAILABLE WIDTH of each line. That distribution is
 * `text_run_measure_fill`, and it lives in core/layout/text_run.h beside css-sizing-3 §2.1's two intrinsic
 * partitions because all three are walks over ONE [UAX14] pass and a second pass is the one way they could
 * disagree about where this run may break.
 *
 * SO THIS COMPONENT COLLECTS, FILLS, AND THEN MEASURES EACH LINE, and the order is forced rather than chosen:
 * §10.8's step 3 is "the distance between the uppermost box top and the lowermost box bottom" of ONE line, so
 * which boxes those maxima are taken over is a question only the fill can answer. A walk over the ELEMENT TREE
 * cannot: an inline box whose text spans three lines is on all three and one whose text fits is on one, and
 * the tree records neither. That is the same fact CSSOM VIEW §6's `getClientRects()` step 3 reports as a
 * fragment count.
 *
 * THE AVAILABLE WIDTH IS ASKED FOR ONLY WHERE IT IS AN OPERAND, which is §9.4.2's own overflow sentence and
 * not an optimisation: "if an inline box cannot be split (e.g., if the inline box contains a single
 * character …), then the inline box OVERFLOWS the line box." A run with no break position inside it is
 * therefore ONE line box at every width — css-text-3 §5.5 "Line Breaking Details" is what makes an inline
 * formatting context of empty inline boxes such a run, since "out-of-flow boxes and inline box boundaries do
 * not introduce a forced line break or soft wrap opportunity in the flow" — and deriving a width for it would
 * run CSS 2.1 §10.3 over this box to discard the result. `text_run_measure_splits` is that question and the
 * fill ASSERTS the theorem rather than letting a caller trust it.
 *
 * §9.4.2's ZERO-HEIGHT LINE BOX IS A SECOND ANSWER AND NOT A ROUNDING OF THE FIRST, which is why the entry
 * reports it separately. "Line boxes that contain no text, no preserved white space, no inline elements with
 * non-zero margins, padding, or borders, and no other in-flow content … and do not end with a preserved
 * newline MUST BE TREATED AS ZERO-HEIGHT LINE BOXES for the purposes of determining the positions of any
 * elements inside of them, and MUST BE TREATED AS NOT EXISTING FOR ANY OTHER PURPOSE." A caller that saw only
 * a height of zero could not tell that case from a line box that measured zero, and §8.3.1 asks for exactly
 * that distinction twice: its adjoining test excepts them by name ("note that certain zero-height line boxes
 * (see 9.4.2) are ignored for this purpose") and its collapse-through note conditions on "it DOES NOT CONTAIN
 * A LINE BOX". So `<div><span></span></div>` is a box with an in-flow child, no line box, and margins that
 * collapse through it — three facts a single number cannot carry.
 *
 * WHAT IS ON THE LINE IS WHAT THE ELEMENT TREE HOLDS, and that is this engine's model rather than an omission
 * this component makes: it builds NO PSEUDO-ELEMENT BOXES anywhere, and core/layout/box_tree.h's named
 * residual is where that gap is recorded, because css-display-3 §2.5's splice is the sequence such a box would
 * have to enter before any line could hold one. So a `content` declaration puts no box on this line for the
 * same reason it puts none in §9.4.1's stack, and reading one HERE would be one component disagreeing with
 * that model rather than fixing it.
 * THIS PARAGRAPH USED TO NAME THE ENFORCEMENT SURFACE AS `getComputedStyle(el, "::before")` THROWING A
 * `NotSupportedError`, AND THE RETIRED WORDING IS KEPT BECAUSE A READER WHO GREPS THIS ENGINE FOR A
 * PSEUDO-ELEMENT SURFACE RE-DERIVES IT. It was wrong in BOTH registers at once, which is why neither half
 * could have caught the other. About THIS TREE: no such throw stands — `NotSupportedError` occurs exactly
 * twice in core/css/css_style_declaration.c and BOTH hits are prose recording the removal, which is what a
 * grep for a removed construct always answers in this codebase. About the STANDARD, the half no grep of this
 * tree could reach: CSSOM §7.2 "Extensions to the Window Interface" is SIX steps and contains no throw at any
 * of them, so the refusal that clause named was a behaviour that member invented and a page could not have
 * told from a real one. WHAT STANDS THERE NOW IS AN ABORT RATHER THAN A REFUSAL A PAGE CAN SEE: §7.2 step 3
 * is a `DFAIL`, so a dev build crashes at it and a release build falls through to the ELEMENT's own style.
 * The observation is that crash's own text, never a `try`/`catch` around a member call.
 * AND THE `content` CLAUSE ENUMERATED WHERE THE GAP IS A PROPERTY — the same shape core/layout/box_tree.h's
 * residual carried, and that residual now records its own retirement in its own words. It read `the day
 * css-content-3's generated content exists it becomes a box`, which makes the missing population look like it
 * waits on ONE property, and that property is not what gates the member this component would meet first.
 *   - ::before AND ::after ARE EMPTY BY CONSTRUCTION, so no `content` support at this component could put one
 *     on a line. css-content-3 §1 "Inserting and Replacing Content: the content property" gives the initial
 *     value `normal` and says of it "For ::before and ::after, this computes to none"; nothing moves it off
 *     `normal`, because the property does not PARSE — grep lexbor's `source/lexbor/css/property/const.h` for
 *     `LXB_CSS_PROPERTY_CONTENT`, with `LXB_CSS_PROPERTY_COLOR` as the armed control that shows the question
 *     reaches an answer at all.
 *   - ::marker IS THE MEMBER THAT ENUMERATION HID, AND IT REACHES THE BOX TREE WITHOUT REACHING THIS LINE.
 *     css-content-3 §1 leaves ITS content at `normal` rather than `none`, and css-display-3 §2.3 "Generating
 *     Marker Boxes: the list-item keyword" generates it from the display value ALONE, which the UA rule table
 *     in core/css/css_style_declaration.c already computes for every `li` — so a marker is reachable with no
 *     author declaration anywhere, and WHOSE gap that is belongs to core/layout/box_tree.h's residual and is
 *     read there rather than restated here. THIS component's answer is a different one, and it rests on a
 *     SECOND unparsed property rather than on a judgement made here: css-lists-3 §3.5 "Positioning
 *     Markers: The list-style-position property" puts the marker on the line only at `inside` ("The ::marker
 *     is an inline element at the start of the list item's contents"), its initial value is `outside`, and
 *     `LXB_CSS_PROPERTY_LIST_STYLE_POSITION` is absent from that same lexbor header against the same armed
 *     control — so `inside` is unreachable. At `outside` §3.5 states no answer to hold this component to: the
 *     marker box "may affect the height of the principal block box and/or the height of its first line box,
 *     and in some cases may cause the creation of a new line box; this interaction is also not defined."
 *   So this component owes the marker NOTHING TODAY, and that is derived rather than assumed: the first thing
 *   that would change it is `list-style-position` parsing, never `content`.
 *
 * NOTHING IS STORED, for core/layout/used_value.h's reason: a layout is per-flow state, so a cached line box
 * is shared state solver/dom_cow.h does not swap and a stale one is another flow's geometry. Every answer is
 * derived per read from the running flow's own tree. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BOX_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BOX_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/block_flow.h"

/* CSS 2.2 §9.4.2 "Inline formatting contexts"' OWN SENTENCE — "The width of a line box is determined by a
 * containing block and the presence of floats" — AS AN ARGUMENT, because for one box in this engine the
 * containing block is not the element whose style the lines are measured with.
 *
 * WHY IT IS AN ARGUMENT AND NOT A SECOND READ OF `style`. `style` answers TWO questions at every entry below:
 * whose properties the box has, and whose content box the lines are filled into. For §9.2.1.1's anonymous
 * block box those two have the same answer, and the fill's own banner proves it rather than assuming it — the
 * anonymous box's non-inherited properties "have their initial value", so CSS 2.1 §10.3.3's constraint
 * equation with six zero terms makes its width the containing block's, which is the enclosing element's
 * content width. For css-flexbox-1 §4 "Flex Items"' ANONYMOUS BLOCK CONTAINER FLEX ITEM the two answers are
 * DIFFERENT NUMBERS: its style is still the flex container's, and its width is css-flexbox-1 §9.7 "Resolving
 * Flexible Lengths"' used main size, which equals the container's inner main size only when §9.7 happened to
 * flex it to the whole line. One operand answering two questions is decided by whichever caller is stricter
 * and the cost lands silently on the other, so the two are split here.
 *
 * `stated` FALSE IS NOT A ZERO WIDTH AND `px` IS NOT READ AT ALL ON THAT ARM. It is the presence flag
 * core/layout/replaced_element.h gives every natural dimension, for the same reason: a real width and a
 * spelling that means there is none must not be one value. It is also not merely a null: the DERIVED arm is
 * LAZY by design, and that laziness is load-bearing rather than an optimisation — this header's own
 * "THE AVAILABLE WIDTH IS ASKED FOR ONLY WHERE IT IS AN OPERAND" paragraph is why, since a run with no break
 * position inside it is ONE line box at every width and deriving one for it would run CSS 2.1 §10.3 over a box
 * whose used width `used_value.c` may still crash for. A caller cannot decide that in advance — whether the
 * run splits is the fill's own answer — so `derive it if you turn out to need it` has to be a value.
 *
 * THE TWO ARE BUILT BY ENTRIES AND NEVER BY HAND so that an unstated width has no `px` for anyone to read. */
typedef struct {
    bool  stated;
    CssPx px;   /* read ONLY where `stated`; §9.4.2's line box width, a CONTENT-box extent in CSS pixels */
} LineBoxAvailableWidth;

/* "determined by a containing block" LEFT TO THE WALK, which reads `style`'s own used content width and reads
   it only if the run turns out to have a break position in it. */
LineBoxAvailableWidth line_box_available_width_derived(void);

/* "determined by a containing block" STATED BY THE CALLER, for a box whose containing rectangle is not
   `style`'s content box. `px` must be non-negative — css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing
   property" floors every inner size at zero and css-flexbox-1 §9.7 "Resolving Flexible Lengths" floors "its
   content-box size at zero" in its own words — and it is asserted at the walk rather than here. */
LineBoxAvailableWidth line_box_available_width_stated(CssPx px);

/* CSS 2.2 §10.6.3's FIRST BULLET for the block container box that establishes ONE inline formatting context —
   "the distance from its top content edge to the first applicable of the following", whose first bullet is
   "the bottom edge of the last line box" — in CSS pixels. THE TWO RUNS ARE QUOTED SEPARATELY BECAUSE THE
   STANDARD DOES NOT WRITE THEM TOGETHER: the sentence names a list and the bullet is an item of it, so the
   one-run spelling that stood here — and that two other sites in this component copied — was a SPLICE of two
   non-contiguous fragments, which reads as a verbatim sentence and is the shape no reader re-checks.
   THE FORMATTING CONTEXT IS ONE OF §9.2.1.1's RUNS OF `style`'s CONTENT, AND `style` IS WHOSE PROPERTIES THE
   BOX HAS. Those are two arguments and not one because §9.2.1.1's anonymous block box has no element: a mixed
   container generates one box per run, and each of them "inherit[s] from the enclosing non-anonymous box" —
   the DIV, not itself — while the content it holds is only its own run. A `BlockFlowRun` names that run by the
   two block-level boxes that bracket it (core/layout/block_flow.h), either of which is NULL at the start or
   the end of the content — so a container with no block-level box at all is the run with both NULL.
   THE RUN IS NOT A SIBLING RANGE AND THE TYPE IS WHAT SAYS SO. §9.2.1.1's second paragraph breaks an inline
   box around an in-flow block-level box inside it, so a run can BEGIN and END part-way through one; the fill
   re-enters the open ancestors and withholds their opening edges, which is the section's own "the border
   would be drawn around C1 (open at the end of the line) and C2 (open at the start of the line)".
   `*any_line_box` RECEIVES §9.4.2's OTHER ANSWER: false when every line box in this formatting context is one
   the section says "must be treated as NOT EXISTING for any other purpose", which is a different fact from a
   height of zero and is what §8.3.1's collapse-through note asks for. It is written on every path, so a caller
   reading it after a crash-free return is reading a measurement rather than whatever it initialised.
   `*last_baseline` RECEIVES CSS 2.2 §10.8.1 "Leading and half-leading"'s OTHER DISTANCE DOWN THE SAME LINE
   BOXES — the offset from the same top content edge to the BASELINE of that same last line box, which is a
   position INSIDE the box the returned height ends at. It is an OUT-PARAMETER AND NOT A SECOND ENTRY for
   the reason the walk behind it states in full: §10.6.3's bottom edge and §10.8.1's baseline are one running
   position read at two points of one loop over one fill, and a second pass could put the baseline on a line
   the height had not counted. It is a distance ONLY WHEN `*any_line_box` IS TRUE; its zero otherwise is not a
   coordinate, exactly as the returned height's is not, and both are written on every path.
   `*first_baseline` RECEIVES THE SAME OFFSET TO THE FIRST EXISTING LINE BOX's BASELINE, and it is here rather
   than in a second entry for exactly the reason the last one is: it is the same running position read at a
   third point of the SAME loop, so it costs nothing and a second reduction over one fill is what this file
   refuses everywhere else. TWO SECTIONS ASK FOR IT BY NAME AND NEITHER IS §10.8.1's. css-inline-3 §4.2.1
   "Alignment Baseline Source: the baseline-source longhand"'s `first` arm is one — §4.2.1's own `auto` is
   "last-baseline alignment for inline-block, first-baseline alignment for everything else", so the alternative
   it names is this number and not §10.8.1's. CSS 2.1 §17.5.3 "Table height algorithms" is the other and is the
   one that made it load-bearing: "The baseline of a cell is the baseline of the first in-flow line box in the
   cell", which is what that section's row baseline — "the maximum distance between the top of the cell box and
   the baseline over all cells that have 'vertical-align: baseline'" — is the maximum of. For a box holding ONE
   line box the two baselines are the same number, and for a box holding several they are not; reading the last
   where §17.5.3 asks for the first makes a row too SHORT whenever a taller cell's first line sits above its
   last, which is a rectangle no reader can tell from a measured one.
   NONE OF THE FOUR IS OPTIONAL, which is why no out-parameter may be NULL: they are the four answers
   ONE reduction has, so a caller that wanted only some of them would still be paying for all four, and a
   nullable one is the shape that invites a second walk to be added for the answer it declined.
   THE CALLER HAS ALREADY ESTABLISHED §9.4.2's OWN CONDITION over the run it passes — that this box contains no
   block-level boxes — because deciding it requires classifying every child, which core/layout/block_flow.c
   does once, both to choose between the two formatting contexts and to delimit §9.2.1.1's runs. A block-level
   box reaching this walk is those two classifications having come apart, and it crashes here saying so.
   `avail` IS §9.4.2's LINE BOX WIDTH and is the one argument `style` does not answer — see
   `LineBoxAvailableWidth` above for why the two were one operand and what that cost. A caller measuring one of
   §9.2.1.1's runs passes `line_box_available_width_derived()`, which is the number this walk always read; a
   caller measuring a box whose containing rectangle is not `style`'s content box states it.
   IT IS ON THIS ENTRY AND NOT ON THE THREE BELOW, WHICH IS A NAMED RESIDUAL AND NOT AN OVERSIGHT.
   `line_box_content_span`, `line_box_glyphs` and `line_box_inline_fragments` reduce the SAME fill and still
   derive their width from `style`, so a box measured through this entry at a stated width and placed through
   one of those would be two readings of one formatting context — line boxes filled at two widths break in two
   places. WHAT IS NOT COVERED is any box whose containing rectangle is not `style`'s content box AND which is
   painted or scrolled rather than only measured. WHAT THE NEXT DIFF BUILDS is the same argument on those three
   entries, threaded to the same `lb_fill`. HOW ITS ABSENCE WOULD SHOW: a box so measured reports a height taken
   over one set of line breaks and glyph positions taken over another, so its painted text overflows or falls
   short of the rectangle its own height claimed. THE PAIR IS UNREACHABLE WHILE NO CALLER BOTH
   STATES A WIDTH AND PLACES THE BOX, which is a property of the callers and not a list of boxes — and whether
   it still holds is one grep for those three entries' call sites, never a fact to take from this sentence. */
CssPx line_box_content_height(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail,
                              bool *any_line_box, CssPx *first_baseline, CssPx *last_baseline);

/* WHERE THE BOXES ON THIS FORMATTING CONTEXT'S LINE BOXES REACH on ONE PHYSICAL AXIS — `*lo` and `*hi` receive
 * the lowest and highest coordinates any of them occupies, as OFFSETS FROM THE ESTABLISHING BOX'S CONTENT BOX
 * ORIGIN on that axis (its left edge for the horizontal one, its top edge for the vertical one). The three
 * arguments before them are `line_box_content_height`'s three and mean exactly what they mean there.
 *
 * IT IS A SECOND REDUCTION OVER ONE FILL, NOT A SECOND LAYOUT. §9.4.2's distribution is run once by the same
 * static both entries go through, for the reason core/layout/text_run.h gives about its own three partitions:
 * two collections of one formatting context are two chances to disagree about where this run may break, and a
 * height and a span that disagreed about which line a box is on would be two documents.
 *
 * WHY IT EXISTS BESIDE THE HEIGHT, WHICH IS NOT THE SAME NUMBER TWICE. CSSOM VIEW §2 "Terminology"'s SCROLLING
 * AREA takes an extreme over "the … margin edge of all of the element's descendants' boxes", and the box
 * holding a text run is the ANONYMOUS INLINE BOX CSS 2.2 §9.2.2.1 "Anonymous inline boxes" generates — the one
 * box in the tree with no element to reach it through, so core/layout/flow_position.h cannot be asked where it
 * is. It is also the box a `scrollWidth` is usually ASKING about: a run wider than its container overflows on
 * the inline axis while contributing nothing to §10.6.3's height, and a container with a DECLARED height never
 * reaches the height walk at all (core/layout/block_flow.c's `bf_height_needs_content`) while its text still
 * overflows it. Neither fact is visible in a height.
 *
 * THE INLINE AXIS IS EXACT WITHOUT A PER-ITEM POSITION, AND THAT IS A THEOREM WITH TWO HALVES rather than an
 * approximation this entry settles for. §9.4.2 gives the LINE BOX the containing block's width — "in general,
 * the left edge of a line box touches the left edge of its containing block and the right edge touches the
 * right edge of its containing block" — and css-text-4 §7.1 "Text Alignment: the text-align shorthand" says
 * where the content sits inside it: "if (after justification, if any) the inline contents of a line box are
 * too long to fit within it, then the contents are START-ALIGNED: any content that doesn't fit overflows the
 * line box's end edge."
 *   A LINE THAT OVERFLOWS is therefore start-aligned by §7.1, so its boxes begin at the content box's own start
 *   edge and reach exactly `TextRunLine.size` from it. Nothing is read for this and nothing could be: §7.1
 *   leaves the alignment no say in it.
 *   A LINE THAT FITS is distributed by `text-align` (§9.4.2: "when the total width of the inline-level boxes on
 *   a line is LESS than the width of the line box containing them, their horizontal distribution within the
 *   line box is determined by the 'text-align' property"), and WHEREVER that puts it, it is inside the line
 *   box, which is inside the content box, which is inside the padding box. So the edge reported for it — the
 *   content box's own — is a coordinate the CALLER'S extreme absorbs, because CSSOM VIEW §2's other operand is
 *   that same element's padding edge. The number is therefore not a per-fragment position and MUST NOT BE READ
 *   AS ONE: this entry answers where the boxes reach OUTSIDE the content box, and inside it answers the content
 *   box. CSSOM VIEW §6's `getClientRects()` wants the POSITION and is still not this entry's caller — it is
 *   `line_box_inline_fragments`', below, which computes the per-item offset and the alignment this one is
 *   constructed to avoid needing. The two answers stay separate because the reason this one needs neither is a
 *   THEOREM about an extreme and not a gap: an entry that took the fragment positions and re-derived an extreme
 *   from them would read a used content width for every formatting context, including the ones §10.3 still
 *   crashes for, in order to reach a number this derivation already has.
 *
 * THE BLOCK AXIS IS A MAXIMUM OVER THE BOXES AND DELIBERATELY NOT THE STACK'S OWN BOTTOM, which is the one
 * place this entry and `line_box_content_height` are answering different questions about the same lines. CSS
 * 2.2 §10.8 makes each line box as tall as "the distance between the uppermost box top and the lowermost box
 * bottom" INCLUDING the STRUT — "exactly as if each line box starts with a zero-width inline box with the
 * element's font and line height properties. We call that imaginary box a 'strut'" — and an imaginary box is
 * not one of "the element's descendants' boxes". `<div style="line-height:100px"><span
 * style="line-height:10px">x</span></div>` has a line box far taller than anything in it, and reporting the
 * stack's bottom would be an overflow no box makes.
 *
 * IT REPORTS THE CONTENT BOX'S OWN BEGINNING CORNER FOR A CONTEXT WITH NOTHING ON ITS LINES, rather than
 * leaving either output unwritten. That corner is inside the padding box on both axes (CSS 2 §8.1 nests them
 * and a padding is non-negative), so it is invisible to CSSOM VIEW §2's extreme — the same property that makes
 * the fitting-line answer above harmless, stated once for the degenerate case too. */
void line_box_content_span(lxb_dom_element_t *style, BlockFlowRun run,
                           bool vertical, CssPx *lo, CssPx *hi);

/* ONE PLACED CHARACTER of this formatting context's line boxes — the code point, the inline box it is in, and
 * the PEN POSITION CSS 2.1 §E.2 "Painting order"'s step 7.2.1 draws it from. The two coordinates are OFFSETS
 * FROM THE ESTABLISHING BOX'S CONTENT BOX ORIGIN, exactly as `line_box_content_span`'s two are and in the
 * same frame core/layout/block_flow.h's `BlockFlowAnonBox` reports its own origin in, so a caller composes
 * one addition and never a second derivation.
 *
 * `baseline_y` IS A BASELINE AND NOT A TOP EDGE, which is the field a reader is most likely to take for the
 * other thing. CSS 2.2 §10.8's step 3 makes a line box "the distance between the uppermost box top and the
 * lowermost box bottom", so its baseline sits the line's own maximum `A'` below its top — and that is the one
 * coordinate every box on the line hangs from, which is why `line_box_inline_fragments` derives its
 * fragments' block axis from the identical number and why this entry reports it rather than a rectangle. A
 * glyph has no rectangle here to report: its ink extent is a property of the FACE's own contours, which
 * core/css/font_metrics.h answers and this component must not, and CSS 2.1 §E.2 asks for "the text" rather
 * than for a box around it.
 *
 * ONE ENTRY PER CHARACTER, AND THE ADVANCES ARE CARRIED AS POSITIONS RATHER THAN AS A LIST OF WIDTHS. That is
 * the stronger half of the contract core/paint/display_list.h's residual states — "a text mark carries THOSE
 * advances and a rasterizer that re-measures is comparing two engines rather than painting one" — because a
 * consumer handed a per-character ORIGIN has no pen to advance and therefore nothing it could re-measure
 * with. Every one of these coordinates is `text_run_measure_line_offset` over the SAME fill this walk ran,
 * which is that component's third answer over its one pass and is the entry its own banner says exists so
 * that "a position derived from a prefix that re-trimmed would not add up to the size derived from the
 * whole". A second sum of one line here is the one way this component could hand a painter a run whose
 * characters and whose width describe different text.
 *
 * IT IS THE CHARACTERS AND NOTHING ELSE ON THE LINE. css-text-3 §4.1.1's Phase I has already collapsed and
 * removed what the line does not hold, and the three other item kinds are refused rather than placed: an
 * inline box EDGE draws nothing at all, a FORCED BREAK's U+000A is a code point HTML §15.3.4 "Phrasing
 * content"'s declaration supplies for [UAX14] and the document does not contain, and an ATOMIC inline's
 * U+FFFC is how css-text-3 §5.5 "Line Breaking Details"' two soft wrap opportunities are expressed — CSS 2.1
 * §E.2 reaches an atomic inline through its own item ("the replaced content, atomically" and the
 * inline-block arm's pseudo-context), never as text. `text_run_measure_item_cp` is what refuses the last two,
 * so the refusal is one rule stated where the kinds are and not a second classification here.
 *
 * A LINE §9.4.2 SAYS "must be treated as ZERO-HEIGHT" CONTRIBUTES NO CHARACTER AND IS NOT A SPECIAL CASE.
 * That rule's own first conjunct is "line boxes that contain no text", so such a line holds no item this
 * entry emits for, and the stack still advances past it by the zero height it has — which is why the loop
 * asks the line's extent whether or not it places anything on it.
 *
 * ANSWERS THE COUNT and stores a newly allocated array of that many at `*out`, WHICH THE CALLER OWNS AND MUST
 * FREE; a count of zero stores NULL, which is core/layout/block_flow.h's own spelling for the same shape. A
 * ZERO IS A POSITIVE ANSWER — a formatting context whose content is an empty inline box, or one whose text
 * css-text-3 §4.1.1 collapsed away entirely, has no character to place and is not an absence of a
 * measurement.
 *
 * `style` MUST BE IN A `horizontal-tb` WRITING MODE, ASSERTED — the same precondition
 * `line_box_inline_fragments` states for its own box, owed here for the same reason and for BOTH
 * coordinates: `origin_x` and `baseline_y` are this box's PHYSICAL left and top, which are §9.4.2's own
 * axes only in that mode.
 *
 * THE ARGUMENTS ARE `line_box_content_height`'s TWO AND MEAN EXACTLY WHAT THEY MEAN THERE: §9.2.1.1's
 * anonymous block box has no element, so `style` is whose computed properties the box has and `run` is which
 * of that container's runs this context is. A caller reaching every context under an element asks
 * core/layout/block_flow.h for both shapes, exactly as CSSOM VIEW §2 "Terminology"'s scrolling area does. */
typedef struct {
    uint32_t cp;                /* the code point that draws — always a css-text-3 §4.1.1 Phase I character */
    lxb_dom_element_t *style;   /* the inline box it is in, whose `font-size` and `color` it has */
    CssPx origin_x;             /* the pen position, from the content box's LEFT edge (horizontal-tb, asserted) */
    CssPx baseline_y;           /* its BASELINE, from the content box's TOP edge (horizontal-tb, asserted) */
} LineBoxGlyph;

size_t line_box_glyphs(lxb_dom_element_t *style, BlockFlowRun run, LineBoxGlyph **out);

/* ONE BOX FRAGMENT of an inline box — CSSOM VIEW §6 "Extensions to the Element Interface"'s getClientRects()
 * step 3's "one for each box fragment", which for an inline box is one per LINE BOX it spans. The four numbers
 * are the fragment's BORDER AREA as OFFSETS FROM THE CONTENT BOX ORIGIN of the block container that establishes
 * the formatting context — the same frame `line_box_content_span` reports in, and the frame
 * core/layout/flow_position.h turns into an initial-containing-block coordinate by adding that box's own origin
 * plus CSS 2 §8.1's leading border and padding.
 *
 * WHY THE BORDER AREA AND NOT THE CONTENT AREA. §6's step 3 says "describing its BORDER AREA", and for an
 * inline box that area is exactly what core/layout/text_run.h collects as the box's two EDGE items:
 * css-sizing-3 §2.2's outer size at each boundary, which CSS 2.2 §9.4.2 puts on the line ("horizontal margins,
 * borders, and padding are respected between these boxes"). An edge is the MARGIN box's contribution, so the
 * inline-axis border area is that span less the box's own two horizontal margins — the one subtraction this
 * component makes, and it is stated here because it is the whole difference between what the fill holds and
 * what §6 asks for.
 *
 * THE BLOCK AXIS IS THE BOX'S OWN CONTENT AREA AND EMPHATICALLY NOT THE LINE BOX'S, and CSS 2.2 §10.6.1
 * "Inline, non-replaced elements" is explicit about it in a sentence written to be misread the other way: "the
 * vertical padding, border and margin of an inline, non-replaced box start at the top and bottom of the CONTENT
 * AREA, and has NOTHING TO DO WITH the 'line-height'. But only the 'line-height' is used when calculating the
 * height of the LINE BOX." Two heights, and §6's step 3 wants the first. `<div style="line-height:100px"><span>
 * x</span></div>` renders a 100px line box around a span whose border area is one font tall, and reporting the
 * line box would be a rectangle no border is drawn on.
 * WHERE THE CONTENT AREA SITS IS THE BASELINE, which is the one number the line box does supply: §10.8's step 3
 * makes the line box "the distance between the uppermost box top and the lowermost box bottom", so its baseline
 * is the line's own maximum `A'` below its top and every box on it hangs from that one line. The content area
 * is then `A` above the baseline and `D` below it, and §8.1's padding and border nest outside it.
 * §10.6.1 LEAVES THE CONTENT AREA'S HEIGHT TO THE UA and this engine's choice is stated rather than assumed:
 * "the height of the content area should be based on the font, but this specification does not specify how. A
 * UA may, e.g., use the em-box or the MAXIMUM ASCENDER AND DESCENDER of the font." The second is taken, and it
 * is taken because it is the SAME `A` and `D` §10.8.1's strut already reads off the first available font — one
 * pair of numbers for both heights, so a fragment's rectangle and the line it sits on cannot come to describe
 * different fonts.
 * A LINE §9.4.2 says "must be treated as ZERO-HEIGHT line boxes for the purposes of determining the positions
 * of any elements INSIDE of them" still carries a fragment, at the position the stack has reached: §6's step 3
 * asks for zero-extent rectangles by name ("including those with a height or width of zero"), and §9.4.2's
 * "not existing for any other purpose" is about §8.3.1's margin collapsing and §10.6.3's height, both of which
 * this entry answers nothing for.
 *
 * AN ATOMIC INLINE-LEVEL BOX HANGS FROM THAT SAME BASELINE BY A DIFFERENT SENTENCE, and the two derivations
 * are separated by whether the box HAS a baseline rather than by which kind of box it is. CSS 2.2 §10.8's
 * `vertical-align` definition states both halves: "for inline non-replaced elements, the box used for
 * alignment is the box whose height is the 'line-height' … FOR ALL OTHER ELEMENTS, THE BOX USED FOR ALIGNMENT
 * IS THE MARGIN BOX", and `baseline` itself — "align the baseline of the box with the baseline of the parent
 * box. IF THE BOX DOES NOT HAVE A BASELINE, ALIGN THE BOTTOM MARGIN EDGE with the parent's baseline." §10.8
 * gives a baseline to an `inline-table` ("The baseline of an 'inline-table' is the baseline of the first row
 * of the table") and to an `inline-block` ("The baseline of an 'inline-block' is the baseline of its last line
 * box in the normal flow, unless it has either no in-flow line boxes or if its 'overflow' property has a
 * computed value other than 'visible', in which case the baseline is the bottom margin edge") and to NOTHING
 * else. THAT SECOND SENTENCE IS ANSWERED FOR EVERY BLOCK CONTAINER §9.4.1's STACK CAN PLACE — the standard
 * writes it under §10.8.1 "Leading and half-leading", which is where this component cites it, and its main arm
 * reaches core/layout/block_flow.h's stack rather than this file's own reduction, so an `inline-block` holding
 * an in-flow BLOCK-LEVEL box is measured and not refused. What is still narrower than the sentence is a
 * block-level child whose own baseline another module owns — a `flex` or `grid` container on that stack —
 * which crashes there naming css-flexbox-1 §8.5 "Flex Container Baselines" and css-grid-1 §10.6 "Grid
 * Container Baselines", so it shows as an abort on an `inline-block` whose stack holds one and never as a
 * wrong number. Its three arms do not produce the same geometry:
 * an `inline-block` sent to its bottom margin edge by either disjunct hangs its whole margin box above the
 * line, while one measured by the MAIN arm has the line's baseline running THROUGH it, at the baseline of the
 * last line box of the formatting context inside it. So a REPLACED element's bottom
 * margin edge sits ON the line's baseline — which is why an image on a line of text leaves the font's
 * descender visible below it. Its border area is then that baseline less its own `margin-bottom`, extending
 * one used BORDER EDGE EXTENT upward (§10.6.2 "Inline replaced elements, block-level replaced elements in
 * normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements"). It is the
 * same pair `lb_atomic_extent` puts above the baseline for §10.8's step 1, read back through §8.1's nesting —
 * and it is that box's pair BECAUSE it has no baseline, never because every atomic inline's `below` is zero.
 *
 * §7.1's ALIGNMENT IS APPLIED HERE AND THE COORDINATE IS COMPLETE. css-text-4 §7.1 "Text Alignment: the
 * text-align shorthand" is a shorthand with a `Computed value:` line of "see individual properties", so what
 * this reads is §7.3 "Default Text Alignment: the text-align-all property" and, for the last line of the block
 * and for a line a forced break ends, §7.4 "Last Line Alignment: the text-align-last property". A caller
 * receives a distance from the content box and never an alignment to apply itself: two callers applying it
 * would be two answers to where one fragment is. */
typedef struct {
    CssPx inline_start, inline_end;   /* from the content box's LEFT edge (horizontal-tb, asserted) */
    CssPx block_start, block_end;     /* from the content box's TOP edge */
} LineBoxFragment;

/* `el`'s FRAGMENTS, in content order, with `*establishing` receiving the block container whose content box
 * origin they are measured from. Answers the count and stores a newly allocated array of that many at `*out`,
 * WHICH THE CALLER OWNS AND MUST FREE; the count is never zero, because a box that generates a box is on at
 * least one line ("line boxes are created as needed to hold inline-level content", and this box's items ARE
 * content the fill partitions).
 *
 * `el` MUST BE A BOX ON A LINE — a computed `display` of `inline` or `inline-block`, in flow, in a
 * `horizontal-tb` writing mode.
 * That is TWO SHAPES and the count is where they differ: a NON-REPLACED inline box is delimited by its two
 * EDGE items and CSS 2.2 §9.4.2 splits it across as many line boxes as it spans, while an ATOMIC inline-level
 * box is delimited by the ONE run item `lb_child` collects for it and is always exactly ONE fragment — CSS 2.2
 * §9.2.2 "Inline-level elements and
 * inline boxes" makes it a box that "participate[s] in [its] inline formatting context as
 * a SINGLE OPAQUE BOX", so it is never the box §9.4.2 "SPLIT[s] into several boxes". Both are asserted.
 * WHICH BOXES ARE ATOMIC HERE IS TWO INDEPENDENT FACTS AND NOT ONE `display`: a REPLACED element (HTML §15.4
 * "Replaced elements", whose computed `display` stays `inline`) and an `inline-block` of either kind, since
 * CSS 2.2 §10.3.10 "'Inline-block', replaced elements in normal flow" delegates the replaced half whole
 * ("Exactly as inline replaced elements.") rather than making it a different box. The block axis then reads
 * §10.8.1's SPLIT of the margin box at the box's own baseline (`lb_atomic_extent`) rather than assuming the
 * bottom margin edge, which is what admitting the `inline-block` required and is one arithmetic over both:
 * `below` is zero for every box the section gives no baseline.
 * AN `inline-table`, `inline-flex` OR `inline-grid` STILL CRASHES IN THE WALK, and for none of them is the
 * missing piece a placement — BUT THEY NO LONGER CRASH FOR ONE REASON, AND THIS PARAGRAPH USED TO SAY THEY
 * DID. An `inline-flex` or `inline-grid` still needs the USED MAIN SIZE its own module owns (css-flexbox-1
 * §9.9.1 "Flex Container Intrinsic Main Sizes", css-grid-1 §5.2 "Sizing Grid Containers") before §9.4.2 has
 * anything to put on a line, and the baseline each of them has falls out of that same module's layout.
 * AN `inline-table` HAS ITS INLINE SIZE: CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property
 * is built (core/layout/table_width.h) and core/layout/used_value.c routes a table box's width to it, so what
 * keeps this one out is the OTHER axis — CSS 2.2 §10.8.1 "Leading and half-leading" makes its baseline "the
 * baseline of the first row of the table", and a row's baseline is CSS 2.1 §17.5.3 Table height algorithms'.
 * WHAT IS MISSING THERE IS THE NUMBER AND NOT THE SECTION, AND THIS LINE USED TO SAY §17.5.3 HAD NO COMPONENT
 * AT ALL: core/layout/table_height.h answers the row HEIGHTS and core/layout/used_value.c routes a table box's
 * `height` to it on both arms, while the BASELINE that section's alignment procedure establishes on the way to
 * a row's maximum is reported nowhere — and is not even computed on the arm this engine takes for a row with
 * at most one baseline-aligned cell, which needs none for the height. So the export is a real derivation and
 * not a field to plumb. The two absences were never one, and a reader taking them for one would build §17.5.2
 * a second time or §17.5.3 a first.
 *
 * IT FINDS THE FORMATTING CONTEXT ITSELF, and that is why it takes an element where the two entries above take
 * a run: the question "which inline formatting context is this box in" is answered by walking PAST every inline
 * ancestor to the nearest block container. WHAT STOOD HERE SAID THAT IS NOT §10.1's WALK BECAUSE "its own
 * exception makes an INLINE ancestor a containing block of a different shape", quoting §10.1's "the bounding box
 * around the padding boxes of the first and the last inline boxes" — and that exception is written in §10.1's
 * FOURTH case, about the nearest POSITIONED ancestor of a `position: absolute` box, not in its second. The
 * section's own worked example settles the second case in the opposite direction: for
 * `<P id="p2">... <EM id="em1"> ... <STRONG id="strong1">second</STRONG> ...</EM></P>` its table of containing
 * blocks gives BOTH `em1` and `strong1` the block established by `p2`, so §10.1's second case steps over an
 * inline ancestor exactly as this walk does, and core/layout/used_value.h now does too rather than crashing
 * there. THE TWO QUESTIONS STILL DIFFER AND THE DIFFERENCE IS NO LONGER THE INLINE STEP: §10.1's OTHER cases
 * answer something else entirely — the root's is the initial containing block (no element at all), a `fixed`
 * box's is the viewport, and an `absolute` box's is a positioned ancestor's PADDING edge, which is where that
 * inline exception actually lives — while "which context is this box on a line of" is the same block container
 * whatever this box's `position` is. So the walks agree on one case out of four and are not the same entry.
 * One walk, here, because both of §6's and §7's consumers would otherwise carry a copy.
 *
 * FINDING IT MEANS FINDING THE RUN AS WELL, because the container is only half an answer when it is MIXED. CSS
 * 2.2 §9.2.1.1 "Anonymous block boxes" — "if a block container box (such as that generated for the DIV above)
 * has a block-level box inside it (such as the P above), then we force it to have only block-level boxes inside
 * it" — puts this box's line boxes inside one of the ANONYMOUS BLOCK BOXES that forcing generates, one per
 * maximal run of inline-level children, and that box is not the container: filling the container's whole child
 * list would flow this box's items together with every other run's, which is a different partition on line
 * boxes that do not exist. So this entry asks core/layout/block_flow.h which of those boxes hold the child it
 * descended through, and fills EACH of them. The runs and their positions come from block_flow.h's own §9.4.1
 * stack rather than being delimited a second time here, because a run's boundaries and its box's position are
 * two halves of one derivation and two copies could disagree about where a margin collapsed.
 *
 * IT IS "WHICH BOXES" AND NOT "WHICH BOX", AND §9.2.1.1's SECOND SENTENCE IS THE WHOLE REASON. "When an inline
 * box contains an in-flow block-level box, the inline box (and its inline ancestors within the same line box)
 * is broken around the block-level box …, splitting the inline box into two boxes (even if either side is
 * empty), one on each side of the block-level box(es). The line boxes before the break and after the break are
 * enclosed in anonymous block boxes." So a box the section BREAKS has a fragment on several of those anonymous
 * block boxes at once, and the thing that identifies one of them is the FRAGMENT rather than the child this
 * walk descended through — which is exactly the shape §6 reports in, "one for each box fragment". An entry
 * that named ONE box would have to pick between them. For every element the section does not break there is
 * one box and this is the walk it always was.
 * WHAT A CALLER GAINS IS COUNT AND NOT KIND: the fragments still arrive in content order, still in one frame,
 * and still describe border areas. A `<span>` around a `display: block` child now reports the two the section
 * says it generates — "even if either side is empty" — where it used to report a crash naming the addressing
 * this entry did not have.
 *
 * `*establishing` IS STILL THE CONTAINER AND THE FRAME IS STILL ITS CONTENT BOX, in both shapes, which is what
 * makes the paragraph above invisible to every caller. §9.2.1.1 gives the anonymous box no element and no
 * margin, border or padding, and its own origin inside the container is a number block_flow.h reports — so this
 * entry ADDS that origin to the coordinates it measures inside the box, and a mixed container's fragments come
 * out in the same frame an unmixed one's do. There is no second frame for a caller to know about, and no
 * element is reported that the element tree does not contain. */
size_t line_box_inline_fragments(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 LineBoxFragment **out);

/* WHERE A NON-REPLACED INLINE BOX'S OWN MARGIN EDGES REACH on ONE PHYSICAL AXIS — `*lo` and `*hi` receive the
 * extreme over ALL of its fragments, in the same frame `line_box_content_span` and `LineBoxFragment` report in
 * (offsets from `*establishing`'s content box origin on that axis), with `*establishing` receiving the block
 * container that frame belongs to.
 *
 * BOTH HALVES OF THE NAME ARE THE PRECONDITION AND THIS ENTRY ASSERTS EACH OF THEM ITSELF, which
 * `line_box_inline_fragments` does not do for it: that entry's own precondition is a computed `display` of
 * `inline` OR `inline-block` and it refuses nothing about replacedness, so NEITHER half is inherited from the
 * call below. CSS 2.2 §8.3 "Margin properties"' exception is what this entry's block arm rests on — "these
 * properties apply to all elements, but VERTICAL MARGINS WILL NOT HAVE ANY EFFECT ON NON-REPLACED INLINE
 * ELEMENTS" — and it is written over exactly ONE box. A REPLACED element's vertical margins DO have an effect,
 * and so do an `inline-block`'s: CSS 2.2 §10.6.6 "Complicated cases" applies to "'Inline-block', non-replaced
 * elements" and says in so many words that "for 'inline-block' elements, the margin box is used when
 * calculating the height of the line box". Neither box needs this entry at all — §10.3.2 and §10.6.2 for the
 * replaced one, §10.3.9 "'Inline-block', non-replaced elements in normal flow" and §10.6.6 for the other, give
 * each both extents, and core/layout/flow_position.h gives each one origin, which is the ordinary composition.
 *
 * THE `display` HALF WAS NOT ASSERTED HERE UNTIL IT WAS WRITTEN DOWN, AND THIS SENTENCE CLAIMED IT WAS — kept
 * rather than quietly repaired, because the retired reason is the one a reader re-derives on meeting the new
 * assert and calling it redundant. It read that the entry asserts both halves and that
 * `line_box_inline_fragments` "no longer does for it", which is true of REPLACEDNESS and silent about
 * `display`: the `display` half was left to core/layout/scrolling_area.c's `sa_is_non_replaced_inline`, which
 * does test both, so the one caller was safe while the ENTRY was not — and `line_box_inline_fragments`' own
 * `display` assert READS as covering the second half while ADMITTING the one box §8.3's exception excludes. A
 * non-replaced `inline-block` therefore passed every assert on this path and took the block arm, which reports
 * §10.6.1's border area AS the margin edge, so its used `margin-top` and `margin-bottom` were dropped with
 * nothing to say so. HOW ITS ABSENCE WOULD HAVE SHOWN: a scrolling area reported INSIDE the real block-axis
 * margin edge of a box whose cascade gives it `display: inline-block` and a vertical margin, with no abort
 * anywhere — which is why no caller could have found it by running. RETIREMENT: this record goes when
 * `line_box_inline_fragments`' two delimitations are two entries each asserting its own shape, because no
 * reader can then re-derive that its one precondition covers this one.
 *
 * IT EXISTS BECAUSE AN INLINE BOX HAS NO `width` AND NO `height`, so the ONE-ORIGIN-PLUS-ONE-EXTENT shape every
 * other box's margin edge is composed from cannot describe it. CSS 2.2 §10.3.1 "Inline, non-replaced elements"
 * is one sentence long — "The 'width' property does not apply" — and §10.6.1 "Inline, non-replaced elements"
 * opens with "The 'height' property does not apply", so core/layout/used_value.h's border-edge EXTENT asserts
 * against exactly this box and a caller that composed one would abort in that file, naming CSSOM §9's
 * applicability contract, for a question that was asked here. §9.4.2 says what is there instead: "when an
 * inline box exceeds the width of a line box, it is SPLIT into several boxes and these boxes are distributed
 * across several line boxes", so the box is a SET of border areas and its margin edge is an extreme over them.
 *
 * ITS CALLER IS CSSOM VIEW §2 "Terminology"'s SCROLLING AREA — "the right margin edge of all of the element's
 * descendants' boxes" — and an inline box is one of those descendants whenever a page asks `scrollWidth` of a
 * container holding a `<span>`. CSSOM VIEW §6's own `scrollWidth` steps have no inline exclusion (its step 1
 * terminates only for "no associated box", where `clientWidth`'s terminates for "the box is inline"), so this
 * is not a corner: it is the shape of every paragraph.
 *
 * THE TWO MARGINS GO ON THE TWO FRAGMENTS THAT CARRY THE BOX'S OWN BOUNDARIES, WHICH ARE THE FIRST AND THE
 * LAST, and that is `line_box_inline_fragments`' own loop rather than a rule restated here. §9.4.2: "when an
 * inline box is split, margins, borders, and padding have NO VISUAL EFFECT where the split occurs (or at any
 * split, when there are several)" — so a middle fragment runs edge to edge with no margin on either side. That
 * entry emits one fragment per line its `[open, close]` item range intersects, in line order, and its lines
 * PARTITION the item collection: the first intersecting line is therefore the one holding `open` and the last
 * the one holding `close`, which is precisely where it took the two margins OFF to report §6's border area.
 * They go back on here and nowhere else.
 *
 * THE BLOCK AXIS TAKES NO MARGIN AT ALL, and that is CSS 2.2 §8.3 "Margin properties" in so many words: "These
 * properties apply to all elements, but VERTICAL MARGINS WILL NOT HAVE ANY EFFECT ON NON-REPLACED INLINE
 * ELEMENTS", restated in the `margin-top`/`margin-bottom` definition as "These properties have no effect on
 * non-replaced inline elements." So on that axis the margin edge IS the border area §10.6.1 nests around the
 * content area, and adding a vertical margin here would report an overflow no user agent draws.
 *
 * THE EXTREME IS OVER EVERY FRAGMENT AND NOT OVER THE TWO ENDS, and §8.3's NEGATIVE margin is what makes those
 * two different answers: "negative values for margin properties are allowed", so a `margin-left: -20px` puts
 * the FIRST fragment's margin edge 20px INSIDE its own border edge, and a middle fragment — whose split edge
 * carries no margin at all — is then the outermost thing the box has. Each fragment contributes the coordinate
 * the split sentence gives IT, and the extreme is over those; a fragment's bare border edge where a margin
 * belongs is not a coordinate any margin edge of this box occupies and is never an operand. */
void line_box_inline_margin_span(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 bool vertical, CssPx *lo, CssPx *hi);

#endif
