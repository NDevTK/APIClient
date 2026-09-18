/* GEOMETRY IN DEVICE PIXELS, AND ITS FLATTENING INTO EDGES. See raster_path.h for why this is its own layer,
   why the segment vocabulary is core/canvas/canvas_path.h's rather than a second copy of it, why a fill takes
   a C stream where a path takes a JS Array, and why a non-finite coordinate is a defined no-op. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "core/canvas/canvas_path.h"
#include "core/graphics/raster_path.h"

/* ---- the stream ------------------------------------------------------------------------------------------ */

void raster_path_init(RasterPath *p)
{
    p->v = NULL;
    p->n = 0;
    p->cap = 0;
    p->cur_x = p->cur_y = 0;
    p->start_x = p->start_y = 0;
    p->nsub = 0;
}

void raster_path_free(RasterPath *p)
{
    free(p->v);
    raster_path_init(p);
}

/* ALLOCATION IS A `CHECK` AND NOT A `DCHECK`, which is CLAUDE.md's rule for allocation and is also right on
   the merits: a dropped op is a segment silently absent from a chain whose closure is the property every
   arithmetic check below rests on, and a shape missing one edge is not a shape a consumer could tell from a
   different shape. */
static void rp_grow(RasterPath *p, size_t need)
{
    size_t cap = p->cap ? p->cap : 32;
    double *v;

    if (p->n + need <= p->cap) return;
    while (cap < p->n + need) {
        CHECK(cap <= SIZE_MAX / 2, "a RasterPath grew past what its capacity can express");
        cap *= 2;
    }
    v = (double *)realloc(p->v, cap * sizeof(double));
    CHECK(v != NULL, "out of memory growing a RasterPath's op stream");
    p->v = v;
    p->cap = cap;
}

/* Append one op. `n` is the count of ARGS, so the op occupies n + 1 elements — asserted against the ONE
   declared width table, exactly as core/canvas/canvas_path.c asserts it against the same table. */
static void rp_push(RasterPath *p, CanvasPathOp op, const double *args, int n)
{
    int i;

    DCHECK(canvas_path_op_width(op) == n + 1,
           "a RasterPath op was appended with an argument count its declared width does not have");
    rp_grow(p, (size_t)n + 1);
    p->v[p->n++] = (double)op;
    for (i = 0; i < n; i++) p->v[p->n++] = args[i];
}

/* HTML §4.12.5.1.6 "Building paths"' "create a new subpath with the point (x, y) as its first (and only)
   point" — the MOVE, the
   current point, the subpath start and the count, in one place so no operation can add a subpath without the
   count following it. */
static void rp_open_subpath(RasterPath *p, double x, double y)
{
    double a[2];

    a[0] = x; a[1] = y;
    rp_push(p, CANVAS_PATH_OP_MOVE, a, 2);
    p->cur_x = p->start_x = x;
    p->cur_y = p->start_y = y;
    p->nsub++;
}

/* HTML §4.12.5.1.6 "Building paths"' "ensure there is a subpath for a coordinate (x, y)". The
   `need new subpath` flag is the
   subpath count (raster_path.h), so opening the subpath is what unsets it. */
static void rp_ensure_subpath(RasterPath *p, double x, double y)
{
    if (p->nsub == 0) rp_open_subpath(p, x, y);
}

static bool rp_finite2(double a, double b) { return isfinite(a) && isfinite(b); }

void raster_path_move_to(RasterPath *p, double x, double y)
{
    if (!rp_finite2(x, y)) return;
    rp_open_subpath(p, x, y);
}

void raster_path_line_to(RasterPath *p, double x, double y)
{
    double a[2];

    if (!rp_finite2(x, y)) return;
    if (p->nsub == 0) { rp_ensure_subpath(p, x, y); return; }
    a[0] = x; a[1] = y;
    rp_push(p, CANVAS_PATH_OP_LINE, a, 2);
    p->cur_x = x; p->cur_y = y;
}

void raster_path_quadratic_curve_to(RasterPath *p, double cpx, double cpy, double x, double y)
{
    double a[4];

    if (!rp_finite2(cpx, cpy) || !rp_finite2(x, y)) return;
    rp_ensure_subpath(p, cpx, cpy);           /* the CONTROL point, not the end — §4.12.5.1.6's step 2 */
    a[0] = cpx; a[1] = cpy; a[2] = x; a[3] = y;
    rp_push(p, CANVAS_PATH_OP_QUAD, a, 4);
    p->cur_x = x; p->cur_y = y;
}

