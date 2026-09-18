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
 * NAMED RESIDUAL — A ROOT ELEMENT THAT GENERATES NO BOX REACHES AN ABORT RATHER THAN AN EMPTY IMAGE.
 * WHAT IS NOT COVERED: a document whose root element's computed `display` is one of css-display-3 §2.5 "Box
 * Generation: the none and contents keywords"' two keywords. CSS 2.1 §9.2 "Controlling box generation" is
 * where the box stops existing, and core/paint/stacking_order.c's stacking-context test asserts its operand
 * generates one — "CSS 2.1 §9.9 is stated over BOXES throughout" — so the walk this entry performs aborts at
 * that precondition instead of answering. It is UNREACHABLE from any caller in this tree today: the only
 * documents that reach this entry are ones engine/host/test_forced.c writes, and the production ABI entries
 * over it have no method on `content.mojom.Renderer` to be called through, so no page's markup can select the
 * arm. The diff that gives them one is the diff that must build the predicate first.
 * WHAT THE NEXT DIFF BUILDS: core/paint/stacking_order.h gains an entry answering whether an element
 * generates a box at all — the `so_generates_box` static in that file, made public and read HERE before the
 * walk — and a root that generates none takes the region's own surface with CSS 2.1 §E.2's step 1 laying
 * nothing on it, which is an empty image and not an absent one.
 * HOW ITS ABSENCE WOULD SHOW: a dev build aborts inside the stacking-context test rather than in this file,
 * under a message about an element that generates no box, on a document whose root carries one of those two
 * keywords; a release build reaches the same walk with the assert compiled out.
 * WHO MAY RETIRE IT: any lane, because both halves are C in this tree and neither needs an artifact. */
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
   image — so a caller that treats it as a failure discards a rendering that is correct as far as it goes. */
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
   nothing is drawn. */
bool document_paint(JSContext *ctx, lxb_html_document_t *dom, RasterSurface *out, DocumentPaintCount *count);

#endif /* ENGINE_HOST_BROWSER_CORE_PAINT_DOCUMENT_PAINT_H */
