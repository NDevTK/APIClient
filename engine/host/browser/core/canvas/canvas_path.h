/* HTML §4.12.5.1.6 "Building paths" — the `CanvasPath` mixin's PATH, and the ten operations that build one.
 *
 * A PATH IS NOT A DEVICE. §4.12.5.1.6 defines one entirely as data — "A path has a list of zero or more
 * subpaths. Each subpath consists of a list of one or more points, connected by straight or curved line
 * segments, and a flag indicating whether the subpath is closed or not" — and every operation below is
 * arithmetic over that list. Nothing here needs a bitmap, a canvas element or a rendering context: the halves
 * of canvas that do are §4.12.5.1.13 "Drawing paths to the canvas" and §4.12.5.1.16 "Pixel manipulation",
 * which are different sections and a different component. That is why this builds as a complete component
 * with no stub in it while no canvas rendering context exists yet — CLAUDE.md §Headless-is-not-valueless.
 *
 * IT IS A MIXIN AND THEREFORE A COMPONENT RATHER THAN A FILE INSIDE ITS FIRST CONSUMER. Web IDL §3.7.3
 * "Interface prototype object" gives a mixin no prototype of its own, so the members below are placed on
 * EVERY interface that includes `CanvasPath` — `Path2D`, and §4.12.5.1's CanvasRenderingContext2D and
 * OffscreenCanvasRenderingContext2D when they exist. Writing them inside Path2D would put the shared half of
 * three interfaces behind whichever one landed first.
 *
 * THE PATH IS A JS ARRAY AND THAT IS THE LOAD-BEARING DECISION. A flow MUTATES a path — `p.lineTo(x, y)` in
 * one arm and not in its sibling — so the list is per-flow state that must fork, park to the cold tier and
 * resume. CLAUDE.md §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE settles the representation for exactly this
 * shape: an Array's mutations are property writes the COW delta already captures (cow_capture runs at
 * JS_SetPropertyInternal2's head, which is where a C caller's JS_SetPropertyUint32 arrives), and the snapshot
 * machinery already carries it. A malloc'd segment vector captured as a POINTER AND A LENGTH would revert the
 * pointer on a context switch and leave the segments reachable from nothing — a leak the runtime's own GC walk
 * cannot see, so no gate would report it.
 *
 * THE LAYOUT IS FIVE HEADER SLOTS AND THEN A FLAT OP STREAM, each op being an opcode followed by a fixed
 * number of doubles. The header exists so that "the last point in the subpath" — which lineTo, arcTo, the
 * curves and closePath all read — is one array read rather than a backward scan of the stream, which would
 * make building an N-segment path O(N^2). Each header slot is an ordinary element, so it forks and parks with
 * everything else; nothing about the path lives outside the array.
 *
 * THERE IS NO STORED `need new subpath` FLAG, AND ITS ABSENCE IS THE ROOT FIX RATHER THAN A SHORTCUT.
 * §4.12.5.1.6 gives a path that flag and sets it "When a path is created"; the only thing that reads it is
 * "ensure there is a subpath", and the only thing that unsets it is that same algorithm. No operation in this
 * mixin ever sets it again, and none removes a subpath — so it is TRUE exactly while the path has no
 * subpaths, and CANVAS_PATH_NSUB is that. Storing it as well would be a second copy of one fact, free to
 * disagree; deriving it makes the disagreement impossible to write. It stays equivalent for the rendering
 * contexts too: HTML §4.12.5.1.13 "Drawing paths to the canvas" says "The beginPath() method steps are to
 * empty the list of subpaths", which drives the count to zero and the derived flag to true in one act.
 *
 * NOTHING HERE APPLIES A TRANSFORMATION MATRIX, AND THAT IS THE MIXIN'S OWN SPLIT RATHER THAN AN OMISSION.
 * §4.12.5.1.6 says "For objects implementing the CanvasDrawPath and CanvasTransform interfaces, the points
 * passed to the methods, and the resulting lines added to current default path by these methods, must be
 * transformed according to the current transformation matrix before being added to the path". `Path2D`
 * implements neither, so its CTM is the identity and every coordinate below is stored as the page passed it.
 * A rendering context including this mixin owes the transform AT ITS OWN CALL SITE — which is also why
 * arcTo's step reading "the last point in the subpath, transformed by the inverse of the current
 * transformation matrix (so that it is in the same coordinate system as the points passed to the method)" is
 * a plain read of the stored point here and must not be copied as one by a caller that has a CTM. */
#ifndef ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_PATH_H
#define ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_PATH_H

#include <stdbool.h>

#include "quickjs.h"

/* The five header slots, then the op stream. Read and written as ordinary array elements. */
enum {
    CANVAS_PATH_CUR_X = 0,    /* "the last point in the subpath" */
    CANVAS_PATH_CUR_Y = 1,
    CANVAS_PATH_START_X = 2,  /* the first point of the LAST subpath — closePath's new subpath start */
    CANVAS_PATH_START_Y = 3,
    CANVAS_PATH_NSUB = 4,     /* the number of subpaths; the derived `need new subpath` flag is NSUB == 0 */
    CANVAS_PATH_OPS = 5
};

