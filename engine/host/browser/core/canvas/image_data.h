/* HTML §8.11.1 "The ImageData interface" — a rectangular bitmap a page constructs directly, holding its pixels
 * in a typed array the page then reads and writes.
 *
 * IT IS A COMPLETE COMPONENT WITH NO RENDERING CONTEXT ANYWHERE, on the same terms Path2D is beside it, and
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

#include "quickjs.h"

/* Declared once per AGENT: the class, the constructor and the five attributes. REGISTERS the per-realm install
   below, which is the whole of what a realm owes this component — see core/realm.h for why that list may not
   be hand-copied into each host. */
void image_data_init(JSContext *ctx);

/* §8.11.1's interface prototype object for ONE realm, its Web IDL §3.7.1 interface object, and §3.8's property
   reference for the one name. `ImageData` is `[Exposed=(Window,Worker)]`, so the name is placed from the realm
   intrinsic rather than from a per-document column, which a worker realm never reaches. */
void image_data_install_realm(JSContext *ctx);
void image_data_free(void);

/* §8.11.1's brand, for the one caller outside this file that needs it: Web IDL §3.6 step 12's platform-object
   clause, asked of a value at a declared `ImageData` position. It is a predicate and not a class comparison
   for core/idl_args.h's stated reason — a component's own test is what "implements the interface" means. */
bool image_data_is(JSValueConst v);

#endif /* ENGINE_HOST_BROWSER_CORE_CANVAS_IMAGE_DATA_H */
