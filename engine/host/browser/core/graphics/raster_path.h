/* GEOMETRY IN DEVICE PIXELS, AND ITS FLATTENING INTO EDGES — the first half of turning a path into pixels.
 *
 * WHY THIS IS A DIRECTORY OF ITS OWN AND NOT A FILE INSIDE ITS FIRST CONSUMER. Every other directory under
 * `core/` is named for a SPEC AREA — dom, html, css, fetch, canvas, paint, geometry, fonts — and there is no
 * standard that defines how a pixel gets its value. HTML §4.12.5.1.22 "Drawing model" step 1 is "Render the
 * shape or image onto an infinite transparent black bitmap, creating image A, as described in the previous
 * sections", and the previous sections describe WHAT is rendered and never HOW: §4.12.5.1 "The 2D rendering
 * context" mentions the subject exactly once, to say that "Anti-aliasing can similarly be implemented using
 * oversampling with bitmaps of a higher resolution than the final image on the display" — a "can", in a
 * passage about the coordinate space. So this belongs to core/canvas/ no more than to core/paint/: put it in
 * the first and the display-list road inherits §4.12.5's vocabulary for ink that has no canvas in it; put it
 * in the second and canvas inherits CSS 2.1 §E.2's. Blink's name for the layer that has neither is
 * `platform/graphics` and Gecko's is `gfx`; this tree already maps Blink's `platform/fonts` onto
 * `core/fonts`, so the same mapping lands the established name here.
 *
 * THE SEGMENT VOCABULARY IS core/canvas/canvas_path.h's AND IS NOT RESTATED. That header says of its opcodes
 * that "A kind per method would be several spellings of one segment for whatever reads the stream to paint
 * it" — and this is that reader, so a second enum here with the same six values would be the second spelling
 * its own paragraph forbids, free to disagree about an arity the day one of them gains a kind. The arity
 * table is likewise READ (`canvas_path_op_width`) rather than copied. The dependency points from graphics to
 * canvas, which is the wrong way round for a layer and is a consequence of where the vocabulary lives today
 * rather than of anything here: FOR THE CANVAS LANE — the natural home of `CanvasPathOp` and
 * `canvas_path_op_width` is this directory, and moving them changes nothing but two includes.
 *
 * A `RasterPath` IS THE SAME STREAM IN C, AND THAT IS THE LOAD-BEARING INTERFACE DECISION. core/canvas/
 * canvas_path.h holds its stream in a JS Array for a reason that is entirely about FORKING — "a flow MUTATES
 * a path ... so the list is per-flow state that must fork, park to the cold tier and resume" — and none of
 * that reasoning reaches a fill. A fill is a pure function of a path and a surface: it mutates no page state,
 * it is not observable half-done, and the flow that calls it is the flow that consumes its pixels. Three
 * things settle the representation, and the first is the one a reader should check rather than take:
 *   - The display-list consumer has NO JS PATH AT ALL. core/paint/display_list.h's marks carry `CssPx`
 *     rectangles and four border sides and the string `JSValue` does not occur in it, and its own header says
 *     "NOTHING HERE FORKS AND NOTHING HERE MINTS A CONCOLIC". A fill that required a JS Array would make the
 *     display-list road mint one to describe a rectangle, in a component whose whole statement is that it
 *     does not reach a realm.
 *   - A fill must run where there is no `JSContext`. `raster_selftest` in engine/host/test_forced.c takes
 *     none, which is the property that makes the rasterizer exercisable before any rendering context exists.
 *   - `JS_GetPropertyUint32` in the inner loop of the pixel path is a property lookup per coordinate.
 * The reverse coupling is one loop and it belongs to the CANVAS side, not here: §4.12.5.1.6's own note that
 * "the points passed to the methods, and the resulting lines added to current default path by these methods,
 * must be transformed according to the current transformation matrix" makes a rendering context's `fill()`
 * apply its CTM as it copies, so the copy and the transform are the same walk and splitting them would build
 * an untransformed intermediate nobody wants. FOR THE CANVAS LANE — that adapter is yours, and the one thing
 * it may not do elementwise is `CANVAS_PATH_OP_ARC`, for the reason canvas_path.h already gives about
 * `addPath`: "an ARC is transformed by its PARAMETERS and not by its points".
 *
 * EVERY COORDINATE HERE IS A DEVICE PIXEL AND NOTHING HERE APPLIES A MATRIX, which is canvas_path.h's own
 * split one layer down: it stores what the page passed and leaves the CTM to its includer, and this stores
 * what its caller resolved and leaves the CTM to the same place. There is exactly one transform in the road
 * and it is at the rendering context.
 *
 * A NON-FINITE COORDINATE IS A DEFINED NO-OP AND NEVER AN ASSERT. §4.12.5.1.6 answers it for every one of its
 * ten operations — "If any of the arguments are infinite or NaN, then return" — and a caller's number is
 * INPUT whatever layer it arrived through, so a `DCHECK` on one would hand whoever supplied it an abort
 * switch for the engine. The builders below refuse it exactly as §4.12.5.1.6 does, and the refusal is what
 * buys the invariant everything downstream may assert: every coordinate in a built `RasterPath` is finite. */
#ifndef ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTER_PATH_H
#define ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTER_PATH_H

#include <stdbool.h>
#include <stddef.h>

#include "core/canvas/canvas_path.h"   /* `CanvasPathOp` and `canvas_path_op_width` — ONE segment vocabulary */

