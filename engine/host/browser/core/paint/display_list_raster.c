/* THE INK BECOMING PIXELS. See display_list_raster.h for why the extent is an operand rather than a field on
   a list, why a canvas mark's own rectangle is read by nothing here, why a rectangle goes through the path
   road rather than through a scanline loop of this file's own, and what the border mark is waiting on. */
#include <math.h>
#include <stddef.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_length.h"
#include "core/css/font_metrics.h"   /* the ONE face every advance in every layout was measured against */
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

/* ONE FILL — a path this file has already built, a colour, and the two counters. It is an entry rather than
   inline code because a border mark lays UP TO FOUR of them where a fill mark lays one, and the flatten, the
   sink and the two-counter identity are the same three statements at every one. Two spellings of them would
   be two answers to one question, which is the argument core/graphics/rasterizer.h makes about its own fill
   rules and the same reason this component has no scanline loop.
   A PATH AND AN EDGE LIST PER FILL, FRESH. `raster_path_flatten` APPENDS to its output, so a shared edge list
   would fill each shape with every shape before it. */
static void dlr_fill(const RasterPath *p, const CssColor *color, RasterSurface *surface,
                     DisplayListRasterCount *count)
{
    RasterEdges e;
    RasterPaint paint;
    size_t nspans;

    /* THE COLOUR IS READ WITHOUT ASKING THE CASCADE ANYTHING. core/paint/display_list.h's append asserts that
       every mark's colour has already been through CSS Color 4 §11 "Converting Colors" into sRGB — and it
       asks that over a SWITCH, so a border mark's FOUR side colours are each held to it and its unused
       `color` field is not; the CLAMP and the 8-bit quantization are core/graphics/raster_surface.h's, which
       that same header says. */
    DCHECKF(color->space == CSS_COLOR_SPACE_SRGB,
            "a mark's colour reached the rasterizer in colour space %d rather than in sRGB, which "
            "core/paint/display_list.c asserts at the one door ink enters a list by", (int)color->space);
    raster_edges_init(&e);
    raster_path_flatten(p, RASTER_FLATTEN_TOLERANCE_PX, &e);
    raster_paint_init(&paint, surface, color->c[0], color->c[1], color->c[2], color->a);
    /* THE REGION HANDED TO THE FILL IS THE SURFACE'S OWN, which is what makes core/graphics/raster_surface.c's
       bounds `CHECK` on every span an identity rather than a hope: the fill discards what falls outside the
       region it was given, so a region larger than the surface would be a run composited past the end of the
       allocation. */
    nspans = raster_fill(&e, RASTER_FILL_NONZERO, surface->width, surface->height,
                         raster_paint_span, &paint);
    /* TWO COUNTERS IN TWO COMPONENTS, HELD TO EACH OTHER. core/graphics/raster_surface.h states why the sink
       keeps its own: "a fill that emitted runs nobody received and a fill that emitted none are the same
       number at the caller, and two counts that must agree are not". Both are this codebase's own arithmetic,
       so a disagreement is a run that was reported to nobody. */
    DCHECKF(nspans == paint.spans,
            "a fill emitted %zu runs and the surface received %zu", nspans, paint.spans);
    raster_edges_free(&e);
    count->spans += paint.spans;
    count->pixels += paint.pixels;
}

/* ONE AXIS-ALIGNED RECTANGLE, through the path road for display_list_raster.h's own reason: a rectangle whose
   edges do not land on pixel boundaries has FRACTIONAL COVERAGE at every edge pixel, and a coverage written
   here would be a second answer to the question core/graphics/rasterizer.c already answers analytically. */
static void dlr_fill_rect(double x, double y, double w, double h, const CssColor *color,
                          RasterSurface *surface, DisplayListRasterCount *count)
{
    RasterPath p;

    raster_path_init(&p);
    raster_path_rect(&p, x, y, w, h);
    dlr_fill(&p, color, surface, count);
    raster_path_free(&p);
}

/* ONE CONVEX QUADRILATERAL, its four vertices in order — MOVE, three LINEs and a CLOSE, which is exactly
   `raster_path_rect`'s own spelling one shape over and reaches neither `cos`, `sin` nor `hypot`.
   THAT IS NOT AN INCIDENTAL PROPERTY AND IS THE REASON A SIDE IS A QUADRILATERAL RATHER THAN AN OUTLINE.
   core/graphics/raster_path.c's residual narrows the byte-for-byte agreement of two HOSTS to paths with no
   `CANVAS_PATH_OP_ARC` in them, because a vertex off `cos` and `sin` is a function of the platform's math
   library; those calls sit in `rp_flatten_arc`, `rp_quad_segments` and `rp_bezier_segments` and a MOVE, a
   LINE and a CLOSE reach `rp_emit` alone. So a border drawn this way keeps core/graphics/rasterizer.h's
   cross-host checksum oracle intact, and the first style that needs a curve is the first that does not. */
static void dlr_quad(RasterPath *p, const double q[4][2])
{
    raster_path_move_to(p, q[0][0], q[0][1]);
    raster_path_line_to(p, q[1][0], q[1][1]);
    raster_path_line_to(p, q[2][0], q[2][1]);
    raster_path_line_to(p, q[3][0], q[3][1]);
    raster_path_close_path(p);
}

/* THE SHOELACE AREA OF ONE QUADRILATERAL, unsigned. This exists for the assert in `dlr_border` and for
   nothing else: it reads the VERTICES that are about to be handed to the path, so the number it answers is a
   function of the coordinates the fill will actually see rather than of the extents they were built from. */
static double dlr_quad_area(const double q[4][2])
{
    double twice = 0.0;
    int k;

    for (k = 0; k < 4; k++) {
        const double *a = q[k], *b = q[(k + 1) & 3];
        twice += a[0] * b[1] - b[0] * a[1];
    }
    return fabs(twice) * 0.5;
}

/* WHAT THE FOUR WEDGES COVER, LESS CSS 2.1 §8.1 "Box dimensions"' BORDER AREA — positive when something is
   covered twice and negative when something is covered by nothing.
   IT IS CALLED FROM THE DCHECK IN `dlr_border` AND FROM NOWHERE ELSE, which is core/paint/display_list.c's
   own idiom for a predicate an assert owns and is why the four shoelaces cost a release build nothing: a
   `DCHECKF`'s condition is `((void)sizeof(cond))` there and its message arguments go through
   `APICLIENT_FMT_UNUSED`, which is a `sizeof` too, so neither is evaluated. `clang -Wall -DAPICLIENT_DEV=0`
   therefore reports this as not needed and not emitted; a reader meeting that has found the assert compiled
   out and not a dead predicate, and the answer is neither to drop it nor to call it outside the DCHECK. */
