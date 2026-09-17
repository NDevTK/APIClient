/* THE INK BECOMING PIXELS. See display_list_raster.h for why the extent is an operand rather than a field on
   a list, why a canvas mark's own rectangle is read by nothing here, why a rectangle goes through the path
   road rather than through a scanline loop of this file's own, and what the border mark is waiting on. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_length.h"
#include "core/graphics/raster_path.h"
#include "core/graphics/raster_surface.h"
#include "core/graphics/rasterizer.h"
#include "core/paint/display_list.h"
#include "core/paint/display_list_raster.h"

void display_list_raster_region_size(const CssPx region[4], double device_px_per_css_px,
                                     int *width, int *height)
{
    double w, h;

    DCHECK(region != NULL && width != NULL && height != NULL,
           "CSS 2.1 §2.3.1 \"The canvas\"'s region was converted to device pixels through a null");
    /* BOTH OPERANDS ARE THIS ENGINE'S OWN ARITHMETIC AND NEITHER IS A PAGE'S NUMBER, which is the line
       CLAUDE.md draws for what a DCHECK may stand on. The region is core/frame/viewport.h's initial
       containing block and the ratio is its `devicePixelRatio`; a page can declare neither. */
    DCHECKF(isfinite(region[2].px) && region[2].px >= 0.0 && isfinite(region[3].px) && region[3].px >= 0.0,
            "CSS 2.1 §2.3.1's rendered region is %g x %g CSS pixels — an extent is a distance between "
            "parallel edges and cannot be negative, and a non-finite one is a derivation that lost an "
            "operand rather than a size any medium established", region[2].px, region[3].px);
    DCHECKF(isfinite(device_px_per_css_px) && device_px_per_css_px > 0.0,
            "a surface was sized at %g device pixels per CSS pixel — CSSOM VIEW §4 \"Extensions to the Window "
            "Interface\"' `devicePixelRatio` is a ratio of two lengths, so a zero or a negative is "
            "core/frame/viewport.h's answer having gone wrong and never a number a document stated",
            device_px_per_css_px);
    w = ceil(region[2].px * device_px_per_css_px);
    h = ceil(region[3].px * device_px_per_css_px);
    /* A `CHECK` AND NOT A `DCHECK`, because the two casts below run in RELEASE as well as in dev and a
       double outside the range of an int converts with UNDEFINED BEHAVIOUR. A guard compiled out leaves that
       conversion with nothing in front of it, which is CLAUDE.md's data integrity. */
    CHECKF(w >= 0.0 && w <= 2147483647.0 && h >= 0.0 && h <= 2147483647.0,
           "a surface of %g x %g device pixels cannot be named by the dimensions a raster surface carries",
           w, h);
    *width = (int)w;
    *height = (int)h;
}

/* ONE MARK'S AREA IN DEVICE PIXELS, or false when the kind lays no area. The rectangle a `DISPLAY_MARK_FILL_
   CANVAS` carries is NOT READ, which is display_list_raster.h's own paragraph and core/paint/display_list.h's
   rule: CSS 2.1 §2.3.1 "The canvas" makes the area infinite, so its intersection with a finite surface is the
   whole of that surface whatever region this user agent established. */
static bool dlr_fill_area(const DisplayMark *m, double s, const RasterSurface *surface, double out[4])
{
    switch (m->kind) {
    case DISPLAY_MARK_FILL_RECT:
        out[0] = m->rect[0].px * s;
        out[1] = m->rect[1].px * s;
        out[2] = m->rect[2].px * s;
        out[3] = m->rect[3].px * s;
        return true;
    case DISPLAY_MARK_FILL_CANVAS:
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = (double)surface->width;
        out[3] = (double)surface->height;
        return true;
    case DISPLAY_MARK_BORDER:
        DFAIL("a `DISPLAY_MARK_BORDER` reached the rasterizer, which has no arm for it — see "
              "core/paint/display_list_raster.h's residual. FOUR SIDE RECTANGLES ARE NOT THE THING TO BUILD: "
              "two adjacent sides OVERLAP where they meet, so a box with two border colours would come out "
              "with one painted over the other in a square whose winner is whichever loop ran last. What the "
              "next diff lays is four MITRED QUADRILATERALS through `raster_path_move_to` and "
              "`raster_path_line_to` — each side a trapezoid from its own outer edge to the inner edge, "
              "meeting its neighbours on the diagonal through the padding-edge corner — which partitions the "
              "border area with no overlap and no gap, and which css-backgrounds-3 §4.4 \"Color and Style "
              "Transitions\" admits, that section constraining the region without settling the shape: "
              "\"However it is not defined what these transitions look like or what function maps from this "
              "ratio to a point on the curve\". AND THE STYLE IS THE OTHER HALF: CSS 2.1 §8.5.3's "
              "`<border-style>` has ten values and eight of them draw something other than a filled band, so "
              "an arm that filled every non-`none` side solid would paint a `dotted` rule as a `solid` one, "
              "which is WRONG ink rather than narrow ink");
        /* THE RELEASE ARM, AND IT DRAWS NOTHING. A kind this file has no area for is one whose ink it
           cannot place, so the two answers available are no ink and a guess, and only one of them can put
           the WRONG thing on a page. It LEAVES THE SURFACE COHERENT, which is the question CLAUDE.md makes
           a release arm answer: a bitmap short one mark is a picture with a piece missing, and every later
           mark composites onto it exactly as it would have. */
        return false;
    }
    /* A KIND OUTSIDE THE VOCABULARY, WHICH THE DOOR ALREADY REFUSED. core/paint/display_list.c asserts
       `dl_kind_is_defined` on every append and display_list.h states that an append is "the ONLY way ink
       enters a list", so this is unreachable and a crash here would be a second answer to a question that
       component already owns. THERE IS NO `default:` ABOVE ON PURPOSE: `-Wswitch` is what names this site
       the day a fourth kind lands, which display_list.h names as the mechanism, and a `default:` would take
       that away. */
    return false;
}