void raster_path_bezier_curve_to(RasterPath *p, double c1x, double c1y, double c2x, double c2y,
                                 double x, double y)
{
    double a[6];

    if (!rp_finite2(c1x, c1y) || !rp_finite2(c2x, c2y) || !rp_finite2(x, y)) return;
    rp_ensure_subpath(p, c1x, c1y);
    a[0] = c1x; a[1] = c1y; a[2] = c2x; a[3] = c2y; a[4] = x; a[5] = y;
    rp_push(p, CANVAS_PATH_OP_CUBIC, a, 6);
    p->cur_x = x; p->cur_y = y;
}

/* HTML §4.12.5.1.6 "Building paths"' "determine the point on an ellipse steps, given ellipse, and angle" is
   core/canvas/canvas_path.h's `canvas_path_ellipse_point` and is CALLED rather than repeated here. This file
   held a second copy of the same four lines, and its own note said why that copy could not be checked: the
   builder evaluates the start angle to place the LINE §4.12.5.1.6's step 3 puts in front of every ARC, the
   flattener chains from the point that LINE already stands at rather than re-evaluating it, so the two were
   never compared and an assert over them would have had two sides that cannot disagree. One copy settles it by
   construction — the arc cannot start anywhere but where the line ends — which is CLAUDE.md §Fix-the-ROOT's
   own test applied to a duplicated formula rather than to a state. */

void raster_path_ellipse(RasterPath *p, double cx, double cy, double radius_x, double radius_y,
                         double rotation, double start_angle, double end_angle, bool counterclockwise)
{
    double sx, sy, ex, ey, a[8], arg[2];
    bool whole;

    if (!rp_finite2(cx, cy) || !rp_finite2(radius_x, radius_y) || !isfinite(rotation) ||
        !rp_finite2(start_angle, end_angle))
        return;
    /* §4.12.5.1.6's step 2 throws an "IndexSizeError" for a negative radius, and a DOMException needs a realm
       this layer does not have — the rendering context refuses it before a coordinate reaches here. Treating
       it as the same silent return is the one outcome available that is not an invented answer. */
    if (radius_x < 0 || radius_y < 0) return;

    /* §4.12.5.1.6's step 4 whole-circumference test, over the angles AS GIVEN — the op below stores them
       unnormalised precisely so that this file and core/canvas/canvas_path.c reach the same arm from the same
       numbers. */
    whole = (!counterclockwise && end_angle - start_angle >= 2 * M_PI) ||
            (counterclockwise && start_angle - end_angle >= 2 * M_PI);
    canvas_path_ellipse_point(cx, cy, radius_x, radius_y, rotation, start_angle, &sx, &sy);
    if (whole) { ex = sx; ey = sy; }
    else canvas_path_ellipse_point(cx, cy, radius_x, radius_y, rotation, end_angle, &ex, &ey);

    /* Step 3 — "If canvasPath's path has any subpaths, then add a straight line from the last point in the
       subpath to the start point of the arc." */
    if (p->nsub == 0) {
        rp_open_subpath(p, sx, sy);
    } else {
        arg[0] = sx; arg[1] = sy;
        rp_push(p, CANVAS_PATH_OP_LINE, arg, 2);
        p->cur_x = sx; p->cur_y = sy;
    }
    a[0] = cx; a[1] = cy; a[2] = radius_x; a[3] = radius_y; a[4] = rotation;
    a[5] = start_angle; a[6] = end_angle; a[7] = counterclockwise ? 1 : 0;
    rp_push(p, CANVAS_PATH_OP_ARC, a, 8);
    p->cur_x = ex; p->cur_y = ey;
}

void raster_path_close_path(RasterPath *p)
{
    double sx, sy;

    if (p->nsub == 0) return;   /* "must do nothing if the object's path has no subpaths" */
    sx = p->start_x;
    sy = p->start_y;
    /* Two acts, and the opcode is the first alone: CLOSE says the subpath ending here is closed, and the new
       subpath §4.12.5.1.6 creates at the same first point is the ordinary MOVE every subpath begins with. */
    rp_push(p, CANVAS_PATH_OP_CLOSE, NULL, 0);
    rp_open_subpath(p, sx, sy);
}

