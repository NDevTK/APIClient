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
 * expiry is enqueued through JS_EnqueueCallTask, which runs it as a call-root flow: preemptible, forkable and
 * parkable like any other program.
 *
 * IT IS A TASK, AND THE WORD IS LOAD-BEARING. 8.6 says "queue a global task on the timer task source", and
 * 8.1.7 says the event loop performs a MICROTASK CHECKPOINT between one task and the next — so every promise
 * reaction outstanding when a timer expires runs BEFORE the callback does. Enqueued as a microtask instead, a
 * `setTimeout(f, 0)` cut into the middle of a promise chain: `delay(0)` observed a stream write that the chain
 * had not settled yet, and the failure looked like a stream bug rather than an event-loop one. Both hops are
 * tasks — the step that picks the earliest expiry and the callback it fires — so two timers set for the same
 * moment still run in the order they were set.
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

/* THE VIRTUAL CLOCK, for the components that need to stamp a moment (an Event's timeStamp). One clock: a second
   time source would order events differently from the task queue that ran them, which is the one thing the
   virtual clock exists to keep consistent. */
double timer_now(void) { return g_now; }

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

/* THE CLOCK IS THE EVENT LOOP'S — see timer.h. §8.1.7.3's rendering task source becomes due at moments on the
   same clock, so the two are compared rather than raced, and the comparison needs both halves askable. */
double timer_next_due(void)
{
    Timer *t = timer_earliest();
    return t ? t->when : -1;
}

void timer_advance_to(double when)
{
    Timer *t = timer_earliest();

    DCHECK(when >= g_now, "the event loop's clock was asked to move BACKWARDS — a moment already passed is a "
                          "task that should have run before whatever is asking");
    DCHECK(!t || when <= t->when,
           "the event loop's clock was moved PAST a due timer — a task source that steps over an earlier one "
           "runs the page's work in an order the page did not write, which is exactly what virtual time makes "
           "easy to get wrong (a long timeout landing in the middle of the work it was set to outlast)");
    g_now = when;
}

static void (*g_script_sink)(const char *src);

void timer_set_script_sink(void (*queue)(const char *src)) { g_script_sink = queue; }


/* FIRE THE EARLIEST DUE TIMER — the event loop's own step, and the reason it is not a queued task.
 *
 * IT USED TO BE ONE, AND THAT WAS THE BUG. Setting a timer enqueued a "step" task immediately, so a timer
 * announced itself DUE the moment it was created; whenever the queue reached that step it advanced the clock to
 * the expiry, however far away, and fired. Under a wall clock that is harmless — ten seconds is longer than the
 * queue takes to drain — but virtual time makes "ten seconds from now" arrive the instant the step is reached,
 * with other tasks still queued behind it. A page's long timeout therefore landed in the middle of its own
 * work: testharness arms one per file, and it fired while the tests it guards were still queued, marking 140
 * stream subtests that go on to pass as "Test timed out".
 *
 * HTML SAYS WHEN INSTEAD. §8.1.7's event loop runs a task from a task source that has one DUE; the timer
 * source's task becomes due at its expiry. So nothing is queued in advance — the DRIVER asks this, and only
 * when it has nothing else to run, which is exactly when "the clock may move" is true. Everything that is
 * already due (a queued task, a pending microtask, a reply the host owes) is by construction ahead of it.
 *
 * AND THE SECOND HOP IS GONE WITH IT. The step existed because a queued job cannot be pulled back out, so
 * clearTimeout had to work by emptying the entry and letting the step find nothing. With nothing queued in
 * advance, a cleared timer simply is not there to be found — the compensation went with the thing it
 * compensated for.
 *
 * Returns 1 when a timer fired (the driver has work again), 0 when there is none. */
int timer_run_due(JSContext *ctx)
{
    Timer *t = timer_earliest();
    JSValue fn;
    JSValue *args = NULL;
    int n, i;

    if (!t)
        return 0;

    /* 8.6: the clock advances to this task's expiry — every later timer is measured from here. */
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
           counter cutting it off. It needs no re-enqueue now: it is simply the earliest entry again when the
           driver next has nothing to do. */
        t->when = g_now + (t->every > 0 ? t->every : 1);
    } else {
        timer_entry_free(ctx, t);
    }

    /* THE CALLBACK IS STILL A TASK, and still the page's own code — so it goes through the flow machinery
       exactly as before. Only the decision of WHEN moved. */
    JS_EnqueueCallTask(ctx, fn, n, (JSValueConst *)args);
    JS_FreeValue(ctx, fn);
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, args[i]);
    js_free(ctx, args);
    return 1;
}

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
        DCHECK(g_script_sink != NULL,
               "setTimeout was given a STRING handler and this host registered no way to evaluate one — HTML "
               "8.6 evaluates it when the timer fires, and dropping it would lose whatever it was going to do");
        g_script_sink(src);
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
    return JS_NewInt32(ctx, t->id);   /* nothing is queued: the driver asks when the clock may move */
}

