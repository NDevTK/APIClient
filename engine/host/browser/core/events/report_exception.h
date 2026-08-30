/* HTML §8.1.4.6 "report an exception". See report_exception.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_REPORT_EXCEPTION_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_REPORT_EXCEPTION_H
#include <stdbool.h>
#include <stddef.h>   /* size_t — report_exception_position takes the caller's buffer capacity */
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

/* …AND THE PART OF THAT EXTRACTION A CALLER MAY WANT WITHOUT AN EVENT: §8.1.4.6's `filename`, `lineno` and
 * `colno`, derived from the backtrace the value already carries. One component owns the derivation because
 * two would disagree the first time either is corrected — the second reader is solver/result.h's page-error
 * stream, which reports WHICH SCRIPT threw and would otherwise re-parse the rendered frame itself.
 *
 * THE EMPTY FILENAME IS A POSITIVE ANSWER AND NEVER A HOLE. Every non-Error a page can throw carries no
 * backtrace, so "" states that this value has no throw site to report — which is §8.1.4.6's own answer, and is
 * exactly the fact a reader partitioning errors by script has to be able to see rather than default past.
 * Runs NO page code (JS_GetErrorStackString reads the slot; a host reporting what went wrong must not depend
 * on the code that went wrong). `exception` is BORROWED. */
void report_exception_position(JSContext *ctx, JSValueConst exception, char *file, size_t file_cap,
                               uint32_t *pline, uint32_t *pcol);

/* The private key §8.1.4.6 step 6's "in error reporting mode" flag hangs off the global by. One per AGENT.
 * THE RELEASE TAKES THE RUNTIME, because core/platform.h's release column is what runs it: what this component
 * holds is the AGENT's, and agent state is freed against the runtime the declaration was made in. Taking a
 * JSContext is what made it a line each of the three hosts had to remember. */
void report_exception_init(JSContext *ctx);
void report_exception_free(JSRuntime *rt);

/* §8.1.4.6 STEP 7.3 — "Otherwise, the user agent may report exception to a developer console."
 *
 * WHO gets told is the HOST's question, exactly as unhandled_rejection.h's report hook is, and the two are one
 * sentence of one standard read a section apart: an exception nothing handled and a rejection nothing handled
 * are the same fact about the page. It is reached ONLY when step 7's notHandled is true — a listener that
 * called `preventDefault()` cancelled the event, which is the page saying it did its own reporting, and a
 * console line written anyway would report as unhandled an exception the page handled.
 * Registering none is a positive statement, never a hole: step 7 runs either way. */
void report_exception_set_console_hook(void (*fn)(JSContext *ctx, JSValueConst exception));

/* HTML §8.1.4.4 "Calling scripts", run a classic script step 8's THIRD BULLET, as the FLOW it has to be.
 *
 * The third reach of the one algorithm, and the caller it exists for is the SCHEDULER: a classic script's
 * evaluation completed abruptly, and the scheduler has no step machine to hold the request form above. What it
 * has is a frame slot, so the report is handed back AS a frame — `JS_FlowResume`d, preempted per opcode,
 * forked with the flow and parked to the cold tier like the program that threw, because the `error` listeners
 * it runs are the page's code and may do any of those.
 *
 * IT MUST BE THE FLOW'S VERY NEXT WORK AND NOT A QUEUE ENTRY. The report is step 8.3.1 and "clean up after
 * running script" is 8.3.2, whose step 3 performs the microtask checkpoint — so the `error` listeners run
 * BEFORE anything the script queued, which neither a microtask (behind what the script queued) nor a task
 * (behind the whole checkpoint) can express.
 *
 * `ctx` IS THE PROGRAM'S REALM — "script's settings object's global object" — and never whichever realm the
 * scheduler happens to hold. `exception` is BORROWED (the caller took it off the context with
 * JS_GetException and owns it); the frame dups it. Returns the handle JS_FlowNew returns. */
JSValue *report_exception_flow(JSContext *ctx, JSValueConst exception);

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