static double dlr_wedge_surplus(const double q[4][4][2], double w, double h,
                                double t, double r, double b, double l)
{
    double wedges = 0.0;
    int i;

    for (i = 0; i < 4; i++) wedges += dlr_quad_area(q[i]);
    return wedges - (w * h - (w - l - r) * (h - t - b));
}

/* THE FOUR MITRED WEDGES OF ONE BORDER MARK, in device pixels, indexed top, right, bottom, left — which is
 * core/paint/display_list.h's own order for `side` and `used_value_border_widths_px`' order for the widths it
 * came from, so the index cannot come apart from the derivation by a rotation.
 *
 * EACH SIDE IS A TRAPEZOID AND NOT A RECTANGLE, WHICH IS THE WHOLE OF WHAT THIS GEOMETRY DECIDES. Four side
 * RECTANGLES overlap in a square at every corner, so a box with two border colours would come out with one
 * painted over the other in a square whose winner is whichever loop ran last — WRONG ink rather than narrow
 * ink. A wedge runs from a side's own outer edge to the padding edge and meets its two neighbours on the
 * DIAGONAL through the padding-edge corner, which partitions the border area with no overlap and no gap.
 *
 * THE MITRE IS A CONFORMING CHOICE AND THE STANDARD SAYS SO IN AS MANY WORDS. css-backgrounds-3 §3.2 "Line
 * Patterns: the border-style properties" ends with "Note: This specification does not define how borders of
 * different styles should be joined in the corner", which is the note that governs a SQUARE corner and is
 * therefore this component's; css-backgrounds-3 §4.4 "Color and Style Transitions" is one level down inside
 * §4 "Rounded Corners" and constrains the region for a corner that has radii — "Color and style transitions
 * must be contained within the segment of the border that intersects the smallest rectangle that contains
 * both border radii as well as the center of the inner curve (which may be a point representing the corner
 * of the padding edge, if the border radii are smaller than the border width)" — and then leaves the shape:
 * "However it is not defined what these transitions look like or what function maps from this ratio to a
 * point on the curve". At a zero radius that smallest rectangle IS the corner square between the border-box
 * corner and the padding-edge corner, and the diagonal lies inside it, so the mitre satisfies §4.4's MUST as
 * well. §3.2's note is cited first because it is the one that reaches a corner with no radius at all.
 *
 * THE PICK IS LOAD-BEARING FOR THE COMPONENT AND NOT FOR ONE STYLE, which is what separates it from the
 * per-style splits below: without SOME partition of the corner there is no way to draw any border at all,
 * so this choice is forced by the problem, where a `double` line's thickness is a second choice a side
 * that is ALREADY DRAWABLE needs. That is why the two are stated differently and not merely in different
 * places — this one is argued from the problem, and the third `DLR_DOUBLE_LINE_NUM` picks is named as
 * this USER AGENT'S beside the two sections that decline to state it. */
typedef struct { double x0, y0, x1, y1, xi0, yi0, xi1, yi1; } DlrBorderEdges;

static void dlr_border_wedges(const DlrBorderEdges *b, double q[4][4][2])
{
    /* top */
    q[0][0][0] = b->x0;  q[0][0][1] = b->y0;
    q[0][1][0] = b->x1;  q[0][1][1] = b->y0;
    q[0][2][0] = b->xi1; q[0][2][1] = b->yi0;
    q[0][3][0] = b->xi0; q[0][3][1] = b->yi0;
    /* right */
    q[1][0][0] = b->x1;  q[1][0][1] = b->y0;
    q[1][1][0] = b->x1;  q[1][1][1] = b->y1;
    q[1][2][0] = b->xi1; q[1][2][1] = b->yi1;
    q[1][3][0] = b->xi1; q[1][3][1] = b->yi0;
    /* bottom */
    q[2][0][0] = b->x1;  q[2][0][1] = b->y1;
    q[2][1][0] = b->x0;  q[2][1][1] = b->y1;
    q[2][2][0] = b->xi0; q[2][2][1] = b->yi1;
    q[2][3][0] = b->xi1; q[2][3][1] = b->yi1;
    /* left */
    q[3][0][0] = b->x0;  q[3][0][1] = b->y1;
    q[3][1][0] = b->x0;  q[3][1][1] = b->y0;
    q[3][2][0] = b->xi0; q[3][2][1] = b->yi0;
    q[3][3][0] = b->xi0; q[3][3][1] = b->yi1;
}

/* THIS USER AGENT'S `double` SPLIT, AS A RATIO OF INTEGERS SO ITS ONE CONSTRAINT IS A COMPILE-TIME ONE.
 * NO SECTION STATES THE THICKNESS AND BOTH STATE THE SUM. CSS 2.1 §8.5.3 "Border style: 'border-top-style',
 * 'border-right-style', 'border-bottom-style', 'border-left-style', and 'border-style'" gives `double` as
 * "The border is two solid lines. The sum of the two lines and the space between them equals the value of
 * 'border-width'", and css-backgrounds-3 §3.2 "Line Patterns: the border-style properties" renders it as
 * "two parallel solid lines with some space between them" and then says the rest outright in its own
 * parenthesis — "The thickness of the lines is not specified, but the sum of the lines and the space must
 * equal border-width." So the THICKNESS is this user agent's pick and the SUM is the standards', which is
 * why the pick is a FRACTION OF THE USED WIDTH rather than a length: at any width the two lines and the
 * space are NUM/DEN, (DEN-2*NUM)/DEN and NUM/DEN of it and add to exactly DEN/DEN, so the sentence both
 * sections state holds by construction at every width rather than at the one this file was written against.
 * A THIRD IS THE PICK. What the two sections constrain is only that the space not be negative, and that is
 * the `_Static_assert` below — an INTEGER one, which is why the fraction is spelled as a ratio rather than
 * as a `double`: a floating comparison is not an integer constant expression, so it would have to become a
 * runtime check of a literal against a literal, which is the shape CLAUDE.md names as an assert whose two
 * sides cannot disagree. */
#define DLR_DOUBLE_LINE_NUM 1
#define DLR_DOUBLE_LINE_DEN 3
_Static_assert(DLR_DOUBLE_LINE_NUM > 0 && 2 * DLR_DOUBLE_LINE_NUM <= DLR_DOUBLE_LINE_DEN,
               "this user agent's `double` split lays a line of NUM/DEN of the used width at each edge of a "
               "side, so a NUM of zero draws no line at all and a 2*NUM past DEN overlaps the two lines in "
               "the middle. css-backgrounds-3 §3.2's \"the sum of the lines and the space must equal "
               "border-width\" is the sum this ratio satisfies by construction; a NEGATIVE space is the one "
               "way to spell a ratio that does not, and it is the only thing either section forbids");

