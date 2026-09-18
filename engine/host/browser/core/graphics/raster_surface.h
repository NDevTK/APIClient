/* THE DESTINATION — an RGBA bitmap and the one operation that puts a fill's coverage into it.
 *
 * IT IS A SEPARATE COMPONENT FROM THE FILL BECAUSE IT HAS CONSUMERS THE FILL DOES NOT. core/paint/
 * display_list.h's `DISPLAY_MARK_FILL_RECT` and `DISPLAY_MARK_FILL_CANVAS` are areas with no path in them at
 * all, so the display-list road reaches a surface through a rectangle it can state directly; and a fill has
 * sinks that are not surfaces, which is what `RasterSpanFn` exists for. Splitting them is also what makes
 * each one exercisable alone: a fill can be held to a span count with no bitmap under it, and a surface can
 * be held to a composited byte with no path over it.
 *
 * THE COMPONENTS ARE NON-PREMULTIPLIED, AND THE REASON IS A STANDARD RATHER THAN A PREFERENCE. HTML §4.12.5.7
 * "Premultiplied alpha and the 2D rendering context" defines the two representations by what the components
 * MEAN — "Under non-premultiplied alpha, the red, green, and blue components of a pixel represent that
 * pixel's color, and its alpha component represents that pixel's opacity" — and its own table settles the
 * choice for any surface a page reads back component by component: `rgba(255, 127, 0, 0)` and
 * `rgba(0, 127, 255, 0)` are BOTH `0, 0, 0, 0` premultiplied and are distinct non-premultiplied, so eight-bit
 * premultiplied storage is a lossy round trip for exactly the colours a page can set and then read. The
 * arithmetic below pays for that in a division; a surface that composites and is never read back could
 * reasonably choose otherwise, and none exists.
 *
 * THE CLAMP AND THE QUANTIZATION ARE THIS COMPONENT'S BY ITS CONSUMER'S OWN INSTRUCTION. core/css/
 * css_color.h keeps a colour's components unclamped and says why — a colour "can legitimately lie outside the
 * destination's gamut" — and core/paint/display_list.h says where that is resolved: "The clip and the 8-bit
 * quantization belong to whichever surface finally rasterizes". So an out-of-range component here is a
 * LEGITIMATE VALUE a cascade computed and never an invariant to assert on; clamping it is the answer, and a
 * `DCHECK` over it would hand any page that writes `rgb(300 -5 999)` an abort switch. What this may assert is
 * what the FILL handed it, because that is this codebase's own arithmetic.
 *
 * ROUNDING IS `floor(v * 255 + 0.5)` AND IS STATED BECAUSE IT IS OBSERVABLE. Two renderings of one document
 * must agree byte for byte (rasterizer.h), and a rounding mode that differed between two builds would break
 * that without breaking anything a reader could point at; this one is arithmetic on doubles with no library
 * call and no rounding-mode dependence in it. */
#ifndef ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTER_SURFACE_H
#define ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTER_SURFACE_H

#include <stddef.h>
#include <stdint.h>

#include "core/graphics/rasterizer.h"

/* `px` is four bytes per pixel, R G B A, row-major, `4 * width * height` of them. A zero-area surface holds
   NULL rather than an empty allocation, which is a state rather than an absence: `width` and `height` say
   which, and HTML admits a canvas of either dimension zero. */
typedef struct {
    uint8_t *px;
    int      width;
    int      height;
} RasterSurface;

/* §4.12.5.1.22 "Drawing model"'s own starting bitmap — "Render the shape or image onto an infinite
   transparent black bitmap" — so a new surface is transparent black and not opaque white. */
void raster_surface_init(RasterSurface *s, int width, int height);
void raster_surface_free(RasterSurface *s);

/* ONE SOLID PAINT OVER ONE SURFACE, and the counters that make a fill through it checkable. `r`, `g`, `b` are
   sRGB components and `a` is a straight alpha; all four are a cascade's or a page's numbers and are clamped
   at the quantization rather than asserted. `spans` and `pixels` count what this sink was HANDED, which is
   the half `raster_fill`'s return value cannot state: a fill that emitted runs nobody received and a fill
   that emitted none are the same number at the caller, and two counts that must agree are not. */
typedef struct {
    RasterSurface *surface;
    double         r, g, b, a;
    size_t         spans;
    size_t         pixels;
} RasterPaint;

