/* TIMERS — HTML 8.6 "Timers", the timer task source.
 *
 * WHY THIS BLOCKED EVERYTHING. testharness.js finishes a document from inside a timer:
 *
 *     on_event(window, 'load', function() { setTimeout(() => { all_loaded = true;
 *                                            if (tests.all_done()) tests.complete(); }, 0); });
 *
 * With no setTimeout the load handler threw, `all_loaded` stayed false, `all_done()` was false forever and the
 * completion callback never ran — 132 of 175 WPT dom/nodes documents executed every one of their tests and
 * reported none of them. A timer is not a decoration on the event loop; for a great deal of real code it IS the
 * event loop.
 *
 * A TIMER'S TASK IS A FLOW, never a JS_Call. The callback is the page's own code — loops, awaits, concolic
 * branches — so a C activation cannot host it (that is the drive-to-completion the engine aborts on). Each
 * expiry is enqueued through JS_EnqueueCallJob, which runs it as a call-root flow: preemptible, forkable and
 * parkable like any other program.
 *
 * TIME IS VIRTUAL, AND THAT IS THE SPEC'S OWN MODEL. HTML orders the timer task source by expiry, and this
 * engine has no wall clock to wait on: nothing else advances while a timer is outstanding, so the clock jumps to
 * the next expiry. Ordering is therefore (when, id) — `setTimeout(f, 0)` runs before `setTimeout(g, 100)`, and
 * two timers with equal delay run in the order they were set, which is exactly what a browser does. Modelling
 * the ORDER is the point: a page that sequences work across two delays gets the sequence it wrote.
 *
 * CANCELLATION IS REAL, so clearTimeout is not a no-op. A job cannot be pulled back out of the queue once
 * enqueued, so what is enqueued is a STEP of this component — timer_step — and the entry it fires is looked up
 * at that moment. Clearing removes the entry; the step then finds the next one, or nothing. Two hops, each a
 * job, and no C activation ever runs page code.
 *
 * A STRING HANDLER IS A SCRIPT, and an @S sink. `setTimeout("evil()")` compiles and runs its argument in the
 * global scope, which is the engine's existing "queue this source as a script flow" path — the same one an
 * injected <script> takes. It is not evaluated here: this component does not run page code. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/timing/timer.h"
#include "core/idl_args.h"
#include "solver/engine.h"

typedef struct {
    int     id;          /* the handle setTimeout returned; 0 = this slot is free */
    double  when;        /* virtual expiry, in ms since the document started */
    double  every;       /* setInterval's period; -1 for a one-shot */
    JSValue fn;          /* the callback (a function; the string form became a script instead) */
    int     argc;
    JSValue *argv;       /* the extra arguments HTML passes the callback */
} Timer;

static Timer *g_timers;
static int    g_timers_n, g_timers_cap;
static int    g_next_id = 1;
static double g_now;          /* the virtual clock: the expiry of the timer that fired last */

static void timer_entry_free(JSContext *ctx, Timer *t)
{
    int i;
    JS_FreeValue(ctx, t->fn);
    for (i = 0; i < t->argc; i++)
        JS_FreeValue(ctx, t->argv[i]);
    js_free(ctx, t->argv);
    memset(t, 0, sizeof(*t));
}

void timer_reset(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_timers_n; i++)
        if (g_timers[i].id)
            timer_entry_free(ctx, &g_timers[i]);
    js_free(ctx, g_timers);
    g_timers = NULL;
    g_timers_n = g_timers_cap = 0;
    g_next_id = 1;
    g_now = 0;
}

static Timer *timer_slot(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_timers_n; i++)
        if (!g_timers[i].id)
            return &g_timers[i];
    if (g_timers_n == g_timers_cap) {
        int c = g_timers_cap ? g_timers_cap * 2 : 8;
        Timer *a = js_realloc(ctx, g_timers, sizeof(*a) * (size_t)c);
        CHECK(a != NULL, "timer: the timer table allocation failed — a dropped timer loses the flow it owed");
        memset(a + g_timers_cap, 0, sizeof(*a) * (size_t)(c - g_timers_cap));
        g_timers = a; g_timers_cap = c;
    }
    return &g_timers[g_timers_n++];
}

