/* THE SCANLINE FILL. See rasterizer.h for why one accumulation serves both of HTML §4.12.5.1's fill rules,
   why the requirement is determinism rather than agreement with another engine's pixels, and why a span is
   the seam. This file holds the derivation the header points at. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "core/graphics/raster_path.h"
#include "core/graphics/rasterizer.h"

/* A COVERAGE WITHIN THIS OF 0 OR 1 IS SNAPPED TO IT, AND THE NUMBER IS DERIVED RATHER THAN PICKED. The
   accumulation of a shape's interior reaches 1 up to the rounding of a few hundred additions, so without a
   snap the interior of every filled shape would be a run of distinct values a hair under 1 — one span per
   pixel, and a span count that measured floating-point noise rather than the shape. 1e-9 of coverage cannot
   move any quantization of 2^29 levels or fewer, which covers the 8-bit surface this engine composites into
   and a 16-bit one besides. A consumer that wants finer coverage than that is the one that has to revisit
   it, and there is none. */
#define RASTER_COVERAGE_SNAP 1e-9

/* THE RESIDUAL A ROW'S ACCUMULATION IS HELD TO, relative to the row's own total absolute mass. It is loose by
   three or four orders of magnitude against the worst-case rounding of a million additions and still tight
   against the defect it exists for: an unclosed subpath leaves a row with a net signed extent of the order of
   its own edges' rather than of their rounding. It is a DCHECK because both operands are numbers this file
   computed — the closure is `raster_path_flatten`'s guarantee and the arithmetic is this one's — and never
   because a caller's coordinates are trustworthy. */
#define RASTER_ROW_RESIDUAL_REL 1e-6

/* WHERE THE ARITHMETIC COMES FROM, AND IT IS ONE IDENTITY.
 *
 * Write w(u, v) for the crossing count HTML §4.12.5.1 defines — the signed number of times a half-infinite
 * horizontal line from (u, v) crosses the path. A pixel is an area, so the quantity this fill needs for pixel
 * P is the integral of w over P; the two fill rules then differ only in how they answer from it.
 *
 * Because the line is horizontal, w(u, v) depends only on the edges at that same v, so ROWS ARE INDEPENDENT
 * and an edge contributes to a row only through the part of it inside that row's band. That is the property
 * that makes clipping in y exact — a row outside the region is skipped rather than approximated — and it is
 * why there is no active-edge list here.
 *
 * For one edge e whose portion in a row spans the vertical range [ya, yb] and whose x at height v is xe(v),
 * the integral of w over pixel column k of that row is
 *
 *     dir(e) * INTEGRAL over [ya, yb] of clamp(k + 1 - xe(v), 0, 1) dv
 *
 * — the length of the part of [k, k+1] lying to the right of the edge, averaged down the band. Call the mean
 * of that clamp over the band L(k). Then the running total of the accumulation buffer up to column k IS
 * dir*(yb-ya)*L(k), so what each column's slot holds is the DIFFERENCE of two neighbouring L's, and the fill
 * recovers the coverage by a prefix sum along the row.
 *
 * Two consequences the code below relies on. First, L(k) rises from 0 to 1 across the columns the edge
 * touches and is flat outside them, so the differences are zero except over about |x1 - x0| + 2 columns and
 * the loop is proportional to the edge's own length rather than to the row's width. Second, the differences
 * TELESCOPE: their sum over every column is exactly dir*(yb-ya) whatever L is, so folding the two ends of the
 * loop into the first and last slots conserves mass exactly — which is what makes clipping in x exact too,
 * and what makes the per-row residual check below an identity rather than an estimate.
 *
 * `raster_mean_below` is L. It is written in terms of the distance from the column edge rather than in
 * absolute coordinates so that a path far from the origin does not lose the difference to cancellation. */
