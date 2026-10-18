/* A DOCUMENT'S PENDING VISUAL UPDATE — the first clause of HTML §8.1.7.3 "Processing model"'s Unnecessary
 * rendering step, which removes a document only when "doc's node navigable would have no visible effect" and
 * its map of animation frame callbacks is empty. A change to what the document renders since its last update is
 * a visible effect the user agent cannot rule out without rendering, which is the belief this records. It is
 * raised for a realm's active document by every write the rendering reads: DOM §4.2.3's children changed steps
 * (insert, remove and replace data), DOM §4.9's attribute change steps, and every cascade input
 * core/css/css_cascade_pass.h names (a rule, a rule list, a sheet's slots or `disabled` flag, a document's sheet
 * list, a MediaList, the custom element state `:defined` reads). Update the rendering's step 22 takes it for
 * each document it updates.
 *
 * Per-flow state, held as a property of a baseline record exactly as page_reveal.h's `has been revealed` is:
 * one arm of a fork may have mutated its document while its sibling has not. */
#ifndef ENGINE_HOST_BROWSER_CORE_RENDERING_VISUAL_UPDATE_H
#define ENGINE_HOST_BROWSER_CORE_RENDERING_VISUAL_UPDATE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* The agent's declaration — core/platform.c's row: the realm-value slot, the record's key and the children
   changed steps registration. */
void visual_update_init(JSContext *ctx);
/* Each realm's record, built with the realm so it belongs to the pre-boot baseline. */
void visual_update_install_proto(JSContext *ctx);
/* The agent's half, undone — core/platform.c's release column. */
void visual_update_free(JSRuntime *rt);

/* Something `doc` renders was written: raise its pending visual update. Only a realm's active document is
   rendered, so any other document (a DOMParser or createHTMLDocument one) is no-one's, and NULL — the document
   of a style sheet no node owns — is the same answer. */
void visual_update_document_changed(lxb_dom_document_t *doc);

/* Has this document changed since update the rendering last updated it? `docctx` is its own realm. */
bool visual_update_pending(JSContext *docctx);
/* Update the rendering step 22 has updated this document: the pending update is taken. */
void visual_update_taken(JSContext *docctx);

#endif