/* ONE SUB-BAND OF ONE MITRED WEDGE, at depths `a` and `b` of the way from the side's OUTER edge to the
 * padding edge — [0, 1] being the whole wedge and giving its own four vertices back unchanged.
 * IT INTERPOLATES THE TWO MITRE DIAGONALS AND NOT THE TWO PARALLEL EDGES, which is the only construction
 * that stays inside the wedge. `dlr_border_wedges` indexes a wedge outer-first, outer-second, inner-second,
 * inner-first, so q[0]->q[3] and q[1]->q[2] ARE that side's two diagonals and the point at depth t on each
 * is the band's own corner there; the band therefore narrows exactly as the wedge does. A band built
 * instead by insetting the outer edge along its own normal keeps the outer edge's LENGTH, so it leaves the
 * wedge at both ends and paints into the two neighbouring sides — which is the overlap the wedges exist to
 * remove, re-introduced one level down.
 * IT REACHES NO `cos`, `sin` OR `hypot`: every coordinate is one `-`, one `*` and one `+` over vertices
 * `dlr_border_wedges` has already produced, so display_list_raster.h's argument that this road keeps
 * core/graphics/rasterizer.h's cross-host checksum oracle intact survives a `double` side unchanged. */
static void dlr_border_subwedge(const double q[4][2], double a, double b, double out[4][2])
{
    int k;

    for (k = 0; k < 2; k++) {
        out[0][k] = q[0][k] + (q[3][k] - q[0][k]) * a;
        out[1][k] = q[1][k] + (q[2][k] - q[1][k]) * a;
        out[2][k] = q[1][k] + (q[2][k] - q[1][k]) * b;
        out[3][k] = q[0][k] + (q[3][k] - q[0][k]) * b;
    }
}

/* WHAT THREE BANDS OF ONE SIDE COVER, LESS THE WEDGE THEY WERE CUT FROM — positive when something is
   covered twice and negative when something is covered by nothing. It is `dlr_wedge_surplus` one level
   down and is called from the DCHECK in `dlr_border_side` and from nowhere else, for that predicate's own
   stated reason: a `DCHECKF`'s condition and message arguments are both a `sizeof` in release, so neither
   this nor the four shoelaces under it costs a release build anything. */
static double dlr_band_surplus(const double band[3][4][2], const double whole[4][2])
{
    return dlr_quad_area(band[0]) + dlr_quad_area(band[1]) + dlr_quad_area(band[2]) - dlr_quad_area(whole);
}

/* ONE SIDE'S INK, WHICH IS ITS STYLE'S QUESTION AND NOT ITS WIDTH'S. CSS 2.1 §8.5.3 "Border style:
 * 'border-top-style', 'border-right-style', 'border-bottom-style', 'border-left-style', and 'border-style'"
 * and css-backgrounds-3 §3.2 "Line Patterns: the border-style properties" define the same ten values, and
 * exactly ONE of them is a single filled band: CSS 2.1 §8.5.3's `solid`, "The border is a single line segment", which
 * §3.2 renders as "A single line segment". Every other value that paints draws something else, so an arm that
 * filled every non-`none` side solid would paint a `dotted` rule as a `solid` one.
 *
 * A ZERO-WIDTH SIDE RETURNS BEFORE THE STYLE IS READ, and that is the algorithm's own precondition rather
 * than a selector: a band of zero extent covers no pixel whatever is drawn in it, which is the same rule
 * core/paint/box_paint.c applies to a box whose four used widths are all zero. It is also what keeps the
 * refusals below from crashing on ink nobody asked for — a `border-style: dotted; border-width: 0` computes
 * to a used width of zero, which core/layout/used_value.c states at its own site ("0 for a `none`/`hidden`
 * style, 1/3/5px for the three keywords, the absolutized length otherwise").
 *
 * A TRANSPARENT SIDE IS STILL FILLED, deliberately and for core/paint/box_paint.c's own stated reason: an
 * alpha of zero does not settle whether a side paints, so this component composites it and lets the count
 * report the work. There is no alpha test here, which is what keeps `pixels` a function of the GEOMETRY and
 * not of the colour. */
