/* WHAT A DOCUMENT LOOKS LIKE — the one entry that takes a Document to an RGBA bitmap, and the JOIN
 * core/paint/display_list_raster.h deliberately refuses to make.
 *
 * IT IS A COMPONENT BECAUSE THE TWO HALVES BELOW IT EACH REFUSE THE OTHER'S OPERAND, and that refusal is
 * argued at both of them rather than assumed here. core/paint/box_paint.h paints ONE STACKING CONTEXT and
 * takes no surface: its output is CSS 2.1 §E.2 "Painting order"'s marks, and it says so by appending rather
 * than replacing, "so a caller composing several contexts into one surface keeps the order it composed them
 * in". core/paint/display_list_raster.h composites a list onto a surface the caller has ALREADY SIZED, and
 * ends the argument in one sentence: "There is deliberately no entry here that reads a viewport." So the
 * ROOT ELEMENT, the REGION and the DEVICE RATIO are three operands neither of them will assemble, and the
 * component that assembles them is a third one. Writing that assembly at each caller instead is the second
 * copy every header on this road warns about — a rasterizing caller and a painting caller would then each
 * read CSS 2.1 §2.3.1 "The canvas"'s region, and two answers to one question are free to disagree about the
 * size of the image the other one drew.
 *
 * THE REGION IS READ FIRST AND IT IS WHAT THE BOOL ANSWERS. CSS 2.1 §2.3.1 leaves the rendered area to the
 * user agent — "The canvas is infinite for each dimension of the space, but rendering generally occurs within
 * a finite region of the canvas, established by the user agent according to the target medium" — and
 * core/frame/viewport.h is where this engine establishes it. A document that NO NAVIGABLE PRESENTS has no
 * such region at all, which `viewport_canvas_region` answers false for, and that is a state rather than a
 * failure: there is no image, because there is nowhere for one to be. It is asked BEFORE the walk because the
 * walk's own step 1 asks the same component the same question, so a document with no region would otherwise
 * be walked in full to produce a list with no canvas mark in it.
 *
 * THE BOOL ANSWERS THAT ONE QUESTION AND THE COUNT ANSWERS EVERY OTHER, which is the split
 * core/paint/box_paint.h's `offers` and core/paint/display_list_raster.h's three numbers already make one
 * level down: a bitmap with no ink on it means the walk reached no box, or reached boxes that painted
 * nothing, or was STOPPED — and a caller handed only the bitmap cannot separate them. A PAINTER THAT STOPS IS
 * NOT A FALSE ANSWER HERE, because box_paint's own contract makes it a partial picture rather than no
 * picture: "A false answer LEAVES `out` AND `offers` AS THEY STOOD — every mark already appended stays
 * appended". So a stopped walk rasterizes what it laid and reports `complete` false, and the bool stays the
 * region's alone. Two facts, two fields; one bit answering both is the shape that gets read as the other.
 *
 * `out` IS INITIALISED HERE AND FREED BY THE CALLER, ALWAYS, INCLUDING ON A FALSE ANSWER. The zero-area
 * surface core/graphics/raster_surface.h defines — NULL pixels with both dimensions zero — is what a false
 * answer leaves, so there is no arm on which a caller must remember not to free. What `out` may NOT be is a
 * surface that already holds pixels: `raster_surface_init` overwrites `px` without freeing it, so the caller
 * frees before it asks again, exactly as it would before any other initialisation.
 *
 * A ROOT THAT GENERATES NO BOX IS AN EMPTY IMAGE AND NOT AN ABSENT ONE, AND THE KEYWORD THAT REACHES IT IS
 * ONE RATHER THAN css-display-3 §2.5 "Box Generation: the none and contents keywords"' TWO. A root whose
 * computed `display` is `none` generates nothing for CSS 2.1 §E.2 "Painting order" to walk — css-display-3
 * §2.5 states it of the whole subtree, "The element and its descendants generate no boxes or text sequences"
 * — and core/paint/stacking_order.h's stacking-context test is stated over a BOX and crashes rather than
 * answering for one that does not exist. THAT CRASH IS CORRECT AND ITS OWN MESSAGE SAYS WHOSE QUESTION THIS
 * IS: `The caller's own walk is what knows the difference`. This entry is that caller, it asks BEFORE the
 * walk, and the region's own surface with nothing laid on it is the answer.
 * THE OTHER KEYWORD CANNOT REACH A ROOT AT ALL, which is css-display-3 §2.8 "The Root Element's Principal
 * Box" and is ASSERTED here rather than assumed: css-display-3 §2.8 says "Additionally, a display of contents
 * computes to block on the root element", and core/css/css_computed_value.c performs exactly that — so a
 * `contents` arriving here is this engine's own computed value disagreeing with the section it implements
 * and is never anything a page declared. `none` survives the same blockification because css-display-3 §2.7
 * "Automatic Box Type Transformations" says it does: "This has no effect on display types that generate no
 * box at all, such as none or contents".
 *
 * THE RESIDUAL THAT STOOD HERE IS RETIRED, AND IT WAS WRONG IN BOTH CLAUSES A READER ACTS ON — WHICH IS THE
 * PART WORTH KEEPING, because a residual is read once, by somebody who has already decided to build it.
 * ITS NOT-COVERED CLAUSE ENUMERATED INPUTS rather than a property — `one of css-display-3 §2.5's two
 * keywords` — and one of the two is already answered by a step that runs EARLIER than the step the residual
 * was about, so a reader building to the list would have written an arm that cannot run and would have read
 * the `contents` case as unhandled when css-display-3 §2.8 handles it. The check that catches this is one
 * question asked of each named input: WHICH STEP FIRST SEES IT.
 * ITS NEXT-DIFF CLAUSE NAMED THE WRONG COMPONENT AND THE WRONG SHAPE. It said to make `so_generates_box` an
 * entry of core/paint/stacking_order.h, which proposes a css-display-3 §2.5 predicate as a public entry of a
 * CSS 2.1 §9.9 header; and the tree ALREADY answers box generation publicly, in core/dom/element_view.h,
 * whose `element_view_has_box` is HTML's `being rendered` and therefore folds PRESENTATION into the same bit
 * — connectedness, an owner document, a viewport. THOSE TWO FACTS ROUTE TO OPPOSITE ARMS HERE: a document
 * with no rendered region answers FALSE and a document with no root box answers TRUE with an empty image, so
 * one bit for both is the shape this entry's own bool/count split exists to avoid. Neither predicate is read
 * here. What is read is the ROOT's own computed `display`, which css-display-3 §2.8 reduces to one keyword.
 * RETIREMENT: this record goes when a public entry answers css-display-3 §2.5's box generation WITHOUT also
 * answering whether the document is presented, and this file routes to it. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_DOCUMENT_PAINT_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_DOCUMENT_PAINT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/html/html.h>

#include "core/graphics/raster_surface.h"
#include "quickjs.h"

/* WHAT A PAINT DID, in the four numbers a bitmap cannot state and the one bit that says whether it is whole.
   `offers` is core/paint/box_paint.h's own — the steps CSS 2.1 §E.2 "Painting order"'s walk OFFERED, which
   separates a walk that reached no box from one whose boxes painted nothing. `marks`, `spans` and `pixels`
   are core/paint/display_list_raster.h's three, unchanged in meaning: a pixel is counted once per mark that
   covers it, so a document whose marks overlap reports more pixels than the surface holds.
   `complete` IS THE WALK'S OWN ANSWER AND IS NOT THIS ENTRY'S RETURN. False is a painter that met an operand
   it could not compute and stopped, which leaves a PARTIAL picture — every mark it had already laid is in the
   image — so a caller that treats it as a failure discards a rendering that is correct as far as it goes.
   It is TRUE for a root that generates no box, whose picture is whole and empty; `offers` is the field that
   separates that from a walk, because a walk over a root always offers at least one step. */
