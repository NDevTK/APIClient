/* HTML §7.5.9 UNLOADING DOCUMENTS and §7.5.10 DESTROYING DOCUMENTS, plus the two entries that reach them:
 * §7.3.1's destroy-a-child-navigable and §7.2.2.1's close().
 *
 * WHAT WAS HERE BEFORE WAS ONE BYTE. Removing an `<iframe>` set `closed = 1` on its WindowProxy and returned:
 * the child's Document, its Window and its realm stayed exactly as they were, its queued tasks stayed on the
 * queue and would still run, its descendants were never told, and nothing anywhere recorded that a destruction
 * had happened. A page could not tell that from a navigable that had merely been closed, and neither could the
 * engine — which is the tell that it was not an implementation of anything. §7.5.10 is eleven steps and
 * §7.5.10's descendant form is six more, and every one of them is observable.
 *
 * AND `close()` WAS THE SAME BYTE ONE LAYER UP. Both spellings of §7.2.2.1 — window.c's member and the
 * WindowProxy's — set is-closing and stopped, so a page that called `window.close()` got a `closed` of true
 * over a document that was still running: its beforeunload and unload listeners never fired, its timers stayed
 * scheduled, its subframes stayed live. §7.2.2.1 does not close anything itself; it QUEUES §7.3's definitely
 * close, and that is the algorithm that runs the unload, which is the algorithm that runs the destruction. One
 * path, three standards' worth of steps, all of them things a page can watch happen.
 *
 * IT IS A JOB, AND THE SPEC SAYS SO EVERYWHERE. "Destroy a document and its descendants" runs IN PARALLEL,
 * queues a global task per child navigable, and WAITS until the number destroyed equals the child count;
 * "destroy a document" asserts it is running as part of a queued task; "unload a document" asserts the same;
 * §7.4.2.4's check queues one task per document and waits for all of them; §7.2.2.1 step 6 queues definitely
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
 * WHAT IT DOES NOT DO IS FREE THE REALM, AND THAT IS THE OWNERSHIP BOUNDARY RATHER THAN A MISSING PRIMITIVE.
 * Every step of §7.5.9 and §7.5.10 is a state change a page can OBSERVE; reclaiming the JSContext is the user
 * agent's own business and happens by REFERENCE DROP, which core/frame/navigable.c owns at the one moment a
 * realm can die (quickjs's realm-teardown hook, reached from inside the collection). A flow suspended inside
 * the destroyed document holds that realm through COUNTED references — its frames, its jobs, its parked
 * continuation, the dups its COW delta took at each capture — so a realm a flow can resume into cannot reach
 * that teardown at all. Which is exactly why NO flow is abandoned, terminated or paged to make one: §7.5.10
 * step 7 removes QUEUED TASKS, work that has not started, and the standard has no object at all for a
 * continuation suspended mid-program. THIS PARAGRAPH USED TO SAY THE OPPOSITE — that a parked flow "must be
 * abandoned before its heap can go, and a flow does not currently record which realm it is in" — which is a
 * guess that reads as a decision, and it contradicted navigable.c's own teardown, where the argument above is
 * written beside the assert that rests on it. §NO BOUNDS is unaffected by any of this and never had to be
 * bent: "STARVE means deprioritize-and-page, NEVER terminate" is about what the SCHEDULER may do to a flow,
 * and nothing here touches the frontier. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_DOCUMENT_LIFECYCLE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_DOCUMENT_LIFECYCLE_H

#include "quickjs.h"

/* §7.3.1's DESTROY A CHILD NAVIGABLE, steps 4-5 — the container has already dropped its content navigable
   (that is step 3, and it belongs to the element, which is where the slot is). What is left is the part that
   is about the DOCUMENT: destroy it and everything below it, as a job.
   `proxy` is the child navigable's WindowProxy. Calling it for a navigable whose active document has already
   been destroyed is a no-op, which is §7.5.10's own answer for a document that is not fully active. */
void document_lifecycle_destroy_child(JSContext *ctx, JSValueConst proxy);

/* §7.2.2.1's ("Opening and closing windows") `close()` METHOD STEPS — THE ONE CLOSE PATH, and both spellings
   of the method reach it: the Window's member and the WindowProxy's are one method on one navigable, so a test
   either of them made alone would be a test the other could disagree with (they already did — the proxy
   honoured step 2's early return for a frame and the Window's member did not).
   `ctx` is the INCUMBENT realm, which step 6 asks two of its three questions about: whether the incumbent's
   browsing context is familiar with the one being closed, and whether the incumbent's navigable is allowed by
   sandboxing to navigate it. `proxy` is thisTraversable — the navigable the receiver names.
   It sets is closing SYNCHRONOUSLY (so the caller's next line reads `closed` as true) and QUEUES §7.3's
   definitely close, which fires beforeunload over the whole subtree, then unloads it, then destroys it. */