static void dlr_border_side(const DisplayBorderSide *side, const double q[4][2], RasterSurface *surface,
                            DisplayListRasterCount *count)
{
    RasterPath p;
    /* THE THREE BANDS A `double` SIDE IS CUT INTO, outer line, space, inner line — declared here rather than
       in the arm so that this entry stays one translation unit's worth of C89-shaped declarations like every
       other function in this file, and read by that arm alone. */
    double band[3][4][2];
    double f;

    if (side->width.px == 0.0) return;
    switch (side->style) {
    /* CSS 2.1 §8.5.3's `none` — "No border; the computed border width is zero" — and its `hidden`, "Same as
       'none', except in terms of border conflict resolution for table elements". Both paint nothing and the
       early return above has already taken them, since both compute to a used width of zero; they are named
       here so that the day one of them stops doing so this arm says what it draws. */
    case DISPLAY_BORDER_STYLE_NONE:
    case DISPLAY_BORDER_STYLE_HIDDEN:
        return;
    /* CSS 2.1 §8.5.3's `solid`, "The border is a single line segment" — the whole wedge, and the only one of
       the ten that is a SINGLE filled band. */
    case DISPLAY_BORDER_STYLE_SOLID:
        raster_path_init(&p);
        dlr_quad(&p, q);
        dlr_fill(&p, &side->color, surface, count);
        raster_path_free(&p);
        return;
    /* CSS 2.1 §8.5.3's `double`, "The border is two solid lines" — the wedge's outer and inner thirds, with
       `DLR_DOUBLE_LINE_NUM`/`DLR_DOUBLE_LINE_DEN` above carrying this user agent's pick of the third and the
       two citations that leave it to one.
       TWO BANDS IN ONE PATH AND ONE FILL, WHICH IS WHAT KEEPS `dlr_fill`'s OWN ACCOUNT TRUE: that entry says
       a border mark lays "UP TO FOUR of them", one per side, and a second fill here would make it eight and
       would composite any pixel the two bands shared TWICE — at an alpha below one that is a visibly darker
       run, and it is the same double-composite the mitre exists to remove one level up. The two bands are
       disjoint and wound the same way, so `RASTER_FILL_NONZERO` fills both from one path and a row crossing
       both is two runs of ONE fill rather than one run of each of two. */
    case DISPLAY_BORDER_STYLE_DOUBLE:
        f = (double)DLR_DOUBLE_LINE_NUM / (double)DLR_DOUBLE_LINE_DEN;
        dlr_border_subwedge(q, 0.0, f, band[0]);
        dlr_border_subwedge(q, f, 1.0 - f, band[1]);
        dlr_border_subwedge(q, 1.0 - f, 1.0, band[2]);
        /* THE SENTENCE BOTH SECTIONS STATE, ASSERTED AS AREAS RATHER THAN RESTATED AS A COMMENT. "The sum of
           the two lines and the space between them equals the value of 'border-width'" is a claim that the
           three bands COVER this side with no overlap and no gap, which over one wedge is the same shape as
           the partition `dlr_border` holds the four wedges to — and like that one it is read off the
           VERTICES about to be handed to the path, so it is a statement about what the fill will see rather
           than about the fractions the bands were built from. THE DEFECT IT CATCHES IS A BAND CUT FROM THE
           WRONG PAIR OF RAYS: such a band still has an area and still looks like a band, and the three then
           overlap or leave a gap. The tolerance is `dlr_border`'s own, written against this WEDGE's area
           because that is the magnitude these twelve shoelace terms accumulate over. */
        DCHECKF(fabs(dlr_band_surplus((const double (*)[4][2])band, q)) <= 1e-9 * (dlr_quad_area(q) + 1.0),
                "a `double` side's two lines and the space between them miss the wedge they were cut from, "
                "which is %g device pixels, by %g. CSS 2.1 §8.5.3 \"Border style: 'border-top-style', "
                "'border-right-style', 'border-bottom-style', 'border-left-style', and 'border-style'\" "
                "requires that \"The sum of the two lines and the space between them equals the value of "
                "'border-width'\", so the three bands PARTITION the side — a SURPLUS is two of them "
                "overlapping and a SHORTFALL is a strip of the side nothing covers, and either is "
                "`dlr_border_subwedge` having interpolated something other than this wedge's two mitre "
                "diagonals",
                dlr_quad_area(q), dlr_band_surplus((const double (*)[4][2])band, q));
        /* AND THE SPACE IS WHAT MAKES IT TWO LINES, so a wedge with any area at all has one. Without this
           the arm is held only to a partition, and a split of [0, 1/2] and [1/2, 1] satisfies a partition
           EXACTLY while painting the side as one unbroken band — the single wrong answer that is invisible
           in every count this component reports, since two touching bands lay the same spans and the same
           pixels as the `solid` arm above. */
        DCHECKF(dlr_quad_area(q) <= 0.0 || dlr_quad_area((const double (*)[2])band[1]) > 0.0,
                "a `double` side cut from a wedge of %g device pixels left a space of %g between its two "
                "lines. CSS 2.1 §8.5.3 gives `double` as \"The border is two solid lines\" and "
                "css-backgrounds-3 §3.2 \"Line Patterns: the border-style properties\" as \"two parallel "
                "solid lines with some space between them\", so a space of zero draws them touching, which "
                "is ONE line and is this engine's `solid`",
                dlr_quad_area(q), dlr_quad_area((const double (*)[2])band[1]));
        raster_path_init(&p);
        dlr_quad(&p, (const double (*)[2])band[0]);
        dlr_quad(&p, (const double (*)[2])band[2]);
        dlr_fill(&p, &side->color, surface, count);
        raster_path_free(&p);
        return;
    case DISPLAY_BORDER_STYLE_DOTTED:
        DFAIL("a `dotted` border side reached the rasterizer, which draws only CSS 2.1 §8.5.3's `solid` and its `double` — see "
              "core/paint/display_list_raster.h's residual. IT IS THE ONE STYLE WHOSE GEOMETRY IS NOT THIS "
              "ROAD'S: css-backgrounds-3 §3.2 \"Line Patterns: the border-style properties\" gives it as \"A "
              "series of round dots\", and a ROUND dot is `raster_path_ellipse`, whose vertices come off "
              "`cos` and `sin` — so it is the first ink here whose bytes are a function of the platform's "
              "math library, which is exactly the population core/graphics/raster_path.c's residual narrows "
              "its two-host agreement away from. WHAT THE NEXT DIFF BUILDS: that residual's answer first, and "
              "only then the dots; and a RHYTHM, which §3.2 leaves open in its own note — \"There is no "
              "control over the spacing of the dots and dashes, nor over the length of the dashes. "
              "Implementations are encouraged to choose a spacing that makes the corners symmetrical\"");
        return;
    case DISPLAY_BORDER_STYLE_DASHED:
        DFAIL("a `dashed` border side reached the rasterizer, which draws only CSS 2.1 §8.5.3's `solid` and its `double` — see "
              "core/paint/display_list_raster.h's residual. THE SHAPE IS ALREADY THIS ROAD'S AND THE RHYTHM "
              "IS THE GAP: css-backgrounds-3 §3.2 \"Line Patterns: the border-style properties\" gives it as "
              "\"A series of square-ended dashes\", which is quadrilaterals and needs no curve, and then says "
              "in its own note that \"There is no control over the spacing of the dots and dashes, nor over "
              "the length of the dashes. Implementations are encouraged to choose a spacing that makes the "
              "corners symmetrical\". WHAT THE NEXT DIFF BUILDS: a dash length and gap picked from the side's "
              "used width and stated as this user agent's, distributed along the wedge's OUTER edge so that "
              "§3.2's symmetry note is satisfiable, with each dash clipped to its own wedge — a dash that "
              "crossed the mitre diagonal would paint into a neighbour's side, which is the overlap the "
              "wedges exist to remove");
        return;
    /* CSS 2.1 §8.5.3's FOUR 3-D STYLES, which differ from each other in appearance and from everything above
       in one way that matters here: their colour is DERIVED rather than declared. CSS 2.1 §8.5.3 says "The color of
       borders drawn for values of 'groove', 'ridge', 'inset', and 'outset' depends on the element's border
       color properties, but UAs may choose their own algorithm to calculate the actual colors used", and
       css-backgrounds-3 §3.2 names the usual one — "(This is typically achieved by creating a "shadow" from
       two colors that are slightly lighter and darker than the specified border-color.)". */
    case DISPLAY_BORDER_STYLE_GROOVE:
    case DISPLAY_BORDER_STYLE_RIDGE:
    case DISPLAY_BORDER_STYLE_INSET:
    case DISPLAY_BORDER_STYLE_OUTSET:
        DFAIL("a `groove`, `ridge`, `inset` or `outset` border side reached the rasterizer, which draws only "
              "CSS 2.1 §8.5.3's `solid` and its `double` — see core/paint/display_list_raster.h's "
              "residual. THE MISSING THING "
              "IS A COLOUR AND NOT A SHAPE: core/css/css_color.h has `css_color_parse`, `css_color_convert`, "
              "`css_color_quantize_8bit` and `css_color_serialize_html` and no entry that LIGHTENS or DARKENS "
              "a colour at all, so there is nothing here to compute the two tones css-backgrounds-3 §3.2 "
              "describes — \"(This is typically achieved by creating a \\\"shadow\\\" from two colors that "
              "are slightly lighter and darker than the specified border-color.)\" — and CSS 2.1 §8.5.3 "
              "leaves the algorithm to the UA. WHAT THE NEXT DIFF BUILDS: that derivation in "
              "core/css/css_color.h, over a colour space CSS Color 4 names rather than by scaling sRGB "
              "components, which is the same rule this road already obeys about conversion; the SHAPE is ALREADY "
              "HERE and is not a second geometry: `dlr_border_subwedge` above cuts a band of a wedge at "
              "any depth pair, so `groove` and `ridge` are two half-width bands of it and `inset` and "
              "`outset` are the whole wedge, and what those four are waiting on is the COLOUR alone");
        return;
    }
    /* A STYLE OUTSIDE THE VOCABULARY, WHICH THE DOOR ALREADY REFUSED. core/paint/display_list.c asserts
       `dl_border_style_is_defined` over every side of every border mark it appends, so this is unreachable
       and a crash here would be a second answer to a question that component already owns. THERE IS NO
       `default:` ABOVE ON PURPOSE: `-Wswitch` is what names this site the day an eleventh `<border-style>`
       value lands, and a `default:` would take that away. */
}