void raster_path_rect(RasterPath *p, double x, double y, double w, double h)
{
    double a[2];

    if (!rp_finite2(x, y) || !rp_finite2(w, h)) return;                 /* step 1 */
    rp_open_subpath(p, x, y);                                           /* step 2 */
    a[0] = x + w; a[1] = y;         rp_push(p, CANVAS_PATH_OP_LINE, a, 2);
    a[0] = x + w; a[1] = y + h;     rp_push(p, CANVAS_PATH_OP_LINE, a, 2);
    a[0] = x;     a[1] = y + h;     rp_push(p, CANVAS_PATH_OP_LINE, a, 2);
    rp_push(p, CANVAS_PATH_OP_CLOSE, NULL, 0);                          /* step 3 */
    /* Step 4 — "Create a new subpath with the point (x, y) as the only point in the subpath." A one-point
       subpath is not nothing: HTML §4.12.5.1.6 says "Subpaths with only one point are ignored when painting the
       path", which is a statement about PAINTING, and such a subpath is still the one a following lineTo
       extends. The flattening below closes it against itself, which is the zero-length edge the fill skips. */
    rp_open_subpath(p, x, y);
}

/* ---- the edge list --------------------------------------------------------------------------------------- */

void raster_edges_init(RasterEdges *e) { e->v = NULL; e->n = 0; e->cap = 0; }
void raster_edges_free(RasterEdges *e) { free(e->v); raster_edges_init(e); }

static void re_push(RasterEdges *e, double x0, double y0, double x1, double y1)
{
    if (e->n == e->cap) {
        size_t cap = e->cap ? e->cap : 64;
        RasterEdge *v;

        CHECK(cap <= SIZE_MAX / (2 * sizeof(RasterEdge)),
              "a flattened edge list grew past what its capacity can express");
        cap *= 2;
        v = (RasterEdge *)realloc(e->v, cap * sizeof(RasterEdge));
        CHECK(v != NULL, "out of memory growing a flattened edge list");
        e->v = v;
        e->cap = cap;
    }
    /* EVERY EMITTED COORDINATE IS FINITE, AND THIS IS NOT VACUOUS. The builders above refuse a non-finite
       argument, so every CONTROL point is finite; what makes the interior points finite too is that this file
       evaluates a curve by repeated `rp_lerp`, whose result is a convex combination of two finite operands
       and therefore never leaves their interval. Evaluating the same curve from its expanded polynomial form
       would not have that property, and the assert is what holds the evaluation to the form that does. */
    DCHECK(isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1),
           "a flattened edge carried a non-finite coordinate — a curve is evaluated as a convex combination "
           "of finite control points, which cannot leave their interval, so this is this file's arithmetic "
           "and never the coordinate a caller passed");
    e->v[e->n].x0 = x0;
    e->v[e->n].y0 = y0;
    e->v[e->n].x1 = x1;
    e->v[e->n].y1 = y1;
    e->n++;
}

/* ---- flattening ------------------------------------------------------------------------------------------ */

/* A CONVEX COMBINATION AND NEVER `a + (b - a) * t`. The two agree in exact arithmetic and do not agree about
   OVERFLOW: `b - a` can leave the representable range for two finite endpoints, and `a*(1-t) + b*t` cannot,
   because each term is bounded by its own operand and the sum by the larger of the two. That is the whole of
   why `re_push`'s finiteness assert is an assert about this file rather than about its caller's numbers. */
static double rp_lerp(double a, double b, double t) { return a * (1.0 - t) + b * t; }

/* HOW MANY SEGMENTS, AS A COUNT RATHER THAN A RECURSION. A uniform subdivision needs no stack, no depth and
   no per-step flatness test, and it is DETERMINISTIC in the one sense this project needs: the same path and
   the same tolerance produce the same segments in the same order on every run, which is what makes a
   comparison of two renderings a comparison of the two documents.
   THE COUNT IS NOT A BOUND. It is derived from the curve and the tolerance and is never clamped; where it
   exceeds what memory can address, `re_push`'s allocation `CHECK` is what fires, which is the physical floor
   CLAUDE.md distinguishes from a cap. See this file's NAMED RESIDUAL for the case that makes that floor
   reachable and for the diff that removes it. */
static size_t rp_segments(double nd)
{
    const double lim = (double)(SIZE_MAX / sizeof(RasterEdge) / 4);

    if (!(nd >= 1.0)) return 1;     /* NaN, or a curve already flat enough for one chord */
    CHECK(nd <= lim,
          "a curve needs more flattened segments at this tolerance than memory can address — the count is a "
          "function of the curve's own extent, so this is the physical floor and not a cap");
    return (size_t)nd;
}

