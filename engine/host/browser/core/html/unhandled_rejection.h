/* UNHANDLED PROMISE REJECTIONS — HTML §8.1.7.5: the two lists, and what is still unhandled at a checkpoint. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_UNHANDLED_REJECTION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_UNHANDLED_REJECTION_H
#include "quickjs.h"

/* Install the runtime's rejection tracker and build the baseline list. */
void unhandled_rejection_init(JSContext *ctx);
void unhandled_rejection_free(JSContext *ctx);
/* `PromiseRejectionEvent` as a global — §8.1.7.5's own interface, chained to Event. */
void unhandled_rejection_install(JSContext *ctx, JSValueConst global);

/* WHAT AN UNREPORTED REJECTION MEANS is the SOLVER's answer, not this component's: the browser half fires
   `unhandledrejection` and honours preventDefault, and whatever survives that is handed here. Installed once,
   like the document-load hook. */
void unhandled_rejection_set_report_hook(void (*fn)(JSContext *ctx, JSValueConst reason));

/* §8.1.7.5 "notify about rejected promises": for every rejection still unhandled, queue the `unhandledrejection`
   fire (which reports through the hook above unless the page cancels it) and CLEAR the list. Returns how many
   were queued — the caller has more work to run when that is non-zero. Taking rather than reading is what makes
   a second checkpoint notify about nothing twice. */
int unhandled_rejection_notify(JSContext *ctx);

#endif