/* ONE BORDER MARK — CSS 2.1 §E.2 "Painting order"'s "border of element", all four sides, in the order
 * core/paint/display_list.h indexes them. */
static void dlr_border(const DisplayMark *m, double s, RasterSurface *surface,
                       DisplayListRasterCount *count)
{
    DlrBorderEdges b;
    double q[4][4][2];
    double w, h, t, r, bw, l;
    int i;

    /* EVERY OPERAND BELOW IS THIS ENGINE'S OWN ARITHMETIC, which is the line CLAUDE.md draws for what a
       DCHECK may stand on: the rectangle is core/dom/element_view.c's `element_view_bounding_box_px` and the
       four widths are core/layout/used_value.c's `used_value_border_widths_px`, neither of which is a number
       a document declared. core/paint/display_list.c already asserts the widths non-negative at the append;
       what it does not ask, and what only a consumer that has both in one hand can, is whether the two
       DERIVATIONS AGREE. */
    DCHECKF(isfinite(m->rect[2].px) && m->rect[2].px >= 0.0 &&
            isfinite(m->rect[3].px) && m->rect[3].px >= 0.0,
            "a border mark's box is %g x %g CSS pixels — CSS 2.1 §8.1 \"Box dimensions\"' border edge is the "
            "outer edge of an AREA, so a negative or non-finite extent is core/dom/element_view.c's rectangle "
            "having lost an operand rather than a box any layout produced",
            m->rect[2].px, m->rect[3].px);
    w = m->rect[2].px * s;
    h = m->rect[3].px * s;
    t = m->side[0].width.px * s;
    r = m->side[1].width.px * s;
    bw = m->side[2].width.px * s;
    l = m->side[3].width.px * s;
    /* THE TWO DERIVATIONS HELD TO EACH OTHER. CSS 2.1 §8.1 makes the border box the content box plus padding
       plus border on each axis and every one of those terms is non-negative, so the two widths on an axis
       cannot exceed the box — a border edge that did would put the padding edge INSIDE OUT, and the four
       wedges below would then be a bow tie whose nonzero winding paints a shape no cascade asked for rather
       than crashing. */
    DCHECKF(l + r <= w && t + bw <= h,
            "a border box of %g x %g device pixels carries used widths of %g/%g/%g/%g (top/right/bottom/left) "
            "— CSS 2.1 §8.1 \"Box dimensions\" makes the border box the content box plus padding plus border "
            "on each axis and all three are non-negative, so the two widths on an axis cannot exceed it. "
            "core/dom/element_view.c's rectangle and core/layout/used_value.c's widths are two derivations of "
            "ONE box and this is where they meet",
            w, h, t, r, bw, l);
    b.x0 = m->rect[0].px * s;
    b.y0 = m->rect[1].px * s;
    b.x1 = b.x0 + w;
    b.y1 = b.y0 + h;
    b.xi0 = b.x0 + l;
    b.yi0 = b.y0 + t;
    b.xi1 = b.x1 - r;
    b.yi1 = b.y1 - bw;
    dlr_border_wedges(&b, q);

    /* THE PARTITION, ASSERTED AGAINST A NUMBER THE WEDGES DID NOT PRODUCE. The four shoelace areas are read
       off the VERTICES about to be handed to the path; the right-hand side is the border area written as the
       difference of two products of EXTENTS, which shares no term with them. The two agree exactly in real
       arithmetic — the top and bottom wedges sum to (t+b)(2w-l-r)/2 and the left and right to (l+r)(2h-t-b)/2,
       whose total is wt+wb+hl+hr-(l+r)(t+b), which is w*h-(w-l-r)*(h-t-b).
       IT IS NOT A CHECK THAT CANNOT FAIL, AND THE DEFECT IT CATCHES IS THE ONE THIS ARM EXISTS TO AVOID: four
       side RECTANGLES sum to w(t+b)+h(l+r), which exceeds the border area by exactly (l+r)(t+b) — the four
       corner squares, counted twice — so the rectangle mistake shows up here as a surplus with a name rather
       than as ink whose winner is whichever loop ran last.
       THE TOLERANCE IS FLOATING-POINT AND NOT A MARGIN OF DESIGN. A shoelace over coordinates of magnitude
       `w` accumulates about `w*h * DBL_EPSILON` of representation error across its forty terms; a geometric
       surplus is (l+r)(t+b), which is at least the square of the smallest border width a display can carry
       and is many orders above it. */
    DCHECKF(fabs(dlr_wedge_surplus((const double (*)[4][2])q, w, h, t, r, bw, l)) <= 1e-9 * (w * h + 1.0),
            "the four mitred wedges of a border miss CSS 2.1 §8.1 \"Box dimensions\"' border area of %g device "
            "pixels by %g — the wedges PARTITION that area with no overlap and no gap, so a SURPLUS is the "
            "corners being covered twice (four side rectangles overlap by exactly (l+r)(t+b), which is %g "
            "here) and a SHORTFALL is a corner nothing covers",
            w * h - (w - l - r) * (h - t - bw),
            dlr_wedge_surplus((const double (*)[4][2])q, w, h, t, r, bw, l), (l + r) * (t + bw));

    for (i = 0; i < 4; i++) dlr_border_side(&m->side[i], q[i], surface, count);
}

