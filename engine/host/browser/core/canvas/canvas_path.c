/* HTML §4.12.5.1.6 "Building paths" — the CanvasPath mixin's path and its ten operations. See canvas_path.h
   for the representation and for why nothing here needs a device. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/canvas/canvas_path.h"

/* ---- the array ------------------------------------------------------------------------------------------ */

int canvas_path_op_width(CanvasPathOp op)
{
    switch (op) {
    case CANVAS_PATH_OP_MOVE:  return 3;
    case CANVAS_PATH_OP_LINE:  return 3;
    case CANVAS_PATH_OP_QUAD:  return 5;
    case CANVAS_PATH_OP_CUBIC: return 7;
    case CANVAS_PATH_OP_ARC:   return 9;
    case CANVAS_PATH_OP_CLOSE: return 1;
    }
    /* The operand is this component's OWN enum over a closed set, so the arm is unreachable by construction
       and is a guard rather than an unbuilt capability — CLAUDE.md §A-crash-in-the-`default:`-arm. */
    DFAIL("a CanvasPath op stream carried an opcode this component does not define");
    return 0;
}

static double cp_elem(JSContext *ctx, JSValueConst path, uint32_t i)
{
    JSValue v = JS_GetPropertyUint32(ctx, path, i);
    double d = 0;

    /* Every element of this array is written by this file and is always a Number. A non-Number here is this
       codebase's own logic having gone wrong, never page input — the array is never handed to a page. */
    DCHECK(JS_IsNumber(v), "a CanvasPath array element was not a Number — the stream is written only here");
    if (JS_ToFloat64(ctx, &d, v) < 0) d = 0;
    JS_FreeValue(ctx, v);
    return d;
}

static void cp_set(JSContext *ctx, JSValueConst path, uint32_t i, double d)
{
    /* An ordinary element write, which is the whole point of the representation: cow_capture runs at
       JS_SetPropertyInternal2's head, so this write joins the running flow's delta with nothing to remember. */
    JS_SetPropertyUint32(ctx, path, i, JS_NewFloat64(ctx, d));
}

static uint32_t cp_len(JSContext *ctx, JSValueConst path)
{
    JSValue lv = JS_GetPropertyStr(ctx, path, "length");
    int64_t n = 0;

    if (JS_ToInt64(ctx, &n, lv) < 0) n = 0;
    JS_FreeValue(ctx, lv);
    DCHECK(n >= CANVAS_PATH_OPS, "a CanvasPath array was shorter than its own header");
    return (uint32_t)n;
}

double canvas_path_header(JSContext *ctx, JSValueConst path, int slot)
{
    DCHECK(slot >= 0 && slot < CANVAS_PATH_OPS, "a CanvasPath header slot outside the declared header");
    return cp_elem(ctx, path, (uint32_t)slot);
}

bool canvas_path_is_empty(JSContext *ctx, JSValueConst path)
{
    return canvas_path_header(ctx, path, CANVAS_PATH_NSUB) == 0;
}

/* Append one op. `n` is the count of ARGS, so the op occupies n + 1 elements — asserted against the declared
   width so a writer and the table cannot disagree. */
static void cp_push(JSContext *ctx, JSValueConst path, CanvasPathOp op, const double *args, int n)
{
    uint32_t at = cp_len(ctx, path);
    int i;

    DCHECK(canvas_path_op_width(op) == n + 1,
           "a CanvasPath op was appended with an argument count its declared width does not have");
    cp_set(ctx, path, at, (double)op);
    for (i = 0; i < n; i++) cp_set(ctx, path, at + 1 + (uint32_t)i, args[i]);
}

static void cp_set_cur(JSContext *ctx, JSValueConst path, double x, double y)
{
    cp_set(ctx, path, CANVAS_PATH_CUR_X, x);
    cp_set(ctx, path, CANVAS_PATH_CUR_Y, y);
}

/* A new subpath whose first point is (x, y): the MOVE, the two point slots and the count, in one place so no
   operation can add a subpath without the count following it. */
