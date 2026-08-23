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
 * the next expiry. Ordering is therefore (when, insertion order) — `setTimeout(f, 0)` runs before
 * `setTimeout(g, 100)`, and two timers with equal delay run in the order they were set, which is exactly what a
 * browser does. Modelling the ORDER is the point: a page that sequences work across two delays gets the
 * sequence it wrote. The clock itself is the EVENT LOOP's and lives there (core/timing/event_loop.c).
 *
 * §8.7 Timers's MAP OF ACTIVE TIMERS IS THE GLOBAL'S, AND IT TIME-TRAVELS — the two facts that decide where it lives.
 *
 *   PER GLOBAL, because HTML says so: "each object that implements the WindowOrWorkerGlobalScope mixin has a
 *   map of active timers" and a timer identifier of its own. It was ONE agent-wide `js_realloc`'d array with
 *   no Window key, which §7.5.9 step 18 could not clear for an unloading document without taking a same-origin
 *   popup's timers with it — a DCHECK in core/frame/document_lifecycle.c named exactly this and is gone with
 *   the reason for it. So the map is a per-realm record, reached through the realm (core/realm.h), and the
 *   event loop's step asks every fully active document of the agent for its earliest entry.
 *
 *   PER FLOW, because a timer is state one code flow set and another must not see. The array owned a JSValue
 *   callback and a `js_malloc`'d argument vector, captured by nothing: `timer_run_due` picked the earliest
 *   entry of the whole agent regardless of which flow set it, and JS_EnqueueCallTask then attributed it to
 *   whichever flow was RUNNING — so flow A's `setTimeout(cb, 0)` fired in flow B's job queue under B's COW
 *   delta, which is precisely the "one flow's async effect leaks into another's timeline" bug class. A
 *   `clearTimeout` in one arm cleared it for every arm, and the identifier was one counter for the agent so
 *   two forked arms got DIFFERENT handles for the same source line and a replayed decision vector's
 *   `clearTimeout(id)` named something else. The map is therefore a JS Array on the realm's record: its
 *   mutations are property writes the per-flow COW delta already captures, it parks to the cold tier and
 *   resumes with the flow, and both arms of a fork now mint the SAME handle for the same source line.
 *
 * CANCELLATION IS REAL, so clearTimeout is not a no-op. A job cannot be pulled back out of the queue once
 * enqueued, so nothing is enqueued in advance: the driver asks for the earliest entry only when it has nothing
 * else to run, and a cleared entry is simply not there to be found.
 *
 * A STRING HANDLER IS A SCRIPT, and an @S sink. `setTimeout("evil()")` compiles and runs its argument in the
 * global scope, which is the engine's existing "queue this source as a script flow" path — the same one an
 * injected <script> takes. It is not evaluated here: this component does not run page code.
 * THE SECOND HALF OF THAT SENTENCE WAS PROSE ONLY, and the body is where it became true. When this arm landed
 * nothing called an @S detector from anywhere in this engine's production path — `solve_eval_sink` had ONE
 * caller in the tree and it was the fixture — so solve_js.c's whole ECMAScript §12 derivation was reachable
 * only from a test and a real page's `setTimeout(taintedString)` was detected by nothing.
 * THIS ARM IS NOW ONE OF TWO SEAMS RATHER THAN THE ONLY ONE, and the split is ownership. The ECMAScript
 * intrinsics announce themselves (quickjs's JS_SetEvalSinkHook, which solve_init registers), so 19.2.1 eval in
 * both its direct spellings, indirect eval, 20.2.1.1.1's `new Function` and ShadowRealm.prototype.evaluate all
 * reach the same detector without this component. What stays HERE is exactly what the engine cannot see: 8.6's
 * handler is a DOMString this component compiles LATER through `g_script_sink`, so the value has to be
 * announced where it ARRIVES and not where some other algorithm evaluates it.
 * Worse, it did not merely go unnoticed: the
 * union's non-callable arm resolves to DOMString, idl_args.c passes unknown external input across the boundary
 * AS ITSELF so opacity survives the coercion, and the body's `JS_IsString` assertion then fired on it — a
 * canonical XSS sink took the document down on an assert against attacker input. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/timing/timer.h"
#include "core/timing/event_loop.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"   /* §8.7 Timers compiles a STRING handler in the entry global's document */
#include "solver/engine.h"
#include "solver/concolic.h"   /* §8.7 Timers's handler may be unknown external input, which crosses the IDL as itself */
#include "solver/solve.h"      /* …and an unknown handler string is the @S JS-context sink */
#include "solver/decide.h"     /* …and an unknown `timeout` makes §8.7 step 4 and its ORDER two forks, not two guesses */

/* ONE ENTRY OF §8.7 Timers's MAP, as an Array — the shape §8.12 Animation frames's map of animation frame callbacks uses, for the
   reason CLAUDE.md gives: platform data a flow queues is a JS value, so the collector owns it, the COW delta
   captures it and the snapshot machinery parks it. A hole in the map is `undefined` — a free slot, which the
   next timer set in this realm reuses, so a page that sets and clears a million timeouts does not grow the
   array. The extra arguments HTML passes the callback are the entry's tail rather than a second allocation. */
enum { TE_HANDLE = 0, TE_WHEN, TE_EVERY, TE_SEQ, TE_FN, TE_ARG0 };

static int      g_slot = -1;
static JSAtom   g_atom_map = JS_ATOM_NULL, g_atom_next = JS_ATOM_NULL;
static int      g_ready;
static int      g_id_set_timeout, g_id_set_interval, g_id_clear_timeout, g_id_clear_interval;
static void   (*g_script_sink)(uint32_t doc, const char *src);

static void timer_install_map(JSContext *ctx);

/* THIS REALM's record, OWNED. */
static JSValue timer_store(JSContext *ctx)
{
    JSValue st;

    DCHECK(g_ready, "a Window's map of active timers was reached before §8.7 Timers was declared");
    st = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(st),
           "a realm answered §8.7 Timers's map of active timers with no map — every global is given one at creation, "
           "so this realm never ran timer_install_map and its `setTimeout` would register into nothing");
    return st;
}

/* THIS REALM's map, OWNED. */
static JSValue timer_map(JSContext *ctx)
{
    JSValue st = timer_store(ctx), q = JS_GetProperty(ctx, st, g_atom_map);

    JS_FreeValue(ctx, st);
    DCHECK(JS_IsArray(q), "§8.7 Timers's map of active timers lost its entry list");
    return q;
}