static double raster_mean_below(int k, double x0, double x1)
{
    double u = (double)k + 1.0, s0, s1, f0, f1;

    if (x1 <= x0) {
        double c = u - x0;

        return c <= 0.0 ? 0.0 : (c >= 1.0 ? 1.0 : c);
    }
    /* The antiderivative of clamp(u - x, 0, 1), written as a function of s = x - u: it is s below -1, the
       parabola -0.5 - 0.5*s*s between -1 and 0, and the constant -0.5 above 0. It is continuous at both
       joins and non-decreasing, so the difference below is in [0, x1 - x0] and the mean is in [0, 1]. */
    s0 = x0 - u;
    s1 = x1 - u;
    f0 = s0 <= -1.0 ? s0 : (s0 <= 0.0 ? -0.5 - 0.5 * s0 * s0 : -0.5);
    f1 = s1 <= -1.0 ? s1 : (s1 <= 0.0 ? -0.5 - 0.5 * s1 * s1 : -0.5);
    return (f1 - f0) / (x1 - x0);
}

/* HTML §4.12.5.1's two rules, over the one crossing count. `nonzero` is that a point is outside when the
   crossings in one direction equal the crossings in the other — a count of zero — so coverage is the
   magnitude of the count, saturated. `evenodd` is that a point is outside when the count is even, so coverage
   is the triangle wave: 0 at 0, 1 at 1, 0 at 2, and the interpolation between them is what a pixel straddling
   two winding regions is actually covered by. */
static double raster_fold(double count, RasterFillRule rule)
{
    double c;

    if (rule == RASTER_FILL_NONZERO) {
        c = fabs(count);
        if (c > 1.0) c = 1.0;
    } else {
        c = fmod(fabs(count), 2.0);
        if (c > 1.0) c = 2.0 - c;
    }
    if (c < RASTER_COVERAGE_SNAP) c = 0.0;
    else if (c > 1.0 - RASTER_COVERAGE_SNAP) c = 1.0;
    /* ASSERTED WHERE IT IS COMPUTED AND NOT WHERE IT IS USED. Every operand above is this file's own — the
       accumulation is this file's arithmetic over a closed chain the flattener guarantees — so a value out of
       range is this component's logic having gone wrong and never a number a caller passed. */
    DCHECK(c >= 0.0 && c <= 1.0,
           "a folded coverage outside [0, 1] — the nonzero arm saturates a magnitude and the even-odd arm is "
           "a triangle wave over a remainder, so neither can leave the unit interval");
    return c;
}

static double raster_lerp(double a, double b, double t) { return a * (1.0 - t) + b * t; }

static int raster_clamp_int(double d, int lo, int hi)
{
    if (!(d > (double)lo)) return lo;     /* also takes NaN, which no caller can produce here */
    if (d > (double)hi) return hi;
    return (int)d;
}

