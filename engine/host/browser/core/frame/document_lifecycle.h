/* HTML §7.5.9 UNLOADING DOCUMENTS and §7.5.10 DESTROYING DOCUMENTS, plus the two entries that reach them:
 * §7.3.1's destroy-a-child-navigable and §7.2.5.2's close().
 *
 * WHAT WAS HERE BEFORE WAS ONE BYTE. Removing an `<iframe>` set `closed = 1` on its WindowProxy and returned:
 * the child's Document, its Window and its realm stayed exactly as they were, its queued tasks stayed on the
 * queue and would still run, its descendants were never told, and nothing anywhere recorded that a destruction
 * had happened. A page could not tell that from a navigable that had merely been closed, and neither could the
 * engine — which is the tell that it was not an implementation of anything. §7.5.10 is eleven steps and
 * §7.5.10's descendant form is six more, and every one of them is observable.
 *
 * AND `close()` WAS THE SAME BYTE ONE LAYER UP. Both spellings of §7.2.5.2 — window.c's member and the
 * WindowProxy's — set is-closing and stopped, so a page that called `window.close()` got a `closed` of true
 * over a document that was still running: its beforeunload and unload listeners never fired, its timers stayed
 * scheduled, its subframes stayed live. §7.2.5.2 does not close anything itself; it QUEUES §7.3's definitely
 * close, and that is the algorithm that runs the unload, which is the algorithm that runs the destruction. One
 * path, three standards' worth of steps, all of them things a page can watch happen.
 *
 * IT IS A JOB, AND THE SPEC SAYS SO EVERYWHERE. "Destroy a document and its descendants" runs IN PARALLEL,
 * queues a global task per child navigable, and WAITS until the number destroyed equals the child count;
 * "destroy a document" asserts it is running as part of a queued task; "unload a document" asserts the same;
 * §7.4.2.4's check queues one task per document and waits for all of them; §7.2.5.2 step 6 queues definitely
 * close. So none of this is a call the removing or closing steps can make — it is work on the one frontier,
 * preemptible and parkable like every other job, which is also the only shape in which the waits are
 * expressible at all. A synchronous version would have to drive its children to completion, which is the thing
 * this engine does not do at any depth.
 *
 * NUMBER-DESTROYED IS A FACT ABOUT EACH NAVIGABLE, NOT A SHARED COUNTER. The spec writes the count as a
 * variable the child's task increments and the wait as a spin on it, which is one mutable integer shared by N
 * tasks — and a shared integer is exactly what cannot exist here, because two forked arms tearing down the same
 * subtree would be counting into each other's timeline. The equivalent that survives forking is a count on the
 * WINDOWPROXY of the navigable that is waiting, captured per flow by that record's COW capture, counted DOWN,
 * and PER OPERATION — a page's `unload` listener can remove an `<iframe>`, which starts a destroy over a
 * subtree while an unload is still counting children in the same tree.
 *
 * WHAT IT DOES NOT DO IS FREE THE REALM, and that is the NEXT subproblem rather than a gap in this one. The
 * spec's steps are all state changes a page can observe; reclaiming the JSContext is the user agent's own
 * business and is blocked on a primitive that does not exist yet — a flow parked inside the destroyed document
 * must be abandoned before its heap can go, and a flow does not currently record which realm it is in.
 * navigable.c's allocation CHECK names that ceiling where a reader stands when it bites. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_DOCUMENT_LIFECYCLE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_DOCUMENT_LIFECYCLE_H

#include "quickjs.h"

/* §7.3.1's DESTROY A CHILD NAVIGABLE, steps 4-5 — the container has already dropped its content navigable
   (that is step 3, and it belongs to the element, which is where the slot is). What is left is the part that
   is about the DOCUMENT: destroy it and everything below it, as a job.
   `proxy` is the child navigable's WindowProxy. Calling it for a navigable whose active document has already
   been destroyed is a no-op, which is §7.5.10's own answer for a document that is not fully active. */
void document_lifecycle_destroy_child(JSContext *ctx, JSValueConst proxy);

/* §7.2.5.2's `close()` METHOD STEPS — THE ONE CLOSE PATH, and both spellings of the method reach it: the
   Window's member and the WindowProxy's are one method on one navigable, so a test either of them made alone
   would be a test the other could disagree with (they already did — the proxy honoured step 2's early return
   for a frame and the Window's member did not).
   `ctx` is the INCUMBENT realm, which step 6 asks two of its three questions about: whether the incumbent's
   browsing context is familiar with the one being closed, and whether the incumbent's navigable is allowed by
   sandboxing to navigate it. `proxy` is thisTraversable — the navigable the receiver names.
   It sets is closing SYNCHRONOUSLY (so the caller's next line reads `closed` as true) and QUEUES §7.3's
   definitely close, which fires beforeunload over the whole subtree, then unloads it, then destroys it. */
void document_lifecycle_window_close(JSContext *ctx, JSValueConst proxy);

#endif
