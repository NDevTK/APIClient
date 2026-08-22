/* REVEAL A DOCUMENT — HTML §7.4.6.3, and the `PageRevealEvent` it fires.
 *
 * This is "update the rendering" STEP 6, and it is the first step of that algorithm that runs the page's own
 * code. It is here rather than in rendering.c for the reason every other component in this tree is its own
 * file: it is one algorithm with one piece of per-document state ("has been revealed") and one interface, and
 * the rendering loop's job is to CALL it in the right place among twenty-two others.
 *
 * WHY IT IS NOT OPTIONAL. Every document is revealed exactly once, at its first rendering opportunity, whether
 * or not it animates — so it is what makes a document with no `requestAnimationFrame` still HAVE a rendering
 * opportunity, and therefore what makes the rendering loop something that runs on a real page rather than only
 * on an animating one. A rendering loop that only ever woke for animation callbacks would be a loop the whole
 * of §8.1.7.3's ordering could never be measured against.
 *
 * `has been revealed` IS PER-FLOW STATE, held as a property of a baseline object exactly as §8.9's map is: one
 * arm of a fork may have reached its first frame while its sibling has not, and a parked flow resumes owed the
 * reveal it was owed. A C-side boolean would be one document's answer for every flow at once.
 *
 * THE VIEW TRANSITION IS NULL, AND THAT IS A COMPUTED VALUE RATHER THAN A SHRUG. Step 3 reads doc's ACTIVE VIEW
 * TRANSITION, and a document has one only if something started one — `document.startViewTransition` and the
 * whole of CSS View Transitions are absent from this engine, so nothing can have. The absence is ASSERTED
 * rather than assumed: the moment ViewTransition is installed, this component's DCHECK fires at the step that
 * must then read a real one. */
#ifndef ENGINE_HOST_BROWSER_CORE_RENDERING_PAGE_REVEAL_H
#define ENGINE_HOST_BROWSER_CORE_RENDERING_PAGE_REVEAL_H

#include <stdbool.h>

#include "quickjs.h"

void page_reveal_init(JSContext *ctx);
/* §3.7's per-realm half: this realm's PageRevealEvent.prototype and this document's "has been revealed"
   record. Declared into realm.h's one list, so every realm goes through it. */
void page_reveal_install_proto(JSContext *ctx);
/* The interface OBJECT on the Window. */
void page_reveal_install(JSContext *ctx, JSValueConst global);
void page_reveal_free(JSContext *ctx);

/* §7.4.6.3 step 1, asked from the other side: is this document still UNREVEALED? The rendering loop reads it
   as part of deciding whether the navigable might have a rendering opportunity at all — a document that has
   never been shown has a visible effect pending by definition, which is exactly what §8.1.7.3 step 4's
   "no visible effect" test is about. */
bool page_reveal_pending(JSContext *ctx);

/* §7.4.6.3 steps 1-4a: return without an event if the document has already been revealed; otherwise set
   `has been revealed`, read the active view transition, and MINT the PageRevealEvent. Answers the event
   (OWNED) for the caller to dispatch, or JS_UNDEFINED when step 1 returned.
   The FIRE is the caller's because §2.9's dispatch is synchronous and runs the page's listeners: it is a
   request a step machine parks on, not a call this component can make from C. */
JSValue page_reveal_begin(JSContext *ctx);

#endif