/* ONE PLACED CHARACTER — a code point to a glyph to an outline to the same `dlr_fill` every other kind goes
 * through. The three lengths are multiplied by `s` and nothing else is: display_list_raster.h's own rule is
 * that this file applies the ONE transform in the road, and core/css/font_metrics.h states the other side of
 * the same seam — its `em_px` is "how many of the destination's own pixels one em is", so "the ratio arrives
 * multiplied in, or it does not arrive".
 *
 * THE THREE OUTCOMES ARE NOT COLLAPSED AND ONLY ONE OF THEM DRAWS, which is core/fonts/glyph_outline.h's
 * split read at the one place it decides something.
 *   `GLYPH_OUTLINE_OK` FILLS, AND IT IS ALSO WHAT AN EMPTY GLYPH ANSWERS — that outcome is "appended —
 *   possibly nothing, for a glyph that has no outline" in its own header's words, which is a U+0020 in every
 *   face that has one. So a space reaches the fill with an empty path and lays no span, and that is the same
 *   answer as a glyph nobody could draw rather than a different one: the path is what says how much ink there
 *   is, and `count->spans` beside it is what a reader distinguishes them by.
 *   `GLYPH_OUTLINE_UNSUPPORTED` IS A CAPABILITY THIS ENGINE HAS NOT BUILT and aborts naming it, which is
 *   CLAUDE.md's category (2) rather than a broken invariant: the glyph is VALID and the decoder for it is
 *   missing. It is reachable from a document's own text, which is the same forcing function a page's throw on
 *   an absent global is.
 *   THIS ARM USED TO NAME THE COMPOSITE GLYPH AND IS RE-AIMED RATHER THAN DELETED, because the reasoning is
 *   the outcome's and was never that one construct's. A composite is DECODED now — core/fonts/
 *   glyph_outline.h walks the component graph the standard describes — so what is left in the third outcome
 *   is the component placed by POINT ALIGNMENT rather than by an offset vector, which that header carries as
 *   a named residual with the next diff spelled out. The crash message below names THAT, because a crash
 *   that went on naming the composite would send its next reader to build what is already here, which is the
 *   one failure mode CLAUDE.md gives this mechanism.
 *   `GLYPH_OUTLINE_MALFORMED` IS A `DCHECK` AND THE DIFFERENCE IS WHOSE BYTES SAID SO. core/css/font_metrics.c
 *   reads one face and it is this engine's own committed `DEFAULT_FONT_SFNT`, whose whole read is already a
 *   `CHECK` there; a page's own `@font-face` bytes do not reach it, which that file says in its own words. So
 *   a malformed outline here is a claim about bytes this repository ships and is an invariant, where the same
 *   outcome over a document's font would be input and could only ever be a refusal. */
static void dlr_glyph(const DisplayMark *m, double s, RasterSurface *surface,
                      DisplayListRasterCount *count)
{
    RasterPath p;
    const char *reject = NULL;
    GlyphOutlineResult r;

    raster_path_init(&p);
    r = font_metrics_glyph_outline(m->glyph.cp, m->glyph.em.px * s,
                                   m->glyph.origin_x.px * s, m->glyph.origin_y.px * s, &p, &reject);
    if (r == GLYPH_OUTLINE_UNSUPPORTED)
        DFAILF("U+%04X selected a glyph built in a way this engine has no decoder for. core/fonts/"
               "glyph_outline.h answers that outcome rather than crashing because such a glyph is a VALID "
               "glyph a good font is entitled to carry, so what is missing is the decoder and not a repair "
               "here. EXACTLY ONE construct is in that outcome today and the header's residual names it: a "
               "COMPONENT PLACED BY POINT ALIGNMENT, which OpenType 'glyf' — Glyph Data's \"Composite glyph "
               "description\" reaches when a component's ARGS_ARE_XY_VALUES flag is CLEAR and its two "
               "arguments are a point number in the parent and a point number in the child instead of an "
               "offset vector. BUILD THE POINT LIST it needs: the parent's accumulated points in design "
               "units, renumbered as each child is incorporated, which the decoder does not retain today "
               "because it writes segments as it walks",
               (unsigned)m->glyph.cp);
    DCHECKF(r != GLYPH_OUTLINE_MALFORMED,
            "U+%04X's outline in the SHIPPED face broke a rule: %s. core/css/font_metrics.c holds exactly one "
            "face and it is this repository's own `DEFAULT_FONT_SFNT`, whose table directory read is already "
            "an always-fatal CHECK there — a page's own `@font-face` bytes do not reach it — so this is a "
            "claim about bytes this tree committed and never about a document's. Re-run "
            "engine/fontsubset.mjs rather than editing the bytes",
            (unsigned)m->glyph.cp, reject == NULL ? "(no reason given)" : reject);
    if (r == GLYPH_OUTLINE_OK) dlr_fill(&p, &m->color, surface, count);
    raster_path_free(&p);
}

/* WHAT ONE SPAN OF AN IMAGE MARK NEEDS — the bitmap, where the destination sits in DEVICE pixels, and the
   surface it lands on. `dx`, `dy`, `dw` and `dh` are the mark's rectangle already multiplied by the one
   transform in the road, which is display_list_raster.h's rule and is why nothing below multiplies again. */
typedef struct {
    const DisplayBitmap *bm;
    RasterSurface       *surface;
    double               dx, dy, dw, dh;
    size_t               spans;
    size_t               pixels;
} DlrImagePaint;

