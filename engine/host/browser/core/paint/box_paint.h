/* THE PAINTER — the JOIN between core/paint/paint_order.h's offers and core/paint/display_list.h's ink, and
 * the component that performs CSS 2.1 §E.2 "Painting order"'s own sub-lists.
 *
 * WHAT THE SEAM IS. paint_order.h offers `(step, element)` and says in its own words that it states CSS 2.1
 * §E.2's TOP-LEVEL order and "does not state the order of marks WITHIN one of those offers — CSS 2.1 §E.2's own
 * sub-lists". Two of those sub-lists are ENUMERATIONS and are that component's residual; the rest are a MARK
 * VOCABULARY, which "belongs to whatever lays ink". This is that. Every operand of the walk is a computed
 * `display`, `position`, `float` and `z-index` plus tree order; every operand HERE is a colour, a rectangle, a
 * font and an image — which is exactly why the two are separable and why the walk could be complete at a
 * revision where almost none of the ink existed.
 *
 * THE OFFER COUNT IS AN ANSWER AND NOT AN INSTRUMENT, which is why it is a required out-parameter rather than
 * something a caller may leave out. A display list of ZERO marks means two different things — the walk ran and
 * every box it offered was transparent, or the walk never reached a box at all — and those take opposite work:
 * the first is a document with no ink in it and the second is a painter that was never asked anything. A
 * caller handed only the list cannot tell them apart, and a number that cannot separate an ABSENCE from a ZERO
 * is the shape CLAUDE.md names by hand. So the count of offers comes back beside the list, the entry crashes
 * rather than accepting nowhere to put it, and a caller reporting one reports both.
 *
 * A CASCADE VALUE IS THE PAGE'S AND IS NEVER ASSERTED ON. This file reads colours out of a document, so every
 * read goes through core/css/css_computed_value.h's `css_used_color`, which owns the two crashes a colour can
 * produce and states them against the SPEC rather than against the document: a property spelling this engine
 * has no registry row for, and a `<color>` production the parse does not have. What this file may assert is
 * what it computed itself — that a walk it started offered a step it defines, that a list it built took the
 * mark it appended — and nothing about what any page declared.
 *
 * NAMED RESIDUAL — CSS 2.1 §E.2's STEP 1 IS TWO ITEMS AND THIS PAINTS THE FIRST.
 * WHAT IS NOT COVERED: the canvas's background IMAGE. CSS 2.1 §E.2's step 1 second item is "background image
 * of element, over the entire canvas, anchored at the origin that would be used if it was painted for the
 * root element", and CSS 2.1 §14.2 "The background" states the anchoring again for the propagated case —
 * "Such backgrounds must also be anchored at the same point as they would be if they were painted only for
 * the root element".
 * NEITHER OF THE TWO THINGS THAT USED TO BLOCK STEP 1 BLOCKS THIS: the EXTENT is `bp_canvas_region`'s answer
 * and the CONDITION is `bp_canvas_background_element`'s, and the colour item uses both today.
 * THIS CLAUSE USED TO NAME A THIRD — it said what blocked the item was the IMAGE MARK, which
 * core/paint/display_list.h had no kind for. THE MARK EXISTS: `DISPLAY_MARK_IMAGE`, which
 * `bp_replaced_content` appends for both of CSS 2.1 §E.2's replaced-content items. The sentence went on
 * denying it while core/paint/display_list.h's own residual recorded the kind as landed, which is one
 * component disagreeing with itself in the UNDER-CLAIM direction: a reader deciding whether to BUILD an image
 * mark is the only reader such a sentence has, so a stale one argues for a second kind beside the one that is
 * already there.
 * WHAT BLOCKS IT IS THE OPERAND, and that is ONE gap and not one per step, because every image item of every
 * step of CSS 2.1 §E.2 wants the same operand and no step has it.
 * THE ANCHORING IS AN IMAGE CONCERN AND NOT A SECOND EXTENT, which is worth saying because both sentences
 * quoted above are about it: a `background-position` names the origin a repeating image is laid from, and a
 * SOLID COLOUR covering an area CSS 2.1 §2.3.1 "The canvas" makes infinite has no origin to be anchored at.
 * So the colour item needed none of it and the image item needs all of it.
 * WHAT THE NEXT DIFF BUILDS: not this component's. What must EXIST before an image mark can be appended
 * anywhere is an `<image>` that has become PIXELS — this engine's `<image>` road ends at a validity test,
 * core/css/css_image.h answering whether a component value matches css-images-3 §2 "Image Values: the <image>
 * type" while deliberately keeping the author's own bytes, so nothing turns a `<url>` into anything a surface
 * could composite. The diff that unblocks every image item at once is the one that makes such a thing exist,
 * and CSS 2.1 §E.2's step 1 then gains its second item here beside the first.
 * HOW ITS ABSENCE WOULD SHOW: a page whose background is declared as an image alone paints no page
 * background, while the same page declaring a colour beside the image paints the colour and none of the image
 * — so a document comes out with its fallback colour where a browser puts its artwork.
 * RETIREMENT: this record goes when `box_paint_stacking_context` appends a mark for CSS 2.1 §E.2's step 1
 * second item.
 *
 * NAMED RESIDUAL — THE TWO STEPS THAT STILL APPEND NOTHING, AND THE TWO DIFFERENT THINGS THEY WANT.
 * WHAT IS NOT COVERED: `PAINT_STEP_TABLE_BORDERS` and `PAINT_STEP_INLINE_LINE_BOXES` are each counted as an
 * offer and append nothing, and the two are not waiting on the same thing.
 * THIS RECORD USED TO NAME A THIRD, `PAINT_STEP_REPLACED_CONTENT`, and to say it wanted a VOCABULARY — that
 * CSS 2.1 §E.2's step 7.1 was a SURFACE rather than a mark and core/paint/display_list.h had no kind for it.
 * The kind exists and that step now appends through `bp_replaced_content`, which is the same producer step
 * 7.2.1's item 4 third arm reaches.
 * `PAINT_STEP_INLINE_LINE_BOXES` wants neither a mark nor an
 * enumeration any more, and THIS SENTENCE USED TO SAY IT WANTED THE ENUMERATION core/paint/paint_order.h's
 * residual (c) NAMES: step 6's sub-list is the same step 7.2.1 `bp_step_7_2_1` now performs, so the sequence
 * exists and the gap moved. What it wants is an ENTRY INTO that walk for a box that is ON a line rather than
 * one that establishes the lines — the establishing container, the `BlockFlowRun` this box's items are in and
 * that container's content box origin, which is the triple `bp_line_boxes` derives for a block and which
 * core/layout/line_box.h's `line_box_inline_fragments` already finds for an inline box in its own first step. `PAINT_STEP_TABLE_BORDERS` wants neither: the BORDER mark exists
 * and this file lays it for CSS 2.1 §E.2's step 2 and step 4 BLOCK arms. What that item wants is an
 * ENUMERATION — its text is "all table borders (in tree order for separated borders)" and paint_order.h offers
 * it ONCE carrying the table element, so the boxes whose borders it covers are not named and painting the
 * table's own border at that offer would lay one box's ink where CSS 2.1 §E.2 asks for every box's.
 * AND THE STYLE THIS FILE READS IS THE ELEMENT'S OWN, WHICH IS THE OTHER HALF OF WHY ITEM 7 IS NOT THIS DIFF.
 * `bp_border_style` reads `border-<side>-style` off the box, and for a table or a cell under CSS 2.1 §17.6.2
 * "The collapsing border model" the style that WON at an edge is that model's answer and not the element's
 * declaration — so the widths (which core/layout/used_value.h resolves per model) and the styles would
 * disagree. It is NOT reachable from the two arms painted here: CSS 2.1 §9.2.1 "Block-level elements and block
 * boxes" closes block-level at "'block', 'list-item', and 'table'", a `display: table` box takes CSS 2.1
 * §E.2's TABLE arm rather than its block arm, and every other collapsed-model box is a cell — so every box
 * these two arms reach is one the separated model covers. It becomes reachable the day item 7 is painted, and
 * the diff that paints it owes the collapsed style beside the collapsed width.
 * WHAT THE NEXT DIFF BUILDS: item 7's separated-model enumeration, which is paint_order.h's own residual — a
 * tree-order walk of the table's subtree gated on `border-collapse` being `separate`, after which this file's
 * arm for that step calls `bp_border` per offered box and needs nothing new here.
 * HOW ITS ABSENCE WOULD SHOW: a document's block boxes come out with their borders and a TABLE comes out with
 * none of them — no rule between its cells, none round the table — while every background level the table has
 * is painted; text inside a `<span>` that is itself a stacking context is missing while the same text in an
 * ordinary paragraph is drawn; and every replaced element is an empty area.
 * RETIREMENT: this record loses a clause as each of the three steps gains ink, and goes when every step
 * paint_order.h offers appends at least one mark or a surface.
 *
 * RETIRED CLAUSE — THE TEXT MARK, KEPT BECAUSE ITS NEXT-DIFF HALF NAMED A MECHANISM THE COMPONENT IT POINTED
 * AT FORBIDS. It read, in one run:
 * `The TEXT mark is next and carries core/layout/text_run.h's OWN advances: the geometry core/dom/element_view.h reports was measured with core/fonts/open_type_metrics.h, so a rasterizer that measures text for itself composites one engine's ink onto another engine's layout.`
 * Its SPEC half is exact and is the contract the glyph kind was built to. Its MECHANISM half — a mark that
 * CARRIES the advances — was not buildable: a run's advances are one per character and therefore
 * VARIABLE-LENGTH, so such a mark holds a pointer, and core/paint/display_list.h states that there is no
 * pointer on a mark and nothing there holds a borrowed one. What landed is ONE MARK PER CHARACTER carrying its
 * PEN POSITION, which discharges the same contract more strongly: a consumer handed a position per character
 * has no pen to advance and therefore nothing it could re-measure with. The full argument, and the two other
 * designs that were refused, are recorded at core/paint/display_list.h's own retired clause.
 *
 * NAMED RESIDUAL — CSS 2.1 §17.5.1's FOUR INTERMEDIATE TABLE LAYERS, WHICH ARE A GEOMETRY AND NOT A MARK.
 * WHAT IS NOT COVERED: `PAINT_STEP_COLUMN_GROUP_BACKGROUND`, `PAINT_STEP_COLUMN_BACKGROUND`,
 * `PAINT_STEP_ROW_GROUP_BACKGROUND` and `PAINT_STEP_ROW_BACKGROUND` are each counted as an offer and append
 * nothing, while the two levels either side of them — the table box's own plane and the cells — are painted.
 * The reason is not the colour and not the mark kind: it is that CSS 2.1 §17.5.1 "Table layers and
 * transparency" states each of these four areas as a DERIVATION OVER THE CELLS rather than as the box's own.
 * A column group's background "covers exactly the full area of all cells that originate in the column group
 * even if they span outside the column group"; a column's the same over its own cells; a row group "extends
 * from the top left corner of its topmost cell in the first column to the bottom right corner of its
 * bottommost cell in the last column"; and a row "is as wide as the row groups and as tall as a normal single
 * row spanning cell". None of those is the element's border box, which is the one rectangle
 * core/dom/element_view.h answers, so painting them at that rectangle would be ink in a place CSS 2.1 §17.5.1
 * does not put it — WRONG rather than narrow, which is why the offer is passed over rather than approximated.
 * WHAT THE NEXT DIFF BUILDS: §17.5.1's four areas over core/layout/table_grid.h, which already enumerates
 * which cells originate in which row, column, row group and column group — so it is a union of border boxes
 * the grid can name rather than a new layout.
 * HOW ITS ABSENCE WOULD SHOW: a table whose stripes are declared on its rows, columns or groups paints only
 * the cells' own backgrounds and the table's own plane, so a zebra-striped table comes out uniform while a
 * table striped by a rule on the cells comes out correctly.
 * RETIREMENT: this record goes when `box_paint_stacking_context` appends a mark for each of the four steps. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_BOX_PAINT_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_BOX_PAINT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/paint/display_list.h"
#include "quickjs.h"

/* WHAT BECAME OF ONE OFFER — the four answers that PARTITION the offers a walk made, which is the split the
   offer count alone could not state.
   THE COUNT WAS ONE NUMBER ANSWERING THREE QUESTIONS, which is the shape this header's own paragraph above
   already names one level up and which the count itself then had: a walk reporting offers and no marks is a
   walk every box of which was legitimately transparent, OR a walk that met a step this engine lays no ink
   for, OR a walk that STOPPED — and those take opposite work. The first is a correct picture of a document
   with no ink in it; the second names a capability to build; the third is a partial picture whose remaining
   boxes were never looked at.
   EXACTLY ONE MEMBER IS RECORDED PER OFFER and `box_paint_stacking_context` asserts that the four sum to the
   offer count, which is what makes the total unable to move without one of its parts moving. */
