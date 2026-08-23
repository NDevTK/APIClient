/* HTML §8.1.4.6 "report an exception". See report_exception.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_REPORT_EXCEPTION_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_REPORT_EXCEPTION_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request this machine parks on */

/* THE OPERATION AS A REQUEST, because step 6.2 FIRES AN EVENT and firing one runs the page's code.
 *
 * Three algorithms invoke a callback with "report" and CONTINUE: DOM §2.9 inner invoke step 2.11, HTML §8.12 Animation frames's
 * animation-frame callback, and RESIZE OBSERVER §3.4.6's loop error. Each is already a step machine, so the
 * work record is the CALLING machine's — it visits and releases it, exactly as abort.h's AbortSignalWork is
 * the caller's. `cb` needs FOUR slots because event_target_fire_run's buffer is [this, fn, target, event].
 *   JS_STEP_CALL or JS_STEP_YIELD = return it (the report has parked — on the page's `error` listeners, or on
 * the scheduler's answer at one of its own rest points), 0 = the report has finished. */
typedef struct {
    /* WHICH STEP OF §8.1.4.6 THIS REPORT IS AT. RX_STAGES in report_exception.c declares them and the dispatch
       there is generated from that one declaration; this was `0 = not started, 1 = the error event is in
       flight`, a private numbering that says nothing about the algorithm and that a cross-session resume can
       only guess at. */
    uint8_t stage;
    /* STEP 6.1'S FLAG IS HELD, NOT INFERRED FROM THE STAGE. The unlock below read `stage != 0` for "this record
       set the global's error reporting mode and owes it back", which is the negation this engine has now been
       bitten by twice: it is a claim about EVERY stage that is not the first, so it is wrong for every stage
       added after it — and the extract stage §8.1.4.6 has since gained is exactly such a stage, one at which
       this record has NOT taken the mode. The lock is not a step of the algorithm, it is a thing the algorithm
       is HOLDING, so it is its own field and the unlock asks about it directly. */
    uint8_t reporting;
    uint8_t phase;     /* event_target_fire_run's own */
    JSValue ev;        /* the ErrorEvent, minted once and held across the dispatch (owned) */
    EventFireCb cb;
} ReportExceptionWork;

/* HTML §8.1.4.6's OTHER ALGORITHM — "to EXTRACT ERROR INFORMATION from a JavaScript value exception" — which is
 * a sibling of report-an-exception rather than a step of it, and which has a second caller: HTML §7.2.6.8's
 * abort-a-NavigateEvent step 4 extracts error information from the abort reason and its step 6 fires
 * `navigateerror` "with additional attributes initialized according to errorInfo".
 *
 * THE ErrorEvent *IS* THIS ENGINE'S errorInfo. The standard's errorInfo is "a map keyed by IDL attributes" whose
 * only consumer anywhere is "additional attributes initialized according to errorInfo", so the map and the event
 * built from it hold the same five values — and the event is the form that PARKS, which a C map of borrowed
 * strings could not. `attributes[error]` is the exception itself, which the standard pins; the other four are
 * "implementation-defined values derived from exception", and this engine derives them from the message and from
 * the backtrace the value carries, without running any of the page's code (a host reporting what went wrong must
 * not depend on the code that went wrong).
 *
 * `type` and `cancelable` are the FIRING algorithm's, not the extraction's — §8.1.4.6 fires `error` cancelable,
 * §7.2.6.8 fires `navigateerror` with DOM's plain fire-an-event — and they are here because the one thing every
 * caller does with an errorInfo is dispatch it. `exception` is BORROWED; the result is OWNED. */
JSValue extract_error_information(JSContext *ctx, JSValueConst exception, const char *type, bool cancelable);

/* The private key §8.1.4.6 step 6's "in error reporting mode" flag hangs off the global by. One per AGENT. */
void report_exception_init(JSContext *ctx);
void report_exception_free(JSContext *ctx);

void report_exception_work_start(ReportExceptionWork *w);
void report_exception_work_visit(JSContext *ctx, ReportExceptionWork *w, JSStepVisit *v);
/* STEP 6.1'S FLAG, GIVEN BACK — the half of this record's teardown that is NOT a reference and therefore not
   something the visit above can carry. A step machine whose own `visit` names this record discharges the
   references through that declaration (the driver's teardown discharges it) and calls THIS for the flag; calling the whole
   release from such a machine would be the second list its declaration exists to be. */
void report_exception_work_unlock(JSContext *ctx, ReportExceptionWork *w);
/* Both halves, for a caller that holds this record OUTSIDE a step machine's declaration. */
void report_exception_work_release(JSContext *ctx, ReportExceptionWork *w);
/* `exception` is BORROWED — the caller took it off the context with JS_GetException and owns it. */
int  report_exception_run(JSContext *ctx, ReportExceptionWork *w, JSValueConst exception, JSValue in,
                          JSValue **out_cb, int *out_argc);

#endif