/* The length of a JS Array — the map, the entry, or the walk's list of navigables. */
static uint32_t arr_len(JSContext *ctx, JSValueConst q)
{
    JSValue len = JS_GetPropertyStr(ctx, q, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static double timer_entry_num(JSContext *ctx, JSValueConst e, int field)
{
    JSValue v = JS_GetPropertyUint32(ctx, e, (uint32_t)field);
    double d = 0;

    DCHECK(field != TE_WHEN, "an entry's EXPIRY was read as a number — it is the one field of the entry that "
                             "can be unknown external input, so it is read through timer_entry_when");
    DCHECK(JS_IsNumber(v), "an entry of §8.7 Timers's map of active timers lost one of its numeric fields — the "
                           "handle, the period and the insertion order are all written at the set");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

/* THE EXPIRY, WHICH IS THE ONE FIELD OF AN ENTRY THAT MAY BE UNKNOWN — read as a VALUE and never as a double.
 *
 * §8.7's `run steps after a timeout` step 3 is "Set global's map of active timers[timerKey] to startTime plus
 * milliseconds", and `milliseconds` reaches this component from a page that may have written
 * `setTimeout(f, someUnknown)` — an orphan's argument, a reply field, an attacker source. A `long` conversion
 * cannot refuse it (§3.2.4.5 runs ConvertToInt(V, 32, "signed"), which is TOTAL without [EnforceRange]: NaN and
 * both infinities become +0 and everything else is folded modulo 2**32), and §Solver forbids picking a number
 * for it — so the SUM is unknown too, and it is written and read as the value it is. OWNED. */
static JSValue timer_entry_when(JSContext *ctx, JSValueConst e)
{
    JSValue v = JS_GetPropertyUint32(ctx, e, TE_WHEN);

    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "an entry of §8.7 Timers's map of active timers holds an expiry that is neither a moment on the "
           "event loop's clock nor unknown external input — every writer here writes one of the two");
    return v;
}

/* §8.7 step 3's PLUS, run by the ENGINE. `start` is the current high resolution time and `ms` is the converted
   `timeout`; where `ms` is unknown the sum is ECMAScript §13.15.3's `+` over an unknown operand, so the result
   carries the operand's provenance AND — when it has one — the real arithmetic on its example. Composing a
   number here instead would be the invention §Solver forbids, and carrying a bare label instead of running the
   operator would be the recorded transform-expression §Re-execution forbids. OWNED. */
static JSValue timer_expiry(JSContext *ctx, double start, JSValueConst ms)
{
    JSValue sp[2];

    if (!concolic_is(ms)) {
        double d = 0;
        DCHECK(JS_IsNumber(ms), "§8.7's `timeout` reached the expiry neither a number nor unknown external "
                                "input — §3.2.4.5's conversion is the declaration's and answers one of the two");
        JS_ToFloat64(ctx, &d, ms);
        return JS_NewFloat64(ctx, start + d);
    }
    sp[0] = JS_NewFloat64(ctx, start);
    sp[1] = JS_DupValue(ctx, ms);
    /* The hook's stack effect is js_add_slow's: both operands freed, the result placed in sp[-2]. */
    if (!concolic_add_hook(ctx, sp + 2, JS_CONCOLIC_ADD_PLUS))
        DFAIL("§13.15.3's `+` declined an operand this component has already established is UNKNOWN — the "
              "concolic value semantics are not installed in this host, so every operator over an unknown "
              "falls through to the ordinary-object path and the next coercion throws out of an expression "
              "the page never wrote (solver/concolic.h: concolic_install_hooks)");
    return sp[0];
}

/* HOW TWO ENTRIES ARE ORDERED ON THE TIMER TASK SOURCE, AND A FORK WHERE EITHER EXPIRY IS UNKNOWN.
 *
 * HTML §8.7's `run steps after a timeout` states the order as a comparison of the two invocations' expiries:
 * "Wait until any invocations of this algorithm that had the same global and orderingIdentifier, that started
 * before this one, and whose milliseconds is less than or equal to this one's, have completed." That is a
 * comparison the ENGINE makes, never one the page wrote — so where an operand is unknown it is not a value to
 * guess at but a QUESTION WITH TWO REAL ANSWERS, and both of them are programs the page can produce. It FORKS:
 * this flow takes one order and a sibling flow takes the other, on the one frontier, exactly as `if (x < 700)`
 * does. Clamping the unknown to a number instead would both invent a value the code never computed AND silently
 * choose one of the two orders, which is the half of that mistake nothing downstream could ever report.
 *
 * TWO QUESTIONS AND NOT ONE, because §8.7's own key has two components. `<` decides the order; `==` is what the
 * INSERTION ORDER then breaks, and the insertion numbers are concrete on both sides (§8.1.7's counter hands out
 * a real number at every set), so the tie-break inside that arm is decided rather than forked. Folding the two
 * into a single `<=` would put "equal, and set first" into whichever arm the fold happened to pick, and that is
 * the case the tie-break exists for. */
enum { TIMER_REL_BEFORE = 0, TIMER_REL_SAME };

static int timer_rel(JSContext *ctx, JSValueConst a, JSValueConst b, int q)
{
    JSValue pred;
    int d;

    DCHECK(q == TIMER_REL_BEFORE || q == TIMER_REL_SAME,
           "the timer task source was asked a relation that is neither of the two its order is composed of");
    if (!concolic_is(a) && !concolic_is(b)) {
        double x = 0, y = 0;
        JS_ToFloat64(ctx, &x, a);
        JS_ToFloat64(ctx, &y, b);
        return q == TIMER_REL_BEFORE ? x < y : x == y;
    }
    /* The relation is NAMED by the spec clause it comes from, which is what tells the two asks apart in this
       flow's constraint — see solver/concolic.h's concolic_new_rel for why the name and not an opcode. */
    pred = concolic_new_rel(ctx,
                            q == TIMER_REL_BEFORE
                              ? "HTML §8.7 run steps after a timeout: milliseconds <"
                              : "HTML §8.7 run steps after a timeout: milliseconds ==",
                            a, b);
    d = solver_decide(ctx, pred);
    JS_FreeValue(ctx, pred);
    DCHECK(d >= 0,
           "the timer task source's ORDER was asked over an unknown expiry and the solver answered that the "
           "value is not unknown — solver_decide answers -1 for an ordinary value and for a flow that is not "
           "running, and the timer source's order is only ever decided from inside a flow's own step");
    DCHECK(SOLVER_ARM(d) == 0 || SOLVER_ARM(d) == 1,
           "a two-armed decision answered with an arm that is neither of them");
    /* THE ARM IS RIGHT AND THE SIBLING HAS NOWHERE TO BE BUILT, WHICH IS A FACT ABOUT WHERE THIS IS ASKED
       RATHER THAN ABOUT THE QUESTION. solver_decide prepares the other arm's decision and pin blobs and returns
       the FORKED bit, and every consumer of that bit is an ACTIVATION: the interpreter's branch_arm_fork
       snapshots the frame at the `if`, and a step machine's JS_STEP_FORK snapshots the machine at its ask. This
       ask has neither. §8.1.7's choice of which task source runs next is made in the scheduler's idle branch —
       the flow is switched in, so the delta and the constraint are the right ones, but there is no pc, no
       operand stack and no coroutine state to clone, so nothing consumes the prepared blobs and the NEXT fork
       anywhere in the agent trips engine_prepare_fork's "a sibling's snapshot state was prepared while a
       PREVIOUS one was still unconsumed". Measured: 20 documents of one WPT area aborted there, three layers
       from here, with nothing in the message about timers.
       WHAT TO BUILD IS THE SCHEDULER'S OWN VALUE FORK, and solver/decide.h already names it and says why it is
       a different mechanism: "A flow forks over a VALUE as well as over a predicate... Nothing about that is a
       branch — the asking program asked no question it could have answered two ways, so there is no slot to
       record and the sibling's path is its parent's, unchanged" (decide_fork_same_path, assembled by
       engine.c's flow_answer_fork). The event loop's choice of next task IS that shape exactly: no program
       asked it, so the sibling has no resume point to snapshot and instead stands on the parent's path and
       re-reaches this walk, where its recorded arm replays. Build that seam, ask it here instead of
       solver_decide, and the two-arm order is complete.
       DO NOT reach for the other repair — deciding the order from the unknown's EXAMPLE, or from whichever
       entry the walk happened to reach first. Both delete an arm, and the deleted arm is a real program: it is
       the order in which the page's other timer runs second. */
    if (SOLVER_FORKED(d))
        DFAIL("§8.1.7 chose between two timer expiries where one is UNKNOWN, and the fork it needs cannot be "
              "built here: the choice is made in the scheduler's idle branch, which has no activation to "
              "snapshot, so solver_decide's prepared sibling is stranded and the next fork in the agent trips "
              "engine_prepare_fork. Build the scheduler's VALUE fork (solver/decide.h decide_fork_same_path + "
              "engine.c flow_answer_fork — a sibling that stands on its parent's path and re-reaches this "
              "walk) and ask that here instead of the interpreter's branch fork");
    return SOLVER_ARM(d) == 1;
}

/* THE EARLIEST ENTRY OF ONE GLOBAL's MAP: least expiry, ties broken by the order it was set. Returns its index,
   or -1 when this global has none.
 *
 * AND THIS IS WHERE THE ISOLATION IS ASSERTED, because it is the one place every entry a flow can see is
 * looked at. §8.7 Timers's map and the event loop's insertion counter ride the SAME per-flow COW delta, so a flow can
 * only see an entry whose sequence number its OWN counter has already handed out — an entry from a sibling's
 * timeline carries a number this flow never allocated. That is the exact statement of "a timer set by one flow
 * is not visible to another", and it is what fires if either half of the pair is ever moved back out of the
 * heap while the other stays in it. */
/* `*pwhen` is the running best expiry — JS_UNDEFINED on entry, OWNED by the caller on return. */
static int timer_earliest_in(JSContext *ctx, JSValue *pwhen, double *pseq)
{
    JSValue q = timer_map(ctx);
    uint32_t i, n = arr_len(ctx, q);
    double seq_next = event_loop_task_seq_peek(ctx);
    int best = -1;

    DCHECK(JS_IsUndefined(*pwhen),
           "the timer walk was handed a running best it did not allocate — the expiry it carries is an owned "
           "value, so a caller that starts it at anything else leaks the one it was already holding");
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i), when;
        double seq;

        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }   /* a free slot */
        when = timer_entry_when(ctx, e);
        seq = timer_entry_num(ctx, e, TE_SEQ);
        JS_FreeValue(ctx, e);
        DCHECK(seq < seq_next,
               "a flow reached a timer that was queued with an event-loop insertion number this flow never "
               "allocated — §8.7 Timers's map of active timers and §8.1.7's insertion counter ride the SAME per-flow "
               "COW delta, so an entry another flow's timeline queued cannot be visible here. One of the two "
               "is being answered from outside the delta, and the effect is flow A's setTimeout firing in flow "
               "B's job queue under B's heap");
        if (best < 0 || timer_rel(ctx, when, *pwhen, TIMER_REL_BEFORE)
                     || (timer_rel(ctx, when, *pwhen, TIMER_REL_SAME) && seq < *pseq)) {
            JS_FreeValue(ctx, *pwhen);
            best = (int)i;
            *pwhen = when;
            *pseq = seq;
        } else {
            JS_FreeValue(ctx, when);
        }
    }
    JS_FreeValue(ctx, q);
    return best;
}