typedef enum {
    BOX_PAINT_INKED,   /* the offer appended at least one mark to the list */
    BOX_PAINT_SILENT,  /* every operand computed and none of them was ink — the CORRECT zero */
    BOX_PAINT_UNBUILT, /* the step, or the replaced element's kind, has no ink in this engine at this
                          revision — a capability named by one of this header's residuals */
    BOX_PAINT_STOPPED, /* the painter met an operand it could not compute and stopped the walk. It is ONE PER
                          OFFER THAT WAS STILL OPEN and not one per walk: CSS 2.1 §E.2's step 7.2.1 counts a
                          sub-offer for an inline-level replaced element INSIDE the step 7.2 offer that is
                          still being served, so a stop inside it is recorded for the sub-offer and again for
                          its parent as the false answer travels out. The INNERMOST one is where the operand
                          was; a count above one is a nesting depth and never a second failure */
    BOX_PAINT_OUTCOMES
} BoxPaintOutcome;

/* AND WHY IT LAID NOTHING, WHICH IS A SECOND POPULATION AND NOT A SUB-PARTITION OF THE FIRST. An offer is
   CSS 2.1 §E.2's STEP and a step's sub-list holds several items — its step 2 block arm is a background and a
   border, its step 7.2 is every box on every line box of one container — so one offer can decline for TWO
   reasons at once and a partition of the offers cannot express that. A `DISPLAY_MARK_FILL_RECT` laid beside a
   border area of zero extent is an INKED offer that nevertheless met `BOX_PAINT_DECLINE_NO_BORDER_AREA`, and
   a reader asking how many boxes declared no border wants it counted.
   SO THESE ARE NOT EXCLUSIVE AND THEY DO NOT SUM TO ANYTHING. Each counts the offers that met that reason at
   least once, their DENOMINATOR is the offer count, and each is therefore bounded by it — which is the one
   arithmetic statement that can be made about them and is asserted. Reading them as a partition is reading
   the other enum.
   THE ONE THING THAT TIES THE TWO TOGETHER IS ASSERTED AT THE OFFER: a `BOX_PAINT_SILENT` offer that met NO
   reason is a producer that declined and said nothing, which is the forcing function that makes a decline
   site added later state itself rather than disappear into a number. */