void raster_paint_init(RasterPaint *p, RasterSurface *s, double r, double g, double b, double a);

/* A `RasterSpanFn`. Source-over, which is HTML §4.12.5.1.17 "Compositing"'s default `globalCompositeOperation`
   and the only operator this component has; a second operator is a second landing and there is nothing here
   that pretends otherwise. */
void raster_paint_span(void *user, int y, int x, int len, double coverage);

/* One pixel's four components, for a fixture or a comparison. Out-of-range coordinates are this codebase's
   own indexing error and are asserted. */
void raster_surface_get(const RasterSurface *s, int x, int y, uint8_t rgba[4]);

/* HOW MANY BYTES `px` IS, WHICH IS THE ONE THING A CONSUMER OF THE WHOLE BITMAP NEEDS AND THE ONE THING THIS
   COMPONENT DID NOT STATE. A reader of a single pixel is served above and a reader of a HASH below; a reader
   of the RUN had only the three public fields and the arithmetic, so every such reader is a second copy of
   `raster_surface_init`'s allocation size — and the second copy is the one with no overflow refusal in front
   of it, safe only because a surface that exists was allocated by a line in another function. That is a
   property held by convention, so this is the assert that makes it hold by construction, and
   `raster_surface_checksum` reads its loop bound from here rather than deriving it again.

   THE ZERO IS TWO-SIDED AGAINST THE ALLOCATION, and that is what this entry buys beyond one multiply. The
   struct above states that a zero-area surface holds NULL rather than an empty allocation; this asserts it
   from both ends, so an extent of zero and an absent `px` are one fact rather than two that may drift. The
   NULL test `raster_surface_checksum` used to open with is DELETED by that assert rather than kept beside it:
   a bound of zero runs no loop, so there is nothing left for the test to protect.

   IT IS A LENGTH AND NOT A TERMINATOR, WHICH IS THE WHOLE POINT FOR ANY TRANSPORT. Non-premultiplied RGBA8
   contains 0x00 by construction — every fully transparent pixel is four of them, and every black channel is
   one — so a byte run of pixels has no spelling as a C string and its extent can be carried only as a number
   beside it. A consumer that recovers a length with `strlen` ends at the first transparent pixel.

   NAMED RESIDUAL — WHAT IS NOT COVERED: this states the extent of a surface's pixels and NOT a value that
   owns a copy of them. A surface is a local of whatever composed the display list and dies with it, so a
   caller that must hand the bytes to something outliving that pass has nothing here to hold them in; and no
   entry of the production ABI (`engine/host/qjs_abi.h`) carries bytes OUTWARD at all — every returning entry
   there answers a scalar or a NUL-terminated `const char *`.
   WHAT THE NEXT DIFF BUILDS: a record owning a copied pixel run with this length beside it, and the paired
   ABI entries that hand a host the pointer and that length as two first-class answers rather than one
   pointer a reader measures for itself. Both halves land together with the renderer binding that carries
   them, because the binding reads a C return as a number or as a NUL-terminated string and has no third arm.
   HOW ITS ABSENCE WOULD SHOW: a consumer outside this component that wants a rendered document's pixels can
   ask this codebase for their EXTENT and for no byte of them, so nothing anywhere renders an image of a
   document; the observation is that no host artifact and no popup surface presents one on any run.
   WHO MAY RETIRE IT: the next diff is a cross-boundary landing whose engine half is live only after the
   renderer artifact is BUILT AND INSTALLED, and in this project only the COORDINATOR builds. A lane may write
   that diff and may not make it live, so this residual is a REQUEST to that actor and not work anybody is
   merely waiting on. */
size_t raster_surface_bytes(const RasterSurface *s);

/* A CHECKSUM OF THE WHOLE BITMAP, which is the one number a reftest oracle is actually made of: two documents
   render identically or they do not, and a comparison inside one engine needs no agreement with any other.
   FNV-1a over the bytes in row-major order — an established construction rather than a coined one, and one
   whose value is a function of the bytes alone, so it means the same thing in the native host and in the wasm
   one. A zero-area surface answers the empty FNV-1a basis, which is a value and not an absence. */
uint64_t raster_surface_checksum(const RasterSurface *s);

#endif /* ENGINE_HOST_BROWSER_CORE_GRAPHICS_RASTER_SURFACE_H */