static void cp_open_subpath(JSContext *ctx, JSValueConst path, double x, double y)
{
    double a[2];

    a[0] = x; a[1] = y;
    cp_push(ctx, path, CANVAS_PATH_OP_MOVE, a, 2);
    cp_set_cur(ctx, path, x, y);
    cp_set(ctx, path, CANVAS_PATH_START_X, x);
    cp_set(ctx, path, CANVAS_PATH_START_Y, y);
    cp_set(ctx, path, CANVAS_PATH_NSUB, canvas_path_header(ctx, path, CANVAS_PATH_NSUB) + 1);
}

/* §4.12.5.1.6's "ensure there is a subpath for a coordinate (x, y)" — "check to see if the path has its need
   new subpath flag set. If it does, then … create a new subpath with the point (x, y) as its first (and only)
   point, as if the moveTo() method had been called, and … then unset the path's need new subpath flag."
   THE FLAG IS THE SUBPATH COUNT (see canvas_path.h), so opening the subpath is what unsets it. */
static void cp_ensure_subpath(JSContext *ctx, JSValueConst path, double x, double y)
{
    if (canvas_path_is_empty(ctx, path)) cp_open_subpath(ctx, path, x, y);
}

static bool cp_finite2(double a, double b) { return isfinite(a) && isfinite(b); }

JSValue canvas_path_new(JSContext *ctx)
{
    JSValue path = JS_NewArray(ctx);
    int i;

    if (JS_IsException(path)) return path;
    /* "its path must be initialized to zero subpaths" — and with it the derived need-new-subpath flag. */
    for (i = 0; i < CANVAS_PATH_OPS; i++) cp_set(ctx, path, (uint32_t)i, 0);
    return path;
}

/* ---- §4.12.5.1.6's operations ---------------------------------------------------------------------------- */

int canvas_path_move_to(JSContext *ctx, JSValueConst path, double x, double y)
{
    if (!cp_finite2(x, y)) return 0;                      /* step 1 */
    cp_open_subpath(ctx, path, x, y);                     /* step 2 */
    return 0;
}

int canvas_path_close_path(JSContext *ctx, JSValueConst path)
{
    double sx, sy;

    /* "must do nothing if the object's path has no subpaths" */
    if (canvas_path_is_empty(ctx, path)) return 0;
    sx = canvas_path_header(ctx, path, CANVAS_PATH_START_X);
    sy = canvas_path_header(ctx, path, CANVAS_PATH_START_Y);
    /* "mark the last subpath as closed, create a new subpath whose first point is the same as the previous
       subpath's first point, and finally add this new subpath to the path" — two acts, and the opcode is the
       first of them alone: CLOSE says the subpath ending here is closed and nothing else, so the new subpath
       is the ordinary MOVE every other subpath begins with rather than a second meaning on one opcode. */
    cp_push(ctx, path, CANVAS_PATH_OP_CLOSE, NULL, 0);
    cp_open_subpath(ctx, path, sx, sy);
    return 0;
}

int canvas_path_line_to(JSContext *ctx, JSValueConst path, double x, double y)
{
    double a[2];

    if (!cp_finite2(x, y)) return 0;                      /* step 1 */
    if (canvas_path_is_empty(ctx, path)) {                /* step 2 */
        cp_ensure_subpath(ctx, path, x, y);
        return 0;
    }
    a[0] = x; a[1] = y;                                   /* step 3 */
    cp_push(ctx, path, CANVAS_PATH_OP_LINE, a, 2);
    cp_set_cur(ctx, path, x, y);
    return 0;
}

int canvas_path_quadratic_curve_to(JSContext *ctx, JSValueConst path, double cpx, double cpy,
                                   double x, double y)
{
    double a[4];

    if (!cp_finite2(cpx, cpy) || !cp_finite2(x, y)) return 0;   /* step 1 */
    cp_ensure_subpath(ctx, path, cpx, cpy);                     /* step 2 — the CONTROL point, not the end */
    a[0] = cpx; a[1] = cpy; a[2] = x; a[3] = y;                 /* steps 3 and 4 */
    cp_push(ctx, path, CANVAS_PATH_OP_QUAD, a, 4);
    cp_set_cur(ctx, path, x, y);
    return 0;
}