void document_lifecycle_window_close(JSContext *ctx, JSValueConst proxy);

/* HTML §7.4.6.1 "Updating the traversable"'s DEACTIVATE A DOCUMENT FOR A CROSS-DOCUMENT NAVIGATION, for the
 * Document a navigation has REPLACED — the entry a NAVIGATION reaches this file through, and it is NOT
 * §7.3.1.6's.
 *
 * §7.3.1.6 "Navigable destruction" destroys a NAVIGABLE: its three algorithms are destroy-a-child-navigable
 * (a container removed), destroy-a-top-level-traversable and close/definitely-close (a tab going away). A
 * navigation destroys no navigable — the navigable is precisely what survives; what changes is which Document
 * is ACTIVE in it. The two meet one step down: §7.4.6.1 calls "unload a document and its descendants", whose
 * per-document body (§7.5.9 "Unloading documents") ends "if oldDocument's salvageable state is false, then
 * destroy oldDocument", which is §7.5.10 "Destroying documents" — the same body §7.3.1.6 reaches by its own
 * route. So ONE machine serves both, and only the ENTRY differs. Naming §7.3.1.6 for a navigation is the
 * failure §Browser-half warns about twice over: it reads as authoritative and sends the reader to a section
 * that defines a different operation over a different object.
 *
 * IT CARRIES NO CONTINUATION, AND THAT IS A FACT ABOUT THIS USER AGENT AND NOT A STEP LEFT OUT. §7.4.6.1 hands
 * the unload `afterPotentialUnloads`, which resumes APPLYING THE HISTORY STEP — populating and activating the
 * incoming Document. Here the REAL BROWSER performed the navigation and the incoming Document was handed to
 * this agent as a realm of its own BEFORE this is called, which is also the standard's own order (§7.4.6.1
 * unloads `displayedDocument` given `targetEntry's document`, so the target's Document exists first). There is
 * no continuation this engine owes, and LC_AFTER_NONE is the whole of it.
 *
 * NOR `firePageSwapBeforeUnload`, and that one WOULD be a stub if it were faked: §7.4.6.1's `pageswap` is
 * fired at the outgoing document carrying the activation the user agent is ABOUT to perform, and this agent is
 * told about a navigation that has already happened, by a zone that performed none of it.
 *
 * AND NO `beforeunload`. §7.4.2.4's check-if-unloading-is-canceled runs BEFORE a navigation is committed and
 * can CANCEL it; by the time the trusted zone can state that a document was replaced, the browser has
 * committed. Firing it here would offer a page the chance to cancel something that already happened, and would
 * do it with the accumulator js_beforeunload_step names as unbuilt.
 *
 * `proxy` IS THE NAVIGABLE WHOSE ACTIVE DOCUMENT IS BEING REPLACED, AND `ctx` IS THE INCOMING DOCUMENT'S REALM
 * — not the outgoing one's, and that is the standard's own shape rather than a convenience. §7.4.6.1 takes
 * `targetEntry` and hands its Document to the unload, and §7.5.9's "unload a Document oldDocument, given an
 * optional Document newDocument" takes it too; this file's other caller passes the EMBEDDER's realm for the
 * same reason (document_lifecycle_destroy_child), so `ctx` has always been "whoever is doing this" and never
 * "the thing being done to".
 * IT IS ALSO WHAT KEEPS §7.5.10 STEP 7 FROM EATING THIS OPERATION. The step removes every queued task whose
 * enqueuing realm is the DESTROYED document's, and these jobs are queued in `ctx` — so with the outgoing
 * document's own realm passed here, the FIRST timeline to reach its step 7 would drop every OTHER timeline's
 * queued unload, and those timelines would go on running a document the browser replaced with nothing to say
 * so. The incoming document's realm is by construction not the one being destroyed.
 *
 * Called once PER TIMELINE — the destruction is state a page observes (`pagehide` and `unload` fire at
 * listeners a script registered, so they live in the COW delta of the flow that ran that script), so
 * solver/engine.h's engine_unload_document is what walks the frontier and this stays the one-navigable
 * operation. */
void document_lifecycle_unload_replaced(JSContext *ctx, JSValueConst proxy);

#endif