/* HTML §8.6's RUN STEPS AFTER A TIMEOUT — see timer.h. It is the SAME timer source `setTimeout` uses, and
 * that is the whole design: the spec's `milliseconds` are measured on one clock and its ordering clause is
 * about invocations on one queue, so an engine algorithm scheduled anywhere else would be ordered against the
 * page's timers by nothing at all.
 *
 * THE COMPLETION STEPS ARE A CALLABLE, which is not a wrapper around C — it is what lets them SUSPEND. §8.6
 * "performs" them at the expiry, and an engine algorithm that runs page code (signalling abort runs the page's
 * abort algorithms and fires an event) has to be a flow when it does. A step-machine function object is
 * already exactly that, and it then rides the same JS_EnqueueCallTask every page callback does, so there is
 * one path from expiry to execution rather than a second one for the engine's own work.
 *
 * THE ORDERING IDENTIFIER IS NOT A PARAMETER, and that is an answer rather than an omission. Its whole effect
 * is that an earlier invocation with the same identifier and a smaller-or-equal `milliseconds` completes
 * first — and on one virtual clock a smaller delay started earlier already has an earlier expiry, with
 * timer_earliest breaking an exact tie by the monotonically increasing handle. So the ordering the identifier
 * buys is the ordering this queue already has, for every identifier at once. */
int timer_after(JSContext *ctx, double ms, JSValueConst steps)
{
    Timer *t;

    DCHECK(JS_IsFunction(ctx, steps),
           "§8.6's completion steps must be callable — they are performed at the expiry and an engine algorithm "
           "that runs page code has to be a flow when it does, which is what a callable makes it");
    if (!(ms >= 0))
        ms = 0;   /* §8.6 clamps a negative or non-finite timeout to 0, for an engine caller as for a page */
    t = timer_slot(ctx);
    t->id = g_next_id++;
    t->when = g_now + ms;
    t->every = -1;   /* §8.6's completion steps are performed once; there is no interval form of this */
    t->fn = JS_DupValue(ctx, steps);
    t->argc = 0;
    t->argv = NULL;
    return t->id;
}

/* §8.6's timerKey, used to cancel — the same clearing `clearTimeout` performs, reached by an engine caller.
   Cancelling a key that names nothing does nothing, which is `clearTimeout`'s own answer and is what a
   caller cancelling a timeout that has already fired needs. */
void timer_cancel(JSContext *ctx, int key)
{
    int i;

    for (i = 0; i < g_timers_n; i++)
        if (g_timers[i].id == key) {
            timer_entry_free(ctx, &g_timers[i]);
            return;
        }
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

/* HTML 8.6.4 queueMicrotask: a MICROTASK, which is the whole of what it is for — it runs inside the current
   checkpoint, ahead of every task, including a timer set for zero. */
static JSValue js_queue_microtask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "queueMicrotask requires a callback");
    JS_EnqueueCallJob(ctx, argv[0], 0, NULL);
    return JS_UNDEFINED;
}

static int g_id_set_timeout, g_id_set_interval, g_id_clear_timeout, g_id_clear_interval;

/* HTML 8.6's own IDL, declared rather than approximated:
     long setTimeout(TimerHandler handler, optional long timeout = 0, any... arguments)
     undefined clearTimeout(optional long handle = 0)
   `handler` is a (DOMString or Function) union and `timeout` is a long, so BOTH can run the page's code — a
   toString on the non-callable arm, a valueOf on the delay — and neither is a string. The `any...` tail is
   simply not listed: a position the IDL does not name is passed through as it is.
   DECLARED ONCE PER AGENT, installed per realm: a declaration builds a pool entry and a member has ONE, and
   `idl_optional_from` names the member the LAST declaration made — so it belongs beside it, here. */
void timer_init(JSContext *ctx)
{
    static const IdlArgType SET_TIMER[2] = { IDL_STRING_UNLESS_CALLABLE, IDL_LONG };
    static const IdlArgType CLEAR_TIMER[1] = { IDL_LONG };

    g_id_set_timeout = idl_method_id(ctx, SET_TIMER, 2, js_set_timer, 0);
    idl_optional_from(1);   /* 8.6: `setTimeout(handler, optional timeout, ...arguments)` */
    g_id_set_interval = idl_method_id(ctx, SET_TIMER, 2, js_set_timer, 1);
    idl_optional_from(1);   /* 8.6: `setInterval(handler, optional timeout, ...arguments)` */
    g_id_clear_timeout = idl_method_id(ctx, CLEAR_TIMER, 1, js_clear_timer, 0);
    idl_optional_from(0);   /* 8.6: `clearTimeout(optional long id = 0)` */
    g_id_clear_interval = idl_method_id(ctx, CLEAR_TIMER, 1, js_clear_timer, 0);
    idl_optional_from(0);   /* 8.6: `clearInterval(optional long id = 0)` */
}

void timer_install(JSContext *ctx, JSValueConst global)
{
    /* THE DRIVER MUST ASK, so it is told where to. Registered like the document's load-stage hook and for the
       same reason: the scheduler may not depend on the browser half by name. A host that registers nothing
       simply never advances the clock, and a page whose work is behind a timer stalls visibly rather than
       having that timer fire out of order. */
    engine_set_timer_hook(timer_run_due);
    JSValue g = (JSValue)global;
    idl_install_method(ctx, g, "setTimeout", 2, g_id_set_timeout);
    idl_install_method(ctx, g, "setInterval", 2, g_id_set_interval);
    idl_install_method(ctx, g, "clearTimeout", 1, g_id_clear_timeout);
    idl_install_method(ctx, g, "clearInterval", 1, g_id_clear_interval);
    JS_SetPropertyStr(ctx, g, "queueMicrotask",
                      JS_NewCFunction(ctx, js_queue_microtask, "queueMicrotask", 1));
}