int canvas_path_bezier_curve_to(JSContext *ctx, JSValueConst path, double cp1x, double cp1y,
                                double cp2x, double cp2y, double x, double y)
{
    double a[6];

    if (!cp_finite2(cp1x, cp1y) || !cp_finite2(cp2x, cp2y) || !cp_finite2(x, y)) return 0;  /* step 1 */
    cp_ensure_subpath(ctx, path, cp1x, cp1y);                                               /* step 2 */
    a[0] = cp1x; a[1] = cp1y; a[2] = cp2x; a[3] = cp2y; a[4] = x; a[5] = y;                 /* steps 3, 4 */
    cp_push(ctx, path, CANVAS_PATH_OP_CUBIC, a, 6);
    cp_set_cur(ctx, path, x, y);
    return 0;
}

/* §4.12.5.1.6's arcTo. THE STEP ORDER IS OBSERVABLE AND IS KEPT: "Ensure there is a subpath for (x1, y1)" is
   step 2 and the negative-radius throw is step 3, so `new Path2D().arcTo(1, 1, 2, 2, -1)` leaves a subpath at
   (1, 1) behind AND throws. Sorting the cheap check first would be a different program. */
int canvas_path_arc_to(JSContext *ctx, JSValueConst path, double x1, double y1, double x2, double y2,
                       double radius)
{
    double x0, y0, v1x, v1y, v2x, v2y, l1, l2, u1x, u1y, u2x, u2y, cosv, cross, theta;
    double d, t1x, t1y, t2x, t2y, bx, by, bl, hyp, cx, cy, a0, a1, arg[2], earg[8];

    if (!cp_finite2(x1, y1) || !cp_finite2(x2, y2) || !isfinite(radius)) return 0;   /* step 1 */
    cp_ensure_subpath(ctx, path, x1, y1);                                            /* step 2 */
    if (radius < 0)                                                                  /* step 3 */
        return JS_ThrowDOMException(ctx, "IndexSizeError",
                                    "arcTo was given a negative radius"), -1;

    /* Step 4 — "the last point in the subpath, transformed by the inverse of the current transformation
       matrix". A CanvasPath that is not also a CanvasTransform has the identity as its CTM, so this is the
       stored point; a rendering context including this mixin owes the inverse at its own call site. */
    x0 = canvas_path_header(ctx, path, CANVAS_PATH_CUR_X);
    y0 = canvas_path_header(ctx, path, CANVAS_PATH_CUR_Y);

    v1x = x0 - x1; v1y = y0 - y1;
    v2x = x2 - x1; v2y = y2 - y1;
    l1 = hypot(v1x, v1y);
    l2 = hypot(v2x, v2y);
    u1x = l1 != 0 ? v1x / l1 : 0; u1y = l1 != 0 ? v1y / l1 : 0;
    u2x = l2 != 0 ? v2x / l2 : 0; u2y = l2 != 0 ? v2y / l2 : 0;
    cross = u1x * u2y - u1y * u2x;

    /* Steps 5 and 6 — the two degenerate arms and the collinear one, all three "add the point (x1, y1) to the
       subpath, and connect that point to the previous point (x0, y0) by a straight line". */
    if (l1 == 0 || l2 == 0 || radius == 0 || cross == 0) {
        arg[0] = x1; arg[1] = y1;
        cp_push(ctx, path, CANVAS_PATH_OP_LINE, arg, 2);
        cp_set_cur(ctx, path, x1, y1);
        return 0;
    }

    /* Step 7 — "The Arc … the shortest arc given by circumference of the circle that has radius radius, and
       that has one point tangent to the half-infinite line that crosses the point (x0, y0) and ends at the
       point (x1, y1), and that has a different point tangent to the half-infinite line that ends at the point
       (x1, y1) and crosses the point (x2, y2)". The two tangent points sit at radius/tan(theta/2) along each
       ray from (x1, y1), and the centre at radius/sin(theta/2) along their bisector. */
    cosv = u1x * u2x + u1y * u2y;
    if (cosv > 1) cosv = 1;
    if (cosv < -1) cosv = -1;
    theta = acos(cosv);
    d = radius / tan(theta / 2);
    t1x = x1 + u1x * d; t1y = y1 + u1y * d;
    t2x = x1 + u2x * d; t2y = y1 + u2y * d;
    bx = u1x + u2x; by = u1y + u2y;
    bl = hypot(bx, by);
    DCHECK(bl > 0, "arcTo reached its bisector with two opposed rays — that is the collinear arm above");
    hyp = radius / sin(theta / 2);
    cx = x1 + bx / bl * hyp;
    cy = y1 + by / bl * hyp;
    a0 = atan2(t1y - cy, t1x - cx);
    a1 = atan2(t2y - cy, t2x - cx);

    /* "Connect the point (x0, y0) to the start tangent point by a straight line, adding the start tangent
       point to the subpath, and then connect the start tangent point to the end tangent point by The Arc,
       adding the end tangent point to the subpath." */
    arg[0] = t1x; arg[1] = t1y;
    cp_push(ctx, path, CANVAS_PATH_OP_LINE, arg, 2);
    earg[0] = cx; earg[1] = cy; earg[2] = radius; earg[3] = radius; earg[4] = 0;
    earg[5] = a0; earg[6] = a1;
    /* The turn at (x1, y1) from the incoming ray to the outgoing one has the sign of the cross product, and
       the SHORT arc between the tangent points runs the other way round the circle. */
    earg[7] = cross < 0 ? 1 : 0;
    cp_push(ctx, path, CANVAS_PATH_OP_ARC, earg, 8);
    cp_set_cur(ctx, path, t2x, t2y);
    return 0;
}

