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
/* §8.1.4.7 Unhandled promise rejections' `PromiseRejectionEvent` prototype for ONE realm; run beside the
   realm's other intrinsics, once per realm. (§8.1.7.2 stood here and is "Queuing tasks".) */
void unhandled_rejection_install_proto(JSContext *ctx);
/* PER REALM — see event.h. OWNED: the caller frees. */
JSValue unhandled_rejection_proto(JSContext *ctx);
/* THE AGENT HALF, UNDONE — called ONLY from core/platform.c's release column, which is why it takes the
   RUNTIME: what it frees is agent state, and a host that has to remember the call is a host that can forget it
   (the WPT runner did, and every file in that gate ended on the runtime's own leak walk). */
void unhandled_rejection_free(JSRuntime *rt);
/* `PromiseRejectionEvent` as a global — §8.1.4.7 Unhandled promise rejections' own interface, chained to Event. */
void unhandled_rejection_install(JSContext *ctx, JSValueConst global);

/* WHAT AN UNREPORTED REJECTION MEANS is the SOLVER's answer, not this component's: the browser half fires
   `unhandledrejection` and honours preventDefault, and whatever survives that is handed here. Installed once,
   like the document-load hook. */
void unhandled_rejection_set_report_hook(void (*fn)(JSContext *ctx, JSValueConst reason));

/* §8.1.4.7 Unhandled promise rejections' "notify about rejected promises": for every rejection still
   unhandled, queue the `unhandledrejection`
   fire (which reports through the hook above unless the page cancels it) and CLEAR the list. Returns how many
   were queued — the caller has more work to run when that is non-zero. Taking rather than reading is what makes
   a second checkpoint notify about nothing twice. */
int unhandled_rejection_notify(JSContext *ctx);

#endif