/* THE ERROR OF A UNIFORM SUBDIVISION, DERIVED RATHER THAN TUNED. For a Bezier B, the chord over a parameter
   interval of length h deviates from the curve by at most h^2·max|B''|/8. A quadratic has the constant
   B'' = 2(P0 - 2P1 + P2), so n uniform segments give an error of at most |P0 - 2P1 + P2|/(4n^2), and
   n >= sqrt(|P0 - 2P1 + P2| / (4·tolerance)) is what holds it under the tolerance. */
static size_t rp_quad_segments(double x0, double y0, double x1, double y1, double x2, double y2, double tol)
{
    double d = hypot(x0 - 2 * x1 + x2, y0 - 2 * y1 + y2);

    return rp_segments(ceil(sqrt(d / (4.0 * tol))));
}

/* A cubic's B''(t) = 6[(1-t)(P0 - 2P1 + P2) + t(P1 - 2P2 + P3)] is linear in t, so its magnitude is at most
   6·max of the two second differences and n >= sqrt(3·max/(4·tolerance)). */
static size_t rp_cubic_segments(double x0, double y0, double x1, double y1, double x2, double y2,
                                double x3, double y3, double tol)
{
    double a = hypot(x0 - 2 * x1 + x2, y0 - 2 * y1 + y2);
    double b = hypot(x1 - 2 * x2 + x3, y1 - 2 * y2 + y3);
    double m = a > b ? a : b;

    return rp_segments(ceil(sqrt(3.0 * m / (4.0 * tol))));
}

typedef struct {
    RasterEdges *out;
    double cur_x, cur_y;
    double sub_x, sub_y;
    bool   have_sub;
} RpFlatten;

static void rp_emit(RpFlatten *f, double x, double y)
{
    re_push(f->out, f->cur_x, f->cur_y, x, y);
    f->cur_x = x;
    f->cur_y = y;
}

static void rp_flatten_quad(RpFlatten *f, double cx, double cy, double x, double y, double tol)
{
    double x0 = f->cur_x, y0 = f->cur_y;
    size_t n = rp_quad_segments(x0, y0, cx, cy, x, y, tol), i;

    for (i = 1; i <= n; i++) {
        double t = (double)i / (double)n;
        double ax = rp_lerp(x0, cx, t), ay = rp_lerp(y0, cy, t);
        double bx = rp_lerp(cx, x, t), by = rp_lerp(cy, y, t);

        rp_emit(f, rp_lerp(ax, bx, t), rp_lerp(ay, by, t));
    }
}

static void rp_flatten_cubic(RpFlatten *f, double c1x, double c1y, double c2x, double c2y,
                             double x, double y, double tol)
{
    double x0 = f->cur_x, y0 = f->cur_y;
    size_t n = rp_cubic_segments(x0, y0, c1x, c1y, c2x, c2y, x, y, tol), i;

    for (i = 1; i <= n; i++) {
        double t = (double)i / (double)n;
        double ax = rp_lerp(x0, c1x, t),  ay = rp_lerp(y0, c1y, t);
        double bx = rp_lerp(c1x, c2x, t), by = rp_lerp(c1y, c2y, t);
        double cx = rp_lerp(c2x, x, t),   cy = rp_lerp(c2y, y, t);
        double dx = rp_lerp(ax, bx, t),   dy = rp_lerp(ay, by, t);
        double ex = rp_lerp(bx, cx, t),   ey = rp_lerp(by, cy, t);

        rp_emit(f, rp_lerp(dx, ex, t), rp_lerp(dy, ey, t));
    }
}

/* HTML §4.12.5.1.6 "Building paths"' arc, READ BACK OUT OF THE STORED ANGLES. The whole-circumference test
   is the same test the builder made, over the same unnormalised numbers, so the two reach the same arm; the
   rest is that section's own sentence — "the arc is the path along the circumference of this ellipse from
   the start point to the end point, going counterclockwise if counterclockwise is true, and clockwise
   otherwise ... the arc can never
   cover an angle greater than 2π radians" — which is the angular difference reduced into [0, 2π).
   THE FIRST POINT IS NOT EVALUATED. §4.12.5.1.6's step 3 puts a MOVE or a LINE at the start point in front of
   every ARC, so the chain already stands there; chaining from it rather than from a freshly evaluated
   `canvas_path_ellipse_point(start_angle)` is what makes the subpath a closed chain with no gap in it whatever
   the two evaluations round to. */