int canvas_path_rect(JSContext *ctx, JSValueConst path, double x, double y, double w, double h)
{
    double a[2];

    if (!cp_finite2(x, y) || !cp_finite2(w, h)) return 0;     /* step 1 */

    /* Step 2 — "Create a new subpath containing just the four points (x, y), (x+w, y), (x+w, y+h), (x, y+h),
       in that order, with those four points connected by straight lines." */
    cp_open_subpath(ctx, path, x, y);
    a[0] = x + w; a[1] = y;         cp_push(ctx, path, CANVAS_PATH_OP_LINE, a, 2);
    a[0] = x + w; a[1] = y + h;     cp_push(ctx, path, CANVAS_PATH_OP_LINE, a, 2);
    a[0] = x;     a[1] = y + h;     cp_push(ctx, path, CANVAS_PATH_OP_LINE, a, 2);
    cp_push(ctx, path, CANVAS_PATH_OP_CLOSE, NULL, 0);        /* step 3 */
    /* Step 4 — "Create a new subpath with the point (x, y) as the only point in the subpath." A one-point
       subpath is not nothing: HTML §4.12.5.1.6 says "Subpaths with only one point are ignored when painting
       the path", which is a statement about PAINTING, and such a subpath is still the one a following lineTo
       extends. */
    cp_open_subpath(ctx, path, x, y);
    return 0;
}

/* §4.12.5.1.6's "determine the point on an ellipse steps, given ellipse, and angle". The construction — the
   eccentric circle, the chord perpendicular to the major axis, the crossing point — is the standard parametric
   point (radiusX·cos θ, radiusY·sin θ) in the ellipse's own frame, which is then rotated by `rotation` and
   translated to the centre. */
static void cp_ellipse_point(double x, double y, double rx, double ry, double rot, double angle,
                             double *px, double *py)
{
    double ct = cos(angle), st = sin(angle), cr = cos(rot), sr = sin(rot);

    *px = x + rx * ct * cr - ry * st * sr;
    *py = y + rx * ct * sr + ry * st * cr;
}