/* THE EARLIEST ENTRY OF THE WHOLE EVENT LOOP. §8.7 Timers gives every global its own map and §8.1.7 runs them all on
   ONE task source, so the source's next task is the least (expiry, insertion order) across every fully active
   document of this agent — a same-origin popup's `setTimeout(f, 0)` is ahead of this document's
   `setTimeout(g, 100)`, which is what one event loop MEANS. The walk is navigable.c's because the tree is;
   `docctx` is the realm the task is queued on, which is the global whose map holds it and not whichever realm
   happened to drive the loop. */
/* `*pwhen` is JS_UNDEFINED on entry and OWNED by the caller on return, whatever the answer. */
static JSContext *timer_earliest(JSContext *ctx, int *pidx, JSValue *pwhen, double *pseq)
{
    JSValue all = navigable_tree_order(ctx);
    uint32_t i, n = arr_len(ctx, all);
    JSContext *best = NULL;

    DCHECK(JS_IsUndefined(*pwhen),
           "the event loop's timer walk was handed a running best it did not allocate — see timer_earliest_in");
    for (i = 0; i < n; i++) {
        JSValue proxy = JS_GetPropertyUint32(ctx, all, i), when = JS_UNDEFINED;
        JSContext *docctx = window_proxy_realm(ctx, proxy);
        double seq = 0;
        int idx;

        DCHECK(docctx != NULL, "a navigable in this agent's tree order answered with no realm — the walk "
                               "reports only materialized ones, and a materialized navigable has a realm");
        idx = timer_earliest_in(docctx, &when, &seq);
        if (idx >= 0 && (!best || timer_rel(ctx, when, *pwhen, TIMER_REL_BEFORE)
                              || (timer_rel(ctx, when, *pwhen, TIMER_REL_SAME) && seq < *pseq))) {
            JS_FreeValue(ctx, *pwhen);
            best = docctx;
            *pidx = idx;
            *pwhen = when;
            *pseq = seq;
        } else {
            JS_FreeValue(ctx, when);
        }
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, all);
    return best;
}

/* IS THE TIMER SOURCE'S NEXT TASK DUE BEFORE `moment`? — §8.1.7.3 step 2.1's own question, asked as a question.
 *
 * IT HANDED OUT THE MOMENT AND THAT WAS THE WRONG SHAPE. This answered `double`, and its one caller
 * (core/rendering/rendering.c) compared the number against the next frame — which works exactly while every
 * expiry is a number, and a `setTimeout(f, someUnknown)` makes one that is not. A moment nothing computed has no
 * double to hand over, and inventing one here would decide the frame-versus-timer order for the whole program
 * from inside an accessor. The COMPARISON is the thing both sides want, it is the only use the moment was ever
 * put to, and asked here it forks (timer_rel) exactly as any other order over an unknown does. The clock the
 * moments live on is the event loop's — see core/timing/event_loop.h.
 * Returns 0 when no timer is set at all, which is the same answer as "not before" and is right for both: the
 * rendering opportunity proceeds either way. */
int timer_due_before(JSContext *ctx, double moment)
{
    JSValue when = JS_UNDEFINED, m;
    double seq = 0;
    int idx = -1, r;

    if (!timer_earliest(ctx, &idx, &when, &seq))
        return 0;
    m = JS_NewFloat64(ctx, moment);
    r = timer_rel(ctx, when, m, TIMER_REL_BEFORE);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, when);
    return r;
}

void timer_set_script_sink(void (*queue)(uint32_t doc, const char *src)) { g_script_sink = queue; }

