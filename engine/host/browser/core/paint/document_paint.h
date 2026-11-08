/* WHAT A DOCUMENT LOOKS LIKE — HTML §8.1.7.3 update the rendering step 22's paint ("update the rendering or user
 * interface of doc and its node navigable to reflect the current state"), and the raster that turns what it left
 * into an RGBA bitmap.
 *
 * It is a component because the two halves below it each refuse the other's operand: core/paint/box_paint.h
 * paints one stacking context into a display list and takes no surface, and core/paint/display_list_raster.h
 * composites a list onto a surface the caller has already sized and reads no viewport. The root element, CSS
 * 2.1 §2.3.1 "The canvas"'s region and the device ratio are three operands neither assembles.
 *
 * The paint and the raster are two entries because they happen at two moments. The paint is step 22, run by
 * core/rendering/rendering.c inside a layout job it enters and resting between offers, and it reads the tree, the cascade and the viewport of
 * the world standing at that rendering opportunity; its frame is immutable and core/paint/painted_frame.h keeps
 * the document's last one. The raster reads the frame alone, so an image is of the frame the page's own event
 * loop painted and never of a tree read later.
 *
 * The region is read first. A document no navigable presents has none, and its frame holds no picture: the
 * raster answers false with a zero-area surface. A root that generates no box (css-display-3 §2.5's `none`; §2.8
 * computes `contents` to `block` on the root) or no document element at all is a whole, empty picture: the
 * region's surface with no mark, `complete` true and zero offers. A painter that stops is not a false answer:
 * box_paint keeps every mark it laid, so the frame is a partial picture with `complete` false. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_DOCUMENT_PAINT_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_DOCUMENT_PAINT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/graphics/raster_surface.h"
#include "core/paint/box_paint.h"   /* `BoxPaintCensus` — what became of each of CSS 2.1 §E.2's offers, and
                                       why the ones that laid nothing laid nothing */
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
    size_t   marks;
    size_t   spans;
    size_t   pixels;
    bool     complete;
    /* WHAT BECAME OF EACH OF THOSE OFFERS, and WHY the ones that laid nothing laid nothing —
       core/paint/box_paint.h's two populations, carried whole rather than summarised. Its own `offers` is the
       DENOMINATOR of both and is what a caller quoting any of these numbers quotes beside them; a decline
       reported without it is a numerator alone.
       THIS STRUCT USED TO CARRY ITS OWN `offers` FIELD BESIDE THE CENSUS AND IT IS DELETED RATHER THAN
       ASSERTED EQUAL. Two fields holding one fact is the pair that drifts, and an assert between them would
       have been the vacuous kind — this entry assigned one from the other on the line above it, so the two
       sides could not disagree under any state of the program. The field that survives is the census's,
       because the census is where the number is DERIVED. */
    BoxPaintCensus census;
    /* AND HOW BIG THE TREE WAS THAT THE WALK RAN OVER, which is the one fact `offers` cannot carry and the
       one a reader needs most.
       AN OFFER COUNT IS A STATEMENT ABOUT THE WALK AND THIS IS A STATEMENT ABOUT THE DOCUMENT, taken at the
       same instant, by the same walker, over the same subtree — DOM §4.8 "Interface ShadowRoot"'s
       shadow-including inclusive descendants of the root element, which is the population CSS 2.1 §E.2's own
       walk advances through. Two readings of one moment: a walk offering seven steps over three elements is
       a document with nothing in it, and a walk offering seven over nine hundred is a walk that reached
       almost none of a document that HAS something in it, and those take opposite work. Nothing else this
       entry reports separates them, because every number beside it is the walk's own.
       IT IS NOT AN ASSERT AND CANNOT BE ONE, which is worth stating so the next reader does not spend a diff
       trying: no sound inequality holds between the two. Every element may legitimately generate no box
       (css-display-3 §2.5's `none`, which `po_box_generation` skips WITH its subtree), so the offers may be
       far below the elements; and CSS 2.1 §E.2's table arm offers SEVEN steps for one `display: table`
       element while its steps 4 and 7 offer two per in-flow block-level descendant, so the offers may be far
       above them. The pair is a COMPARISON a reader makes and not a bound this engine may assert. */
    unsigned elements;
} DocumentPaintCount;

/* Step 22's output for one document: the region it painted for, CSS 2.1 §E.2's list and its census. */
typedef struct DocumentPaintFrame DocumentPaintFrame;

/* Paint `doc` in its realm `ctx`, resting between CSS 2.1 §E.2's offers. `begin` answers a newly allocated frame
   the caller owns (document_paint_frame_free, which also releases a paint abandoned mid-walk); `step` paints one
   offer and answers whether another remains; `finish` closes the walk, after which the frame is immutable. Each
   of the three runs with the layout job the walk's questions are asked on entered (asserted at `begin`), so
   every computed value is held in that job's cascade record; the job outlives the walk. */
DocumentPaintFrame *document_paint_begin(JSContext *ctx, lxb_dom_document_t *doc);
bool document_paint_step(DocumentPaintFrame *f);
/* Whether the next offer's geometry is answered: CSSOM View §6's client rectangles of its element, asked under a
   resting drive this opens on the entered job, so the offer reads them from the job's memo. False ("not yet")
   when the activation must rest, its questions suspended on the job; true when no offer is left. Asked before
   each `step`, with no drive open. */
bool document_paint_ready(DocumentPaintFrame *f);
void document_paint_finish(DocumentPaintFrame *f);
bool document_paint_finished(const DocumentPaintFrame *f);
void document_paint_frame_free(DocumentPaintFrame *f);

/* Rasterize `f` into `out`, writing what the paint and the raster did to `count`. False where the frame holds no
   rendered region, and then `out` is a zero-area surface and every field of `count` is zero. `out` is
   initialised on every arm and is the caller's to free on every arm; it may not already hold pixels. */
bool document_paint_raster(const DocumentPaintFrame *f, RasterSurface *out, DocumentPaintCount *count);

#endif /* ENGINE_HOST_BROWSER_CORE_PAINT_DOCUMENT_PAINT_H */
