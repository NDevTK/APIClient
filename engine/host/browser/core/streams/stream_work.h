/* THE PLUMBING EVERY STREAM COMPONENT SHARES — the Streams Standard's §4 and §5 halves both need it.
 *
 * WHY IT IS ITS OWN FILE. §4's ReadableStream and §5's WritableStream are two interfaces over one design: a
 * queue, a high-water mark, algorithms the page supplied, and promises settled at every edge. Three pieces of
 * that are not either interface's — the RECORD a machine suspends in while it settles promises, the
 * PromiseResolve every "react to what the page returned" step begins with, and the ATTACH that reacts without
 * reading a page-visible `.then`. Written twice they would drift, and the first thing to drift would be the
 * initialisation rule below, which has already been paid for five times.
 *
 * There is no state here and no interface here: it is three operations and the record they operate on. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_STREAM_WORK_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_STREAM_WORK_H
#include <stdbool.h>

#include "quickjs.h"
#include "quickjs-step.h"

typedef struct {
    uint8_t stage;   /* the owning MACHINE's own step */
    uint8_t phase;   /* step_call_run's, for whichever call is in flight */
    /* TWO MORE SEQUENCE STATES, because they NEST: §4.5's pull runs §4.2's error when the page's `pull`
       throws, and the two must not share a byte. What the values mean is the OWNING component's — §4 spells
       them with its own S_ and P_ enums, §5 with its own — and this record only promises there are two. */
    uint8_t settle;
    uint8_t pull;
    JSValue func;    /* the function being called, or the promise a chain is being attached to */
    JSValue value;   /* its argument */
    JSValue chain;   /* a capability's resolve function, while PromiseResolve is in flight */
    JSValue err;     /* the reason a ControllerError sequence is carrying, until the stream adopts it */
    JSValue cb[3];   /* step_call_run's buffer: [this, func, arg] */
} StreamWork;

/* THE SLOTS ARE UNDEFINED BEFORE THEY ARE ANYTHING ELSE. A step state arrives ZEROED, and a zeroed JSValue is
   the INTEGER 0 (JS_TAG_INT is 0) rather than undefined — so a slot read before it is written yields 0, which
   is a real value the page can see. It has cost this project four bugs and it cost this component a fifth: the
   `closed` promise of a drained stream fulfilled with the number 0. Every machine that owns one of these calls
   this on its first entry, so the trap has one place to not be. `stage` is deliberately untouched: the machine
   owns it, and it has usually already been set by the time this runs. */
void stream_work_start(StreamWork *w)
;

void stream_work_visit(JSContext *ctx, StreamWork *w, JSStepVisit *v)
;

void stream_work_release(JSContext *ctx, StreamWork *w)
;

/* PromiseResolve(%Promise%, v) — 27.2.4.7 — as a sub-sequence. §4.5 reacts to what `start` and `pull` RETURNED,
 * which may be a plain value, a page THENABLE, or a promise; the one operation covering all three is a
 * capability whose RESOLVE function is called with it, and calling that function is exactly where 27.2.1.3.2
 * step 8 reads `then` off the page's object. So it is a call request like every other run of the page's code,
 * rather than a `JS_IsFunction(then)` test that would answer a patched thenable wrongly.
 * Takes `w->value`; leaves the capability's promise in `w->func`. */
int stream_promise_of_run(JSContext *ctx, StreamWork *w, int reject, JSValue in,
                               JSValue **out_cb, int *out_argc)
;

/* The reactions' own CAPABILITY, for a caller that must hand it on: §4.2's `from` returns "the result of
   reacting to nextPromise" as its pull algorithm's answer, so §4.5 chains the next pull on THAT rather than on
   the raw next promise. A step id of -1 means no handler for that side, which is how a rejection is left to
   propagate through the capability instead of being caught. */
JSValue stream_react_cap(JSContext *ctx, JSValueConst promise, int ok_id, int err_id,
                                JSValueConst *data, int n)
;

int stream_react(JSContext *ctx, JSValueConst promise, int ok_id, int err_id,
                        JSValueConst *data, int n)
;

#endif
