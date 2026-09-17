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
 * NAMED RESIDUAL — CSS 2.1 §E.2's STEP 1, WHICH IS THE CANVAS AND HAS NO RECTANGLE.
 * WHAT IS NOT COVERED: `PAINT_STEP_ROOT_BACKGROUND` is counted as an offer and produces no mark, so nothing
 * this component paints reaches the canvas. CSS 2.1 §E.2 step 1 is "background color of element over the
 * entire canvas", and what blocks it is the EXTENT and not the colour: core/paint/display_list.h's one mark
 * kind carries a RECTANGLE, and CSS 2.1 §2.3.1 "The canvas" says "the canvas is infinite for each dimension of
 * the space, but rendering generally occurs within a finite region of the canvas established by the user agent
 * according to the target medium" — so which finite region a canvas fill covers is a fact about the SURFACE
 * being rasterized and not one any document states. CSS 2.1 §14.2 "The background"'s CONDITION is not what
 * blocks it: both of its conjuncts are read in this file already, and the arm below that decides which element
 * may paint its own background is the same rule read from the other side.
 * WHAT THE NEXT DIFF BUILDS: a SECOND mark kind in core/paint/display_list.h — a fill whose extent is the
 * surface's rather than a rectangle's — and step 1's arm here, which asks CSS 2.1 §14.2's condition it already
 * asks and takes the background properties of the root or of its first body child accordingly. Nothing else is
 * missing: the colour of either element is `css_used_color`'s answer today.
 * HOW ITS ABSENCE WOULD SHOW: a document whose background is declared on its root or on its body paints no
 * background AT ALL. The body case is the common one and is the sharper symptom, because CSS 2.1 §14.2 moves
 * that colour off the body's own box as well — "must not paint a background for that child element" — so a
 * page carrying nothing but `body { background: #fff }` comes out with every other box painted and no page
 * background anywhere, rather than with a background that merely stops at the body's edges.
 * RETIREMENT: this record goes when `box_paint_stacking_context` appends a mark for
 * `PAINT_STEP_ROOT_BACKGROUND`.
 *
 * NAMED RESIDUAL — THE STEPS WHOSE MARK KIND DOES NOT EXIST YET.
 * WHAT IS NOT COVERED: `PAINT_STEP_TABLE_BORDERS`, `PAINT_STEP_INLINE_LINE_BOXES`,
 * `PAINT_STEP_REPLACED_CONTENT` and `PAINT_STEP_LINE_BOXES` are each counted as an offer and append nothing.
 * They are CSS 2.1 §E.2's table-arm item 7 — "all table borders (in tree order for separated borders)" — and
 * its steps 6, 7.1 and 7.2, which reach "the replaced content, atomically" and a line-box sub-list ending in
 * "the text": a BORDER mark, a SURFACE and a TEXT mark, none of which core/paint/display_list.h has a kind for.
 * WHAT THE NEXT DIFF BUILDS: the BORDER mark first, because every operand of it exists — CSS 2.1 §8.5 "Border
 * properties"' widths, styles and colours are each in lexbor's property registry and core/layout/used_value.h
 * answers the used width per side — and it lands for the block arms this file already paints before it lands
 * for item 7, whose own enumeration over a table's internal boxes is paint_order.h's residual rather than this
 * one's. The TEXT mark is next and carries core/layout/text_run.h's OWN advances: the geometry
 * core/dom/element_view.h reports was measured with core/fonts/open_type_metrics.h, so a rasterizer that
 * measures text for itself composites one engine's ink onto another engine's layout.
 * HOW ITS ABSENCE WOULD SHOW: a painted document is flat coloured areas with no text, no images and no borders
 * — every rectangle where CSS 2.1 §E.2 puts it and nothing drawn inside any of them.
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