/* SET ONE TIMER in `ctx`'s global's map — §8.7 Timers's shared tail for `setTimeout`, `setInterval` and the engine's
   own "run steps after a timeout". Returns the handle §8.7 Timers gives back.
 *
 * THE HANDLE COMES FROM THE GLOBAL and the INSERTION ORDER comes from the event loop, and those are two
 * different counters because they answer two different questions: §8.7 Timers's identifier is what `clearTimeout`
 * names, and it is per-global (two same-origin documents both hand out 1); the insertion order breaks a tie
 * between two globals' entries on the one task source, so it has to be the loop's. Both ride the per-flow
 * delta, which is why two arms of a fork mint the SAME handle for the same source line. */
/* `delay` IS A VALUE AND NOT A DOUBLE, because §8.7's `timeout` can be unknown external input and the expiry
   this writes is then unknown too — see timer_expiry. `every` is still a double: an unknown PERIOD is a
   different question and js_set_timer refuses it at the door, naming why. */
static int timer_set(JSContext *ctx, JSValueConst delay, double every, JSValueConst fn, int argc,
                     JSValueConst *argv)
{
    JSValue st = timer_store(ctx), q, entry, nv;
    uint32_t handle = 0, i, slot, n;

    nv = JS_GetProperty(ctx, st, g_atom_next);
    JS_ToUint32(ctx, &handle, nv);
    JS_FreeValue(ctx, nv);
    DCHECK(handle >= 1, "§8.7 Timers's timer identifier started below 1 — 0 is the handle a page gets back for "
                        "nothing, and `clearTimeout(0)` must name no entry");
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, handle + 1));
    JS_FreeValue(ctx, st);

    entry = JS_NewArray(ctx);
    CHECK(!JS_IsException(entry), "timer: OOM recording a timer — a dropped one is work the page asked for "
                                  "and never gets");
    JS_SetPropertyUint32(ctx, entry, TE_HANDLE, JS_NewUint32(ctx, handle));
    JS_SetPropertyUint32(ctx, entry, TE_WHEN, timer_expiry(ctx, event_loop_now(ctx), delay));
    JS_SetPropertyUint32(ctx, entry, TE_EVERY, JS_NewFloat64(ctx, every));
    JS_SetPropertyUint32(ctx, entry, TE_SEQ, JS_NewFloat64(ctx, event_loop_task_seq(ctx)));
    JS_SetPropertyUint32(ctx, entry, TE_FN, JS_DupValue(ctx, fn));
    for (i = 0; i < (uint32_t)argc; i++)
        JS_SetPropertyUint32(ctx, entry, TE_ARG0 + i, JS_DupValue(ctx, argv[i]));

    q = timer_map(ctx);
    n = arr_len(ctx, q);
    for (slot = 0; slot < n; slot++) {   /* reuse a cleared entry's slot before growing the map */
        JSValue e = JS_GetPropertyUint32(ctx, q, slot);
        int free_slot = !JS_IsObject(e);
        JS_FreeValue(ctx, e);
        if (free_slot) break;
    }
    JS_SetPropertyUint32(ctx, q, slot, entry);
    JS_FreeValue(ctx, q);
    return (int)handle;
}

/* CLEAR ONE HANDLE from `ctx`'s global's map. §8.7 Timers: a handle that names nothing does nothing, which is what a
   page clearing a timeout that has already fired needs. */
static void timer_clear(JSContext *ctx, uint32_t want)
{
    JSValue q = timer_map(ctx);
    uint32_t i, n = arr_len(ctx, q);

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i);
        int hit;

        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        hit = (uint32_t)timer_entry_num(ctx, e, TE_HANDLE) == want;
        JS_FreeValue(ctx, e);
        if (hit) { JS_SetPropertyUint32(ctx, q, i, JS_UNDEFINED); break; }
    }
    JS_FreeValue(ctx, q);
}

/* HTML §7.5.9 step 18's "clear window's map of active timers", for the unloading document's global — the step
 * that could not be written while this component kept ONE agent-wide list with no Window key, because clearing
 * what existed would have taken a same-origin popup's timers with it.
 *
 * IT IS THE MAP AND NOT THE IDENTIFIER. §8.7 Timers's timer identifier keeps counting: the document is going away, so
 * nothing will ask it again, and resetting it would be a second statement of a fact the destruction already
 * makes. */
void timer_clear_map(JSContext *ctx)
{
    JSValue q = timer_map(ctx);
    uint32_t i, n = arr_len(ctx, q);

    /* ENTRY BY ENTRY, not by truncating `length`. Each element write is what the per-flow COW delta captures;
       a length truncation deletes the elements underneath it with nothing captured for them, so unapplying
       this flow would restore the length and not the entries a sibling still holds. */
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, q, i, JS_UNDEFINED);
    JS_FreeValue(ctx, q);
}

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
 * AND IT IS THE RUNNING FLOW'S TIMER, WITHOUT ASKING WHICH FLOW THAT IS. The driver calls this from inside a
 * flow's step, so that flow's COW delta is the one applied: every map it walks holds exactly the entries that
 * flow set or inherited, and a sibling's are not in the heap to be found. A flow whose timer is due while
 * ANOTHER flow is being stepped therefore does not fire here, and must not — its expiry is a moment on ITS
 * clock, and the WFQ already owes it a turn. It fires when that flow is next scheduled and reaches this same
 * idle branch, which is the one system CLAUDE.md allows: no timer wheel beside the frontier, no cross-flow
 * wakeup, nothing to re-rank.
 *
 * Returns 1 when a timer fired (the driver has work again), 0 when there is none. */