size_t raster_fill(const RasterEdges *edges, RasterFillRule rule, int width, int height,
                   RasterSpanFn sink, void *user)
{
    double minx, maxx, miny, maxy, *acc, *rowmass;
    int bx0, bx1, by0, by1, bw, rows, y, c;
    size_t i, spans = 0, cells;

    DCHECK(width >= 0 && height >= 0,
           "a fill was given a negative device dimension — a region comes from a surface this codebase "
           "allocated, so a negative one is this codebase's own conversion and not a page's number");
    DCHECK(sink != NULL, "a fill with no span sink computes a coverage nobody reads");
    if (edges->n == 0 || width <= 0 || height <= 0) return 0;

    minx = maxx = edges->v[0].x0;
    miny = maxy = edges->v[0].y0;
    for (i = 0; i < edges->n; i++) {
        const RasterEdge *e = &edges->v[i];

        if (e->x0 < minx) minx = e->x0;
        if (e->x1 < minx) minx = e->x1;
        if (e->x0 > maxx) maxx = e->x0;
        if (e->x1 > maxx) maxx = e->x1;
        if (e->y0 < miny) miny = e->y0;
        if (e->y1 < miny) miny = e->y1;
        if (e->y0 > maxy) maxy = e->y0;
        if (e->y1 > maxy) maxy = e->y1;
    }

    /* THE BUFFER IS THE INK'S BOUNDING BOX AND NOT THE SURFACE'S, so a small shape on a large surface costs
       its own area. The column range is widened by one on each side because a column's slot holds a
       difference of two neighbouring L's and the leftmost edge of the ink can therefore write one column to
       its left; the extra column at the right end is the slot everything beyond the region folds into, which
       is what keeps the mass conserved and the residual check below an identity. */
    bx0 = raster_clamp_int(floor(minx) - 1.0, 0, width);
    bx1 = raster_clamp_int(ceil(maxx) + 1.0, 0, width);
    by0 = raster_clamp_int(floor(miny), 0, height);
    by1 = raster_clamp_int(ceil(maxy), 0, height);
    if (bx1 < bx0) bx1 = bx0;
    rows = by1 - by0;
    bw = bx1 - bx0 + 1;
    if (rows <= 0) return 0;

    cells = (size_t)rows * (size_t)bw;
    CHECK((size_t)rows <= SIZE_MAX / (size_t)bw && cells <= SIZE_MAX / sizeof(double),
          "a fill's accumulation buffer is larger than an address can name");
    acc = (double *)calloc(cells, sizeof(double));
    CHECK(acc != NULL, "out of memory allocating a fill's accumulation buffer");
    rowmass = (double *)calloc((size_t)rows, sizeof(double));
    CHECK(rowmass != NULL, "out of memory allocating a fill's per-row mass");

    /* THE EDGES ARE WALKED IN PATH ORDER AND NOTHING SORTS THEM, which is the determinism requirement stated
       in rasterizer.h made concrete: floating-point addition is not associative, so a sort with ties would
       make the rendered bytes a function of the sort's tie-breaking rather than of the path. */
    for (i = 0; i < edges->n; i++) {
        const RasterEdge *e = &edges->v[i];
        double ya, yb, xa, xb, span, ytop, ybot;
        int dir, ry0, ry1;

        if (e->y0 == e->y1) continue;   /* a horizontal edge crosses no scanline and inks nothing */
        if (e->y0 < e->y1) { ya = e->y0; xa = e->x0; yb = e->y1; xb = e->x1; dir = 1; }
        else               { ya = e->y1; xa = e->x1; yb = e->y0; xb = e->x0; dir = -1; }
        span = yb - ya;

        /* Clipped in y BEFORE the row loop, so an edge far outside the region costs no iterations. Exact,
           because a row's crossing count reads only the edges at that row. */
        ytop = ya < (double)by0 ? (double)by0 : ya;
        ybot = yb > (double)by1 ? (double)by1 : yb;
        if (ybot <= ytop) continue;
        ry0 = (int)floor(ytop);
        ry1 = (int)floor(ybot);
        if (ry1 >= by1) ry1 = by1 - 1;

        for (y = ry0; y <= ry1; y++) {
            double rt = ytop > (double)y ? ytop : (double)y;
            double rb = ybot < (double)(y + 1) ? ybot : (double)(y + 1);
            double t0, t1, xA, xB, x0, x1, d, m_prev, *row;
            int kA, kB, k;

            if (rb <= rt) continue;
            t0 = (rt - ya) / span;
            t1 = (rb - ya) / span;
            if (t0 < 0.0) t0 = 0.0; else if (t0 > 1.0) t0 = 1.0;
            if (t1 < 0.0) t1 = 0.0; else if (t1 > 1.0) t1 = 1.0;
            /* A CONVEX COMBINATION AND NOT `xa + t * (xb - xa)`, for raster_path.c's reason: the difference
               of two finite endpoints can leave the representable range and this cannot. */
            xA = raster_lerp(xa, xb, t0);
            xB = raster_lerp(xa, xb, t1);
            x0 = xA < xB ? xA : xB;
            x1 = xA < xB ? xB : xA;
            d = (double)dir * (rb - rt);

            row = acc + (size_t)(y - by0) * (size_t)bw;
            rowmass[y - by0] += fabs(d);

            kA = raster_clamp_int(floor(x0) - 1.0, bx0, bx1);
            kB = raster_clamp_int(floor(x1) + 1.0, bx0, bx1);
            /* The two ends carry EVERYTHING beyond them — `raster_mean_below(kA)` is all the mass at or below
               column kA and `1 - raster_mean_below(kB)` is all of it above kB — so the terms TELESCOPE to `d`
               exactly whatever the clamps did, and a shape running off either side of the region loses no
               mass to the clipping. Rolling `m_prev` is what makes that telescoping visible as well as
               halving the evaluations. */
            m_prev = raster_mean_below(kA, x0, x1);
            row[kA - bx0] += d * m_prev;
            for (k = kA + 1; k <= kB; k++) {
                double m = raster_mean_below(k, x0, x1);

                row[k - bx0] += d * (m - m_prev);
                m_prev = m;
            }
            row[kB - bx0] += d * (1.0 - m_prev);
        }
    }

    /* THE PREFIX SUM, THE FOLD AND THE RUNS. */
    for (y = by0; y < by1; y++) {
        const double *row = acc + (size_t)(y - by0) * (size_t)bw;
        double prefix = 0.0, run_cov = 0.0, resid;
        int run_start = bx0, emitted = 0, skipped = 0;

        /* THE PIXELS ARE bx0 .. bx1-1 AND THE SLOT AT bx1 IS NOT ONE. `bx1` is at most `width`, so it is
           either the first column outside the region or the first column the ink cannot reach; either way
           the running total there is zero by the identity below, so a pixel at it would be a span of no
           coverage. The residual read after the loop is what that slot is for. */
        for (c = bx0; c < bx1; c++) {
            double cov;

            prefix += row[c - bx0];
            cov = raster_fold(prefix, rule);
            if (c == bx0) { run_cov = cov; run_start = c; continue; }
            if (cov != run_cov) {
                if (run_cov > 0.0) { sink(user, y, run_start, c - run_start, run_cov); spans++;
                                     emitted += c - run_start; }
                else skipped += c - run_start;
                run_cov = cov;
                run_start = c;
            }
        }
        if (bx1 > bx0) {
            if (run_cov > 0.0) { sink(user, y, run_start, bx1 - run_start, run_cov); spans++;
                                 emitted += bx1 - run_start; }
            else skipped += bx1 - run_start;
        }
        /* A COUNT THAT CANNOT BE TRUE. A run of zero coverage is not reported, so nothing downstream can tell
           a row this fill never looked at from one it looked at and found empty; holding the two halves to the
           row's own width is what makes the silence a statement. */
        DCHECKF(emitted + skipped == bx1 - bx0,
                "a fill's reported and skipped pixels do not sum to the row it walked — emitted %d, skipped "
                "%d, row %d wide", emitted, skipped, bx1 - bx0);

        /* AND THE IDENTITY THE WHOLE ARITHMETIC RESTS ON. `raster_path_flatten` closes every subpath, and
           within any horizontal band the signed vertical extents of a closed chain sum to zero; the column
           folds above conserve mass exactly; so the running total at the end of the buffer — including the
           slot everything beyond the region folded into — must return to zero. A residual is an unclosed
           subpath, a dropped edge or a column fold that lost mass, and nothing else. */
        prefix += row[bx1 - bx0];
        resid = fabs(prefix);
        DCHECKF(resid <= RASTER_ROW_RESIDUAL_REL * rowmass[y - by0] + 1e-12,
                "a fill's row accumulation did not return to zero — residual %g against a row mass of %g, "
                "which says the edge list handed to this fill is not a closed chain",
                resid, rowmass[y - by0]);
    }

    free(rowmass);
    free(acc);
    return spans;
}