/* One opcode per SEGMENT KIND §4.12.5.1.6 names, never one per METHOD. `arc()`'s own steps are "run the
   ellipse method steps with this, x, y, radius, radius, 0, startAngle, endAngle, and counterclockwise", so it
   is the ellipse op and not a ninth kind; `rect()` is four points connected by straight lines and a closed
   flag, so it is MOVE + three LINEs + CLOSE. A kind per method would be several spellings of one segment for
   whatever reads the stream to paint it. */
typedef enum {
    CANVAS_PATH_OP_MOVE  = 0,  /* x y */
    CANVAS_PATH_OP_LINE  = 1,  /* x y */
    CANVAS_PATH_OP_QUAD  = 2,  /* cpx cpy x y */
    CANVAS_PATH_OP_CUBIC = 3,  /* c1x c1y c2x c2y x y */
    CANVAS_PATH_OP_ARC   = 4,  /* cx cy radiusX radiusY rotation startAngle endAngle counterclockwise */
    CANVAS_PATH_OP_CLOSE = 5   /* — */
} CanvasPathOp;

/* The element count of one op INCLUDING its opcode. Declared here so a reader of the stream and a writer of it
   cannot disagree about an arity; canvas_path.c asserts the table covers every opcode. */
int canvas_path_op_width(CanvasPathOp op);

/* §4.12.5.1.6's "When an object implementing the CanvasPath interface is created, its path must be
   initialized to zero subpaths" — a new Array with the header slots at zero. Returns an exception on
   allocation failure, which the caller propagates. */
JSValue canvas_path_new(JSContext *ctx);

/* §4.12.5.1.6's ten operations. Each takes the path array the including interface holds.
   THE RETURN IS 0 OR -1 WITH AN EXCEPTION PENDING, never a JSValue, because every one of them is `undefined`
   in the IDL and the only thing a caller can do with the answer is propagate it. The three that can throw are
   the three the standard says throw: arcTo and ellipse raise an "IndexSizeError" DOMException for a negative
   radius. The rest return 0 always — an infinite or NaN argument is a silent return in every one of these
   algorithms ("If either of the arguments are infinite or NaN, then return"), which is a DEFINED outcome and
   not an error. */
int canvas_path_move_to(JSContext *ctx, JSValueConst path, double x, double y);
int canvas_path_close_path(JSContext *ctx, JSValueConst path);
int canvas_path_line_to(JSContext *ctx, JSValueConst path, double x, double y);
int canvas_path_quadratic_curve_to(JSContext *ctx, JSValueConst path, double cpx, double cpy,
                                   double x, double y);
int canvas_path_bezier_curve_to(JSContext *ctx, JSValueConst path, double cp1x, double cp1y,
                                double cp2x, double cp2y, double x, double y);
int canvas_path_arc_to(JSContext *ctx, JSValueConst path, double x1, double y1, double x2, double y2,
                       double radius);
int canvas_path_rect(JSContext *ctx, JSValueConst path, double x, double y, double w, double h);
int canvas_path_arc(JSContext *ctx, JSValueConst path, double x, double y, double radius,
                    double start_angle, double end_angle, bool counterclockwise);
int canvas_path_ellipse(JSContext *ctx, JSValueConst path, double x, double y, double radius_x,
                        double radius_y, double rotation, double start_angle, double end_angle,
                        bool counterclockwise);

/* §4.12.5.1.7's "add all subpaths of path to output" — the operation the `Path2D(path)` constructor's Path2D
   arm is written in terms of, and the half of `addPath` that does not transform.
   IT TAKES NO MATRIX, WHICH IS THE ADD-PATH RESIDUAL'S SHAPE RATHER THAN A MISSING ARGUMENT. §4.12.5.1.7
   addPath step 5 is "Transform all the coordinates and lines in c by the transform matrix matrix", and an
   ARC is transformed by its PARAMETERS and not by its points — an affine map of an ellipse is another
   ellipse whose centre, two radii and rotation all move, which is a decomposition and not a coordinate map.
   A `const double *m` parameter here with NULL at its only two call sites would be untested code wearing a
   finished argument (CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS); the diff that builds addPath adds the
   parameter together with the caller that passes it.
   THE DESTINATION'S HEADER TAKES THE SOURCE'S, because with no transform the appended stream ends exactly
   where the source's does: its last point is the destination's last point, and its subpath count adds. */
int canvas_path_add_all(JSContext *ctx, JSValueConst dst, JSValueConst src);

/* Whether the path has no subpaths — §4.12.5.1.7 addPath step 1's "If the Path2D object path has no subpaths,
   then return", and the derived `need new subpath` flag. */
bool canvas_path_is_empty(JSContext *ctx, JSValueConst path);

/* Read a header slot as a double. The slots are written by this component alone and are always Numbers, which
   is asserted rather than defaulted. */
double canvas_path_header(JSContext *ctx, JSValueConst path, int slot);

#endif /* ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_PATH_H */
