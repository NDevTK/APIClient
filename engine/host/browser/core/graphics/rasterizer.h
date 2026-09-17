/* THE SCANLINE FILL — an edge list and a fill rule in, runs of coverage out. The one place in this engine
 * where geometry becomes pixels, and the reason core/graphics exists at all (see raster_path.h).
 *
 * ONE ACCUMULATION, TWO FILL RULES, AND THAT IS WHY BOTH ARE HERE RATHER THAN IN TWO FUNCTIONS.
 * HTML §4.12.5.1 "The 2D rendering context" defines the two in one paragraph and in terms of ONE quantity — the
 * number of crossings of a half-infinite line: "The \"nonzero\" value indicates the nonzero winding rule,
 * wherein a point is considered to be outside a shape if the number of times a half-infinite straight line
 * drawn from that point crosses the shape's path going in one direction is equal to the number of times it
 * crosses the path going in the other direction", and "The \"evenodd\" value indicates the even-odd rule,
 * wherein a point is considered to be outside a shape if the number of times a half-infinite straight line
 * drawn from that point crosses the shape's path is even." Both read the SAME crossing count and differ only
 * in how they answer from it, so this computes that count once per pixel — as a real number, because a pixel
 * is an area and the count over an area is an average — and FOLDS it two ways. Two implementations would be
 * two answers to one question, free to disagree about the pixel where it matters most.
 *
 * THE REQUIREMENT IS DETERMINISM AND NOT AGREEMENT WITH ANY PARTICULAR ENGINE'S PIXELS, and that is a
 * property of what the standard says rather than a concession. HTML states no coverage rule anywhere: its
 * only mention of the subject is §4.12.5.1's "Anti-aliasing can similarly be implemented using oversampling
 * with bitmaps of a higher resolution than the final image on the display" — a "can", non-normatively, in a
 * passage about the coordinate space — and §4.12.5.1.22 "Drawing model" step 1 says only "Render the shape or
 * image onto an infinite transparent black bitmap, creating image A, as described in the previous sections".
 * So no oracle outside this engine can decide a pixel here. The oracle that CAN is the reftest: this document
 * must render identically to that one, a comparison inside one engine. Everything below that looks like a
 * choice is made for that: the segment count is a closed form rather than an adaptive recursion, the
 * accumulation is over the edges IN PATH ORDER (floating-point addition is not associative, so an edge order
 * that depended on a sort would make the bytes depend on the sort), and the two runs of one build agree.
 *
 * COVERAGE IS ANALYTIC AND NOT SAMPLED, which is the same decision one level down: a supersampled coverage is
 * a function of the sample grid's phase, so two renderings of one shape at two offsets disagree by the
 * grid rather than by the shape. What this computes is the exact area integral of the crossing count over
 * each pixel, for the one-edge-per-pixel case that dominates a real path, with the areas of several edges in
 * one pixel added — which is what every production rasterizer does and is where the word "analytic" stops
 * being exact. The derivation is at `raster_mean_below` in rasterizer.c.
 *
 * A SPAN IS A RUN OF ONE COVERAGE, AND IT IS THE SEAM BECAUSE THE FILL HAS MORE THAN ONE CONSUMER. The
 * interior of any shape is coverage 1 over a run of pixels, so a per-pixel callback would be a call per
 * interior pixel to say the same thing; and a span count is a number the fill COMPUTES, which is what makes
 * it something a fixture can hold to an answer. Two sinks exist at this landing — core/graphics/
 * raster_surface.h's compositing one and engine/host/test_forced.c's counting one — so the indirection has
 * readers rather than being a parameter with one caller passing NULL.
 * ZERO-COVERAGE RUNS ARE NOT EMITTED, which is why the fill also returns a count: a consumer cannot recover
 * how much of the row it was never told about, and `raster_fill`'s own per-row assert is what holds the two
 * halves to the row's width. */
#ifndef ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTERIZER_H
#define ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTERIZER_H

#include <stddef.h>

#include "core/graphics/raster_path.h"

/* HTML §4.12.5.1's `CanvasFillRule`, as a C vocabulary. The two names are that enumeration's own and the
   order carries no meaning; nothing here maps a string, because a string is an IDL conversion and belongs
   where the IDL is. */
typedef enum {
    RASTER_FILL_NONZERO = 0,
    RASTER_FILL_EVENODD
} RasterFillRule;

/* ONE RUN OF ONE COVERAGE IN ONE ROW. `x` is the first pixel column, `len` is at least 1, `coverage` is in
   [0, 1] and is never 0 — a run of zero coverage is ink nothing downstream could distinguish from its
   absence, which is the rule core/paint/display_list.h already applies to an alpha of zero. */
typedef void (*RasterSpanFn)(void *user, int y, int x, int len, double coverage);

/* Fill `edges` into a `width` x `height` device region under `rule`, calling `sink` for every run of nonzero
   coverage, and return the number of runs. Nothing is allocated that outlives the call.
   THE REGION IS THE FILL'S AND NOT A SURFACE'S, so a caller may fill into a coverage mask, a counter or a
   bitmap without this component knowing which. Pixels outside it are never reported, which is
   §4.12.5.1.22 "Drawing model"'s own last step — "When compositing onto the output bitmap, pixels that would
   fall outside of the output bitmap must be discarded" — performed where the pixels are decided rather than
   where they are stored.
   `edges` MUST BE A CLOSED CHAIN PER SUBPATH, which is what `raster_path_flatten` produces and what
   HTML §4.12.5.1.13 "Drawing paths to the canvas"' "Open subpaths must be implicitly closed when being
   filled" requires of a fill. The
   arithmetic rests on it: within any horizontal band the signed vertical extents of a closed chain sum to
   zero, so every row's accumulation must return to zero, and this asserts per row that it does. */
size_t raster_fill(const RasterEdges *edges, RasterFillRule rule, int width, int height,
                   RasterSpanFn sink, void *user);

#endif /* ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTERIZER_H */
