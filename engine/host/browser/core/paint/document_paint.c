/* See core/paint/document_paint.h for why this is a component and not a line at each caller. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/frame/viewport.h"
#include "core/graphics/raster_surface.h"
#include "core/layout/layout_job.h"
#include "core/dom/element_view.h"   /* CSSOM View §6's client rectangles, asked before an offer reads them */
#include "solver/rest_unit.h"
#include "core/paint/box_paint.h"
#include "core/paint/display_list.h"
#include "core/paint/display_list_raster.h"
#include "core/paint/document_paint.h"
#include <lexbor/core/base.h>   /* lexbor_calloc: the allocator the engine gives its refusal edge */
#include "core/dom/shadow_root.h"    /* DOM §4.8's shadow-including tree order — the SAME walker CSS 2.1
                                        §E.2's own walk advances through, which is what makes the element
                                        count below a reading of the walk's own population */
#include "quickjs.h"

/* HOW BIG THE TREE IS THAT CSS 2.1 §E.2 "Painting order" IS ABOUT TO BE WALKED OVER — DOM §4.8 "Interface
 * ShadowRoot"'s shadow-including inclusive descendants of `root` that are ELEMENTS, counted here so that the
 * offer count beside it is read against a denominator rather than alone.
 *
 * IT IS THE SAME WALKER AND THE SAME SUBTREE AS THE PAINT, WHICH IS THE WHOLE POINT. core/paint/paint_order.c
 * advances with `shadow_root_next_in_shadow_including` from the context element and says in its own words
 * that a child navigable's Document is reached by NEITHER of that walker's two edges — so this counts exactly
 * the population §E.2's steps could have collected from, and a document whose content sits in shadow trees is
 * counted the same way the painter sees it. A plain child walk here would answer LOWER than the walk it is
 * the denominator of, which is the one direction that turns a comparison into a contradiction.
 *
 * IT IS NOT A CHECK AND NOTHING IS ASSERTED ABOUT THE RATIO — core/paint/document_paint.h's field holds the
 * reason, which is that no sound inequality exists in either direction.
 *
 * THE COST IS ONE TREE WALK PER PAINT and it is paid beside a walk that asks the CASCADE about every element
 * many times over; this reads a node type and a pointer. The alternative — deriving it inside §E.2's own walk
 * — would count only the elements that walk REACHED, which is the number `offers` already is. */
static unsigned dp_element_count(JSContext *ctx, lxb_dom_element_t *root)
{
    lxb_dom_node_t *r = lxb_dom_interface_node(root);
    lxb_dom_node_t *n;
    unsigned count = 0;

    for (n = r; n != NULL; n = shadow_root_next_in_shadow_including(ctx, n, r))
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) count++;
    DCHECK(count >= 1u,
           "the shadow-including inclusive descendants of a root ELEMENT hold no element at all — the walk "
           "starts AT that element and DOM §4.8's tree order is INCLUSIVE, so a zero is this count and that "
           "walker disagreeing about whether a node is one of its own inclusive descendants");
    return count;
}

/* What step 22 left: the region and ratio it painted for, and CSS 2.1 §E.2's list with its census. Immutable once
   document_paint_frame returns it. */
struct DocumentPaintFrame {
    bool           region;     /* CSS 2.1 §2.3.1 established a rendered region; false leaves everything below zero */
    CssPx          rect[4];    /* that region, in CSS pixels */
    double         dpr;        /* the device ratio the region is rasterized at, read once at the paint */
    int            w, h;       /* the region's device size (display_list_raster_region_size) */
    bool           walked;     /* the root generated a box and §E.2's walk ran over it */
    DisplayList    dl;         /* initialised on every arm, so the raster and the release need no arm of their own */
    BoxPaintCensus census;
    bool           complete;
    unsigned       elements;
    /* While the walk is in progress: §E.2's painter and what the element census is taken over. */
    BoxPaintWalk      *walk;
    JSContext         *ctx;
    lxb_dom_element_t *root;
};