static void rp_flatten_arc(RpFlatten *f, const double *a, double tol)
{
    double cx = a[0], cy = a[1], rx = a[2], ry = a[3], rot = a[4];
    double sa = a[5], ea = a[6];
    bool ccw = a[7] != 0;
    double sweep, r, step, nd;
    size_t n, i;

    if ((!ccw && ea - sa >= 2 * M_PI) || (ccw && sa - ea >= 2 * M_PI)) {
        sweep = ccw ? -2 * M_PI : 2 * M_PI;
    } else {
        double d = fmod(ccw ? sa - ea : ea - sa, 2 * M_PI);

        if (d < 0) d += 2 * M_PI;
        sweep = ccw ? -d : d;
    }

    /* |P''(θ)| = hypot(radiusX·cos θ, radiusY·sin θ) <= max(radiusX, radiusY), so an angular step Δ deviates
       from its chord by at most Δ^2·max(radiusX, radiusY)/8 — the same second-derivative bound the two Bezier
       counts use, which is why one tolerance means the same thing for all three kinds.
       A STEP IS ALSO HELD AT A QUADRANT, which only ever RAISES the count: without it an ellipse smaller than
       the tolerance would take a single chord for a whole revolution and collapse to a point. */
    r = rx > ry ? rx : ry;
    step = M_PI / 2;
    if (r > 0) {
        double byerr = sqrt(8.0 * tol / r);

        if (byerr < step) step = byerr;
    }
    nd = ceil(fabs(sweep) / step);
    n = rp_segments(nd);
    for (i = 1; i <= n; i++) {
        double px, py;

        canvas_path_ellipse_point(cx, cy, rx, ry, rot, sa + sweep * ((double)i / (double)n), &px, &py);
        rp_emit(f, px, py);
    }
}

void raster_path_flatten(const RasterPath *p, double tolerance_px, RasterEdges *out)
{
    RpFlatten f;
    size_t i = 0;

    /* THE TOLERANCE IS THIS CODEBASE'S OWN NUMBER AND NOT A CALLER'S COORDINATE, which is what makes asserting
       it a `DCHECK` rather than the abort switch an assert over page input would be. */
    DCHECK(isfinite(tolerance_px) && tolerance_px > 0,
           "a flattening tolerance that is not a positive finite number of device pixels");

    f.out = out;
    f.cur_x = f.cur_y = f.sub_x = f.sub_y = 0;
    f.have_sub = false;

    while (i < p->n) {
        CanvasPathOp op = (CanvasPathOp)(int)p->v[i];
        int w = canvas_path_op_width(op);
        const double *a = &p->v[i + 1];

        DCHECK((size_t)w <= p->n - i,
               "a RasterPath op stream ended inside an op — the stream is written only by this component and "
               "every writer goes through one table for the width");
        switch (op) {
        case CANVAS_PATH_OP_MOVE:
            /* HTML §4.12.5.1.13 "Drawing paths to the canvas"' "Open subpaths must be implicitly closed
               when being filled (without affecting the
               actual subpaths)" — the close happens HERE, in the flattening, which is what that parenthesis
               asks for and is why `p` is const. */
            if (f.have_sub) rp_emit(&f, f.sub_x, f.sub_y);
            f.cur_x = f.sub_x = a[0];
            f.cur_y = f.sub_y = a[1];
            f.have_sub = true;
            break;
        case CANVAS_PATH_OP_LINE:
            DCHECK(f.have_sub, "a LINE op with no subpath open — every builder ensures a subpath first");
            rp_emit(&f, a[0], a[1]);
            break;
        case CANVAS_PATH_OP_QUAD:
            DCHECK(f.have_sub, "a QUAD op with no subpath open — every builder ensures a subpath first");
            rp_flatten_quad(&f, a[0], a[1], a[2], a[3], tolerance_px);
            break;
        case CANVAS_PATH_OP_CUBIC:
            DCHECK(f.have_sub, "a CUBIC op with no subpath open — every builder ensures a subpath first");
            rp_flatten_cubic(&f, a[0], a[1], a[2], a[3], a[4], a[5], tolerance_px);
            break;
        case CANVAS_PATH_OP_ARC:
            DCHECK(f.have_sub,
                   "an ARC op with no subpath open — §4.12.5.1.6's step 3 puts a MOVE or a LINE at the start "
                   "point in front of every one of them, and the flattening chains from it");
            rp_flatten_arc(&f, a, tolerance_px);
            break;
        case CANVAS_PATH_OP_CLOSE:
            DCHECK(f.have_sub, "a CLOSE op with no subpath open — the builder returns early for an empty path");
            rp_emit(&f, f.sub_x, f.sub_y);
            break;
        default:
            /* The operand is a `CanvasPathOp` this component wrote into its own stream, so the arm is
               unreachable by construction and is a guard rather than an unbuilt capability —
               CLAUDE.md §A-crash-in-the-`default:`-arm. `canvas_path_op_width` above holds the same list. */
            DFAIL("a RasterPath op stream carried an opcode the segment vocabulary does not define");
            break;
        }
        i += (size_t)w;
    }
    if (f.have_sub) rp_emit(&f, f.sub_x, f.sub_y);
}