/* ONE RUN OF AN IMAGE, COMPOSITED. This is `raster_paint_span`'s job with a PER-PIXEL source instead of one
 * colour, and it is a second sink rather than a parameter on that one for the reason core/graphics/
 * raster_surface.h gives about keeping its own counters: a sink that sometimes read a colour and sometimes
 * sampled a bitmap would be one function answering two questions, and the branch would be asked once per
 * pixel of every fill in the engine.
 *
 * THE COVERAGE IS THE RASTERIZER'S AND THE SAMPLE IS THIS FUNCTION'S, which is the split that keeps an
 * image's EDGES identical to every other mark's. `raster_fill` computes analytic coverage for the destination
 * rectangle exactly as it does for a `DISPLAY_MARK_FILL_RECT`, so an image whose edges do not land on pixel
 * boundaries is antialiased by the same code that antialiases a background — and this sink multiplies that
 * coverage into the SAMPLE's own alpha rather than deciding either of them.
 *
 * SOURCE-OVER ON STRAIGHT ALPHA, which is the operator core/graphics/raster_surface.c states and which
 * core/image/png_decode.h's output is already in — "non-premultiplied RGBA8" in its own words — so there is no
 * un-premultiply here and there must not be one: a divide by a zero alpha is how a fully transparent source
 * pixel becomes a NaN colour.
 *
 * NAMED RESIDUAL — THE RESAMPLING IS NEAREST-NEIGHBOUR. WHAT IS NOT COVERED: css-images-3 §5.2 "Determining
 * How To Scale an Image: the image-rendering property" gives `auto` the latitude that a user agent "may use
 * any algorithm", so nearest-neighbour is CONFORMANT and this is a quality residual rather than a spec gap —
 * but that property's `smooth` and `high-quality` values ask for a filter this has none of, and `pixelated`
 * asks for exactly this one, so the three cannot be told apart today. WHAT THE NEXT DIFF BUILDS: the
 * `image-rendering` computed value read here, with a bilinear arm for `smooth`/`high-quality` and this
 * sampler kept as the `pixelated` and `crisp-edges` arm. HOW ITS ABSENCE WOULD SHOW: an image composited into
 * a rectangle that is not its natural size has hard stair-stepped edges inside it — visible on any document
 * that gives an `img` a `width` its file does not have — while its OUTER edge stays smooth, because that one
 * is the rasterizer's coverage and not this sampler's. */
static void dlr_image_span(void *user, int y, int x, int len, double coverage)
{
    DlrImagePaint *p = (DlrImagePaint *)user;
    const RasterSurface *s;
    const DisplayBitmap *bm;
    uint8_t *row;
    int i;

    DCHECK(p != NULL && p->surface != NULL, "an image span with no surface under it");
    s = p->surface;
    bm = p->bm;
    /* THE SAME TWO ASSERTIONS core/graphics/raster_surface.c MAKES AND FOR THE SAME REASONS, which is this
       sink discharging that file's contract rather than restating it: the bounds are a `CHECK` because the
       loop below WRITES through `x` and `y` in release as well as in dev, and the coverage is a `DCHECK`
       because a coverage out of range is a wrong PIXEL and not a wrong ADDRESS. Neither operand is a
       document's: both are numbers core/graphics/rasterizer.c computed from a region this codebase
       allocated. */
    CHECKF(y >= 0 && y < s->height && x >= 0 && len >= 1 && x <= s->width - len,
           "an image span outside the surface it is being composited into — row %d, columns %d..%d of a %dx%d "
           "surface", y, x, x + len - 1, s->width, s->height);
    DCHECKF(coverage > 0.0 && coverage <= 1.0,
            "an image span of coverage %g — a run of zero coverage is ink nothing downstream could tell from "
            "its absence and is not emitted, and a coverage above one is a fold that did not saturate",
            coverage);
    if (s->px == NULL) return;

    p->spans++;
    p->pixels += (size_t)len;

    row = s->px + ((size_t)y * (size_t)s->width + (size_t)x) * 4;
    for (i = 0; i < len; i++) {
        uint8_t *q = row + (size_t)i * 4;
        const uint8_t *src;
        double as, keep, ad, ao;
        long sx, sy;

        /* THE SAMPLE IS TAKEN AT THE PIXEL'S CENTRE — the `+ 0.5` — which is what makes this sampler pick the
           source pixel the destination pixel's MIDDLE lands in rather than the one its LEFT EDGE lands in.
           WHERE IT MATTERS IS NARROWER THAN IT LOOKS, AND THE NARROW STATEMENT IS THE TRUE ONE: at a 1:1
           composite and at any INTEGER scale the two spellings agree exactly, because the offset is then
           smaller than one source pixel's worth of destination and floors to the same index — 2 source pixels
           into a 2- or 40-wide destination give an identical answer with and without it. It bites at a
           NON-INTEGER scale, where dropping it biases every sample toward the source's origin: 2 source
           pixels into a 3-wide destination map to 0,1,1 with the offset and 0,0,1 without, so the second
           source pixel loses a third of its area to the first. That is a half-destination-pixel shift of the
           whole image toward the top-left, and it is a real defect rather than a rounding taste — it is also
           exactly why a probe that only composites at 1:1 and at 20x cannot see it. */
        sx = (long)(((double)x + (double)i + 0.5 - p->dx) * (double)bm->w / p->dw);
        sy = (long)(((double)y + 0.5 - p->dy) * (double)bm->h / p->dh);
        /* A SAMPLE OFF THE EDGE IS CLAMPED AND NOT SKIPPED. The rasterizer hands this sink only pixels the
           destination rectangle covers, so a coordinate outside the source can only be the last pixel of a
           row rounding outward by one — clamping keeps the run's colour and skipping would leave a
           transparent seam along two edges of every scaled image. */
        if (sx < 0) sx = 0;
        if (sx >= (long)bm->w) sx = (long)bm->w - 1;
        if (sy < 0) sy = 0;
        if (sy >= (long)bm->h) sy = (long)bm->h - 1;
        src = bm->rgba + ((size_t)sy * (size_t)bm->w + (size_t)sx) * 4;

        as = ((double)src[3] / 255.0) * coverage;
        if (as <= 0.0) continue;    /* a fully transparent source pixel changes no byte, whatever the coverage */
        ad = (double)q[3] / 255.0;
        keep = ad * (1.0 - as);
        ao = as + keep;
        if (ao <= 0.0) { q[0] = q[1] = q[2] = q[3] = 0; continue; }
        q[0] = raster_surface_quantize((((double)src[0] / 255.0) * as + ((double)q[0] / 255.0) * keep) / ao);
        q[1] = raster_surface_quantize((((double)src[1] / 255.0) * as + ((double)q[1] / 255.0) * keep) / ao);
        q[2] = raster_surface_quantize((((double)src[2] / 255.0) * as + ((double)q[2] / 255.0) * keep) / ao);
        q[3] = raster_surface_quantize(ao);
    }
}

/* ONE IMAGE — its destination rectangle through the same path road every other rectangle takes, filled by the
   sampling sink above. The four lengths are multiplied by `s` and nothing else is, which is
   display_list_raster.h's own rule.
   A ZERO-AREA DESTINATION LAYS NO INK AND IS NOT AN ERROR: a replaced element whose used width or height is
   zero is a legitimate box CSS 2.1 §10.3 admits, and `raster_fill` would emit no span for it anyway — the
   early return is what keeps the division in the sampler above from being asked about a zero extent. */
