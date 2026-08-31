/* HTML §8.1.8.1 Event handlers — the EVENT HANDLER PROCESSING ALGORITHM. See event_handler.h for why an event
 * handler is not a listener and why this is a sub-algorithm rather than a machine of its own.
 *
 * THE THREE RETURN-VALUE CASES ARE NOT A UNIFORM RULE WITH TWO EXCEPTIONS TO SKIP. The standard writes them as
 * one `if`/`if`/otherwise and every arm is load-bearing:
 *
 *   BeforeUnloadEvent  — "return value will have been coerced into either null or a DOMString"; a non-null one
 *                        sets the canceled flag AND fills an empty `returnValue`. This is how `onbeforeunload`
 *                        has asked the user to stay since before preventDefault existed.
 *   special error      — "If return value is TRUE, then set event's canceled flag." Backwards from every other
 *                        handler on the platform, and the reason `window.onerror = () => true` means "handled".
 *   otherwise          — "If return value is FALSE, then set event's canceled flag." Most of the web:
 *                        `<a href=… onclick="return false">`, `<form onsubmit="return validate()">`.
 *
 * `true` and `false` here are the ECMAScript VALUES and not ToBoolean of them — the callback types return `any`,
 * so §8.1.8.1 is comparing a value to a boolean. `return 0` does not cancel a click and `return "yes"` does not
 * cancel an error; collapsing either to truthiness would cancel events no browser cancels.
 *
 * AND THAT COMPARISON IS A FORK WHEN THE HANDLER RETURNED UNKNOWN EXTERNAL INPUT. A handler that ends
 * `return cfg.suppress` hands this a concolic, and whether the event is cancelled decides whether §8.1.4.6's
 * report runs and whether a form submits — two feasible worlds, so the algorithm declares the fork and the
 * driver snapshots the other arm, exactly as DOM §2.7's flattening does one file over. Outcome 0 is "not that
 * boolean", because step_fork_run requires the ordinary completion to be numbered first and the ordinary
 * completion of a handler is that nothing was cancelled.
 *
 * THE BeforeUnloadEvent ARM CANNOT SEE ONE, and that is a consequence rather than an assumption: its return
 * value has been through Web IDL's `DOMString?`, and step_tostring_run answers JS_STEP_UNKNOWN for unknown
 * external input rather than producing a String — so an `onbeforeunload` returning one completes the whole
 * dispatch as a derived unknown and never reaches step 6. The arm asserts that instead of forking. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "core/events/before_unload_event.h"
#include "core/events/error_event.h"
#include "core/events/event.h"
#include "core/events/event_handler.h"
#include "core/events/event_target.h"

/* WHERE ONE INVOCATION RESTS, AS §8.1.8.1 NUMBERS ITS STEPS. Zero is "no invocation in flight" and is what the
   caller's own resume routing reads, so the first real stage is 1. */
enum {
    EH_IDLE = 0,
    EH_INVOKE,    /* step 5: the handler's own body, which is the page's code */
    EH_COERCE,    /* Web IDL: OnBeforeUnloadEventHandlerNonNull's `DOMString?` return type, which runs toString */
    EH_PROCESS    /* step 6: process return value — a fork when the value is unknown external input */
};

/* The two constraint keys step 6's comparisons ask under. Each names the ARM of §8.1.8.1 step 6 it belongs to:
   one op string for both would let the solver's record of an `onerror` decide an `onsubmit`, and they are
   opposite comparisons. */
#define EH_OP_ERROR   "HTML §8.1.8.1 step 6 `return value is true` (special error event handling)"
#define EH_OP_GENERAL "HTML §8.1.8.1 step 6 `return value is false`"

void event_handler_work_start(EventHandlerWork *w)
{
    w->stage = EH_IDLE;
    w->rv = JS_UNDEFINED;
}

void event_handler_work_visit(JSContext *ctx, EventHandlerWork *w, JSStepVisit *v)
{
    v->val(ctx, &w->rv);
}

/* Does this event's type equal `want` — one string compare, spelled once because step 4 and step 6 each make
   one and a second spelling is a second place for the comparison to be case-folded or truncated. */
static bool eh_type_is(JSContext *ctx, JSValueConst event, const char *want)
{
    JSValue type = event_type(ctx, event);
    const char *s = JS_IsString(type) ? JS_ToCString(ctx, type) : NULL;
    bool is = s != NULL && strcmp(s, want) == 0;

    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, type);
    return is;
}