int canvas_path_ellipse(JSContext *ctx, JSValueConst path, double x, double y, double radius_x,
                        double radius_y, double rotation, double start_angle, double end_angle,
                        bool counterclockwise)
{
    double sx, sy, ex, ey, a[8], arg[2];
    bool whole;

    /* Step 1 */
    if (!cp_finite2(x, y) || !cp_finite2(radius_x, radius_y) || !isfinite(rotation) ||
        !cp_finite2(start_angle, end_angle))
        return 0;
    /* Step 2 */
    if (radius_x < 0 || radius_y < 0)
        return JS_ThrowDOMException(ctx, "IndexSizeError",
                                    "ellipse was given a negative radius"), -1;

    /* Step 4's own definition of the start and end points, read before step 3 because step 3's straight line
       runs "to the start point of the arc" and therefore needs it. "If counterclockwise is false and endAngle
       − startAngle is greater than or equal to 2π, or, if counterclockwise is true and startAngle − endAngle
       is greater than or equal to 2π, then the arc is the whole circumference of this ellipse, and both the
       start point and the end point are the result of running the determine the point on an ellipse steps
       given this ellipse and startAngle." */
    whole = (!counterclockwise && end_angle - start_angle >= 2 * M_PI) ||
            (counterclockwise && start_angle - end_angle >= 2 * M_PI);
    cp_ellipse_point(x, y, radius_x, radius_y, rotation, start_angle, &sx, &sy);
    if (whole) { ex = sx; ey = sy; }
    else cp_ellipse_point(x, y, radius_x, radius_y, rotation, end_angle, &ex, &ey);

    /* Step 3 — "If canvasPath's path has any subpaths, then add a straight line from the last point in the
       subpath to the start point of the arc." With no subpath there is none to extend, and step 4's "Add the
       start and end points of the arc to the subpath" then needs one, which the start point opens. */
    if (canvas_path_is_empty(ctx, path)) {
        cp_open_subpath(ctx, path, sx, sy);
    } else {
        arg[0] = sx; arg[1] = sy;
        cp_push(ctx, path, CANVAS_PATH_OP_LINE, arg, 2);
    }

    /* Step 4. The two angles are stored AS GIVEN rather than normalised: the whole-circumference test above is
       a statement about their difference, so a consumer that paints the op re-reads the same numbers and
       reaches the same arm. "Even if the arc covers the entire circumference of the ellipse … the path is not
       closed unless the closePath() method is appropriately invoked" — so no CLOSE is appended here. */
    a[0] = x; a[1] = y; a[2] = radius_x; a[3] = radius_y; a[4] = rotation;
    a[5] = start_angle; a[6] = end_angle; a[7] = counterclockwise ? 1 : 0;
    cp_push(ctx, path, CANVAS_PATH_OP_ARC, a, 8);
    cp_set_cur(ctx, path, ex, ey);
    return 0;
}

/* "The arc() method, when invoked, must run the ellipse method steps with this, x, y, radius, radius, 0,
   startAngle, endAngle, and counterclockwise." It is that call and nothing else — an arc is not a segment kind
   of its own, so there is no second implementation here to drift from the one above. */
int canvas_path_arc(JSContext *ctx, JSValueConst path, double x, double y, double radius,
                    double start_angle, double end_angle, bool counterclockwise)
{
    return canvas_path_ellipse(ctx, path, x, y, radius, radius, 0, start_angle, end_angle, counterclockwise);
}

/* ---- §4.12.5.1.7's "add all subpaths of path to output" --------------------------------------------------- */

int canvas_path_add_all(JSContext *ctx, JSValueConst dst, JSValueConst src)
{
    uint32_t n = cp_len(ctx, src), at = cp_len(ctx, dst), i;
    double nsub;

    for (i = CANVAS_PATH_OPS; i < n; i++) {
        /* The stream is copied verbatim, opcodes and all, so the walk needs no per-op arm and cannot get an
           arity wrong; the width table is asserted where ops are WRITTEN. */
        cp_set(ctx, dst, at + (i - CANVAS_PATH_OPS), cp_elem(ctx, src, i));
    }
    nsub = canvas_path_header(ctx, src, CANVAS_PATH_NSUB);
    if (nsub == 0) return 0;      /* nothing was appended, so the destination's own last point still stands */
    cp_set_cur(ctx, dst, canvas_path_header(ctx, src, CANVAS_PATH_CUR_X),
               canvas_path_header(ctx, src, CANVAS_PATH_CUR_Y));
    cp_set(ctx, dst, CANVAS_PATH_START_X, canvas_path_header(ctx, src, CANVAS_PATH_START_X));
    cp_set(ctx, dst, CANVAS_PATH_START_Y, canvas_path_header(ctx, src, CANVAS_PATH_START_Y));
    cp_set(ctx, dst, CANVAS_PATH_NSUB, canvas_path_header(ctx, dst, CANVAS_PATH_NSUB) + nsub);
    return 0;
}