typedef enum {
    BOX_PAINT_DECLINE_TRANSPARENT,          /* a background colour whose alpha is zero. css-backgrounds-3
                                               §2.2's `Initial: transparent` makes this every box that
                                               declares none, so it is the commonest reason there is */
    BOX_PAINT_DECLINE_PROPAGATED_TO_CANVAS, /* CSS 2.1 §14.2 "The background" moved this background onto the
                                               canvas, so the ink is at CSS 2.1 §E.2's step 1 and not here */
    BOX_PAINT_DECLINE_ROOT_BACKGROUND,      /* CSS 2.1 §E.2's step 2 item 1 "unless it is the root element" */
    BOX_PAINT_DECLINE_NO_BORDER_AREA,       /* four USED border widths of zero, which covers no pixel */
    BOX_PAINT_DECLINE_NO_INLINE_CONTEXT,    /* a block-level box establishing no inline formatting context —
                                               CSS 2.2 §9.2.1's container holding only block-level boxes */
    BOX_PAINT_DECLINE_NO_CHARACTERS,        /* an inline formatting context whose line boxes placed none */
    BOX_PAINT_DECLINE_NO_REPLACED_CONTENT,  /* a replaced element this engine has an arm for whose pixels are
                                               absent — no request, no reply, a refusal, `broken`, no decoder,
                                               or a canvas in HTML §4.12.5's context mode NONE */
    BOX_PAINT_DECLINE_UNBUILT_REPLACED,     /* a replaced element whose content this agent cannot composite —
                                               see this header's residual for which sentence empties each */
    BOX_PAINT_DECLINE_UNBUILT_STEP,         /* a CSS 2.1 §E.2 step this painter lays no ink for at all */
    BOX_PAINT_DECLINES
} BoxPaintDecline;

