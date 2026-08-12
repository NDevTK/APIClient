/* HTML §7.5.10 DESTROYING DOCUMENTS, and §7.3.1's destroy-a-child-navigable that reaches it.
 *
 * WHAT WAS HERE BEFORE WAS ONE BYTE. Removing an `<iframe>` set `closed = 1` on its WindowProxy and returned:
 * the child's Document, its Window and its realm stayed exactly as they were, its queued tasks stayed on the
 * queue and would still run, its descendants were never told, and nothing anywhere recorded that a destruction
 * had happened. A page could not tell that from a navigable that had merely been closed, and neither could the
 * engine — which is the tell that it was not an implementation of anything. §7.5.10 is eleven steps and
 * §7.5.10's descendant form is six more, and every one of them is observable.
 *
 * IT IS A JOB, AND THE SPEC SAYS SO TWICE. "Destroy a document and its descendants" runs IN PARALLEL, queues a
 * global task per child navigable, and WAITS until the number destroyed equals the child count; "destroy a
 * document" asserts it is running as part of a queued task. So this is not a call the removing steps can make
 * — it is work on the one frontier, preemptible and parkable like every other job, which is also the only
 * shape in which the wait is expressible at all. A synchronous version would have to drive its children to
 * completion, which is the thing this engine does not do at any depth.
 *
 * NUMBER-DESTROYED IS A FACT ABOUT EACH CHILD, NOT A SHARED COUNTER. The spec writes step 4 as a counter the
 * child's task increments and step 5 as a wait on it, which is one mutable integer shared by N tasks — and a
 * shared integer is exactly what cannot exist here, because two forked arms destroying the same subtree would
 * be counting into each other's timeline. The equivalent that survives forking is to ask each child whether it
 * has been destroyed, which is state the child's own navigable already has to hold (§7.2.5's `closed` getter
 * reads it), captured per flow by the WindowProxy record's COW capture. The wait is then a re-read, which is
 * what a parked stage is for.
 *
 * WHAT IT DOES NOT DO IS FREE THE REALM, and that is the NEXT subproblem rather than a gap in this one. The
 * spec's eleven steps are all state changes a page can observe; reclaiming the JSContext is the user agent's
 * own business and is blocked on a primitive that does not exist yet — a flow parked inside the destroyed
 * document must be abandoned before its heap can go, and a flow does not currently record which realm it is
 * in. navigable.c's allocation CHECK names that ceiling where a reader stands when it bites. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_DOCUMENT_LIFECYCLE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_DOCUMENT_LIFECYCLE_H

#include "quickjs.h"

/* §7.3.1's DESTROY A CHILD NAVIGABLE, steps 4-5 — the container has already dropped its content navigable
   (that is step 3, and it belongs to the element, which is where the slot is). What is left is the part that
   is about the DOCUMENT: destroy it and everything below it, as a job.
   `proxy` is the child navigable's WindowProxy. Calling it for a navigable whose active document has already
   been destroyed is a no-op, which is §7.5.10's own answer for a document that is not fully active. */
void document_lifecycle_destroy_child(JSContext *ctx, JSValueConst proxy);

#endif
