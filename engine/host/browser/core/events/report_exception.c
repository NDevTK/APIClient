/* HTML §8.1.4.6 "REPORT AN EXCEPTION" — the algorithm three callers needed and none of them could reach.
 *
 * DOM §2.9's inner invoke step 2.11 says a listener that throws has its exception REPORTED and the walk
 * CONTINUES. HTML §8.9 says the same of an animation-frame callback, and RESIZE OBSERVER §3.4.6's "deliver
 * resize loop error" is the identical fire. Without this component each of those had exactly two options —
 * unwind the machine (which skips the rest of the algorithm and swallows the exception with nothing anywhere
 * to say a page threw) or swallow it silently — and rendering.c carried a DFAIL naming this file rather than
 * choosing either.
 *
 * IT IS A REQUEST, not a call, because step 5.2 FIRES AN EVENT and firing one runs the page's `onerror`. The
 * work record belongs to the CALLING machine, exactly as abort.h's AbortSignalWork does: a fork mid-report must
 * not hand two arms one dispatch, and the caller is what has a `visit`.
 *
 * WHAT IS NOT MODELLED, AND WHY THAT IS NOT A STUB. errorInfo's `message`, `filename`, `lineno` and `colno` are
 * IMPLEMENTATION-DEFINED by the standard's own words ("implementation-defined values derived from exception"),
 * so deriving the message from the exception without a script position is a permitted implementation and not a
 * gap. `error` is the exception itself and is the one the standard pins. The MUTED-ERRORS branch (steps 3-4) is
 * about a classic script fetched cross-origin without CORS, which is a fact about the script fetch; when the
 * loader records it, this is the one place that reads it. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_slots.h"
#include "core/events/error_event.h"
#include "core/events/event_target.h"
#include "core/events/report_exception.h"

/* §8.1.4.6 step 5's IN ERROR REPORTING MODE, which is a flag on the GLOBAL and not on this component: a report
   whose own `error` listener throws must not report that throw recursively, and "recursively" is per global.
   It hangs off the global under a private Symbol, which is also what makes it per-flow for free — a flag set by
   one forked arm is a property write the COW delta captures. */
static JSValue g_key;
static int g_ready;

void report_exception_init(JSContext *ctx)
{
    DCHECK(!g_ready, "report_exception_init ran twice — the key is one per AGENT");
    g_key = JS_NewSymbol(ctx, "inErrorReportingMode", false);
    CHECK(!JS_IsException(g_key), "the error-reporting-mode key allocation failed");
    g_ready = 1;
}

void report_exception_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;
    g_ready = 0;
}

static bool reporting_mode(JSContext *ctx, JSValueConst global, int set, bool on)
{
    JSAtom k;
    JSValue v;
    bool was;

    DCHECK(g_ready, "the error reporting mode was asked for before report_exception_init ran");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return false;
    /* AN OWN SLOT, never a property LOOKUP: a miss on the global is the solver's absent-state seam, which mints
       a concolic for a name nobody defined — and a concolic flag would fork the report. */
    if (JS_GetOwnSlot(ctx, &v, global, k) <= 0)
        v = JS_UNDEFINED;
    was = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (set)
        JS_SetProperty(ctx, (JSValue)global, k, JS_NewBool(ctx, on));
    JS_FreeAtom(ctx, k);
    return was;
}

void report_exception_work_start(ReportExceptionWork *w)
{
    w->stage = 0;
    w->phase = 0;
    w->ev = JS_UNDEFINED;
    w->cb[0] = w->cb[1] = w->cb[2] = w->cb[3] = JS_UNDEFINED;
}

void report_exception_work_visit(JSContext *ctx, ReportExceptionWork *w, JSStepVisit *v)
{
    int i;
    v->val(ctx, &w->ev);
    for (i = 0; i < 4; i++)
        v->val(ctx, &w->cb[i]);
}

void report_exception_work_release(JSContext *ctx, ReportExceptionWork *w)
{
    int i;
    /* THE FLAG IS A LOCK, SO ITS OWNER HAS TO RELEASE IT ON EVERY EXIT. A report that is ABANDONED — the flow
       it belongs to is dropped, or the dispatch machine holding it is torn down while the `error` event is
       still in flight — would otherwise leave the global in error reporting mode forever, and step 5 then
       silently skips EVERY later report on that global. That is invisible: the first exception is reported and
       every one after it is swallowed, which reads exactly like "reporting does not work sometimes". */
    if (w->stage != 0) {
        JSValue g = JS_GetGlobalObject(ctx);
        reporting_mode(ctx, g, /*set*/ 1, false);
        JS_FreeValue(ctx, g);
        w->stage = 0;
    }
    JS_FreeValue(ctx, w->ev);
    w->ev = JS_UNDEFINED;
    for (i = 0; i < 4; i++) { JS_FreeValue(ctx, w->cb[i]); w->cb[i] = JS_UNDEFINED; }
}

/* §8.1.4.6 step 2: "extract error information from exception". `error` is the exception; the other four are
   implementation-defined, and the message is derived WITHOUT running the page's `toString` — a host reporting
   what went wrong must not depend on the code that went wrong, which is exactly what JS_DiagCString is for. */
static JSValue report_error_event(JSContext *ctx, JSValueConst exception)
{
    char *owned = NULL;
    const char *what = JS_DiagCString(ctx, exception, &owned);
    JSValue message = JS_NewString(ctx, what ? what : "Uncaught exception");
    JSValue filename = JS_NewString(ctx, "");
    JSValue ev;

    JS_DiagFreeCString(ctx, what, owned);
    ev = error_event_new(ctx, message, filename, /*lineno*/ 0, /*colno*/ 0, exception);
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, filename);
    return ev;
}

int report_exception_run(JSContext *ctx, ReportExceptionWork *w, JSValueConst exception, JSValue in,
                         JSValue **out_cb, int *out_argc)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValueConst global = g;
    bool not_canceled = true;
    int r;

    JS_FreeValue(ctx, g);   /* a realm owns its global for the realm's whole life — this is a borrow */
    if (w->stage == 0) {
        JS_FreeValue(ctx, in);
        /* §8.1.4.6 step 5: the whole of steps 5.1-5.3 happen only when the global is NOT already reporting.
           Nothing else in the algorithm is observable headless, so a re-entrant report is finished here. */
        if (reporting_mode(ctx, global, /*set*/ 0, false))
            return 0;
        w->ev = report_error_event(ctx, exception);
        if (JS_IsException(w->ev)) { w->ev = JS_UNDEFINED; return 0; }
        reporting_mode(ctx, global, /*set*/ 1, true);   /* step 5.1 */
        w->stage = 1;
        in = JS_UNDEFINED;
    }
    /* step 5.2: fire `error` at the global, using ErrorEvent, cancelable. It is the SAME §2.9 dispatch every
       other fire in this engine uses — reached as a REQUEST because this caller can park. */
    r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), global, w->ev, in,
                              &not_canceled, out_cb, out_argc);
    if (r)
        return r;
    reporting_mode(ctx, global, /*set*/ 1, false);      /* step 5.3 */
    /* step 6's notHandled is `not_canceled`: an `onerror` that returns true cancels the event, and that is how
       a page says it handled the error. Headless there is no developer console for step 7 to write to, so the
       flag has no further consumer here — a WORKER's global is where step 6.2 gives it one. */
    JS_FreeValue(ctx, w->ev);
    w->ev = JS_UNDEFINED;
    w->stage = 0;
    return 0;
}
