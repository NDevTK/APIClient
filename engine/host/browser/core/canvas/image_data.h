/* HTML §8.11.1 "The ImageData interface" — a rectangular bitmap a page constructs directly, holding its pixels
 * in a typed array the page then reads and writes.
 *
 * IT WAS A COMPLETE COMPONENT WITH NO RENDERING CONTEXT ANYWHERE, on the same terms Path2D was beside it, and
 * the discriminator §NO-STUBS names is a question about the STANDARD: which algorithm writes this interface's
 * observables? Every one of `width`, `height`, `data`, `pixelFormat` and `colorSpace` is written by §8.11.1's
 * own *initialize an ImageData object*, which the two constructor algorithms call and which reads nothing
 * outside the arguments it was given. There is no connection to open, no bitmap to sample and no context to
 * paint into — so the interface is completable in one landing, and a `data` that round-trips is what makes it
 * one rather than the shape §NO-STUBS rates worse than the absence.
 *
 * THAT IT IS BUILT AT ALL IS A MEASUREMENT AND NOT A GUESS. engine/absentrank.mjs ranks the platform global
 * names the corpus of real frozen bundles uses that this tree reaches on no global, ordered by what the
 * absence COSTS, and `ImageData` is the top row of its THROWS band — every use unguarded, so each one raises a
 * ReferenceError and ends the flow. There is no `if (window.ImageData)` anywhere in that corpus to flip, which
 * is the hazard §NO-STUBS is actually about; the day a page writes one, absence is the answer a browser
 * without it gives and the argument has to be made again.
 *
 * ITS CONSTRUCTOR IS THE FIRST MEMBER OF THIS PLATFORM WHOSE Web IDL §3.6 DISTINGUISHING ARGUMENT INDEX IS NOT
 * THE POSITION ITS SHORTER OVERLOAD ENTRY ENDS AT, and that is why core/idl_args.c grew a field for it rather
 * than this file growing an `if`. The two entries differ at index 0 and the shorter one ends at index 2, so
 * the entry §3.6 step 12 chooses at index 0 is what decides index 2's TYPE — `optional ImageDataSettings
 * settings = {}` on one and `optional unsigned long sh` on the other. Resolving that here would be the
 * per-body copy of a rule the argument machine exists to have one of.
 *
 * A RECEIVER IS PAGE-SUPPLIED INPUT. Every attribute below brand-checks `this` and throws a Web IDL §3.7.6
 * TypeError when it is not an ImageData — never a DCHECK, which would hand any page an abort switch for the
 * whole engine by writing `Object.getOwnPropertyDescriptor(ImageData.prototype, "data").get.call(null)`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CANVAS_IMAGE_DATA_H
#define ENGINE_HOST_BROWSER_CORE_CANVAS_IMAGE_DATA_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/idl_args.h"   /* IdlDictMember — §8.11.1's ImageDataSettings is declared below, once */

/* Declared once per AGENT: the class, the constructor and the five attributes. REGISTERS the per-realm install
   below, which is the whole of what a realm owes this component — see core/realm.h for why that list may not
   be hand-copied into each host. */
void image_data_init(JSContext *ctx);

/* §8.11.1's interface prototype object for ONE realm, its Web IDL §3.7.1 interface object, and §3.8's property
   reference for the one name. `ImageData` is `[Exposed=(Window,Worker)]`, so the name is placed from the realm
   intrinsic rather than from a per-document column, which a worker realm never reaches. */
void image_data_install_realm(JSContext *ctx);
void image_data_free(void);