int timer_run_due(JSContext *ctx)
{
    JSValue whenv = JS_UNDEFINED;
    double when = 0, seq = 0, every;
    int idx = -1;
    JSContext *docctx = timer_earliest(ctx, &idx, &whenv, &seq);
    JSValue q, e, fn, *args = NULL;
    uint32_t i, n;

    if (!docctx)
        return 0;

    /* THE ONE THING AN UNDECIDED MOMENT STILL COSTS, AND IT IS THE CLOCK ITSELF.
       Everything up to here is answered: the expiry is written as the value §13.15.3's `+` computed, the ORDER
       against every other entry is a fork whose two arms are two real programs, and the arm that runs a
       CONCRETE timer while an unknown one waits is complete — that is the arm a document's own work runs in.
       What is left is the arm that FIRES this one. §8.7's map of active timers holds `startTime plus
       milliseconds` and this engine's virtual clock jumps there, so firing a timer whose expiry is unknown puts
       the EVENT LOOP'S CLOCK at an unknown moment — and that is not a fidelity gap to paper over, it is the
       truth: after a timer with an unknown delay has fired, how much time has passed IS unknown, and
       `performance.now()` must say so.
       WHAT TO BUILD IS THE CONCOLIC CLOCK, and it is core/timing/event_loop.c's field rather than this file's:
       `eventLoopNow` becomes a value, event_loop_now answers one, and event_loop_advance_to takes one. Three
       sub-questions come with it and none of them is optional — (1) its own MONOTONICITY assert (`when >= now`)
       is undecidable over an unknown and must be carried by the DECISION that chose this entry rather than
       re-evaluated, because the fork that picked it is exactly the proof; (2) HR-TIME §4's coarsening
       (core/timing/hr_time.c: floor to the resolution grid, minus the time origin) becomes a derivation over
       the unknown, and hr_time_relative's `coarse >= origin` assert has the same problem as (1); (3)
       core/rendering/rendering.c's `next < now` and its `last render opportunity time` become forks of their
       own, the second of which asserts `when <= now`. Do NOT reach for a number here: any moment picked is one
       the code never computed AND a silent choice of one of the two orders, which is the half of the mistake
       nothing downstream could report.
       DO NOT clamp, and do not read the example either — this entry reached the front of the queue precisely
       because a FORK put it there, so an example would answer a question an arm has already decided. */
    if (concolic_is(whenv))
        DFAIL("the timer the event loop selected expires at an UNKNOWN moment, so firing it moves §8.1.7's "
              "virtual clock to a moment no arm has decided. Build the concolic clock in "
              "core/timing/event_loop.c — see the paragraph at this line for its three sub-questions "
              "(monotonicity carried by the deciding fork, HR-TIME §4 coarsening as a derivation, and "
              "core/rendering/rendering.c's frame-versus-timer comparison as a fork)");
    JS_ToFloat64(docctx, &when, whenv);
    JS_FreeValue(docctx, whenv);

    /* 8.6: the clock advances to this task's expiry — every later timer is measured from here. It is the ONE
       source that is due, so nothing else constrains the move. */
    event_loop_advance_to(docctx, when, when);

    q = timer_map(docctx);
    e = JS_GetPropertyUint32(docctx, q, (uint32_t)idx);
    DCHECK(JS_IsObject(e), "the timer the event loop selected is no longer in its global's map — nothing runs "
                           "between the selection and this read");
    /* Take what the callback needs BEFORE the entry can be reused: an interval re-arms in place and a one-shot
       frees its slot, so neither the function nor the arguments may still be read out of the entry after. */
    fn = JS_GetPropertyUint32(docctx, e, TE_FN);
    DCHECK(JS_IsFunction(docctx, fn),
           "§8.7 Timers's map held an entry whose handler is not callable — the string form became a script instead "
           "of an entry, so nothing else can be in the map");
    n = arr_len(docctx, e);
    DCHECK(n >= TE_ARG0,
           "an entry of §8.7 Timers's map of active timers is shorter than its own fixed head — the handle, expiry, "
           "period, insertion order and handler are all written at the set, and the extra arguments HTML "
           "passes the callback are what follows them");
    n -= TE_ARG0;
    if (n > 0) {
        args = js_malloc(docctx, sizeof(*args) * (size_t)n);
        CHECK(args != NULL, "timer: the callback argument copy failed");
        for (i = 0; i < n; i++)
            args[i] = JS_GetPropertyUint32(docctx, e, TE_ARG0 + i);
    }
    every = timer_entry_num(docctx, e, TE_EVERY);
    if (every >= 0) {
        /* setInterval: the same entry is scheduled again at now + period. UNBOUNDED by design — a page's
           interval never stops on its own, and the WFQ deprioritises one that stops emitting rather than a
           counter cutting it off. It needs no re-enqueue now: it is simply the earliest entry again when the
           driver next has nothing to do. Its insertion order is REFRESHED, because the re-arm is a new task on
           the source and a timer set in the meantime for the same moment was set first.
           §8.7 Timers does not re-arm by adding a period: the task's last step "perform the timer initialization steps
           AGAIN, given global, handler, timeout, arguments, true, and id" — the SAME algorithm, so step 5 runs
           on the re-arm exactly as it ran on the `setInterval` call. */
        DCHECK(every >= 4,
               "§8.7 Timers's TIMER NESTING LEVEL is not built, and a repeating timer whose `timeout` is under 4ms is "
               "the case whose answer it decides. Step 3 reads the level off the currently running task ("
               "\"if the surrounding agent's event loop's currently running task is a task that was created by "
               "this algorithm, then let nesting level be the task's timer nesting level; otherwise 0\"), the "
               "task's last step re-runs the WHOLE algorithm for a repeat, and steps 10 and 11 (\"Increment "
               "nestingLevel by one\" / \"Set task's timer nesting level to nestingLevel\") carry it onto the "
               "task it queues — so an interval's re-arms walk 1,2,3,... and step 5 (\"if nesting level is "
               "greater than 5, and timeout is less than 4, then set timeout to 4\") takes over from the sixth. "
               "The same three steps govern a RECURSIVE `setTimeout(f,0)`, which is the more common spelling and "
               "which this component gets wrong in the same way and cannot assert here: with no clamp its chain "
               "re-arms at the same virtual moment for ever, so `timer_earliest` picks it every time and a "
               "`setTimeout(g,1)` set beside it NEVER RUNS — a livelock real Chrome does not have, on the "
               "ordering that is the whole point of a virtual clock. THE LEVEL BELONGS TO THE TASK, which is why "
               "it cannot be a field of this component: Blink keeps it on the DOMTimer and sets the context's "
               "current level around the fire, and this engine's timer task is a FlowJob (solver/flow.h) that "
               "runs after this function has returned. So carry it there — JS_EnqueueCallTask takes the level, "
               "the drainer publishes it as the running task's, `js_set_timer` reads it (0 when the running job "
               "is not a timer task) and applies steps 4-5, and this re-arm passes its own plus one. Then the "
               "substitute below deletes with this assert.");
        /* THE RE-ARM USES §8.7 Timers's OWN NUMBER, not a number of this component's. `1` stood here and appears
           nowhere in §8.7 Timers — it was doing the job step 5 does, badly: it is wrong for every re-arm of a
           zero-period interval, where the spec says 0 for the first five and 4 for every one after. 4 is the
           steady state of an interval that outlives its nesting level, so a release build (where the assert
           above is compiled out) is the spec for the unbounded tail rather than for a finite prefix. */
        JS_SetPropertyUint32(docctx, e, TE_WHEN, JS_NewFloat64(docctx, when + (every >= 4 ? every : 4)));
        JS_SetPropertyUint32(docctx, e, TE_SEQ, JS_NewFloat64(docctx, event_loop_task_seq(docctx)));
    } else {
        JS_SetPropertyUint32(docctx, q, (uint32_t)idx, JS_UNDEFINED);
    }
    JS_FreeValue(docctx, e);
    JS_FreeValue(docctx, q);

    /* THE CALLBACK IS STILL A TASK, and still the page's own code — so it goes through the flow machinery
       exactly as before. Only the decision of WHEN moved. It is queued in the realm whose map held it, which
       is §8.7 Timers's "queue a GLOBAL task": the global is the one the timer was set on, never whichever realm the
       driver happens to be stepping. */
    JS_EnqueueCallTask(docctx, fn, (int)n, (JSValueConst *)args);
    JS_FreeValue(docctx, fn);
    for (i = 0; i < n; i++)
        JS_FreeValue(docctx, args[i]);
    js_free(docctx, args);
    return 1;
}

