/* A DOCUMENT'S LAST PAINTED FRAME — what HTML §8.1.7.3 update the rendering step 22 ("update the rendering or user
 * interface of doc and its node navigable to reflect the current state") last left for a document: the frame
 * core/paint/document_paint.h paints, which is what the document looks like until its next rendering
 * opportunity.
 *
 * It is state and not a cache. A frame is step 22's output at one moment; re-deriving it from later state would
 * paint a different frame. So it is held per realm in a record the realm builds with itself (the pre-boot
 * baseline), and publishing writes that record, which the running flow's COW delta captures: each world reads the
 * frame its own timeline painted, a fork shares its parent's frame object (immutable once published) rather than
 * copying it, and a flow the cold tier resumes replays its recipe through the same step 22 and publishes the same
 * frame again. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_PAINTED_FRAME_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_PAINTED_FRAME_H

#include "core/paint/document_paint.h"
#include "quickjs.h"

/* Declared once per agent (core/platform.c's `painted_frame` row): the frame object's class and the record's
   slot. The per-realm record is built by painted_frame_install on core/realm.h's one list. */
void painted_frame_init(JSContext *ctx);
void painted_frame_install(JSContext *ctx);
void painted_frame_free(JSRuntime *rt);

/* Step 22 painted `f` for the document of `docctx`'s realm: the record now holds it, and owns it. */
void painted_frame_publish(JSContext *docctx, DocumentPaintFrame *f);

/* The frame step 22 last published for `docctx`'s document in the running world, borrowed until the next
   publish, or NULL when no rendering opportunity of this world has painted it. */
const DocumentPaintFrame *painted_frame_last(JSContext *docctx);
/* The object holding the last frame published in `docctx`'s realm (a new reference), or null. */
JSValue painted_frame_object(JSContext *docctx);
/* The frame a published frame object holds. */
const DocumentPaintFrame *painted_frame_of(JSValueConst v);

#endif /* ENGINE_HOST_BROWSER_CORE_PAINT_PAINTED_FRAME_H */