/* THE PIXELS, AS A C CONSUMER SEES THEM — the accessor §4.12.5.1.16 "Pixel manipulation" needs and the reason
 * it is HERE rather than a second reach into the class from the rendering context. The five observables above
 * are reached through the interface's own getters, and a getter returns a JSValue a C caller would then have
 * to unwrap, brand-check and bounds-check for itself at every call site; stating the unwrap once is what makes
 * the two directions of *put pixels from an ImageData onto a bitmap* and getImageData's own pixel copy read
 * the same width, the same height and the same buffer.
 *
 * `rgba` IS NON-PREMULTIPLIED, FOUR BYTES PER PIXEL, ROW-MAJOR, and that is the same representation
 * core/graphics/raster_surface.h states for a surface, for the same reason it gives: HTML §4.12.5.7
 * "Premultiplied alpha and the 2D rendering context"'s own table makes `rgba(255, 127, 0, 0)` and
 * `rgba(0, 127, 255, 0)` BOTH `0, 0, 0, 0` premultiplied, so eight-bit premultiplied is a lossy round trip for
 * a bitmap a page reads back. An ImageData the page constructed and a canvas bitmap therefore hold their
 * bytes identically and a copy between them is a copy.
 *
 * IT ANSWERS FALSE RATHER THAN THROWING, AND THE CALLER OWNS THE REFUSAL. There are three ways this fails and
 * they are three different exceptions in three different algorithms — a receiver that is not an ImageData is
 * Web IDL §3.7.6's TypeError, a detached buffer is putImageData's "InvalidStateError", and a "rgba-float16"
 * array is not a byte buffer at all — so a refusal spelled here would be one algorithm's answer given to all
 * of them. The DCHECK-free contract is deliberate for the same reason image_data.h's own paragraph gives: an
 * ImageData is PAGE-SUPPLIED INPUT, and every one of these states is a page's to reach.
 *
 * THREE IS THE WHOLE LIST AND TWO FLAGS PARTITION IT EXACTLY, which is why no refusal-reason field belongs
 * here: `is_image_data` false is the first, `detached` true is the second, and both flags clear is the third.
 * A FOURTH state was reachable and had no bit — an array SHORTER than `4 * width * height` — and it shared the
 * third's spelling, so putImageData refused a page's shrunk buffer under a crash naming float16. It is not a
 * missing enumerator. It is a state HTML §4.12.5.1.16 "Pixel manipulation" has no step for, because Web IDL
 * §3.2.26 "Buffer source types" refuses a resizable or shared buffer at a position declaring neither §3.3.1
 * "[AllowResizable]" nor §3.3.2 "[AllowShared]" — which `ImageDataArray` does not — so the conversion keeps it
 * out and the accessor asserts rather than reporting it. core/idl_args.c's IDL_ULONG_OR_IMAGE_DATA_ARRAY row
 * is where that refusal is made; a fourth bit here would have been the symptom recorded instead. */
typedef struct {
    uint8_t *rgba;      /* 4 * width * height bytes, or NULL when `ok` is false */
    uint32_t width;
    uint32_t height;
    bool     detached;  /* the [[ViewedArrayBuffer]] is detached — putImageData's own "InvalidStateError" */
    bool     is_image_data; /* the value is an ImageData at all — Web IDL §3.7.6's TypeError otherwise */
} ImageDataPixels;

/* Fill `out` from `v`. Returns true only when `out->rgba` is a readable and writable
   `4 * width * height` byte run; `out->is_image_data` and `out->detached` say which refusal a false is. */
bool image_data_pixels(JSContext *ctx, JSValueConst v, ImageDataPixels *out);

/* A new `rgba-unorm8` ImageData of `width` x `height` in `color_space` (an index into the
   `PredefinedColorSpace` list, in the IDL's own order), its pixels transparent black — which is §4.12.5.1.16's
   own "Initialize the image data of newImageData to transparent black" and is what a freshly allocated
   Uint8ClampedArray already is, so nothing here writes zeroes over zeroes.
   THE `ctx` IS THE MEMBER'S OWN AND NEVER A CACHED ONE, for §A-PER-REALM-FACT's reason: the array is minted
   from `ctx`'s Uint8ClampedArray intrinsic, so a child navigable's getImageData must reach this with its own
   realm's context. Returns an exception on allocation failure, which the caller propagates. */
JSValue image_data_new(JSContext *ctx, uint32_t width, uint32_t height, int color_space);

/* The `PredefinedColorSpace` index an ImageData carries, for getImageData's settings chain. -1 when `v` is not
   an ImageData. */
int image_data_color_space(JSValueConst v);

/* §8.11.1's `dictionary ImageDataSettings` AND the `PredefinedColorSpace` value list, DECLARED HERE so that a
 * member which takes one — §4.12.5.1.16's `getImageData` and `createImageData` both do — declares the same
 * dictionary this interface's own constructor declares rather than a second copy of it. Two tables would be
 * free to disagree about a default the day the enumeration gains a value, and Web IDL §3.2.18 Enumeration
 * types numbers a fork's outcomes by the list's ORDER, so a divergence there is not cosmetic.
 * The array is `IMAGE_DATA_SETTINGS_N` long and both outlive every declaration, which is what
 * `idl_method_id_dict` requires of the members it keeps a pointer to. */
#define IMAGE_DATA_SETTINGS_N 2
extern const IdlDictMember IMAGE_DATA_SETTINGS[IMAGE_DATA_SETTINGS_N];
extern const char *const IMAGE_DATA_COLOR_SPACES[];

/* The index of `name` in `PredefinedColorSpace`, or -1 — the one place the enumeration's order is read. */
int image_data_color_space_index(const char *name);


#endif /* ENGINE_HOST_BROWSER_CORE_CANVAS_IMAGE_DATA_H */