/* NAMED RESIDUAL — A CURVE IS FLATTENED AT ITS OWN SCALE AND NOT AT THE SURFACE'S.
   WHAT IS NOT COVERED: nothing here consults the device region, so the segment count of a curve is a function
   of the magnitude of its own coordinates. §4.12.5.1.6 admits any finite number, so a page writing
   `bezierCurveTo` with coordinates of 1e18 asks for a count that reaches `rp_segments`' allocation floor on a
   canvas of a few hundred pixels — work, and then a crash, out of all proportion to the ink. It is not
   reachable today because nothing converts a JS path into a `RasterPath`; it becomes live with the first
   caller that does.
   WHAT THE NEXT DIFF BUILDS: hull rejection against the device clip box, which is EXACT in three of the four
   directions rather than an approximation. A Bezier lies inside the convex hull of its control points, so a
   curve whose hull has no y in [0, height) crosses no row of the surface and neither does its chord — and a
   curve whose hull is entirely at x >= width contributes only to columns §4.12.5.1.22 "Drawing model" already
   discards ("When compositing onto the output bitmap, pixels that would fall outside of the output bitmap
   must be discarded"). Both may be replaced by their chord with no pixel changing. The LEFT side is the one
   that needs an argument rather than an observation: everything at x < 0 folds into column 0 and therefore
   contributes its signed vertical extent to every pixel of the row, so a chord is exact there only where the
   piece is monotone in y — which makes the left arm a split at the curve's own y-extrema (at most one for a
   quadratic, two for a cubic) and then the chords of the monotone pieces.
   HOW ITS ABSENCE WOULD SHOW: the edge count a flatten produces, read beside the surface's dimensions, grows
   with the magnitude of the path's coordinates rather than with the area the path can ink.
   RETIREMENT: this record goes when `raster_path_flatten` is given the device region and a curve outside it
   costs a chord. */

/* NAMED RESIDUAL — AN ARC'S VERTICES ARE A FUNCTION OF THE PLATFORM'S MATH LIBRARY.
   WHAT IS NOT COVERED: core/graphics/rasterizer.h states the requirement as two renderings of one document
   agreeing byte for byte, which is what a reftest oracle is made of and which this component meets: every
   step of the accumulation is a `+`, `-`, `*`, `/` or `sqrt`, and IEEE 754 fixes all five exactly. The
   FLATTENING is not all five. `canvas_path_ellipse_point` calls `cos` and `sin`, the segment counts call
   `hypot`, and C leaves the accuracy of <math.h> implementation-defined — so two HOSTS' libraries may answer
   a last bit apart, which moves a vertex and, through `ceil`, can move the segment COUNT by one and with it
   the whole polygon. Two renderings on ONE host still agree, so the reftest oracle is intact; what is narrower
   than the header's sentence is a comparison ACROSS the native host and the wasm one.
   WHAT THE NEXT DIFF BUILDS: a sine, a cosine and a hypotenuse of stated precision, so an arc's vertices and
   every segment count are a function of the arithmetic rather than of the platform. There is now ONE ellipse
   point construction rather than two, so the three entries have one call site to serve in
   core/canvas/canvas_path.c and one here — which settles WHERE they belong (reachable from both, and with no
   `JSContext`) and is the second reason to build them rather than to pin a library.
   HOW ITS ABSENCE WOULD SHOW: a document whose ink is made only of lines and Bezier curves checksums
   identically under the two hosts, and one containing an ARC may not — so the discriminator is whether a
   `CANVAS_PATH_OP_ARC` reached the flattening, and never the shape's size, its position or its tolerance.
   RETIREMENT: this record goes when no vertex and no segment count in this component is a function of a
   <math.h> call whose accuracy C leaves to the implementation. */