/* WHAT ONE WALK DID, in the numbers a display list cannot be asked. `offers` is unchanged in meaning and is
   the DENOMINATOR of both arrays beside it; a caller that quotes one of them without it has published a
   numerator alone. Both are LIFETIME COUNTERS over one walk — they rise and never fall, so two of them may be
   differenced and a caller may accumulate them across several stacking contexts composed into one surface. */
typedef struct {
    unsigned offers;
    unsigned outcome[BOX_PAINT_OUTCOMES];
    unsigned decline[BOX_PAINT_DECLINES];
} BoxPaintCensus;

/* CSS 2.1 §E.2 "Painting order"'s INK FOR ONE STACKING CONTEXT, appended to `out` in CSS 2.1 §E.2's own
   sequence, with what the walk did written to `census`.
   `out` must be an INITIALISED list and is appended to rather than replaced, so a caller composing several
   contexts into one surface keeps the order it composed them in; `census` is required for the reason the
   header gives. `context_el` MUST FORM A STACKING CONTEXT — that is `paint_order_walk`'s own precondition and
   it crashes there, because CSS 2.1 §E.2's text is stated for "an element generating a stacking context".
   `census` IS ZEROED BY THIS ENTRY ON EVERY ARM, so a caller planting a sentinel in it reads that sentinel
   back only where the entry was never reached at all.
   ANSWERS what the walk answered: true when it ran to the end, and false when this painter met an operand it
   could not compute and stopped it. A false answer LEAVES `out` AS IT STOOD and leaves `census` describing
   the offers the walk DID make — every mark already appended stays appended, which is what paint_order.h's
   visitor contract exists to make possible and is why a document with one unpaintable box is a partial
   picture rather than no picture. The offer that stopped it is the one `BOX_PAINT_STOPPED`. */
bool box_paint_stacking_context(JSContext *ctx, lxb_dom_element_t *context_el, DisplayList *out,
                                BoxPaintCensus *census);

#endif