void display_list_raster(const DisplayList *dl, double device_px_per_css_px, RasterSurface *surface,
                         DisplayListRasterCount *count)
{
    size_t i;

    DCHECK(dl != NULL, "a rasterization of no display list");
    DCHECK(surface != NULL, "a display list was rasterized onto no surface");
    DCHECK(count != NULL,
           "a display list was rasterized with nowhere to write what it did. The count is required rather "
           "than optional for core/paint/box_paint.h's reason about its own `offers`: a surface with no ink "
           "on it means the list was empty, or every mark was transparent, or every mark fell outside the "
           "surface — three states that take opposite work and that a caller handed only the bitmap cannot "
           "tell apart");
    DCHECKF(isfinite(device_px_per_css_px) && device_px_per_css_px > 0.0,
            "a display list was rasterized at %g device pixels per CSS pixel — see "
            "`display_list_raster_region_size` for why that operand is this engine's own number",
            device_px_per_css_px);
    count->marks = 0;
    count->spans = 0;
    count->pixels = 0;

    for (i = 0; i < dl->n; i++) {
        const DisplayMark *m = &dl->v[i];
        RasterPath p;
        RasterEdges e;
        RasterPaint paint;
        double a[4];
        size_t nspans;

        if (!dlr_fill_area(m, device_px_per_css_px, surface, a)) continue;
        /* THE COLOUR IS READ WITHOUT ASKING THE CASCADE ANYTHING. core/paint/display_list.h's append asserts
           that every mark's colour has already been through CSS Color 4 §11 "Converting Colors" into sRGB,
           expressly so that no consumer of a list asks a question the painter already answered; the CLAMP
           and the 8-bit quantization are core/graphics/raster_surface.h's, which that same header says. */
        DCHECKF(m->color.space == CSS_COLOR_SPACE_SRGB,
                "a fill mark's colour reached the rasterizer in colour space %d rather than in sRGB, which "
                "core/paint/display_list.c asserts at the one door ink enters a list by",
                (int)m->color.space);
        /* A PATH AND AN EDGE LIST PER MARK, FRESH. `raster_path_flatten` APPENDS to its output, so a shared
           edge list would fill each mark with every mark before it — and a shared path would need a reset
           entry core/graphics/raster_path.h does not have and that is not this component's to add. */
        raster_path_init(&p);
        raster_path_rect(&p, a[0], a[1], a[2], a[3]);
        raster_edges_init(&e);
        raster_path_flatten(&p, RASTER_FLATTEN_TOLERANCE_PX, &e);
        raster_paint_init(&paint, surface, m->color.c[0], m->color.c[1], m->color.c[2], m->color.a);
        /* THE REGION HANDED TO THE FILL IS THE SURFACE'S OWN, which is what makes core/graphics/
           raster_surface.c's bounds `CHECK` on every span an identity rather than a hope: the fill discards
           what falls outside the region it was given, so a region larger than the surface would be a run
           composited past the end of the allocation. */
        nspans = raster_fill(&e, RASTER_FILL_NONZERO, surface->width, surface->height,
                             raster_paint_span, &paint);
        /* TWO COUNTERS IN TWO COMPONENTS, HELD TO EACH OTHER. core/graphics/raster_surface.h states why the
           sink keeps its own: "a fill that emitted runs nobody received and a fill that emitted none are the
           same number at the caller, and two counts that must agree are not". Both are this codebase's own
           arithmetic, so a disagreement is a run that was reported to nobody. */
        DCHECKF(nspans == paint.spans,
                "a fill emitted %zu runs and the surface received %zu", nspans, paint.spans);
        raster_edges_free(&e);
        raster_path_free(&p);

        count->marks++;
        count->spans += paint.spans;
        count->pixels += paint.pixels;
    }
}