/* §8.1.8.1 step 4: "Let special error event handling be true if event is an ErrorEvent object, event's type is
   `error`, and event's currentTarget implements the WindowOrWorkerGlobalScope mixin."
   ALL THREE CONJUNCTS, and each rules out a real case the other two do not: an `error` Event fired at an
   `<img>` is not an ErrorEvent (so `img.onerror` takes the event, exactly as in a browser); a `navigateerror`
   ErrorEvent is one but is not typed `error`; and an ErrorEvent of type `error` at a Document or an
   XMLHttpRequest has a currentTarget that is not a global, which is why `xhr.onerror` is a one-argument
   handler while `window.onerror` is a five-argument one. */
static bool eh_special_error(JSContext *ctx, JSValueConst event, JSValueConst current_target)
{
    return error_event_is(ctx, event) && event_target_is_window(ctx, current_target) &&
           eh_type_is(ctx, event, "error");
}

/* §8.1.8.2.1's IDL, which is what decides whether step 5's invoke COERCES the completion: every event handler
   IDL attribute in the standard is declared `EventHandler` (return type `any`) or `OnErrorEventHandler` (also
   `any`) except ONE — `attribute OnBeforeUnloadEventHandler onbeforeunload;` on WindowEventHandlers, whose
   callback function type is `DOMString? (Event event)`.
   IT IS ASKED OF THE ATTRIBUTE NAME AND NOT OF THE EVENT, which is the distinction step 6's own note is about:
   a plain `new Event("beforeunload")` dispatched at a Window still reaches `onbeforeunload`, so the coercion
   still happens, and step 6 then lands in its LAST arm where "return value will never be false, since in such
   cases return value will have been coerced into either null or a DOMString". Deriving the coercion from the
   EVENT instead would make `onbeforeunload = () => false` cancel that dispatch, which no browser does. */
static bool eh_return_type_is_domstring_or_null(const char *name)
{
    DCHECK(name != NULL && name[0] == 'o' && name[1] == 'n' && name[2] != 0,
           "§8.1.8.1's `name` argument is the name of an event handler IDL attribute and every one of them is "
           "`on` plus an event type — a bare event type reached this and would have selected the wrong Web IDL "
           "callback function type");
    return strcmp(name, "onbeforeunload") == 0;
}

/* Is `v` the ECMAScript value `b`? A BOOLEAN IDENTITY and not ToBoolean, which is what step 6's comparisons
   are. -1 when the answer depends on unknown external input, which is the caller's fork. */
static int eh_is_boolean(JSValueConst v, bool b)
{
    if (concolic_is(v))
        return -1;
    return (JS_VALUE_GET_TAG(v) == JS_TAG_BOOL && (JS_VALUE_GET_BOOL(v) != 0) == b) ? 1 : 0;
}