typedef struct {
    unsigned offers;
    size_t   marks;
    size_t   spans;
    size_t   pixels;
    bool     complete;
} DocumentPaintCount;

/* RENDER THE DOCUMENT `dom` PRESENTS, in the realm `ctx`, into `out`, writing what it did to `count`.
   ANSWERS FALSE where CSS 2.1 §2.3.1 "The canvas" establishes no rendered region for this document — which
   core/frame/viewport.h decides and nothing here re-derives — and then `out` is a zero-area surface and every
   field of `count` is zero. `out` is INITIALISED by this entry on every arm and is the caller's to free on
   every arm; `count` is required for core/paint/display_list_raster.h's own reason and is written even when
   nothing is drawn.
   A ROOT THAT GENERATES NO BOX ANSWERS TRUE WITH AN EMPTY IMAGE — `out` is the region's own device size,
   every number in `count` is zero and `complete` is TRUE, because nothing was left unpainted rather than a
   painter having stopped. `offers` of ZERO is what tells that state from a walk that ran:
   core/paint/paint_order.c offers CSS 2.1 §E.2 "Painting order"'s step 1 for every root it walks, so a walk
   that happened reports at least one. */
bool document_paint(JSContext *ctx, lxb_html_document_t *dom, RasterSurface *out, DocumentPaintCount *count);

#endif /* ENGINE_HOST_BROWSER_CORE_PAINT_DOCUMENT_PAINT_H */
