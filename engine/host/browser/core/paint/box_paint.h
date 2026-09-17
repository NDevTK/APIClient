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
 * and the CONDITION is `bp_canvas_background_element`'s, and the colour item uses both today. What blocks it
 * is the IMAGE MARK, which core/paint/display_list.h has no kind for — and that is ONE gap and not one per
 * step, because every image item of every step of CSS 2.1 §E.2 wants the same operand and no step has it.
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
 * NAMED RESIDUAL — THE FOUR STEPS THAT STILL APPEND NOTHING, AND THE ONE OF THEM THAT IS NO LONGER WAITING ON
 * A MARK KIND.
 * WHAT IS NOT COVERED: `PAINT_STEP_TABLE_BORDERS`, `PAINT_STEP_INLINE_LINE_BOXES`,
 * `PAINT_STEP_REPLACED_CONTENT` and `PAINT_STEP_LINE_BOXES` are each counted as an offer and append nothing.
 * Three of them want a VOCABULARY — CSS 2.1 §E.2's steps 6, 7.1 and 7.2 reach "the replaced content,
 * atomically" and a line-box sub-list ending in "the text", which are a SURFACE and a TEXT mark that
 * core/paint/display_list.h has no kind for. `PAINT_STEP_TABLE_BORDERS` wants neither: the BORDER mark exists
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
 * arm for that step calls `bp_border` per offered box and needs nothing new here. The TEXT mark is next and
 * carries core/layout/text_run.h's OWN advances: the geometry core/dom/element_view.h reports was measured
 * with core/fonts/open_type_metrics.h, so a rasterizer that measures text for itself composites one engine's
 * ink onto another engine's layout.
 * HOW ITS ABSENCE WOULD SHOW: a document's block boxes come out with their borders and a TABLE comes out with
 * none of them — no rule between its cells, none round the table — while every background level the table has
 * is painted; and no document has any text or any image inside any of its areas.
 * RETIREMENT: this record loses a clause as each of the four steps gains ink, and goes when every step
 * paint_order.h offers appends at least one mark or a surface.
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

/* CSS 2.1 §E.2 "Painting order"'s INK FOR ONE STACKING CONTEXT, appended to `out` in CSS 2.1 §E.2's own
   sequence, with the number of offers the walk made written to `offers`.
   `out` must be an INITIALISED list and is appended to rather than replaced, so a caller composing several
   contexts into one surface keeps the order it composed them in; `offers` is required for the reason the
   header gives. `context_el` MUST FORM A STACKING CONTEXT — that is `paint_order_walk`'s own precondition and
   it crashes there, because CSS 2.1 §E.2's text is stated for "an element generating a stacking context".
   ANSWERS what the walk answered: true when it ran to the end, and false when this painter met an operand it
   could not compute and stopped it. A false answer LEAVES `out` AND `offers` AS THEY STOOD — every mark
   already appended stays appended, which is what paint_order.h's visitor contract exists to make possible and
   is why a document with one unpaintable box is a partial picture rather than no picture. */
bool box_paint_stacking_context(JSContext *ctx, lxb_dom_element_t *context_el, DisplayList *out,
                                unsigned *offers);

#endif