/* THE STREAM, AND THE HEADER canvas_path.h KEEPS FOR THE SAME REASON. The five header slots are FIELDS here
   rather than leading elements because nothing about this array has to fork: the array is a C allocation and
   the header is not part of the thing a COW delta would capture, so the one argument for putting them in the
   stream is gone. `nsub` is the derived `need new subpath` flag exactly as CANVAS_PATH_NSUB is — it is TRUE
   that a path needs a new subpath exactly while it has none, and storing the flag as well would be a second
   copy of one fact free to disagree.
   THE OPCODE IS STORED AS A `double` SO THAT THE TWO STREAMS ARE ONE STREAM. `canvas_path_op_width` counts
   the opcode, so an adapter walking the JS array and this stream reads the same widths off the same table
   and cannot get an arity right for one and wrong for the other. */
typedef struct {
    double *v;
    size_t  n;
    size_t  cap;
    double  cur_x, cur_y;      /* "the last point in the subpath" */
    double  start_x, start_y;  /* the first point of the LAST subpath */
    size_t  nsub;              /* the number of subpaths; `need new subpath` is nsub == 0 */
} RasterPath;

void raster_path_init(RasterPath *p);
void raster_path_free(RasterPath *p);

/* §4.12.5.1.6's operations over that stream. Each is `void` because each of them is total: the only outcome
   any of them has other than appending is §4.12.5.1.6's own silent return on a non-finite argument, and a
   caller can do nothing with that but continue. The two that §4.12.5.1.6 makes THROW — a negative radius on
   `arcTo` and on `ellipse` — throw in the rendering context, which is where a DOMException can exist; by the
   time a radius reaches here the context has already refused it, which is why `raster_path_ellipse` treats a
   negative radius as the same silent return and not as an error it has no way to report. */
void raster_path_move_to(RasterPath *p, double x, double y);
void raster_path_line_to(RasterPath *p, double x, double y);
void raster_path_quadratic_curve_to(RasterPath *p, double cpx, double cpy, double x, double y);
void raster_path_bezier_curve_to(RasterPath *p, double c1x, double c1y, double c2x, double c2y,
                                 double x, double y);
/* §4.12.5.1.6's ellipse method steps, INCLUDING its step 3 — "If canvasPath's path has any subpaths, then add
   a straight line from the last point in the subpath to the start point of the arc" — so an ARC op in this
   stream is always preceded by a MOVE or a LINE that stands at its start point, exactly as it is in the JS
   one. The flattener relies on that and asserts it. */
void raster_path_ellipse(RasterPath *p, double cx, double cy, double radius_x, double radius_y,
                         double rotation, double start_angle, double end_angle, bool counterclockwise);
void raster_path_close_path(RasterPath *p);
/* §4.12.5.1.6's rect(), in its own four steps: a closed four-point subpath and then a one-point subpath at
   (x, y). It is MOVE + three LINEs + CLOSE + MOVE and not a kind of its own, which is canvas_path.c's
   spelling and is copied here rather than simplified so the two streams stay the same stream. */
void raster_path_rect(RasterPath *p, double x, double y, double w, double h);

/* ONE FLATTENED SEGMENT, in device pixels. A horizontal edge is KEPT rather than dropped here: the fill
   ignores it (it crosses no scanline), and dropping it would make this list stop being a closed chain, which
   is the property the fill's own per-row residual check rests on. */
typedef struct { double x0, y0, x1, y1; } RasterEdge;

typedef struct { RasterEdge *v; size_t n, cap; } RasterEdges;

void raster_edges_init(RasterEdges *e);
void raster_edges_free(RasterEdges *e);

/* THE FLATTENING TOLERANCE IS A POLICY INPUT AND NOT A BOUND, and the distinction is the one CLAUDE.md's
   §NO BOUNDS draws by hand: a bound decides that work will not happen, and this decides only how finely a
   curve is APPROXIMATED. Every subpath is flattened, every row is scanned and no span is dropped at any
   value of it. It is a parameter rather than a constant because nothing in HTML §4.12.5 states a value —
   §4.12.5.1 reaches the subject only to say anti-aliasing "can ... be implemented using oversampling" — so
   the number is this user agent's and a caller that wants a different trade is entitled to make it.
   THE DEFAULT IS 0.1 DEVICE PIXELS, which bounds the chord's deviation from the true curve at a tenth of a
   pixel and therefore any one pixel's coverage error at the same tenth — about 26 of an 8-bit surface's 255
   levels in the worst case, and far less for the curvature a real path carries. */
#define RASTER_FLATTEN_TOLERANCE_PX 0.1

/* Flatten `p` into `out`, APPENDING — `out` is not cleared, so a caller may flatten several paths into one
   edge list and fill them as one shape, which is what a rendering context's "fill all the subpaths of the
   intended path" needs when a `Path2D` is added to a default path.
   OPEN SUBPATHS ARE IMPLICITLY CLOSED, which is HTML §4.12.5.1.13 "Drawing paths to the canvas"' own
   requirement and not an optimisation:
   "Open subpaths must be implicitly closed when being filled (without affecting the actual subpaths)". The
   path is `const` here, which is that parenthesis expressed in the type.
   THE RESULT IS A CLOSED CHAIN PER SUBPATH, and that is the contract the fill's arithmetic rests on: within
   any horizontal band, the signed vertical extents of a closed chain sum to zero, so a row's accumulation
   must return to zero and the fill asserts that it does. */
void raster_path_flatten(const RasterPath *p, double tolerance_px, RasterEdges *out);

#endif /* ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTER_PATH_H */