static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    double delay = 0, every;
    JSValue timeout;
    int handle;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setTimeout requires a handler");

    /* `timeout` is a Web IDL `long`, so §3.2.4.5's ConvertToInt(V, 32, "signed") already ran in the
       declaration — UNLESS the page passed unknown external input, which crosses an IDL boundary as itself so
       that opacity survives the coercion. The tag test that stood here asserted the first case and fired on
       the second: `setTimeout(f, timeout * n)` with an unknown `n` is a page's ordinary arithmetic, and
       §Offensive names a forced-exec flow meeting opaque input as the exploration surface rather than a
       broken invariant. idl_number_of answers both — the converted Number for a real value, and for an
       unknown the SAME §3.2.4.5 conversion run on that value's own EXAMPLE.
       THE EXAMPLE IS THE ORDERING KEY, AND THAT IS ALL IT IS. HTML §8.7 Timers orders one invocation against
       another in `run steps after a timeout`: "Wait until any invocations of this algorithm that had the same
       global and orderingIdentifier, that started before this one, and whose milliseconds is less than or
       equal to this one's, have completed" — a comparison the ENGINE makes, never one the page wrote. This
       component realises it as (now + timeout, insertion order) on the virtual clock, so what it needs from
       the value is one number.
       AN UNKNOWN WITH NO EXAMPLE IS NOT A MISSING NUMBER, IT IS A FORK, and §8.7 states the fork itself. Step
       4 is "If timeout is less than 0, then set timeout to 0" — a comparison over exactly this value, whose
       TRUE arm the SPEC then assigns a concrete 0 to. So one arm of every such call runs on a number nothing
       invented, and the other keeps the unknown and orders by the fork timer_rel makes. This is the
       concretize-on-pin rule with the spec doing the pinning: `x === 'admin'` concretizes because the code
       determined the value, and step 4's true arm concretizes because the ALGORITHM determined it. */
    if (argc >= 2 && !idl_number_of(ctx, IDL_LONG, argv[1], &delay)) {
        /* §8.7 step 4 over an unknown: both outcomes are feasible (§3.2.4.5's `long` spans
           [-2147483648, 2147483647] and ConvertToInt is TOTAL, so nothing has excluded either sign), so BOTH
           arms run — this flow takes one and a sibling is parked for the other. */
        int d = solver_outcome(ctx, argv[1],
                               "HTML §8.7 timer initialization steps step 4 (timeout < 0)", 2);
        DCHECK(d >= 0,
               "§8.7 step 4 was asked about an unknown `timeout` and the solver answered that the value is "
               "not unknown, or that no flow is running — a page's `setTimeout` runs inside a flow's own "
               "step, and idl_number_of has already established the value is unknown external input");
        DCHECK(SOLVER_ARM(d) == 0 || SOLVER_ARM(d) == 1,
               "a two-armed decision answered with an arm that is neither of them");
        /* AND THE SIBLING NEEDS A DECLARATION THIS MEMBER DOES NOT HAVE — the same gap timer_rel names, at the
           other end of the algorithm. solver_outcome prepares the other arm and returns the FORKED bit, and
           the only thing that consumes that bit for a C builtin is the interpreter's JS_STEP_FORK driver,
           which reaches it because the builtin DECLARED ITSELF A STEP MACHINE and yielded its ask. This body is
           a plain JSCFunction (idl_method_id), so it is already inside its C activation when it asks and there
           is no machine state to snapshot the other arm at; the prepared blobs are then stranded exactly as
           they are in timer_rel.
           WHAT TO BUILD IS THE DECLARATION: §8.7's members become step machines (idl_method_id_step, the entry
           the synchronous host read already uses), step 4's comparison becomes the machine's outcome ask, and
           the driver snapshots the flow AT the ask so the sibling re-enters the algorithm holding the other
           arm. Nothing about the QUESTION changes — the true arm is still the spec's own 0. */
        if (SOLVER_FORKED(d))
            DFAIL("§8.7 step 4 forked over an unknown `timeout` and the sibling has nowhere to be built: "
                  "`setTimeout` is declared as a plain C body (idl_method_id), so it is already inside its "
                  "activation when it asks and the interpreter's JS_STEP_FORK driver — the one consumer of "
                  "solver_outcome's FORKED bit for a C builtin — never sees it, leaving the prepared sibling "
                  "stranded for the next fork in the agent to trip over. Declare §8.7's members as step "
                  "machines (idl_method_id_step) and make step 4's comparison the machine's outcome ask");
        if (SOLVER_ARM(d) == 1)
            timeout = JS_NewFloat64(ctx, 0);   /* step 4's own assignment, on its own arm */
        else
            timeout = JS_DupValue(ctx, argv[1]);
    } else {
        /* §3.2.4.5's own postcondition, which is what the tag test was reaching for and could not state:
           ConvertToInt takes the integer part modulo 2**32 and folds it into range, so a `long` is ALWAYS an
           integer in [-2147483648, 2147483647] — NaN and the infinities became +0 inside the conversion. */
        DCHECK(delay >= -2147483648.0 && delay <= 2147483647.0 && delay == (double)(int32_t)delay,
               "§3.2.4.5's `long` conversion answered something that is not a long — its result is the integer "
               "part taken modulo 2**32 and folded into range, so a value outside it, or one with a fraction, "
               "means this position was never converted by anything");
        /* §8.7 step 4: "If timeout is less than 0, then set timeout to 0." */
        if (delay < 0)
            delay = 0;
        timeout = JS_NewFloat64(ctx, delay);
    }

    /* THE STRING FORM IS A SCRIPT — compiled and run in global scope, which is the sink `setTimeout(str)` is
       known for. It is queued as a script flow (the path an injected <script> takes), never evaluated here:
       running the page's source inside this C activation is exactly what the flow machinery exists to avoid.
       It has no cancellable entry because it is no longer a timer at that point; it is a script. */
    if (!JS_IsFunction(ctx, argv[0])) {
        /* TimerHandler is `(DOMString or Function)`: the declaration converted the non-callable arm to a
           string already, so this reads one rather than running the page's toString from C. */
        const char *src;
        JSValue st, nv;
        uint32_t handle = 0;

        /* §8.7 step 4 ran above, where the spec puts it, and this arm makes no entry for its result to be the
           expiry of — every exit below is a return, so the converted `timeout` is released once, here. */
        JS_FreeValue(ctx, timeout);

        /* §8.7 Timers's STRING ARM OVER UNKNOWN EXTERNAL INPUT IS A CODE-EXECUTION SINK, NOT A BROKEN INVARIANT.
           The assertion here used to be `JS_IsString(argv[0])`, and it FIRED on `setTimeout(location.hash)`.
           The union's non-callable arm resolves to DOMString, and idl_args.c passes unknown external input
           across the boundary AS ITSELF so that opacity survives the coercion — so a concolic arrives here
           neither callable nor a string, and asserting on it is asserting on ATTACKER INPUT, which is the one
           thing §Offensive programming names as never a @WHY. A canonical XSS sink took the document down.
           AND IT IS THE @S JS-CONTEXT SINK THIS ENGINE HAD NO PRODUCTION CALL SITE FOR. §8.7 Timers creates a classic
           script from the handler, which is an eval in every sense solve.h means, and `solve_eval_sink` had
           exactly one caller in the tree: the fixture. So solve_js.c's whole §12 derivation was reachable only
           from a test, and a real page's `setTimeout(taintedString)` was detected by nothing.
           WHAT IS QUEUED IS NOTHING, AND THAT IS ABSENCE RATHER THAN A DROP: this engine cannot compile a
           string it does not have, and the unknown handler names no program. Supplying one is exactly what the
           @S search does — a candidate run substitutes a concrete breakout at this source, at which point the
           value IS a string, the branch below queues it as §8.7 Timers requires, and a marker in it fires there. */
        if (concolic_is(argv[0])) {
            solve_eval_sink(ctx, argv[0]);
            st = timer_store(ctx);
            nv = JS_GetProperty(ctx, st, g_atom_next);
            JS_ToUint32(ctx, &handle, nv);
            JS_FreeValue(ctx, nv);
            JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, handle + 1));
            JS_FreeValue(ctx, st);
            return JS_NewInt32(ctx, (int32_t)handle);
        }
        DCHECK(JS_IsString(argv[0]),
               "setTimeout's handler reached the body neither callable, nor a string, nor unknown external "
               "input — the TimerHandler union's conversion is the declaration's, not this body's");
        src = JS_ToCString(ctx, argv[0]);
        if (!src)
            return JS_EXCEPTION;
        DCHECK(g_script_sink != NULL,
               "setTimeout was given a STRING handler and this host registered no way to evaluate one — HTML "
               "8.6 evaluates it when the timer fires, and dropping it would lose whatever it was going to do");
        /* IN THIS REALM'S DOCUMENT — §8.7 Timers compiles the string with the entry global object's settings, which
           is the Window whose `setTimeout` was called and not the agent's root. */
        g_script_sink(document_doc(ctx), src);
        JS_FreeCString(ctx, src);
        /* §8.7 Timers still hands back a handle from THIS global's identifier, and it still names no entry — the
           script is queued, and there is nothing left for `clearTimeout` to find. */
        st = timer_store(ctx);
        nv = JS_GetProperty(ctx, st, g_atom_next);
        JS_ToUint32(ctx, &handle, nv);
        JS_FreeValue(ctx, nv);
        JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, handle + 1));
        JS_FreeValue(ctx, st);
        return JS_NewInt32(ctx, (int32_t)handle);   /* §8.7 Timers's return type is a `long` */
    }

    /* THE PERIOD IS A DOUBLE AND AN UNKNOWN ONE IS A DIFFERENT QUESTION FROM AN UNKNOWN DELAY, which is why it
       is refused HERE rather than absorbed. A one-shot's unknown expiry is ordered by a fork and is then done
       with; a REPEAT re-arms from its own period at every fire, and §8.7's task's last step "perform the timer
       initialization steps AGAIN" runs step 5 on each of those — the clamp that needs the TIMER NESTING LEVEL,
       which this component does not have and whose absence the re-arm's own assert in timer_run_due already
       names in full. An unknown period would therefore be re-armed by a rule the engine cannot state, for
       ever. Build the nesting level first; the two meet at step 5. */
    if (magic && concolic_is(timeout))
        DFAIL("`setInterval` was given a PERIOD that is unknown external input, and §8.7's repeat re-runs the "
              "whole timer initialization steps at every fire — so step 5 (\"If nestingLevel is greater than "
              "5, and timeout is less than 4, then set timeout to 4\") governs every re-arm and this "
              "component has no TIMER NESTING LEVEL to evaluate it with. Build the level first (timer_run_due "
              "carries the whole design in the assert on its re-arm), then this reads it and the re-arm's "
              "expiry is §8.7's own number over the unknown rather than a rule invented here");
    /* A one-shot's period is -1 and is never read; a repeat's is the converted `timeout`, which the refusal
       above has established is a number — so nothing here runs ToNumber over an unknown. */
    every = -1;
    if (magic)
        JS_ToFloat64(ctx, &every, timeout);
    handle = timer_set(ctx, timeout, every, argv[0],
                       argc > 2 ? argc - 2 : 0, argc > 2 ? argv + 2 : NULL);
    JS_FreeValue(ctx, timeout);
    return JS_NewInt32(ctx, handle);
    /* nothing is queued: the driver asks when the clock may move */
}

