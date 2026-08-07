/* See stream_work.h. */
#include "check.h"
#include "core/streams/stream_work.h"



/* THE SLOTS ARE UNDEFINED BEFORE THEY ARE ANYTHING ELSE. A step state arrives ZEROED, and a zeroed JSValue is
   the INTEGER 0 (JS_TAG_INT is 0) rather than undefined — so a slot read before it is written yields 0, which
   is a real value the page can see. It has cost this project four bugs and it cost this component a fifth: the
   `closed` promise of a drained stream fulfilled with the number 0. Every machine that owns one of these calls
   this on its first entry, so the trap has one place to not be. `stage` is deliberately untouched: the machine
   owns it, and it has usually already been set by the time this runs. */
void stream_work_start(StreamWork *w)
{
    int k;
    w->func = w->value = w->chain = w->err = JS_UNDEFINED;
    for (k = 0; k < (int)(sizeof(w->cb) / sizeof(w->cb[0])); k++) w->cb[k] = JS_UNDEFINED;
}

void stream_work_visit(JSContext *ctx, StreamWork *w, JSStepVisit *v)
{
    int k;
    v->val(ctx, &w->func);
    v->val(ctx, &w->value);
    v->val(ctx, &w->chain);
    v->val(ctx, &w->err);
    for (k = 0; k < (int)(sizeof(w->cb) / sizeof(w->cb[0])); k++) v->val(ctx, &w->cb[k]);
}

void stream_work_release(JSContext *ctx, StreamWork *w)
{
    int k;
    JS_FreeValue(ctx, w->func);
    JS_FreeValue(ctx, w->value);
    JS_FreeValue(ctx, w->chain);
    JS_FreeValue(ctx, w->err);
    w->func = w->value = w->chain = w->err = JS_UNDEFINED;
    for (k = 0; k < (int)(sizeof(w->cb) / sizeof(w->cb[0])); k++) { JS_FreeValue(ctx, w->cb[k]); w->cb[k] = JS_UNDEFINED; }
}
/* PromiseResolve(%Promise%, v) — 27.2.4.7 — as a sub-sequence. §4.5 reacts to what `start` and `pull` RETURNED,
 * which may be a plain value, a page THENABLE, or a promise; the one operation covering all three is a
 * capability whose RESOLVE function is called with it, and calling that function is exactly where 27.2.1.3.2
 * step 8 reads `then` off the page's object. So it is a call request like every other run of the page's code,
 * rather than a `JS_IsFunction(then)` test that would answer a patched thenable wrongly.
 * Takes `w->value`; leaves the capability's promise in `w->func`. */
int stream_promise_of_run(JSContext *ctx, StreamWork *w, int reject, JSValue in,
                               JSValue **out_cb, int *out_argc)
{
    JSValueConst arg;
    JSValue out;
    int r;

    if (w->phase == 0) {
        JSValue funcs[2];
        JSValue p = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(p)) { JS_FreeValue(ctx, in); return -1; }
        JS_FreeValue(ctx, w->func);
        w->func = p;
        JS_FreeValue(ctx, w->chain);
        w->chain = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
    }
    arg = w->value;
    r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), w->chain, JS_UNDEFINED, 1, &arg, in, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, w->chain);
    w->chain = JS_UNDEFINED;
    return 0;
}
/* The reactions' own CAPABILITY, for a caller that must hand it on: §4.2's `from` returns "the result of
   reacting to nextPromise" as its pull algorithm's answer, so §4.5 chains the next pull on THAT rather than on
   the raw next promise. A step id of -1 means no handler for that side, which is how a rejection is left to
   propagate through the capability instead of being caught. */
JSValue stream_react_cap(JSContext *ctx, JSValueConst promise, int ok_id, int err_id,
                                JSValueConst *data, int n)
{
    JSValue onf = JS_UNDEFINED, onr = JS_UNDEFINED, cap;

    if (ok_id >= 0) {
        onf = JS_NewStepClosure(ctx, ok_id, 1, n, data);
        if (JS_IsException(onf)) return onf;
    }
    if (err_id >= 0) {
        onr = JS_NewStepClosure(ctx, err_id, 1, n, data);
        if (JS_IsException(onr)) { JS_FreeValue(ctx, onf); return onr; }
    }
    cap = JS_PerformPromiseThen(ctx, promise, onf, onr);
    JS_FreeValue(ctx, onf);
    JS_FreeValue(ctx, onr);
    return cap;
}

int stream_react(JSContext *ctx, JSValueConst promise, int ok_id, int err_id,
                        JSValueConst *data, int n)
{
    JSValue cap;

    DCHECK(ok_id >= 0 && err_id >= 0, "a stream reaction was attached before its machines were registered");
    cap = stream_react_cap(ctx, promise, ok_id, err_id, data, n);
    if (JS_IsException(cap)) return -1;
    JS_FreeValue(ctx, cap);
    return 0;
}