int event_handler_run(JSContext *ctx, EventHandlerWork *w, JSStepHdr *hdr, uint8_t *cphase,
                      JSValue *cb, int cb_cap, JSValueConst callback, JSValueConst event,
                      const char *name, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValue current;
    int r, cancels, arm = 0;

    DCHECK(w != NULL && hdr != NULL && cphase != NULL,
           "§8.1.8.1's processing algorithm was run with no record to hold its invocation");
    /* 2 for [this, callback] plus the five arguments step 5 names. Asserted at EVERY entry rather than only on
       the special-error path, because the buffer belongs to the CALLER and a caller that shrank it would
       otherwise be found by the one dispatch in a thousand that carries an ErrorEvent. */
    DCHECK(cb_cap >= 2 + 5,
           "§8.1.8.1 step 5's five-argument invocation was handed a request buffer that cannot hold five "
           "arguments — `OnErrorEventHandlerNonNull` takes (event, source, lineno, colno, error), and a "
           "shorter buffer writes past the caller's own state");

    /* Step 4 and step 5 both read `event's currentTarget`, and DOM §2.9's `invoke` is what sets it — so it is
       read from the EVENT here rather than taken as an argument, which keeps the two reads one fact. */
    current = event_current_target(ctx, event);
    DCHECK(JS_IsObject(current),
           "§8.1.8.1 was run for an event with no currentTarget — the algorithm reads it at step 4 and again "
           "at step 5, and DOM §2.9's `invoke` is what sets it, so this ran outside a dispatch");

    if (w->stage == EH_IDLE) {
        w->stage = EH_INVOKE;
        w->rv = JS_UNDEFINED;
        /* WEB IDL §3.12 Invoking callback functions STEP 3: "If IsCallable(F) is false, throw a TypeError."
           A handler CAN be a non-callable object and this is where that ends: `EventHandler` is a nullable
           callback function annotated [LegacyTreatNonObjectAsNull], so §3.2.19 Callback function types stores
           `el.onclick = {}` instead of throwing at the assignment, and the throw is deferred to here — where
           step 5's "rethrow" hands it to DOM §2.9's inner invoke, which reports it and runs the next listener.
           Asked only on a fresh entry: a RESUME arrives with the callback held in the request buffer and
           JS_UNDEFINED in this argument, so re-asking would throw on every handler that ever suspended. */
        if (!JS_IsFunction(ctx, callback)) {
            JS_ThrowTypeError(ctx, "the event handler is not callable");
            goto abrupt;
        }
    }

    if (w->stage == EH_INVOKE) {
        /* STEP 5. The two arms differ ONLY in the argument list, and that is the whole of the legacy shape:
           "invoking callback with « event's message, event's filename, event's lineno, event's colno, event's
           error »" under special error event handling, and "with « event »" otherwise. Both set the callback
           this value to event's currentTarget, and both invoke with "rethrow" — so a throw is not caught here;
           it goes back to DOM §2.9's inner invoke, which reports it and carries on down the listener list. */
        if (eh_special_error(ctx, event, current)) {
            JSValue argv[5];
            int i;

            /* THE EVENT-KEYED CONDITION AND THE NAME-KEYED IDL MUST AGREE. Step 4 decides the five-argument
               shape from the EVENT (an ErrorEvent typed `error` at a global) while the shape itself belongs to
               §8.1.8.2.1's `attribute OnErrorEventHandler onerror` — the only attribute whose callback type
               takes five. The two are joined only by the fact that the handler for the event type `error` is
               the attribute named `onerror`, so if the attribute list ever handed another name that type, a
               handler declared `EventHandler` would silently be called with five arguments and every page
               reading `arguments[0].message` off it would read a character out of a string. */
            DCHECK(strcmp(name, "onerror") == 0,
                   "§8.1.8.1 step 4's `special error event handling` is true for an attribute that is not "
                   "`onerror` — the five-argument invocation is `OnErrorEventHandlerNonNull`'s and no other "
                   "event handler IDL attribute declares it");
            error_event_handler_arguments(ctx, event, argv);
            r = step_call_run(ctx, cphase, cb, cb_cap, callback, current, 5, (JSValueConst *)argv,
                              in, &w->rv, out_cb, out_argc);
            for (i = 0; i < 5; i++)
                JS_FreeValue(ctx, argv[i]);
        } else {
            r = step_call_run(ctx, cphase, cb, cb_cap, callback, current, 1, &event,
                              in, &w->rv, out_cb, out_argc);
        }
        in = JS_UNDEFINED;   /* the request took it */
        if (r > 0) {
            JS_FreeValue(ctx, current);
            return r;        /* parked INSIDE the handler's body; the resume returns to this same call */
        }
        /* step_call_run reports an abrupt completion as a JS_EXCEPTION in the out value — the encoding JS_Call
           itself uses, see quickjs-step.h on the two halves of the request layer. */
        if (JS_IsException(w->rv)) {
            w->rv = JS_UNDEFINED;
            goto abrupt;
        }
        w->stage = eh_return_type_is_domstring_or_null(name) ? EH_COERCE : EH_PROCESS;
    }

    if (w->stage == EH_COERCE) {
        /* WEB IDL'S `DOMString?` RETURN TYPE, performed as part of step 5's invoke and before step 6 reads
           anything. A nullable type takes BOTH null and undefined to null — which is why a handler that just
           runs and returns nothing does not cancel the unload — and everything else through ToString, which is
           the page's own `toString` and therefore a request. */
        if (JS_IsNull(w->rv) || JS_IsUndefined(w->rv)) {
            JS_FreeValue(ctx, w->rv);
            w->rv = JS_NULL;
        } else if (!JS_IsString(w->rv)) {
            JSValue s = JS_UNDEFINED;

            r = step_tostring_run(ctx, hdr, w->rv, in, &s, out_cb, out_argc);
            in = JS_UNDEFINED;   /* the request took it */
            if (r > 0) {
                JS_FreeValue(ctx, current);
                return r;
            }
            if (r < 0) {
                /* The coercion threw. §8.1.8.1 step 5's "rethrow" covers the whole invoke, its completion's
                   conversion included, so this leaves exactly as a throwing body does. */
                goto abrupt;
            }
            JS_FreeValue(ctx, w->rv);
            w->rv = s;
        }
        w->stage = EH_PROCESS;
    }

    DCHECK(w->stage == EH_PROCESS,
           "§8.1.8.1's processing algorithm resumed at a stage its own step list does not name");
    /* Whatever a resume carried that no request above claimed is not this algorithm's — released rather than
       leaked, which is what the caller does for the same reason at its own re-entry. */
    JS_FreeValue(ctx, in);

    /* STEP 6, FIRST ARM: "If event is a BeforeUnloadEvent object and event's type is `beforeunload`". BOTH
       conjuncts, because they come apart in each direction — `createEvent('BeforeUnloadEvent')` then
       `initEvent('x')` is the interface with another type, and `new Event('beforeunload')` is the type without
       the interface, and the standard's own note is about the second. */
    if (before_unload_event_is(ctx, event) && eh_type_is(ctx, event, "beforeunload")) {
        /* THE ARM AND THE COERCION MUST HAVE AGREED. This arm reads `return value` as "null or a DOMString"
           and says so in as many words, and the only thing that makes it one is the attribute's own Web IDL
           return type — so an invocation arriving here uncoerced is a handler whose declared IDL type and
           whose runtime treatment disagree, which would read a raw `false` as a non-null value and cancel an
           unload the page never asked to cancel. */
        DCHECK(JS_IsNull(w->rv) || JS_IsString(w->rv),
               "§8.1.8.1 step 6's BeforeUnloadEvent arm was reached with a return value that is neither null "
               "nor a DOMString — the attribute's Web IDL type is `OnBeforeUnloadEventHandler`, whose "
               "`DOMString?` return type coerces it, so this invocation took the wrong callback type");
        if (!JS_IsNull(w->rv)) {
            /* "Set event's canceled flag" — THE FLAG, and it is `event_set_canceled` and not §2.2's gated
               algorithm because this step links DOM's `#canceled-flag` and not its `#set-the-canceled-flag`.
               A `beforeunload` the page dispatched itself with `cancelable: false` is cancelled here. */
            event_set_canceled(ctx, event, true);
            /* "If event's returnValue attribute's value is the empty string, then set event's returnValue
               attribute's value to return value." A handler that BOTH assigned `returnValue` and returned a
               string keeps the one it assigned — the standard only fills a hole. */
            if (before_unload_event_return_value_is_empty(ctx, event))
                before_unload_event_set_return_value(ctx, event, w->rv);
        }
        goto done;
    }

    /* STEP 6, SECOND ARM: "If special error event handling is true: if return value is TRUE, then set event's
       canceled flag." Re-derived rather than carried across the park from step 4: it is a function of the
       event's interface, type and currentTarget, none of which a handler can change under itself, so a byte
       held on the record would be a second copy of an answer already derivable — and the copy is the one that
       goes stale. THIRD ARM otherwise: "If return value is FALSE, then set event's canceled flag." */
    {
        bool special = eh_special_error(ctx, event, current);

        cancels = eh_is_boolean(w->rv, special ? true : false);
        if (cancels < 0) {
            r = step_fork_run(ctx, hdr, w->rv, special ? EH_OP_ERROR : EH_OP_GENERAL, 2,
                              JS_OUTCOME_REAL_UNSTATED, &arm);
            if (r) {
                JS_FreeValue(ctx, current);
                return r;
            }
            DCHECK(arm == 0 || arm == 1,
                   "§8.1.8.1 step 6's comparison of one return value against one boolean came back on an arm "
                   "that is neither of the two an identity test has");
            cancels = arm;   /* outcome 0 = it is not that boolean, so nothing is cancelled */
        }
        /* BOTH ARMS SET THE FLAG DIRECTLY — see the BeforeUnloadEvent arm above and event.h: step 6 links
           `#canceled-flag` in all three of its arms and never `#set-the-canceled-flag`, so §2.2's cancelable /
           in-passive-listener gate is not this step's to apply. The two cases that separates are ordinary web:
           `window.onload = () => false` cancels a NON-CANCELABLE event, and `document.body.onwheel = () =>
           false` cancels one being handled by a listener the default passive value made PASSIVE. Routing this
           through the gate left both silently uncancelled, which `defaultPrevented` then answers false for. */
        if (cancels)
            event_set_canceled(ctx, event, true);
    }

done:
    JS_FreeValue(ctx, current);
    JS_FreeValue(ctx, w->rv);
    w->rv = JS_UNDEFINED;
    w->stage = EH_IDLE;
    return 0;

abrupt:
    JS_FreeValue(ctx, current);
    JS_FreeValue(ctx, in);
    JS_FreeValue(ctx, w->rv);
    w->rv = JS_UNDEFINED;
    w->stage = EH_IDLE;
    return -1;
}