/* HTML §8.7 Timers's RUN STEPS AFTER A TIMEOUT — see timer.h. It is the SAME timer source `setTimeout` uses, and
 * that is the whole design: the spec's `milliseconds` are measured on one clock and its ordering clause is
 * about invocations on one queue, so an engine algorithm scheduled anywhere else would be ordered against the
 * page's timers by nothing at all.
 *
 * THE COMPLETION STEPS ARE A CALLABLE, which is not a wrapper around C — it is what lets them SUSPEND. §8.7 Timers
 * "performs" them at the expiry, and an engine algorithm that runs page code (signalling abort runs the page's
 * abort algorithms and fires an event) has to be a flow when it does. A step-machine function object is
 * already exactly that, and it then rides the same JS_EnqueueCallTask every page callback does, so there is
 * one path from expiry to execution rather than a second one for the engine's own work.
 *
 * THE ORDERING IDENTIFIER IS NOT A PARAMETER, and that is an answer rather than an omission. Its whole effect
 * is that an earlier invocation with the same identifier and a smaller-or-equal `milliseconds` completes
 * first — and on one virtual clock a smaller delay started earlier already has an earlier expiry, with the
 * event loop's insertion order breaking an exact tie. So the ordering the identifier buys is the ordering this
 * queue already has, for every identifier at once. */
int timer_after(JSContext *ctx, double ms, JSValueConst steps)
{
    DCHECK(JS_IsFunction(ctx, steps),
           "§8.7 Timers's completion steps must be callable — they are performed at the expiry and an engine algorithm "
           "that runs page code has to be a flow when it does, which is what a callable makes it");
    if (!(ms >= 0))
        ms = 0;   /* §8.7 Timers clamps a negative or non-finite timeout to 0, for an engine caller as for a page */
    /* §8.7 Timers's completion steps are performed once; there is no interval form of this. */
    /* AN ENGINE CALLER'S `milliseconds` IS A REAL DOUBLE, which is the difference between this entry and the
       page's: the caller is an algorithm of this engine and the number is one the engine computed, so there is
       no unknown here to order by a fork. It crosses into the map as the value every expiry is. */
    {
        JSValue msv = JS_NewFloat64(ctx, ms);
        int key = timer_set(ctx, msv, -1, steps, 0, NULL);
        JS_FreeValue(ctx, msv);
        return key;
    }
}

/* §8.7 Timers's timerKey, used to cancel — the same clearing `clearTimeout` performs, reached by an engine caller in
   the global it scheduled the steps on. */