/* The next entry to fire: least expiry, ties broken by the order they were set (the id is monotonic). */
static Timer *timer_earliest(void)
{
    Timer *best = NULL;
    int i;
    for (i = 0; i < g_timers_n; i++) {
        Timer *t = &g_timers[i];
        if (!t->id)
            continue;
        if (!best || t->when < best->when || (t->when == best->when && t->id < best->id))
            best = t;
    }
    return best;
}

static JSValue js_timer_step(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* Enqueue ONE step of this component. Every scheduled expiry has exactly one of these outstanding, so the queue
   holds as many steps as there are timers to fire and a cleared timer simply leaves its step with nothing to do. */
static void timer_enqueue_step(JSContext *ctx)
{
    JSValue step = JS_NewCFunction(ctx, js_timer_step, "", 0);
    CHECK(!JS_IsException(step), "timer: the step closure allocation failed");
    JS_EnqueueCallJob(ctx, step, 0, NULL);
    JS_FreeValue(ctx, step);
}

/* FIRE THE EARLIEST TIMER. Runs as a flow's callee, so it may enqueue but must never call the page's code. */
static JSValue js_timer_step(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Timer *t = timer_earliest();
    JSValue fn;
    JSValue *args = NULL;
    int n, i;

    (void)this_val; (void)argc; (void)argv;
    if (!t)
        return JS_UNDEFINED;   /* every timer this step could have fired was cleared */

    /* 8.6: the clock has advanced to this task's expiry — every later timer is measured from here. */
    DCHECK(t->when >= g_now, "timer: the earliest expiry is behind the virtual clock — time ran backwards");
    g_now = t->when;

    /* Take what the callback needs BEFORE the entry can be reused: an interval re-arms in place, and a one-shot
       frees its slot, so neither the function nor the arguments may still be read out of the entry afterwards. */
    fn = JS_DupValue(ctx, t->fn);
    n = t->argc;
    if (n > 0) {
        args = js_malloc(ctx, sizeof(*args) * (size_t)n);
        CHECK(args != NULL, "timer: the callback argument copy failed");
        for (i = 0; i < n; i++)
            args[i] = JS_DupValue(ctx, t->argv[i]);
    }

    if (t->every >= 0) {
        /* setInterval: the same entry is scheduled again at now + period. UNBOUNDED by design — a page's
           interval never stops on its own, and the WFQ deprioritises one that stops emitting rather than a
           counter cutting it off. */
        t->when = g_now + (t->every > 0 ? t->every : 1);
        timer_enqueue_step(ctx);
    } else {
        timer_entry_free(ctx, t);
    }

    JS_EnqueueCallJob(ctx, fn, n, (JSValueConst *)args);
    JS_FreeValue(ctx, fn);
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, args[i]);
    js_free(ctx, args);
    return JS_UNDEFINED;
}

