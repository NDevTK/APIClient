/* UNHANDLED PROMISE REJECTIONS — HTML §8.1.7.5.
 *
 * WHAT WAS MISSING, AND WHY IT MATTERS HERE MORE THAN IN A BROWSER: a promise that rejects with nobody to catch
 * it was SILENT. Not softened, not swallowed by a fallback — never observed at all. A page error is this
 * engine's report of a capability the page needed and the engine does not have, and an async bundle delivers
 * most of its errors as rejections: an `await` of a missing global, a `.then` whose body touches an unbuilt
 * API. Every one of those looked exactly like a flow that ran and did nothing, which is the worst possible
 * failure mode for a tool whose whole job is to notice what code CAN do.
 *
 * THE TWO LISTS ARE THE SPEC, AND THEY ARE WHY THIS IS NOT JUST A PRINT AT THE THROW SITE. §8.1.7.5 keeps an
 * "about-to-be-notified" list and an "outstanding" list precisely because attaching a handler LATER is normal,
 * correct code — `var p = f(); p.catch(h)` rejects before the catch is attached. Reporting at rejection time
 * would call every one of those an error. The runtime's tracker reports both edges (rejected-with-no-handler,
 * and handled-after-the-fact), so an entry that gets handled is removed and never notified about.
 *
 * THE LIST IS A HEAP OBJECT, for the same reason the custom-element registry is: it is per-flow state. Flow A
 * rejecting a promise is not flow B's rejection, a parked flow must resume owed exactly what it was owed, and
 * a C-global list would report one flow's rejection against another's world. A baseline Array carries all of
 * that through the heap COW with no new primitive.
 *
 * THE CHECKPOINT IS THE FLOW'S END, not HTML's per-microtask-checkpoint. HTML notifies after every microtask
 * checkpoint and then fires `rejectionhandled` to RETRACT a report whose promise was handled afterwards; this
 * engine has no drain to hang a checkpoint on (the scheduler IS the job pump), and reporting once the flow has
 * no work left is strictly the same set minus the retractions. That is why there is no rejectionhandled here:
 * there is no early report for it to retract.
 *
 * WHAT IS HONESTLY ABSENT: the `unhandledrejection` EVENT. Its interface is PromiseRejectionEvent, whose init
 * dictionary has a required `Promise<any>` member and an `any` member, and this engine's Web IDL dictionary
 * conversion takes booleans only — so the event needs the typed-dictionary conversion first, and building it
 * out of order would mean a hand-rolled dictionary read beside the machine that exists to do that. The handler
 * slot (`window.onunhandledrejection`) is installed and, until then, nothing fires it. */
#include "check.h"
#include "quickjs.h"
#include "core/html/unhandled_rejection.h"

/* §8.1.7.5's list, as a baseline heap object so it time-travels with the flow that filled it. Each live entry
   is a two-element array [promise, reason]; a handled entry becomes undefined in place, because the identity
   of a slot is what the handled edge finds it by and compacting would move the ones behind it. */
static JSValue g_list;
static int     g_ready;

static uint32_t list_len(JSContext *ctx)
{
    JSValue v = JS_GetPropertyStr(ctx, g_list, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              bool is_handled, void *opaque)
{
    uint32_t n, i;

    (void)opaque;
    DCHECK(g_ready, "the rejection tracker fired after its list was freed");
    n = list_len(ctx);
    if (!is_handled) {
        /* "add promise to the about-to-be-notified rejected promises list" */
        JSValue e = JS_NewArray(ctx);
        CHECK(!JS_IsException(e), "unhandled rejections: OOM recording a rejection — a dropped one is an error "
                                  "the page reported and this engine never saw");
        JS_SetPropertyUint32(ctx, e, 0, JS_DupValue(ctx, promise));
        JS_SetPropertyUint32(ctx, e, 1, JS_DupValue(ctx, reason));
        JS_SetPropertyUint32(ctx, g_list, n, e);
        return;
    }
    /* "remove promise from the about-to-be-notified rejected promises list" — a handler attached after the
       rejection, which is ordinary code, not an error. */
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_list, i), p;
        bool same;
        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        p = JS_GetPropertyUint32(ctx, e, 0);
        same = JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(promise);
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, e);
        if (same) { JS_SetPropertyUint32(ctx, g_list, i, JS_UNDEFINED); return; }
    }
    /* Not in the list is the ordinary case: the runtime reports the handled edge for every rejected promise,
       including ones handled in the same turn they rejected, which never reached the list at all. */
}

JSValue unhandled_rejection_take(JSContext *ctx)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n, i, k = 0;

    CHECK(!JS_IsException(out), "unhandled rejections: OOM building the notify list");
    DCHECK(g_ready, "an unhandled-rejection checkpoint ran before the tracker was installed");
    n = list_len(ctx);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_list, i);
        if (JS_IsObject(e))
            JS_SetPropertyUint32(ctx, out, k++, JS_GetPropertyUint32(ctx, e, 1));
        JS_FreeValue(ctx, e);
    }
    JS_SetPropertyStr(ctx, g_list, "length", JS_NewInt32(ctx, 0));
    return out;
}

void unhandled_rejection_init(JSContext *ctx)
{
    DCHECK(!g_ready, "unhandled_rejection_init ran twice — one instance is one document");
    /* Built at init so it belongs to the pre-boot BASELINE: a write during a flow is captured by the heap COW.
       A list allocated lazily inside a flow would be that flow's private object and no sibling would see it. */
    g_list = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_list), "the rejected-promise list could not be allocated");
    g_ready = 1;
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), rejection_tracker, NULL);
}

void unhandled_rejection_free(JSContext *ctx)
{
    if (!g_ready) return;
    /* The tracker goes FIRST: teardown frees promises, and a rejected one still on the runtime's list would
       fire the callback into a list this call is about to release. */
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), NULL, NULL);
    g_ready = 0;
    JS_FreeValue(ctx, g_list);
    g_list = JS_UNDEFINED;
}