static void dlr_image(const DisplayMark *m, const DisplayList *dl, double s, RasterSurface *surface,
                      DisplayListRasterCount *count)
{
    DlrImagePaint paint;
    RasterPath p;
    RasterEdges e;
    size_t nspans;

    paint.bm = display_list_bitmap(dl, m->image.bitmap);
    paint.surface = surface;
    paint.dx = m->rect[0].px * s;
    paint.dy = m->rect[1].px * s;
    paint.dw = m->rect[2].px * s;
    paint.dh = m->rect[3].px * s;
    paint.spans = 0;
    paint.pixels = 0;
    if (!(paint.dw > 0.0) || !(paint.dh > 0.0)) return;

    raster_path_init(&p);
    raster_path_rect(&p, paint.dx, paint.dy, paint.dw, paint.dh);
    raster_edges_init(&e);
    raster_path_flatten(&p, RASTER_FLATTEN_TOLERANCE_PX, &e);
    nspans = raster_fill(&e, RASTER_FILL_NONZERO, surface->width, surface->height, dlr_image_span, &paint);
    /* TWO COUNTERS HELD TO EACH OTHER, which is `dlr_fill`'s own identity one kind over: a fill that emitted
       runs nobody received and a fill that emitted none are the same number at the caller, and two counts
       that must agree are not. A run this sink DROPPED for a transparent source still arrives here, because
       the drop is per PIXEL and the span was received. */
    DCHECKF(nspans == paint.spans,
            "an image fill emitted %zu runs and the sampler received %zu", nspans, paint.spans);
    raster_edges_free(&e);
    raster_path_free(&p);
    count->spans += paint.spans;
    count->pixels += paint.pixels;
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
        double s = device_px_per_css_px;

        switch (m->kind) {
        /* THE RECTANGLE KIND, WHOSE RECTANGLE IS ITS AREA. */
        case DISPLAY_MARK_FILL_RECT:
            dlr_fill_rect(m->rect[0].px * s, m->rect[1].px * s, m->rect[2].px * s, m->rect[3].px * s,
                          &m->color, surface, count);
            count->marks++;
            break;
        /* THE CANVAS KIND, WHOSE RECTANGLE IS NOT READ AT ALL — display_list_raster.h's own paragraph and
           core/paint/display_list.h's rule: CSS 2.1 §2.3.1 "The canvas" makes the area infinite, so its
           intersection with a finite surface is the whole of that surface whatever region this user agent
           established. */
        case DISPLAY_MARK_FILL_CANVAS:
            dlr_fill_rect(0.0, 0.0, (double)surface->width, (double)surface->height,
                          &m->color, surface, count);
            count->marks++;
            break;
        /* THE BORDER KIND, WHICH IS UP TO FOUR FILLS AND ONE MARK. `marks` counts marks this rasterizer had
           an arm for and PROCESSED, which is what the two kinds above already mean by it; a border whose
           sides are all zero-width, or whose styles this component still refuses in a release build, is
           counted here and contributes nothing to `spans` and `pixels`, so the two numbers beside it are
           what say how much of it was drawn. */
        case DISPLAY_MARK_BORDER:
            dlr_border(m, s, surface, count);
            count->marks++;
            break;
        /* THE GLYPH KIND, WHICH IS ONE FILL AND ONE MARK — and which is counted even where it lays nothing,
           for the reason the border arm above states: `marks` counts marks this rasterizer had an arm for and
           PROCESSED, so a U+0020 is a mark here that contributes to neither `spans` nor `pixels`, and those
           two beside it are what say how much of the text was drawn. */
        case DISPLAY_MARK_GLYPH:
            dlr_glyph(m, s, surface, count);
            count->marks++;
            break;
        /* THE ONE ARM THAT NEEDS THE LIST AND NOT JUST THE MARK, which is `DisplayImage`'s ownership answer
           arriving at its consumer: the pixels are the LIST's, so the mark's index is resolved against `dl`
           here exactly as a glyph's code point is resolved against core/css/font_metrics.h's one face in the
           arm above. */
        case DISPLAY_MARK_IMAGE:
            dlr_image(m, dl, s, surface, count);
            count->marks++;
            break;
        }
        /* A KIND OUTSIDE THE VOCABULARY, WHICH THE DOOR ALREADY REFUSED. core/paint/display_list.c asserts
           `dl_kind_is_defined` on every append and display_list.h states that an append is "the ONLY way ink
           enters a list", so a fourth kind is unreachable here and a crash would be a second answer to a
           question that component already owns. THERE IS NO `default:` ABOVE ON PURPOSE: `-Wswitch` is what
           names this site the day one lands, which display_list.h names as the mechanism, and a `default:`
           would take that away. */
    }
    /* EVERY MARK TOOK AN ARM AND EVERY ARM COUNTED ITS MARK — the parts sum to the total, asserted rather
       than described, because the paragraph above each arm ALREADY said so and one arm did not do it.
       MEASURED: the `DISPLAY_MARK_GLYPH` arm was the only one of the five that never raised `marks`, under
       a comment reading "THE GLYPH KIND, WHICH IS ONE FILL AND ONE MARK — and which is counted even where it
       lays nothing". The ink was right and only the tally was wrong, which is why nothing downstream looked
       broken: a document's text composited its spans and its pixels and reported ZERO marks for all of it,
       and core/paint/document_paint.c forwards this number verbatim, so every mark figure ever quoted for a
       painted page was short by its whole glyph count.
       IT IS AN EQUALITY RATHER THAN A PER-ARM CHECK BECAUSE THE PER-ARM CHECK IS THE COMMENT THAT FAILED.
       `display_list_append` is the only door into a list and asserts `dl_kind_is_defined`, so every one of
       `dl->n` marks reaches exactly one arm of the switch above — which makes the count of arms taken a fact
       this function may assert about ITSELF rather than a claim about its caller. A sixth kind added without
       its `marks++` therefore fires HERE, on the first list that carries one, instead of silently reporting a
       smaller number that no reader can tell from a page with less ink on it. */
    DCHECKF(count->marks == dl->n,
            "a display list of %zu mark(s) was rasterized and %zu of them were counted. `marks` is this "
            "rasterizer's count of marks it HAD AN ARM FOR AND PROCESSED, and core/paint/display_list.h "
            "makes an append the only way ink enters a list and asserts the kind at that door — so every "
            "mark here reaches an arm, and a shortfall is an arm that ran without raising the count rather "
            "than a mark this component was not built for. The arm to look at is the one whose kind is "
            "missing from the tally, and the repair is one line at that arm and never a weakening here",
            dl->n, count->marks);
}