void timer_cancel(JSContext *ctx, int key)
{
    timer_clear(ctx, (uint32_t)key);
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    double id = 0;

    (void)this_val; (void)magic;
    if (argc < 1)
        return JS_UNDEFINED;   /* §8.7: clearing a handle that names nothing does nothing */
    /* `handle` is a Web IDL `long`, converted by the declaration — or unknown external input crossing as
       itself, whose number is that same §3.2.4.5 conversion run on its example. The tag test that stood here
       asserted against attacker input for the same reason `setTimeout`'s did, and in the same member pair. */
    if (!idl_number_of(ctx, IDL_LONG, argv[0], &id))
        DFAIL("clearTimeout was given a `handle` that is unknown external input carrying NO EXAMPLE, so it "
              "names no particular entry of §8.7's map of active timers. It cannot clear nothing — a handle "
              "the domain permits is one some arm does clear — and it cannot clear every entry, which is "
              "every arm at once. WHAT TO BUILD is the fork over WHICH entry the handle names: solver_outcome "
              "over the value with one completion per live entry plus the one that names none. It is the same "
              "fork §8.7's ORDERING needs in js_set_timer above and is built with it, because both are the "
              "one question of what an unknown integer denotes among this global's timers.");
    timer_clear(ctx, (uint32_t)(int32_t)id);
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

/* HTML 8.6's own IDL, declared rather than approximated:
     long setTimeout(TimerHandler handler, optional long timeout = 0, any... arguments)
     undefined clearTimeout(optional long handle = 0)
   `handler` is a (DOMString or Function or TrustedScript) union and `timeout` is a long, so BOTH can run the
   page's code — a toString on the non-callable arm, a valueOf on the delay — and neither is a string.
   THE `any...` TAIL IS NOT DECLARED, AND IT IS THEREFORE DROPPED — this said the opposite ("a position the
   IDL does not name is passed through as it is") and that is measurably false: a NON-variadic member's
   argument count is min(passed, declared) (idl_args.c), so this body can never see a third position and
   `setTimeout(f, 0, x)` invokes f with no arguments where §8.7 step 9's task invokes it "given arguments".
   Declaring it is not one call to idl_variadic: a variadic member repeats its LAST declared type for the
   tail, which here is IDL_LONG and would coerce every extra argument to a number, so the type list grows a
   third IDL_ANY entry and the member is declared through idl_method_id_ext with nargs 3 and variadic true.
   DECLARED ONCE PER AGENT, installed per realm: a declaration builds a pool entry and a member has ONE, and
   `idl_optional_from` names the member the LAST declaration made — so it belongs beside it, here. */
void timer_init(JSContext *ctx)
{
    static const IdlArgType SET_TIMER[2] = { IDL_STRING_UNLESS_CALLABLE, IDL_LONG };
    static const IdlArgType CLEAR_TIMER[1] = { IDL_LONG };

    DCHECK(!g_ready, "timer_init ran twice — §8.7 Timers's members are declared once per agent");
    /* THE `= 0` ABOVE IS PART OF THE DECLARATION, and it was written in the comment and not in the code.
       §3.6 step 14.2 gives an optional argument whose IDL writes `= …` THAT value, while a position with no
       declared default is ABSENT — so all four members reached their bodies with an undefined where the IDL
       guarantees a number, and each body would have had to invent the 0 its declaration owes it. */
    g_id_set_timeout = idl_method_id(ctx, SET_TIMER, 2, js_set_timer, 0);
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_ZERO, NULL);
    g_id_set_interval = idl_method_id(ctx, SET_TIMER, 2, js_set_timer, 1);
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_ZERO, NULL);
    g_id_clear_timeout = idl_method_id(ctx, CLEAR_TIMER, 1, js_clear_timer, 0);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_ZERO, NULL);
    g_id_clear_interval = idl_method_id(ctx, CLEAR_TIMER, 1, js_clear_timer, 0);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_ZERO, NULL);
    g_atom_map = JS_NewAtom(ctx, "activeTimers");
    g_atom_next = JS_NewAtom(ctx, "timerIdentifier");
    CHECK(g_atom_map != JS_ATOM_NULL && g_atom_next != JS_ATOM_NULL,
          "timer: the map's own keys could not be interned");
    g_slot = realm_value_declare(ctx, "§8.7 Timers map of active timers");
    g_ready = 1;
    realm_declare_intrinsic(timer_install_map);
    /* THE DRIVER MUST ASK, so it is told where to. Registered like the document's load-stage hook and for the
       same reason: the scheduler may not depend on the browser half by name.
       IT IS AN AGENT FACT AND WAS BEING STATED PER REALM. This line was in timer_install — the per-DOCUMENT
       half — so every realm re-registered the same pointer with the ONE frontier, which is the shape
       §per-realm-fact warns about read backwards: one fact, answered from as many places as there are
       documents. There is one event loop per agent and one hook slot on it, so the claim is made once, here,
       and given back once, below. */
    engine_set_timer_hook(timer_run_due);
    agent_state_flag("timer", &g_ready, "the declaration latch");
    agent_state_atom("timer", &g_atom_map, "§8.7 Timers's map key on a global's timer record");
    agent_state_atom("timer", &g_atom_next, "§8.7 Timers's next-handle key on that record");
    agent_state_id("timer", &g_slot, "the per-realm slot §8.7 Timers's map is held in");
    agent_state_ptr("timer", &g_script_sink, "the host edge a string-bodied setTimeout is queued through");
}

/* THE MAP IS BUILT AT REALM INSTALL, which puts a top-level realm's in the pre-boot BASELINE. Built lazily on
   the first `setTimeout` instead it would be whichever FLOW touched it first that owned it, and every sibling
   would then be registering into an object created inside another flow's delta. */
static void timer_install_map(JSContext *ctx)
{
    JSValue st;

    DCHECK(g_ready, "a realm asked for §8.7 Timers's map before the interface was declared");
    /* Running twice in one realm is asserted by realm_value_set, which is where the first value is standing. */
    st = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(st), "timer: this global's map of active timers could not be allocated");
    {
        JSValue q = JS_NewArray(ctx);
        CHECK(!JS_IsException(q), "timer: this global's entry list could not be allocated");
        JS_SetProperty(ctx, st, g_atom_map, q);
    }
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, 1));   /* §8.7 Timers: handles start at 1 */
    realm_value_set(ctx, g_slot, st);
}

void timer_install(JSContext *ctx, JSValueConst global)
{
    JSValue g = (JSValue)global;
    idl_install_method(ctx, g, "setTimeout", 2, g_id_set_timeout);
    idl_install_method(ctx, g, "setInterval", 2, g_id_set_interval);
    idl_install_method(ctx, g, "clearTimeout", 1, g_id_clear_timeout);
    idl_install_method(ctx, g, "clearInterval", 1, g_id_clear_interval);
    JS_SetPropertyStr(ctx, g, "queueMicrotask",
                      JS_NewCFunction(ctx, js_queue_microtask, "queueMicrotask", 1));
}

void timer_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional and whose table asserts a release row
       has a declare. */
    DCHECK(g_ready, "§8.7 Timers's timer machinery was released in an agent that never declared it");
    g_ready = 0;
    /* §8.1.7'S TIMER STEP IS GIVEN BACK BY THE COMPONENT THAT CLAIMED IT. The slot lives on the ONE frontier
       (solver/engine.c) and names `timer_run_due` in this file, so a release that kept it would leave the
       scheduler asking a component whose interned keys and realm slot are gone — the defect
       core/agent_state.h found in idb_transaction. solver_agent_free asserts it, and the whole platform is
       released before the solver is. */
    engine_set_timer_hook(NULL);
    /* The MAPS are the realms' — each is released with its context, which is what the per-realm slot array is
       for, and each flow's own entries go with the delta that holds them. What this owns is the two interned
       keys. */
    JS_FreeAtomRT(rt, g_atom_map);
    JS_FreeAtomRT(rt, g_atom_next);
    g_atom_map = g_atom_next = JS_ATOM_NULL;
    g_slot = -1;
    g_script_sink = NULL;
}
