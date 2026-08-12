/* HTML §8.1.4.6 "report an exception". See report_exception.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_REPORT_EXCEPTION_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_REPORT_EXCEPTION_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request this machine parks on */

/* THE OPERATION AS A REQUEST, because step 5.2 FIRES AN EVENT and firing one runs the page's code.
 *
 * Three algorithms invoke a callback with "report" and CONTINUE: DOM §2.9 inner invoke step 2.11, HTML §8.9's
 * animation-frame callback, and RESIZE OBSERVER §3.4.6's loop error. Each is already a step machine, so the
 * work record is the CALLING machine's — it visits and releases it, exactly as abort.h's AbortSignalWork is
 * the caller's. `cb` needs FOUR slots because event_target_fire_run's buffer is [this, fn, target, event].
 *   JS_STEP_CALL = return it, 0 = the report has finished. */
typedef struct {
    uint8_t stage;     /* 0 = not started, 1 = the `error` event is in flight */
    uint8_t phase;     /* event_target_fire_run's own */
    JSValue ev;        /* the ErrorEvent, minted once and held across the dispatch (owned) */
    EventFireCb cb;
} ReportExceptionWork;

/* The private key §8.1.4.6 step 5's "in error reporting mode" flag hangs off the global by. One per AGENT. */
void report_exception_init(JSContext *ctx);
void report_exception_free(JSContext *ctx);

void report_exception_work_start(ReportExceptionWork *w);
void report_exception_work_visit(JSContext *ctx, ReportExceptionWork *w, JSStepVisit *v);
void report_exception_work_release(JSContext *ctx, ReportExceptionWork *w);
/* `exception` is BORROWED — the caller took it off the context with JS_GetException and owns it. */
int  report_exception_run(JSContext *ctx, ReportExceptionWork *w, JSValueConst exception, JSValue in,
                          JSValue **out_cb, int *out_argc);

#endif
