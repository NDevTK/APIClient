/* HTML §4.12.5 "The canvas element" — `getContext`, the element's BITMAP, and the canvas context mode.
 *
 * WHAT IS HERE IS THE ELEMENT'S HALF AND NOT THE CONTEXT'S. §4.12.5 gives the canvas element three things of
 * its own: a bitmap, a canvas context mode, and the `getContext` table that moves between modes. The rendering
 * context is a different interface in a different section (§4.12.5.1 "The 2D rendering context") and lives in
 * core/canvas/canvas_rendering_context_2d.h; the two meet at the 2D context creation algorithm, which this
 * file CALLS and that one OWNS. Splitting them the other way — a context that reached into the element's
 * storage — would put §4.12.5's bitmap behind whichever context landed first, which is the mistake
 * core/canvas/canvas_path.h already names for the `CanvasPath` mixin.
 *
 * THE BITMAP'S PIXELS ARE A JS TYPED ARRAY, AND THAT IS THE LOAD-BEARING DECISION IN THIS FILE.
 * CLAUDE.md §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE settles it for exactly this shape: a canvas bitmap is
 * state a FLOW WRITES — one arm puts pixels the other never does — so it must fork per flow, park to the cold
 * tier and resume. Held as a `malloc`'d byte run behind a pointer, a context switch would revert the POINTER
 * and leave the bytes reachable from nothing, which is a leak the runtime's own GC walk cannot see, so no gate
 * would report it; held as a `Uint8ClampedArray` in an internal-slot record, every write is an ordinary
 * property write and the COW delta already captures it. It is the same choice core/html/media_element.c makes
 * for a media element's state and for the same reason.
 * AND IT COSTS NOTHING AT THE PIXEL LAYER, because HTML §4.12.5.7 "Premultiplied alpha and the 2D rendering
 * context" already forced the representation: core/graphics/raster_surface.h stores NON-PREMULTIPLIED RGBA8
 * for a surface a page reads back, which is byte-for-byte what §8.11.1's `ImageData` holds — so the bitmap, an
 * ImageData and a `RasterSurface` are one layout and a copy between any two of them is a copy.
 *
 * THE BITMAP IS MATERIALIZED AT CONTEXT CREATION AND NOT AT PARSE, and that is an observation about §4.12.5
 * rather than a saving. A canvas whose context mode is NONE has a bitmap that is "transparent black with a
 * natural width equal to the numeric value of the element's width attribute" — so its only observable is its
 * DIMENSIONS, which core/layout/replaced_element.c already derives from the two content attributes without
 * any storage at all. No algorithm can read a mode-none bitmap's BYTES: `getContext` is what gives a page one.
 * What would move this is a member that reads the bytes without a context — `toDataURL`, `toBlob`, or
 * `transferControlToOffscreen` — and each of those is its own landing.
 *
 * ORIGIN-CLEAN IS THE BITMAP'S AND IS TRUE HERE, WHICH IS §4.12.5.1.2's OWN INITIAL VALUE AND NOT A STUB.
 * "Initially, when one of these bitmaps is created, its origin-clean flag must be set to true", and the only
 * algorithms that clear it are the image-drawing ones (§4.12.5.1.15) and `drawImage`'s pattern sibling, none of
 * which this build has. So `getImageData`'s SecurityError arm is unreachable because nothing can make it
 * reachable, which is a state this engine has made impossible rather than one it has not checked — and the
 * flag is stored rather than derived so that the day an image source lands there is somewhere to clear. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_CANVAS_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_CANVAS_ELEMENT_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* §4.12.5's canvas context mode, in the column order of `getContext`'s own table. `none` is 0 because it is
   the mode a canvas is in "when its canvas context mode is none", which is every canvas until getContext. */
typedef enum {
    CANVAS_MODE_NONE = 0,
    CANVAS_MODE_2D,
    CANVAS_MODE_BITMAPRENDERER,
    CANVAS_MODE_WEBGL,
    CANVAS_MODE_WEBGL2,
    CANVAS_MODE_WEBGPU,
    CANVAS_MODE_PLACEHOLDER
} CanvasContextMode;

/* Declared once per AGENT: the `getContext` declaration and the element state slot key. */
void html_canvas_element_declare(JSContext *ctx);

/* §4.12.5's members on HTMLCanvasElement.prototype, handed the prototype by core/html/html_element.c, which
   owns the element-interface table — see html_dialog.h for why the prototype is passed rather than reached
   for. */
void html_canvas_install(JSContext *ctx, JSValueConst canvas_proto);

void html_canvas_element_free(JSRuntime *rt);

/* ---- the bitmap, for the rendering context that shares it ------------------------------------------------- */

/* THE ELEMENT'S BITMAP, RESOLVED THROUGH THE ELEMENT AND NEVER CACHED BY A CONTEXT. The 2D context creation
   algorithm's step 4 is "Set context's output bitmap to the same bitmap as target's bitmap (so that they are
   shared)", and a context that copied a pointer would hold a SECOND name for one thing, free to go stale the
   moment `set bitmap dimensions` resizes it. The context holds its `canvas` back-reference — which step 3
   initializes and which the IDL exposes anyway — and asks through it, so there is one name for the bitmap and
   sharing is a property of the lookup rather than of anybody's bookkeeping. */
typedef struct {
    uint8_t *rgba;        /* 4 * width * height, non-premultiplied, row-major; NULL for a zero-area bitmap */
    uint32_t width;
    uint32_t height;
    bool     origin_clean;
} CanvasBitmap;

/* Resolve `canvas`'s bitmap. False when the element has none — which is every canvas in context mode none, and
   is a STATE rather than a failure. Never throws. */
bool canvas_bitmap_get(JSContext *ctx, JSValueConst canvas, CanvasBitmap *out);

/* §4.12.5.1's "set bitmap dimensions to width and height", steps 2 to 5 — step 1 is *reset the rendering context
   to its default state*, which the CONTEXT owns and calls this from, so that the one algorithm is not split
   across two files in an order neither of them states. Allocates `width * height * 4` zero bytes (transparent
   black) and writes the two content attributes back to match. Returns -1 with an exception pending. */
int canvas_bitmap_set_dimensions(JSContext *ctx, JSValueConst canvas, uint32_t width, uint32_t height);

/* Clear every byte of the bitmap to transparent black, or — when `opaque` — to opaque black, which is
   §4.12.5.1.2's "the bitmap of such a context starts off as opaque black instead of transparent black" for a
   context whose `alpha` is false. */
void canvas_bitmap_clear(JSContext *ctx, JSValueConst canvas, bool opaque);

/* The same, with the creation algorithm's step 5 argument — "the numeric values of target's width and height
   content attributes" — read here because §4.12.5's 300/150 defaults are this component's. */
int canvas_bitmap_set_dimensions_from_attributes(JSContext *ctx, JSValueConst canvas);

/* The canvas's context mode, and the context object it is in that mode with (JS_NULL in mode none). The second
   is what `getContext`'s "Return the same object as was returned the last time the method was invoked with
   this same first argument" cell answers with; the caller owns the returned value. */
CanvasContextMode canvas_context_mode(JSContext *ctx, JSValueConst canvas);

/* Whether `v` is a `canvas` element — asked of the NODE, which is what §4.12.5 says a canvas element is. */
bool canvas_element_is(JSValueConst v);

#endif /* ENGINE_HOST_BROWSER_CORE_HTML_HTML_CANVAS_ELEMENT_H */
