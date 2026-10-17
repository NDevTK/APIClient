/* A DOCUMENT'S PENDING VISUAL UPDATE — the first clause of HTML §8.1.7.3 "Processing model"'s Unnecessary
 * rendering step, which removes a document only when "doc's node navigable would have no visible effect" and
 * its map of animation frame callbacks is empty. A change to the document's tree since its last update is a
 * visible effect the user agent cannot rule out without rendering, which is the belief this records: DOM
 * §4.2.3's children changed steps, run by insert, remove and replace data, raise it for a realm's active
 * document, and update the rendering's step 22 takes it for each document it updates.
 *
 * Per-flow state, held as a property of a baseline record exactly as page_reveal.h's `has been revealed` is:
 * one arm of a fork may have mutated its document while its sibling has not.
 *
 * Named residual. Not covered: DOM §4.9's attribute changes (a class or style attribute write) and CSSOM edits
 * of a style sheet, which are visible effects too. Next diff builds: the same raise from the attribute change
 * steps and from a style sheet's rule list mutation. How its absence would show: a page whose only change after
 * its first frame is an attribute or a rule gets no further rendering opportunity, so its observers and paint
 * do not run. */
#ifndef ENGINE_HOST_BROWSER_CORE_RENDERING_VISUAL_UPDATE_H
#define ENGINE_HOST_BROWSER_CORE_RENDERING_VISUAL_UPDATE_H

#include <stdbool.h>

#include "quickjs.h"

/* The agent's declaration — core/platform.c's row: the realm-value slot, the record's key and the children
   changed steps registration. */
void visual_update_init(JSContext *ctx);
/* Each realm's record, built with the realm so it belongs to the pre-boot baseline. */
void visual_update_install_proto(JSContext *ctx);
/* The agent's half, undone — core/platform.c's release column. */
void visual_update_free(JSRuntime *rt);

/* Has this document's tree changed since update the rendering last updated it? `docctx` is the document's
   own realm. */
bool visual_update_pending(JSContext *docctx);
/* Update the rendering step 22 has updated this document: the pending update is taken. */
void visual_update_taken(JSContext *docctx);

#endif
