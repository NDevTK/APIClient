/* UNHANDLED PROMISE REJECTIONS — HTML §8.1.4.7 "Unhandled promise rejections": what is still unhandled at a
 * checkpoint. The TWO LISTS it operates on belong to §8.1.3.3 "Realms, settings objects, and global objects": a
 * global object has an "about-to-be-notified rejected promises list" and an "outstanding rejected promises
 * weak set" — so the algorithm and the state it walks cite different sections on purpose.
 * §8.1.7.5 stood here and throughout this component; it is a REAL section, "Dealing with the event loop from
 * other specifications", and a different algorithm. See unhandled_rejection.c's header. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_UNHANDLED_REJECTION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_UNHANDLED_REJECTION_H
#include "quickjs.h"

/* Install the runtime's rejection tracker and build the baseline list. */
void unhandled_rejection_init(JSContext *ctx);
/* Web IDL §3.7's per-realm objects for §8.1.4.7 Unhandled promise rejections' `PromiseRejectionEvent` — the
   §3.7.3 interface prototype object, the §3.7.1 interface object, Web IDL §3.8's property reference for the
   name, and this realm's two rejection drivers; run beside the realm's other intrinsics, once per realm.
   (§8.1.7.2 stood here and is "Queuing tasks".)
   IT IS ONE FUNCTION AND NOT TWO because Web IDL §3.8 Platform objects implementing interfaces is "To define
   the global property references on target, given realm realm" and names no Document, and §8.1.4.7 declares
   `PromiseRejectionEvent` `[Exposed=*]`: an interface object placed from core/platform.c's per-document
   column reaches no realm that has no Document over it. */
void unhandled_rejection_install_realm(JSContext *ctx);
/* PER REALM — see event.h. OWNED: the caller frees. */
JSValue unhandled_rejection_proto(JSContext *ctx);
/* THE AGENT HALF, UNDONE — called ONLY from core/platform.c's release column, which is why it takes the
   RUNTIME: what it frees is agent state, and a host that has to remember the call is a host that can forget it
   (the WPT runner did, and every file in that gate ended on the runtime's own leak walk). */
void unhandled_rejection_free(JSRuntime *rt);

/* WHAT AN UNREPORTED REJECTION MEANS is the SOLVER's answer, not this component's: the browser half fires
   `unhandledrejection` and honours preventDefault, and whatever survives that is handed here. Installed once,
   like the document-load hook.
   AND SO IS WHAT A HANDLED ONE MEANS, ON THE SAME HOOK. §8.1.6.4 "HostPromiseRejectionTracker(promise,
   operation)" step 7.4 is the standard's own statement that a report made at §8.1.4.7 step 4.1.3 can turn out
   to have been about a page that did nothing wrong — a bundle attaching `.catch` in a later task. A SECOND
   hook for that edge would let a consumer register the report and never the retraction, so a false entry
   would stand by default in exactly the consumer that had not thought about it; one hook carrying the edge
   makes the pair impossible to take apart. The enum rather than a flag so the consumer's handling is
   exhaustive.
   THE VOCABULARY IS THIS SECTION'S AND NOT THE CONSUMER'S. The solver's own console has its own two edges and
   its own words for them; this component names the two SPEC steps, and whoever installs the hook is the seam
   where the two vocabularies meet. That is what keeps a browser component from including a solver header. */
typedef enum {
    REJECTION_REPORTED = 0,    /* §8.1.4.7 step 4.1.3 — reported, the `unhandledrejection` fire not cancelled */
    REJECTION_RETRACTED = 1    /* §8.1.6.4 step 7.4 — the promise has since been handled; take that report back */
} RejectionReportEdge;
void unhandled_rejection_set_report_hook(void (*fn)(JSContext *ctx, JSValueConst reason,
                                                    RejectionReportEdge edge));

/* §8.1.4.7 Unhandled promise rejections' "notify about rejected promises": for every rejection still
   unhandled, queue the `unhandledrejection`
   fire (which reports through the hook above unless the page cancels it) and CLEAR the list. Returns how many
   were queued — the caller has more work to run when that is non-zero. Taking rather than reading is what makes
   a second checkpoint notify about nothing twice. */
int unhandled_rejection_notify(JSContext *ctx);

/* HOW MANY ARE STILL ON THE LIST — the READ the line above cannot be used for, because that one TAKES. It
   counts exactly what `notify` would queue, through the same live-entry test, so the two cannot disagree about
   what "still unhandled" means.
   THE CALLER IS AN ASSERTION AND THAT IS WHY IT EXISTS. The scheduler notifies at the end of every microtask
   checkpoint and a flow may then not FINISH holding an un-notified rejection — a dropped one is an error the
   page reported and this engine never saw, which is indistinguishable from a flow that ran and did nothing.
   Reading it is not free (it interns `length`), so a caller inside a DCHECK computes it OUTSIDE the condition,
   under the dev guard, exactly as engine_host_take does with pending_extra_count. */
int unhandled_rejection_pending(JSContext *ctx);

#endif