DocumentPaintFrame *document_paint_begin(JSContext *ctx, lxb_dom_document_t *doc)
{
    DocumentPaintFrame *f;
    lxb_dom_element_t  *root;
    CssDisplay          display;
    char                dbuf[64];
    bool                boxless;

    DCHECK(ctx != NULL, "a document was painted with no realm to ask it in — CSS 2.1 §2.3.1 \"The canvas\"'s "
                        "region is a fact about the viewport this document is presented on, which "
                        "core/frame/viewport.h reads out of a realm");
    DCHECK(doc != NULL, "a paint was asked of no document");
    DCHECK(layout_job_holds_thread(),
           "a document was painted with no layout job holding the thread — step 22's caller enters the job "
           "the walk's layout questions and computed values are asked on");
    f = lexbor_calloc(1, sizeof *f);
    CHECK(f != NULL, "update the rendering step 22 could not allocate its frame");
    display_list_init(&f->dl);

    /* CSS 2.1 §2.3.1 "The canvas": "rendering generally occurs within a finite region of the canvas, established
       by the user agent according to the target medium". A document no navigable presents has none, and the
       frame then holds no picture. Asked before the walk, whose step 1 would ask the same component. */
    f->region = viewport_canvas_region(ctx, f->rect);
    if (!f->region) return f;
    /* The device ratio is an operand of the raster, read once here so every mark is rasterized at the ratio of
       the world that painted it (two arms of one fork can have two viewports). */
    f->dpr = viewport_device_pixel_ratio(ctx);
    display_list_raster_region_size(f->rect, f->dpr, &f->w, &f->h);

    /* DOM §4.2.1 "Document tree"'s document element, read from the tree as it is now: a page may remove it. */
    root = lxb_dom_interface_element(document_document_element_of(lxb_dom_interface_node(doc)));
    /* css-display-3 §2.5 "Box Generation: the none and contents keywords" for the root, asked here because
       core/paint/stacking_order.h's stacking-context test is stated over a box. css-display-3 §2.8 "The Root
       Element's Principal Box" makes `contents` compute to `block` on the root (core/css/css_computed_value.c
       performs it), so `none` is the one keyword that reaches this arm. It is not element_view_has_box, which
       also answers whether the document is presented, a fact that routes to the region arm above instead. */
    boxless = root == NULL;
    if (!boxless) {
        display = css_computed_display(root);
        DCHECKF(!css_display_spells(display, CSS_KEYWORD_CONTENTS),
                "the root element's computed `display` is `%s`; css-display-3 §2.8 \"The Root Element's "
                "Principal Box\" computes `contents` to `block` on the root, which core/css/css_computed_value.c "
                "performs, so this is that file disagreeing with the section it implements",
                css_display_name(display, dbuf, sizeof dbuf));
        boxless = css_display_spells(display, CSS_KEYWORD_NONE);
    }
    if (boxless) {
        /* Nothing for CSS 2.1 §E.2 to walk: the picture is the region with no mark, and whole. */
        f->complete = true;
        return f;
    }
    /* CSS 2.1 §E.2 "Painting order" over the root's stacking context (CSS 2.1 §9.9.1: the root element forms the
       root stacking context), one offer per `document_paint_step`. */
    f->walked = true;
    f->ctx = ctx;
    f->root = root;
    f->walk = box_paint_begin(ctx, root, &f->dl, &f->census);
    return f;
}

bool document_paint_step(DocumentPaintFrame *f)
{
    DCHECK(f != NULL, "a paint was stepped through nothing");
    /* NAMED RESIDUAL. Not covered: the offer's reads beyond its element's client rectangles (a text run's
       fragments, a replaced element's intrinsic size) still run whole on the job's nested drives, and the
       element census at the end is one walk. Next: each such producer asked in document_paint_ready under the
       same resting drive. How its absence shows: a flow's slice overrun inside step 22 with `_layoutJobRests`
       flat across one offer whose element's rectangles were already answered. */
    return f->walk != NULL && box_paint_step(f->walk);
}

bool document_paint_ready(DocumentPaintFrame *f)
{
    lxb_dom_element_t *el;
    LayoutJobDrive d;
    bool ready;

    DCHECK(f != NULL, "a paint's next offer was asked about through nothing");
    if (f->walk == NULL || !box_paint_next_element(f->walk, &el)) return true;
    rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
    layout_job_drive_begin_resting(&d);
    ready = element_view_client_rects_ask(el, &d);
    layout_job_drive_end(&d);
    rest_unit_end();
    return ready;
}

void document_paint_finish(DocumentPaintFrame *f)
{
    DCHECK(f != NULL, "a paint was finished through nothing");
    if (f->walk == NULL) return;
    /* A false answer is a painter that met an operand it could not compute and stopped; every mark it laid
       stays in the list, so the frame is a partial picture rather than none. */
    f->complete = box_paint_end(f->walk);
    f->walk = NULL;
    f->elements = dp_element_count(f->ctx, f->root);
    f->ctx = NULL;
    f->root = NULL;
}

bool document_paint_finished(const DocumentPaintFrame *f)
{
    DCHECK(f != NULL, "a paint was asked whether it finished through nothing");
    return f->walk == NULL;
}

void document_paint_frame_free(DocumentPaintFrame *f)
{
    DCHECK(f != NULL, "a paint frame was released through nothing");
    /* An abandoned paint (its step machine torn down or forked away mid-walk) releases its walk unread. */
    if (f->walk != NULL) (void)box_paint_end(f->walk);
    display_list_free(&f->dl);
    free(f);
}

bool document_paint_raster(const DocumentPaintFrame *f, RasterSurface *out, DocumentPaintCount *count)
{
    DisplayListRasterCount rc;

    DCHECK(f != NULL && out != NULL && count != NULL,
           "a frame was rasterized from nothing, onto nothing, or with nowhere to write what the raster did");
    DCHECK(f->walk == NULL, "a frame was rasterized while its paint is still walking");
    memset(count, 0, sizeof *count);
    raster_surface_init(out, 0, 0);
    if (!f->region) return false;
    raster_surface_init(out, f->w, f->h);
    count->census = f->census;
    count->complete = f->complete;
    count->elements = f->elements;
    if (f->walked) {
        display_list_raster(&f->dl, f->dpr, out, &rc);
        count->marks = rc.marks;
        count->spans = rc.spans;
        count->pixels = rc.pixels;
    }
    DCHECKF(raster_surface_bytes(out) == (size_t)(f->w > 0 ? f->w : 0) * (size_t)(f->h > 0 ? f->h : 0) * 4u,
            "a %dx%d device surface rasterized for a %g x %g CSS-pixel region holds %zu bytes; the frame sized "
            "both by one call", f->w, f->h, f->rect[2].px, f->rect[3].px, raster_surface_bytes(out));
    return true;
}