/* magic 0 = setTimeout, 1 = setInterval */
static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    double delay = 0;
    Timer *t;
    int i;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setTimeout requires a handler");

    /* `timeout` is a Web IDL `long`, ALREADY converted by the declaration — so this reads a number and runs
       none of the page's code. 8.6: a negative or non-finite timeout is clamped to 0. */
    if (argc >= 2) {
        DCHECK(JS_VALUE_GET_TAG(argv[1]) == JS_TAG_INT || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(argv[1])),
               "setTimeout's `timeout` reached the body unconverted — the IDL declaration is what converts it");
        JS_ToFloat64(ctx, &delay, argv[1]);
    }
    if (!(delay >= 0))
        delay = 0;

    /* THE STRING FORM IS A SCRIPT — compiled and run in global scope, which is the sink `setTimeout(str)` is
       known for. It is queued as a script flow (the path an injected <script> takes), never evaluated here:
       running the page's source inside this C activation is exactly what the flow machinery exists to avoid.
       It has no cancellable entry because it is no longer a timer at that point; it is a script. */
    if (!JS_IsFunction(ctx, argv[0])) {
        /* TimerHandler is `(DOMString or Function)`: the declaration converted the non-callable arm to a
           string already, so this reads one rather than running the page's toString from C. */
        const char *src;
        DCHECK(JS_IsString(argv[0]),
               "setTimeout's handler reached the body neither callable nor a string — the TimerHandler union's "
               "conversion is the declaration's, not this body's");
        src = JS_ToCString(ctx, argv[0]);
        if (!src)
            return JS_EXCEPTION;
        engine_queue_script(src);
        JS_FreeCString(ctx, src);
        return JS_NewInt32(ctx, g_next_id++);
    }

    t = timer_slot(ctx);
    t->id = g_next_id++;
    t->when = g_now + delay;
    t->every = magic ? delay : -1;
    t->fn = JS_DupValue(ctx, argv[0]);
    t->argc = argc > 2 ? argc - 2 : 0;
    t->argv = NULL;
    if (t->argc > 0) {
        t->argv = js_malloc(ctx, sizeof(*t->argv) * (size_t)t->argc);
        CHECK(t->argv != NULL, "timer: the callback argument allocation failed");
        for (i = 0; i < t->argc; i++)
            t->argv[i] = JS_DupValue(ctx, argv[i + 2]);
    }
    timer_enqueue_step(ctx);
    return JS_NewInt32(ctx, t->id);
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    int32_t id = 0;
    int i;

    (void)this_val;
    /* `handle` is a Web IDL `long`, converted by the declaration. */
    if (argc < 1)
        return JS_UNDEFINED;   /* 8.6: clearing a handle that names nothing does nothing */
    DCHECK(JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(argv[0])),
           "clearTimeout's `handle` reached the body unconverted");
    if (JS_ToInt32(ctx, &id, argv[0]) < 0)
        return JS_UNDEFINED;
    for (i = 0; i < g_timers_n; i++)
        if (g_timers[i].id == id) {
            timer_entry_free(ctx, &g_timers[i]);
            break;
        }
    return JS_UNDEFINED;
}

/* HTML 8.6.4 queueMicrotask: the callback runs as its own task on the one queue, like every other job here. */
static JSValue js_queue_microtask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "queueMicrotask requires a callback");
    JS_EnqueueCallJob(ctx, argv[0], 0, NULL);
    return JS_UNDEFINED;
}

void timer_install(JSContext *ctx, JSValueConst global)
{
    JSValue g = (JSValue)global;
    /* HTML 8.6's own IDL, declared rather than approximated:
         long setTimeout(TimerHandler handler, optional long timeout = 0, any... arguments)
         undefined clearTimeout(optional long handle = 0)
       `handler` is a (DOMString or Function) union and `timeout` is a long, so BOTH can run the page's code —
       a toString on the non-callable arm, a valueOf on the delay — and neither is a string. The `any...` tail
       is simply not listed: a position the IDL does not name is passed through as it is. */
    {
        static const IdlArgType SET_TIMER[2] = { IDL_STRING_UNLESS_CALLABLE, IDL_LONG };
        static const IdlArgType CLEAR_TIMER[1] = { IDL_LONG };
        idl_install_method(ctx, g, "setTimeout", 2, idl_method_id(ctx, SET_TIMER, 2, js_set_timer, 0));
        idl_install_method(ctx, g, "setInterval", 2, idl_method_id(ctx, SET_TIMER, 2, js_set_timer, 1));
        idl_install_method(ctx, g, "clearTimeout", 1, idl_method_id(ctx, CLEAR_TIMER, 1, js_clear_timer, 0));
        idl_install_method(ctx, g, "clearInterval", 1, idl_method_id(ctx, CLEAR_TIMER, 1, js_clear_timer, 0));
    }
    JS_SetPropertyStr(ctx, g, "queueMicrotask",
                      JS_NewCFunction(ctx, js_queue_microtask, "queueMicrotask", 1));
}
