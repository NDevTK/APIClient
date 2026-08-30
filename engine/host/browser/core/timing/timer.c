/* TIMERS — HTML §8.7 "Timers", the timer task source.
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
 * IT IS A TASK, AND THE WORD IS LOAD-BEARING. §8.7 says "queue a global task on the timer task source", and
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
 * reach the same detector without this component. What stays HERE is exactly what the engine cannot see: §8.7's
 * handler is a DOMString this component compiles LATER through `g_script_sink`, so the value has to be
 * announced where it ARRIVES and not where some other algorithm evaluates it.
 * Worse, it did not merely go unnoticed: the
 * union's non-callable arm resolves to DOMString, idl_args.c passes unknown external input across the boundary
 * AS ITSELF so opacity survives the coercion, and the body's `JS_IsString` assertion then fired on it — a
 * canonical XSS sink took the document down on an assert against attacker input. */
#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"      /* §8.7's FOUR page-facing members are step machines: each asks a question over a
                                 value that can be unknown external input — the setters step 4's comparison over
                                 `timeout`, the clearers which entry of the map of IDs `id` names */
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
#include "solver/decide.h"     /* …and an unknown expiry makes §8.7's ORDER a fork, not a guess (step 4's is the
                                  MACHINE's own, through quickjs-step.h's step_fork_run) */

/* ONE ENTRY OF §8.7 Timers's MAP, as an Array — the shape §8.12 Animation frames's map of animation frame callbacks uses, for the
   reason CLAUDE.md gives: platform data a flow queues is a JS value, so the collector owns it, the COW delta
   captures it and the snapshot machinery parks it. A hole in the map is `undefined` — a free slot, which the
   next timer set in this realm reuses, so a page that sets and clears a million timeouts does not grow the
   array. The extra arguments HTML passes the callback are the entry's tail rather than a second allocation. */
/* TE_NEST IS §8.7 Timers's TIMER NESTING LEVEL, HELD ON THE ENTRY BECAUSE THE ENTRY IS WHAT QUEUES THE TASK.
   Step 11 is "Set task's timer nesting level to nestingLevel" and the level is therefore the TASK's, not the
   entry's — but this engine does not build the task at the set, it builds it at the EXPIRY, out of this
   entry. §scheduler's rule for exactly that shape is that an operation which becomes a work item takes its
   INPUTS WITH IT rather than reading them back later off whatever it acts on, and the level is an input: it
   was computed at the SET, from the task that was running THEN, and re-deriving it at the fire would read the
   level of whichever task happens to be firing this one. So it rides the entry from step 11 to the moment
   timer_run_due hands it to the task it queues, and nothing else ever reads it. */
/* AN ENTRY IS TWO OF §8.7 Timers's MAPS AT ONCE, AND SEPARATING THEM IS WHAT MAKES SUBSTEPS 9.9-9.12 SAYABLE.
   HTML gives a global TWO ordered maps and they have different lifetimes. The MAP OF setTimeout AND setInterval
   IDs ("Objects that implement the WindowOrWorkerGlobalScope mixin have a map of setTimeout and setInterval
   IDs") is keyed by the identifier the page holds, written at the timer initialization steps' step 14 and
   removed at substep 9.12 — AFTER the handler has run, which is the only reason a `clearInterval(id)` inside
   the handler can stop the next fire. The MAP OF ACTIVE TIMERS ("…have a map of active timers, which is an
   ordered map") is keyed by a timerKey nothing outside the algorithm holds, written at `run steps after a
   timeout` step 3 and removed at its step 4.5 — which runs immediately after step 4.4 performs completionSteps,
   and step 12's completionStep only QUEUES the task, so the removal happens BEFORE a line of the handler runs.
   This entry IS the map-of-IDs record; TE_WHEN's PRESENCE is the map-of-active-timers record standing beside
   it, and `undefined` there is step 4.5 having removed it. Conflating the two is not pedantic: with one flag
   for both, either the entry disappears at the fire (and the handler's `clearInterval` finds nothing, so a
   cleared interval re-arms anyway) or it stays armed while its task is in flight (and a driver that reaches
   timer_run_due again while the handler is parked on a reply fires the SAME entry a second time).
   TE_REPEAT is §8.7's `repeat` and TE_TIMEOUT is its `timeout` — the two arguments substep 9.11 hands back to
   the timer initialization steps. `repeat` is a BOOLEAN and not a period-versus-sentinel, because the sentinel
   it replaces (`every < 0` for a one-shot) made "there is no period" a NUMBER, and a period that may be
   unknown external input has no number to be distinguishable from.
   TE_THIS IS §8.7's STEP 1 `thisArg`, AND IT IS ON THE ENTRY BECAUSE IT SAYS WHICH ALGORITHM QUEUED THE TASK.
   Step 1 is "Let thisArg be global if that is a WorkerGlobalScope object; otherwise let thisArg be the
   WindowProxy that corresponds to global", and substep 9.7 invokes the handler "with callback this value set
   to thisArg". `run steps after a timeout` is a DIFFERENT algorithm reached through this same map, and it
   states no this value at all — so its entries carry `undefined` here, and that is a positive statement rather
   than a hole. Deriving it at the fire from the realm instead would give the same object for §8.7's entries
   and would silently give it to the engine's own completion steps too, which is a claim no standard makes. */
enum { TE_HANDLE = 0, TE_WHEN, TE_REPEAT, TE_TIMEOUT, TE_THIS, TE_NEST, TE_SEQ, TE_FN, TE_ARG0 };

static int      g_slot = -1;
static JSAtom   g_atom_map = JS_ATOM_NULL, g_atom_next = JS_ATOM_NULL;
static int      g_ready;
/* §8.7 Timers's timer initialization steps step 9's TASK, declared once per agent — see js_timer_task_step. */
static int      g_task_stepid = -1;
/* §8.7's FOUR MEMBER DECLARATIONS, EACH A POOL ID, AND THE `= -1` IS PART OF THE DECLARATION. Written bare,
   their pre-init value is `0` — which core/agent_state.h's table reserves for a FLAG or a CLASS ID and which
   the id pool hands out as a real entry, so the failure of carrying one is not that the next agent's install
   fails but that it succeeds against whatever member the new pool put at 0. That is the worse of the two
   shapes: it brands `setTimeout` with a declaration the live agent never made, and nothing anywhere says so. */
static int      g_id_set_timeout = -1, g_id_set_interval = -1,
                g_id_clear_timeout = -1, g_id_clear_interval = -1;
/* §8.7's substep 9.11 DOOR — "perform the timer initialization steps again, given global, handler, timeout,
   arguments, true, and id". It is a FIFTH declaration of the SAME IdlStepDecl, never installed on a global: the
   re-performance is the same algorithm with the same body, and the only thing the two page-facing members
   cannot express is step 2's `previousId`, which this one declares as a third position. Declaring it rather
   than re-implementing the re-arm is the whole point — a second speller of steps 3-5 and 10-14 is the dual
   system §Disposition forbids, and it is exactly what stood in timer_run_due. */
static int      g_id_rearm = -1;
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

    DCHECK(field != TE_WHEN && field != TE_TIMEOUT,
           "an entry's EXPIRY or its `timeout` was read as a number — those are the two fields of the entry "
           "that can be unknown external input, so they are read as VALUES (timer_entry_when, and TE_TIMEOUT "
           "straight off the entry into the task's own argument)");
    DCHECK(JS_IsNumber(v), "an entry of §8.7 Timers's map of active timers lost one of its numeric fields — the "
                           "handle, the timer nesting level and the insertion order are all written at the set");
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
 * for it — so the SUM is unknown too, and it is written and read as the value it is. OWNED.
 *
 * JS_UNDEFINED IS A THIRD ANSWER AND IT IS A POSITIVE ONE: this entry is NOT in the global's map of active
 * timers, because `run steps after a timeout` step 4.5 removed it when its task was queued. The entry itself
 * survives that — it is the map-of-setTimeout-and-setInterval-IDs record, which substep 9.12 removes and
 * substep 9.11 re-arms — so "no expiry" is the whole of what distinguishes an entry whose task is in flight
 * from one waiting to fire. */
static JSValue timer_entry_when(JSContext *ctx, JSValueConst e)
{
    JSValue v = JS_GetPropertyUint32(ctx, e, TE_WHEN);

    DCHECK(JS_IsNumber(v) || concolic_is(v) || JS_IsUndefined(v),
           "an entry of §8.7 Timers's map of active timers holds an expiry that is neither a moment on the "
           "event loop's clock, nor unknown external input, nor `run steps after a timeout` step 4.5's own "
           "removal — every writer here writes one of the three");
    return v;
}

/* THE INDEX OF THE ENTRY `handle` NAMES, or -1 — §8.7 Timers's "global's map of setTimeout and setInterval
   IDs[id]" as a lookup, with ONE speller because three algorithms ask it: `clearTimeout`'s removal, substep
   9.9's "If id does not exist in global's map of setTimeout and setInterval IDs, then abort these steps", and
   step 14's write on the re-arm, whose `id` is step 2's `previousId`.
   SUBSTEPS 9.3 AND 9.10's uniqueHandle TEST IS SUBSUMED AND NOT SKIPPED. Those steps exist because an
   implementation may hand out an id a `clearTimeout` has freed; timer_next_handle is strictly monotone per
   global, so a freed handle is never handed out again and "the entry with this id" cannot be a different
   timer's. The day the identifier stops being monotone — a wrap, a pool, a replay that re-mints — that
   assumption goes with it, and timer_next_handle's own `handle >= 1` DCHECK is where it is stated. */
static int timer_entry_index(JSContext *ctx, uint32_t handle)
{
    JSValue q = timer_map(ctx);
    uint32_t i, n = arr_len(ctx, q);
    int found = -1;

    for (i = 0; i < n && found < 0; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i);

        if (JS_IsObject(e) && (uint32_t)timer_entry_num(ctx, e, TE_HANDLE) == handle)
            found = (int)i;
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, q);
    return found;
}

/* §8.7 step 3's PLUS AND THE TIMER SOURCE'S ORDER ARE THE EVENT LOOP'S, NOT THIS FILE'S — and that is a
 * deletion rather than a move.
 *
 * This component had its own `timer_expiry` (§13.15.3's `+` over a moment and a duration) and its own
 * `timer_rel` (`<` and `==` over two expiries, forked where either is unknown). Both are operations on MOMENTS
 * OF ONE CLOCK, and the clock is core/timing/event_loop.h's. Keeping a second speller of them here was safe
 * only while an expiry could not be unknown: the path constraint is keyed by the IDENTITY of the value a
 * branch tests, and a relation's identity is composed from its NAME and both operands, so "§8.7 run steps
 * after a timeout: milliseconds <" and the rendering loop's own frame-versus-clock comparison were TWO KEYS
 * FOR ONE QUESTION. A flow could answer `A < B` true under one name and false under the other and stand in a
 * world neither arm is in. One clock, one order, one spelling — event_loop_before / event_loop_coincident,
 * whose header carries the reasoning this file used to.
 *
 * WHAT STAYS HERE IS THE PART THAT IS §8.7's AND NOT §8.1.7's: that the source's key has TWO components, the
 * expiry and the insertion order, and that the second breaks a tie in the first. Folding them into one `<=`
 * would put "equal, and set first" into whichever arm the fold happened to pick, and that is the case the
 * tie-break exists for. The insertion numbers are concrete on both sides (§8.1.7's counter hands out a real
 * number at every set), so the tie-break inside the equal arm is decided rather than forked. */

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
        /* NOT IN THE MAP OF ACTIVE TIMERS — `run steps after a timeout` step 4.5 removed this entry's expiry
           when step 4.4 queued its task, and the record that is left is the map-of-IDs one substep 9.12 will
           remove or substep 9.11 will re-arm. Skipping it is what stops the SAME timer being fired twice: the
           driver reaches this walk whenever it has nothing else runnable, and a handler parked on a reply the
           host still owes is exactly such a moment. */
        if (JS_IsUndefined(when)) { JS_FreeValue(ctx, when); JS_FreeValue(ctx, e); continue; }
        seq = timer_entry_num(ctx, e, TE_SEQ);
        JS_FreeValue(ctx, e);
        DCHECK(seq < seq_next,
               "a flow reached a timer that was queued with an event-loop insertion number this flow never "
               "allocated — §8.7 Timers's map of active timers and §8.1.7's insertion counter ride the SAME per-flow "
               "COW delta, so an entry another flow's timeline queued cannot be visible here. One of the two "
               "is being answered from outside the delta, and the effect is flow A's setTimeout firing in flow "
               "B's job queue under B's heap");
        if (best < 0 || event_loop_before(ctx, when, *pwhen)
                     || (event_loop_coincident(ctx, when, *pwhen) && seq < *pseq)) {
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
        if (idx >= 0 && (!best || event_loop_before(ctx, when, *pwhen)
                              || (event_loop_coincident(ctx, when, *pwhen) && seq < *pseq))) {
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
 * put to, and asked here it forks (event_loop_before) exactly as any other order over an unknown does. The clock the
 * moments live on is the event loop's — see core/timing/event_loop.h.
 * Returns 0 when no timer is set at all, which is the same answer as "not before" and is right for both: the
 * rendering opportunity proceeds either way. */
int timer_due_before(JSContext *ctx, JSValueConst moment)
{
    JSValue when = JS_UNDEFINED;
    double seq = 0;
    int idx = -1, r;

    if (!timer_earliest(ctx, &idx, &when, &seq))
        return 0;
    r = event_loop_before(ctx, when, moment);
    JS_FreeValue(ctx, when);
    return r;
}

void timer_set_script_sink(void (*queue)(uint32_t doc, const char *src)) { g_script_sink = queue; }

/* §8.7 Timers's TIMER IDENTIFIER, out of THIS global's own counter — the timer initialization steps' step 2,
   "an implementation-defined integer that is greater than zero and does not already exist in global's map of
   setTimeout and setInterval IDs".
   ONE SPELLER, because §8.7 hands a handle back on THREE paths and only one of them makes an entry: the
   function form (timer_set below), and both string forms, which queue a script and leave nothing for
   `clearTimeout` to find. Three copies of "read the counter, bump it, return the old value" is three places
   for the bump to go missing, and a missing bump is two live timers sharing one identifier — where
   `clearTimeout` clears the wrong one and nothing says so.
   It is an ordinary property write, so the per-flow COW delta captures it: two arms of a fork mint the SAME
   handle for the same source line, which is what makes a replayed decision vector name the same timer. */
static uint32_t timer_next_handle(JSContext *ctx)
{
    JSValue st = timer_store(ctx), nv = JS_GetProperty(ctx, st, g_atom_next);
    uint32_t handle = 0;

    JS_ToUint32(ctx, &handle, nv);
    JS_FreeValue(ctx, nv);
    DCHECK(handle >= 1, "§8.7 Timers's timer identifier started below 1 — 0 is the handle a page gets back for "
                        "nothing, and `clearTimeout(0)` must name no entry");
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, handle + 1));
    JS_FreeValue(ctx, st);
    return handle;
}

/* SET ONE TIMER in `ctx`'s global's map — §8.7 Timers's shared tail for `setTimeout`, `setInterval` and the engine's
   own "run steps after a timeout". Returns the handle §8.7 Timers gives back.
 *
 * THE HANDLE COMES FROM THE GLOBAL and the INSERTION ORDER comes from the event loop, and those are two
 * different counters because they answer two different questions: §8.7 Timers's identifier is what `clearTimeout`
 * names, and it is per-global (two same-origin documents both hand out 1); the insertion order breaks a tie
 * between two globals' entries on the one task source, so it has to be the loop's. Both ride the per-flow
 * delta, which is why two arms of a fork mint the SAME handle for the same source line. */
/* `timeout` IS A VALUE AND NOT A DOUBLE, because §8.7's `timeout` can be unknown external input and the expiry
   this writes is then unknown too — see event_loop_moment_plus. It is BOTH of the two things §8.7 does with
   that variable at this point in the algorithm: step 13 hands it to `run steps after a timeout` as
   `milliseconds` (whose step 3 is the expiry below), and substep 9.11 hands it back to the timer initialization
   steps. One variable, so one field.
   `repeat` IS §8.7's OWN BOOLEAN, carried rather than encoded in the period. The `every < 0` sentinel it
   replaces could only say "no period" by picking a NUMBER, and the moment a period may be unknown external
   input there is no number left to pick — which is precisely why `setInterval` with an unknown period used to
   abort here.
   `nest` IS §8.7's step 11, "Set task's timer nesting level to nestingLevel" — the level the task this entry
   will queue must publish while it runs. It is 0 for a caller that is not the timer initialization steps,
   which is step 3's "Otherwise" read from the other end.
   `prev_id` IS STEP 2's `previousId`: 0 is "previousId was not given" and the handle is minted, non-zero is
   substep 9.11's re-performance, which reuses the identifier the page is still holding. A re-arm therefore
   REWRITES the map-of-IDs record in place (step 14's "Set global's map of setTimeout and setInterval IDs[id]
   to uniqueHandle" is a write on a key that already exists), and a `clearInterval` between the fire and the
   re-arm has removed that key, which is what substep 9.9 checks before this is ever reached.
   `this_arg` IS STEP 1's, BORROWED — the value substep 9.7 invokes the handler with, `undefined` for a caller
   that is not the timer initialization steps. See TE_THIS. */
static int timer_set(JSContext *ctx, JSValueConst timeout, int repeat, int nest, uint32_t prev_id,
                     JSValueConst this_arg, JSValueConst fn, int argc, JSValueConst *argv)
{
    JSValue q, entry;
    uint32_t handle = prev_id ? prev_id : timer_next_handle(ctx), i, slot, n;

    entry = JS_NewArray(ctx);
    CHECK(!JS_IsException(entry), "timer: OOM recording a timer — a dropped one is work the page asked for "
                                  "and never gets");
    JS_SetPropertyUint32(ctx, entry, TE_HANDLE, JS_NewUint32(ctx, handle));
    {
        /* §8.7 step 2 "Let startTime be the current high resolution time given global", step 3 "Set global's
           map of active timers[timerKey] to startTime plus milliseconds" — the sum run by the ENGINE, over a
           start that is itself a moment on this clock and may be unknown once a timer with an unknown expiry
           has fired. */
        JSValue start = event_loop_now(ctx);

        JS_SetPropertyUint32(ctx, entry, TE_WHEN, event_loop_moment_plus(ctx, start, timeout));
        JS_FreeValue(ctx, start);
    }
    JS_SetPropertyUint32(ctx, entry, TE_REPEAT, JS_NewBool(ctx, repeat != 0));
    JS_SetPropertyUint32(ctx, entry, TE_TIMEOUT, JS_DupValue(ctx, timeout));
    JS_SetPropertyUint32(ctx, entry, TE_THIS, JS_DupValue(ctx, this_arg));
    DCHECK(nest >= 0,
           "§8.7 Timers's step 11 was handed a NEGATIVE timer nesting level to set on the task this entry "
           "queues — step 10 increments from a level that is 0 at worst, so every value it can produce is "
           "1 or above and every other caller of this passes step 3's own 0");
    JS_SetPropertyUint32(ctx, entry, TE_NEST, JS_NewInt32(ctx, nest));
    JS_SetPropertyUint32(ctx, entry, TE_SEQ, JS_NewFloat64(ctx, event_loop_task_seq(ctx)));
    JS_SetPropertyUint32(ctx, entry, TE_FN, JS_DupValue(ctx, fn));
    for (i = 0; i < (uint32_t)argc; i++)
        JS_SetPropertyUint32(ctx, entry, TE_ARG0 + i, JS_DupValue(ctx, argv[i]));

    q = timer_map(ctx);
    if (prev_id) {
        /* STEP 14 ON A KEY THAT ALREADY EXISTS. The re-arm's own record replaces the fired one IN PLACE, so the
           page's identifier keeps naming exactly one entry and `clearInterval` after the re-arm finds it. */
        int at = timer_entry_index(ctx, prev_id);

        DCHECK(at >= 0,
               "§8.7 Timers's substep 9.11 re-performed the timer initialization steps with a `previousId` "
               "that names no entry of this global's map of setTimeout and setInterval IDs — substep 9.9 is "
               "the check that this cannot happen, and it runs immediately before the re-performance");
        slot = (uint32_t)at;
    } else {
        n = arr_len(ctx, q);
        for (slot = 0; slot < n; slot++) {   /* reuse a cleared entry's slot before growing the map */
            JSValue e = JS_GetPropertyUint32(ctx, q, slot);
            int free_slot = !JS_IsObject(e);
            JS_FreeValue(ctx, e);
            if (free_slot) break;
        }
    }
    JS_SetPropertyUint32(ctx, q, slot, entry);
    JS_FreeValue(ctx, q);
    return (int)handle;
}

/* CLEAR ONE HANDLE from `ctx`'s global's map. §8.7 Timers: a handle that names nothing does nothing, which is what a
   page clearing a timeout that has already fired needs. */
static void timer_clear(JSContext *ctx, uint32_t want)
{
    int at = timer_entry_index(ctx, want);
    JSValue q;

    if (at < 0)
        return;
    q = timer_map(ctx);
    JS_SetPropertyUint32(ctx, q, (uint32_t)at, JS_UNDEFINED);
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

/* HTML §8.7 Timers's timer initialization steps STEP 9's TASK — "Let task be a task that runs the following
 * substeps" — as a step machine, and it exists because §8.7's step 11 has to have somewhere to put the TIMER
 * NESTING LEVEL.
 *
 * WHY THE TASK HAD TO BECOME A THING. This component used to queue the page's HANDLER as the task, which is
 * the right observable behaviour and the wrong object: step 11 is "Set task's timer nesting level to
 * nestingLevel" and step 3 reads that level back off "the surrounding agent's event loop's currently running
 * task", so with no task object there was no carrier for the one fact those two steps exchange. What that
 * cost is not pedantic — it is the ORDER a virtual clock exists to model. Step 5 ("If nestingLevel is greater
 * than 5, and timeout is less than 4, then set timeout to 4") is the only thing that ever moves a
 * `setTimeout(f, 0)` chain off the moment it is standing on; without it the chain re-arms at the same virtual
 * moment for ever, timer_earliest picks it at every turn, and a `setTimeout(g, 1)` set beside it NEVER RUNS.
 * A poll loop or a chunked worker written as a self-re-arming zero timeout is one of the most common shapes
 * in a real bundle, and it starved everything the page had queued behind it.
 *
 * A MACHINE, BECAUSE THE BRACKET HAS TO SURVIVE A PARK. The level must be readable at EVERY point while the
 * task's steps run, and the handler is the page's code: it forks, it awaits, it is preempted at a loop
 * back-edge and parks. A bracket taken around the drainer's call would therefore cover only the FIRST slice
 * of the callback — the flow resumes through the parked-continuation path, not through the drainer — and a
 * nested `setTimeout` after the first preempt would read level 0 and go unclamped, which is the same
 * livelock arriving one preempt later and much harder to see. A step machine is the engine's one vehicle for
 * "a C algorithm that runs page code and continues afterwards": its state parks and resumes with the flow and
 * is cloned at a fork, so substep 9.7's invocation is bracketed by the two writes below however many times
 * the flow suspends inside it.
 *
 * WHAT IT PUBLISHES INTO IS THE EVENT LOOP'S RECORD (core/timing/event_loop.h), which time-travels with the
 * flow — so two timelines' timer tasks do not see each other's level, and neither does a sibling that forked
 * before this task started. */
/* AND THE TASK IS WHERE SUBSTEPS 9.9-9.12 LIVE, WHICH IS WHERE THE RE-ARM HAD TO MOVE TO.
 *
 * §8.7 puts the interval's re-arm at substep 9.11 — "If repeat is true, then perform the timer initialization
 * steps again, given global, handler, timeout, arguments, true, and id" — INSIDE the task, after 9.7 has
 * invoked the handler and after 9.9 and 9.10 have re-checked that the identifier still names this timer. This
 * component performed it at the FIRE instead, in timer_run_due, and that placement is what made
 * `setInterval(f, someUnknownValue)` abort: the re-arm re-performs step 5 ("If nestingLevel is greater than 5,
 * and timeout is less than 4, then set timeout to 4"), which over an unknown `timeout` is a FORK, and a fork
 * needs a RESUME POINT for the sibling — which a plain C function firing a timer does not have. It also got
 * two orderings wrong that a page can see: the re-armed entry took its insertion number BEFORE the handler
 * ran, so an interval beat a `setTimeout` the handler itself set for the same moment; and a `clearInterval`
 * the handler called had to un-do a re-arm that had already happened rather than being seen by 9.10.
 * Moved here, the re-arm is a CALL of the timer initialization steps' own machine (g_id_rearm), so it is the
 * same body, the same stages and the same TI_CLAMP fork the page-facing setters use — no second speller, and
 * the sibling parks on this task's own frame like every other flow.
 *
 * THE LEVEL STAYS PUBLISHED ACROSS THE WHOLE TAIL, and that is not an extension of the bracket but the
 * bracket's own definition: §8.1.7.3 Processing model step 2.6 is "Perform oldestTask's steps" and 9.11 is one
 * of them, so the re-performance's step 3 reads THIS task's timer nesting level — which is exactly what makes
 * an interval's levels walk 1, 2, 3, … and the clamp take over past 5. timer_run_due used to hand that level
 * to the re-arm off the entry because there was no published one to read; now step 3 answers it.
 *
 * 9.7's "report" IS NOT PERFORMED AT 9.7 — see the residual named at TT_TAIL. */
enum { TT_ARG_HANDLER = 0, TT_ARG_THIS, TT_ARG_NEST, TT_ARG_ID, TT_ARG_REPEAT, TT_ARG_TIMEOUT, TT_ARG_N };

#define TT_STAGES(X)                                                                                          \
    X(TT_INVOKE,                                                                                              \
      "HTML §8.7 Timers timer initialization steps step 9.7 (if handler is a Function, then invoke handler "   \
      "given arguments and \"report\")")                                                                      \
    X(TT_TAIL,                                                                                                \
      "HTML §8.7 Timers timer initialization steps steps 9.9-9.12 (the id and uniqueHandle re-checks, then "   \
      "the repeat's re-performance of the timer initialization steps or the map removal)")
enum { TT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TT_STEPS[] = { TT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    /* HAVE I STARTED — a machine cannot answer that from its stage, because the first stage IS the entry
       stage, and the call buffers below must hold JS_UNDEFINEDs before anything that can fail. */
    uint8_t   started;
    uint8_t   cphase;   /* substep 9.7's call of the handler */
    uint8_t   rphase;   /* substep 9.11's call of the timer initialization steps */
    /* SUBSTEP 9.7 COMPLETED ABRUPTLY. Held rather than returned, because 9.9-9.12 are still the task's steps
       — a throwing `setInterval` handler goes on firing in a browser, and returning here is what would stop
       it. `threw` is its own byte for the reason `placed` is one in TimerInitState: a zeroed JSValue is the
       INTEGER 0 and not undefined, so "is there an exception" cannot be read off the value. */
    uint8_t   threw;
    JSValue   exc;
    /* [this, handler] AND NOTHING MORE, which is timer_init's declaration read at the far end: §8.7's
       `any... arguments` tail is not declared, so no entry of the map carries extra arguments and
       timer_run_due asserts that where it would have to copy them. */
    JSValue   cb[2];
    /* [this, door, handler, timeout, previousId] — substep 9.11's own call, in its own buffer because the two
       requests are alive at different stages and one buffer would let the second collect the first's answer. */
    JSValue   rcb[5];
} TimerTask;

static void tt_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    TimerTask *s = stp;
    int k;

    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
    STEP_CB_FOREACH(s->rcb, k) v->val(ctx, &s->rcb[k]);
    v->val(ctx, &s->exc);
}

static int js_timer_task_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    TimerTask *s = stp;
    JSValueConst handler = step_arg(&s->hdr, TT_ARG_HANDLER);
    JSValue out = JS_UNDEFINED;
    int32_t nest = 0;
    int r;

    JS_ToInt32(ctx, &nest, step_arg(&s->hdr, TT_ARG_NEST));

    STEP_DISPATCH(TT_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(TT_INVOKE);
        if (!s->started) {
            int k;

            s->started = 1;
            s->cphase = 0;
            s->rphase = 0;
            s->threw = 0;
            s->exc = JS_UNDEFINED;
            STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
            STEP_CB_FOREACH(s->rcb, k) s->rcb[k] = JS_UNDEFINED;
            DCHECK(s->hdr.argc == TT_ARG_N,
                   "§8.7 Timers's step 9 task was queued with an argument count timer_run_due does not "
                   "produce — the handler, step 1's `thisArg`, its task's timer nesting level, and the `id`, "
                   "`repeat` and `timeout` substep 9.11 hands back are placed together at the one site that "
                   "queues it, so a different count means a second queuer exists");
            /* §8.1.7.3 Processing model step 2.5: "Set the event loop's currently running task to
               oldestTask." The level rode the map entry to here (see TE_NEST) precisely so this write can be
               made from what the SET decided rather than from what is firing.
               THE ZERO IT REPLACES IS ASSERTED AND NOT RESTORED, because §8.1.7.3 says a task is over before
               the next one begins: step 2.7 sets the currently running task back to null and only then does
               2.8 run the microtask checkpoint. Reading a nonzero level here would mean two tasks are inside
               one another on this flow's timeline — which the driver's own ladder forbids by resuming a
               parked continuation ahead of any job — and restoring it instead of asserting would make that
               state survive silently as a level nobody can account for. */
            DCHECK(event_loop_timer_nesting(ctx) == 0,
                   "§8.7 Timers's step 9 task began while the event loop already had a timer task as its "
                   "currently running task — §8.1.7.3 Processing model step 2.7 sets that back to null "
                   "before step 2.8's checkpoint, so two of them overlapping means a task started inside "
                   "another's steps and the inner one's level is about to be attributed to the outer");
            event_loop_set_timer_nesting(ctx, nest);
        }
        /* §8.7 step 9.7's invocation. `argc` is 0 for the reason the buffer is two slots — see TimerTask.
           THE RECEIVER IS STEP 1's `thisArg`, WHICH IS NOT THE GLOBAL — "otherwise let thisArg be the
           WindowProxy that corresponds to global" — and the difference is one a page reads. A sloppy handler
           is non-strict, so ECMAScript §10.2.1.2 OrdinaryCallBindThis would have replaced an `undefined`
           receiver with the realm's globalThis anyway and the two agreed by accident; a STRICT handler keeps
           what it is given, so `setTimeout(function(){ "use strict"; return this; })` answered `undefined`
           where a browser answers the WindowProxy. It rode the entry from step 1 rather than being derived
           here, because which value it is depends on the ALGORITHM that queued this task and not on the realm
           the task is running in — see TE_THIS. */
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), handler, step_arg(&s->hdr, TT_ARG_THIS), 0, NULL,
                          cb_result, &out, out_cb, out_argc);
        if (r > 0)
            return r;   /* PARKED ON THE PAGE'S CODE: the level stays published, which is the whole point */
        if ((r < 0) || JS_IsException(out)) {
            /* THE THROW IS TAKEN OFF THE CONTEXT AND HELD, NOT RETURNED. §8.7 invokes the handler with
               "report", which means §8.1.4.6 Runtime script errors reports the exception and the ALGORITHM
               CARRIES ON — substeps 9.9-9.12 are still this task's steps, so a `setInterval` whose callback
               throws goes on firing, which is what a browser does. Returning the abrupt here would end the
               task at 9.7 and stop the interval dead. */
            s->exc = JS_GetException(ctx);
            s->threw = 1;
            DCHECK(!JS_IsNull(s->exc),
                   "§8.7 Timers's substep 9.7 reported an abrupt completion with nothing pending on the "
                   "context — a call that answers JS_EXCEPTION leaves its completion live, so a null here "
                   "means something between the call and this line already took it");
        }
        JS_FreeValue(ctx, out);
        STEP_GOTO(s->hdr.stage, TT_TAIL, &s->cphase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(TT_TAIL);
    {
        uint32_t id = 0;
        int repeat = JS_ToBool(ctx, step_arg(&s->hdr, TT_ARG_REPEAT));
        int at;

        DCHECK(s->started,
               "§8.7 Timers's step 9 task reached substep 9.9 without having run substep 9.7 — the invoke "
               "stage is the machine's entry stage and it is the only writer of the state this reads");
        {
            double d = 0;
            JS_ToFloat64(ctx, &d, step_arg(&s->hdr, TT_ARG_ID));
            id = (uint32_t)d;
        }
        /* SUBSTEPS 9.9 AND 9.10 — "If id does not exist in global's map of setTimeout and setInterval IDs,
           then abort these steps" and the uniqueHandle test beside it, which timer_entry_index subsumes for
           the reason stated there. This is the check a `clearTimeout`/`clearInterval` INSIDE the handler is
           seen by, and it is the whole reason the entry outlives the fire: `run steps after a timeout` step
           4.5 removed only the map-of-active-timers record, so what is looked up here is the identifier the
           page still holds. */
        at = timer_entry_index(ctx, id);
        DCHECK(s->rphase == 0 || (at >= 0 && repeat),
               "§8.7 Timers's substeps 9.9-9.12 resumed from substep 9.11's call and no longer agree that "
               "there is a repeat to re-perform — a stage holding a request must reach the same request site "
               "on the way back in, and nothing between the two runs a line of the page's code");
        if (at >= 0 && repeat) {
            /* SUBSTEP 9.11 — "perform the timer initialization steps again, given global, handler, timeout,
               arguments, true, and id." A CALL of that algorithm's own machine, so step 4's fork and step 5's
               fork are asked by the one implementation the page-facing setters use; the flow parks on this
               task's frame exactly as it parks inside the handler above.
               `timeout` IS THE ONE THIS INVOCATION HOLDS, not the one the page wrote: steps 4 and 5 assign to
               that variable and 9.11 hands back what it then names. Both assignments are idempotent under a
               re-performance (0 is not less than 0; 4 is not less than 4), so the two readings agree on every
               number — but the value is what the entry carries, so there is nothing to re-derive. */
            if (s->rphase == 0) {
                JSValueConst rargv[3];
                JSValue door;

                DCHECK(g_id_rearm >= 0,
                       "§8.7 Timers's substep 9.11 was reached before timer_init declared the re-performance "
                       "door — it is declared once per agent beside the two page-facing setters");
                door = idl_step_function(ctx, "timerInitializationSteps", g_id_rearm);
                CHECK(!JS_IsException(door),
                      "timer: §8.7 Timers's substep 9.11 door could not be allocated — a dropped re-arm is an "
                      "interval that silently stops, which is invisible from outside");
                rargv[0] = handler;
                rargv[1] = step_arg(&s->hdr, TT_ARG_TIMEOUT);
                rargv[2] = step_arg(&s->hdr, TT_ARG_ID);
                /* step_call_run DUPS the callee into the request buffer, which is what holds it across the
                   suspension — so this realm's door is released here and the parked call still owns one. */
                r = step_call_run(ctx, &s->rphase, STEP_CB(s->rcb), door, JS_UNDEFINED, 3, rargv, cb_result,
                                  &out, out_cb, out_argc);
                JS_FreeValue(ctx, door);
            } else {
                r = step_call_run(ctx, &s->rphase, STEP_CB(s->rcb), JS_UNDEFINED, JS_UNDEFINED, 0, NULL,
                                  cb_result, &out, out_cb, out_argc);
            }
            if (r > 0)
                return r;   /* the re-performance forked or parked; the level stays published for its step 3 */
            DCHECK(r == 0 && !JS_IsException(out),
                   "§8.7 Timers's substep 9.11 completed abruptly — the timer initialization steps throw only "
                   "out of a §3.6 argument conversion, and this call's three arguments are a callable the map "
                   "held, the `timeout` it was armed with and an integer this engine minted");
            {
                double got = -1;
                JS_ToFloat64(ctx, &got, out);
                DCHECK((uint32_t)got == id,
                       "§8.7 Timers's substep 9.11 returned an identifier that is not the `id` it was given — "
                       "step 2 is \"If previousId was given, let id be previousId\", so a different number "
                       "means the re-performance minted a fresh handle and the page's own `clearInterval` now "
                       "names an entry nothing will ever fire");
            }
            JS_FreeValue(ctx, out);
        } else if (at >= 0) {
            /* SUBSTEP 9.12 — "Otherwise, remove global's map of setTimeout and setInterval IDs[id]." */
            JSValue q = timer_map(ctx);

            JS_SetPropertyUint32(ctx, q, (uint32_t)at, JS_UNDEFINED);
            JS_FreeValue(ctx, q);
        }
        /* §8.1.7.3 Processing model step 2.7: "Set the event loop's currently running task back to null." */
        event_loop_set_timer_nesting(ctx, 0);
        /* NAMED RESIDUAL — §8.7's substep 9.7 says "and \"report\"", and the report is performed HERE rather
           than at 9.7. WHAT IS NOT COVERED: §8.1.4.6 Runtime script errors' report an exception fires an
           `error` event, and the spec fires it BEFORE substeps 9.9-9.12 run; this hands the throw back as the
           task's own abrupt completion and the job machinery reports it (solver/engine.c reports a job's throw
           as a page error) AFTER the re-arm. WHAT THE NEXT DIFF BUILDS: a stage between the two that drives
           core/events/report_exception.h's report_exception_run — the record is a ReportExceptionWork this
           machine's `visit` names, exactly as DOM §2.9's inner invoke and §8.12's animation-frame callback
           already hold one. HOW ITS ABSENCE SHOWS: a page whose `window.onerror` calls `clearInterval` on the
           interval whose handler just threw — under §8.7 the clear is seen by substep 9.10 and there is no
           re-arm, here the re-arm has already happened and the interval fires exactly once more. */
        if (s->threw) {
            JSValue exc = s->exc;

            s->exc = JS_UNDEFINED;
            s->threw = 0;
            JS_Throw(ctx, exc);   /* takes the reference this state was holding */
            return JS_STEP_ABRUPT;
        }
        return JS_STEP_DONE;
    }
}

/* NO `fini`. §8.7's task has no completion value — step 9 states substeps and returns nothing — and the two
   ownership lists a teardown needs are `visit` (the two call buffers and the held exception) and nothing
   else. */
static const JSTrampStepDef TT_DEF = {
    sizeof(TimerTask), js_timer_task_step, NULL, 0,
    .visit = tt_visit,
    .algorithm = "HTML §8.7 Timers timer initialization steps step 9's task",
    .steps = TT_STEPS
};

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
 * HTML SAYS WHEN INSTEAD. §8.1.7 "Event loops" runs a task from a task source that has one DUE; the timer
 * source's task becomes due at its expiry. So nothing is queued in advance — the DRIVER asks this, and only
 * when it has nothing else to run, which is exactly when "the clock may move" is true. Everything that is
 * already due (a queued task, a pending microtask, a reply that has ARRIVED) is by construction ahead of it.
 * A REPLY THE HOST STILL OWES IS NOT DUE AND MUST NOT BE AHEAD OF IT, which is the half this paragraph used
 * to get backwards. §8.1.7.3 "Processing model" step 2 conditions the loop on "a task queue with at least one
 * runnable task"; a `fetch()` runs in parallel and is on no queue until it completes. A driver that parked
 * the flow on that debt BEFORE asking here would leave a due timer unfired for as long as the request was in
 * the air — for ever, where nothing is going to answer — and the flow keeps the timer while never being
 * picked again, which is the razor's "starves" and so a cap. The order is stated at the driver
 * (solver/engine.c's step ladder); what this file owes it is only the honest "there is none" below.
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
    double seq = 0;
    int idx = -1, nest = 0, repeat = 0;
    JSContext *docctx = timer_earliest(ctx, &idx, &whenv, &seq);
    JSValue q, e, fn, timeout, this_arg;
    uint32_t n, handle;

    if (!docctx)
        return 0;

    /* THE CLOCK MOVES TO THIS TASK'S EXPIRY, AND WHETHER IT MOVES AT ALL IS A QUESTION.
       §8.7's map of active timers holds `startTime plus milliseconds`, and this engine's virtual clock jumps
       to the moment the source becomes due — so where `milliseconds` was unknown external input the clock
       goes to a moment nothing computed, which is the TRUTH rather than a gap: after a timer with an unknown
       delay has fired, how much time has passed IS unknown and every timestamp the page reads must say so.
       The one thing that is NOT decided by the walk above is whether that moment is still AHEAD of the clock.
       Every entry the walk compared was compared against another ENTRY; the clock is where the previous task
       left it, and an expiry can be behind it — a timer whose deadline has already passed is one a browser
       runs at the next opportunity, at the current moment, without winding time back. That is a question with
       two real answers over an unknown expiry, so it is ASKED (event_loop_before), and the arm it records is
       what discharges event_loop_advance_to's monotonicity invariant: the assert READS the decision instead
       of re-evaluating it, because re-evaluating a comparison over an unknown is a fork, and an assertion
       that forks manufactures the world in which it holds.
       DO NOT clamp, and do not read the example either — this entry reached the front of the queue precisely
       because a FORK put it there, so an example would answer a question an arm has already decided. */
    {
        JSValue now = event_loop_now(docctx);

        /* It is the ONE source that is due, so nothing else constrains the move — `due` is this same moment.
           On the arm where the expiry is already behind the clock the loop does not move at all: the task is
           due NOW, and winding the clock back to the expiry is the one thing §8.1.7's order forbids. */
        if (!event_loop_before(docctx, whenv, now))
            event_loop_advance_to(docctx, whenv, whenv);
        JS_FreeValue(docctx, now);
    }
    JS_FreeValue(docctx, whenv);

    q = timer_map(docctx);
    e = JS_GetPropertyUint32(docctx, q, (uint32_t)idx);
    DCHECK(JS_IsObject(e), "the timer the event loop selected is no longer in its global's map — nothing runs "
                           "between the selection and this read");
    /* TAKE WHAT THE TASK NEEDS, which is §scheduler's rule that an operation becoming a work item takes its
       INPUTS with it: substeps 9.9-9.12 run after the handler, and by then this entry may have been removed by
       a `clearInterval` or rewritten by the re-performance, so the task carries its handler, its level, its
       `id`, its `repeat` and its `timeout` rather than reading any of them back off the entry. */
    fn = JS_GetPropertyUint32(docctx, e, TE_FN);
    DCHECK(JS_IsFunction(docctx, fn),
           "§8.7 Timers's map held an entry whose handler is not callable — the string form became a script instead "
           "of an entry, so nothing else can be in the map");
    n = arr_len(docctx, e);
    DCHECK(n >= TE_ARG0,
           "an entry of §8.7 Timers's map of active timers is shorter than its own fixed head — the handle, "
           "expiry, repeat, timeout, thisArg, timer nesting level, insertion order and handler are all "
           "written at the set, and the extra arguments HTML passes the callback are what follows them");
    n -= TE_ARG0;
    /* THE EXTRA ARGUMENTS ARE NOT THERE TO TAKE, and that is the declaration's statement rather than this
       one's: timer_init does not declare §8.7's `any... arguments` tail, so a non-variadic member's argument
       count is min(passed, declared) and no writer of this map has ever placed one. The task machine below
       invokes the handler with none and its call buffer is sized for none, so this fires on the same day
       js_set_timer's `argc == 2` does — and names the buffer that has to grow with the declaration. */
    DCHECK(n == 0,
           "an entry of §8.7 Timers's map of active timers carries the EXTRA ARGUMENTS its task must invoke "
           "the handler with, and §8.7 Timers's task machine here has no room for them — its call buffer is "
           "[this, handler] because timer_init declares no `any... arguments` tail. Declaring the tail (see "
           "timer_init, which names what it costs) means this entry's tail travels as the task's own "
           "arguments and TimerTask::cb grows with it");
    /* §8.7 Timers's STEP 11, READ BACK — "Set task's timer nesting level to nestingLevel". The level was
       computed at the SET, out of the task that was running THEN, and it rode the entry to here for the reason
       TE_NEST names; the task publishes it around its own steps, which is what makes step 3 answerable both
       for a nested `setTimeout` inside the handler and for substep 9.11's re-performance. */
    nest = (int)timer_entry_num(docctx, e, TE_NEST);
    handle = (uint32_t)timer_entry_num(docctx, e, TE_HANDLE);
    {
        JSValue rep = JS_GetPropertyUint32(docctx, e, TE_REPEAT);

        DCHECK(JS_IsBool(rep),
               "an entry of §8.7 Timers's map of setTimeout and setInterval IDs lost its `repeat` — it is the "
               "boolean the timer initialization steps were given and it decides substep 9.11 against 9.12, "
               "so a value that is not one means a writer of this map skipped timer_set");
        repeat = JS_ToBool(docctx, rep);
        JS_FreeValue(docctx, rep);
    }
    /* §8.7's `timeout` AS A VALUE, travelling to substep 9.11 UNTOUCHED — this is the whole of what used to be
       impossible here. The re-arm re-performs the timer initialization steps, whose step 5 compares this
       against 4; over unknown external input that comparison is a FORK, and a fork needs a resume point for
       the sibling, which a plain C function firing a timer does not have. So nothing is computed from it here
       at all: it is carried, and the machine the task calls asks the question where a machine can. */
    timeout = JS_GetPropertyUint32(docctx, e, TE_TIMEOUT);
    /* §8.7's STEP 1 `thisArg`, decided at the SET and carried to substep 9.7's invocation — see TE_THIS. */
    this_arg = JS_GetPropertyUint32(docctx, e, TE_THIS);
    DCHECK(JS_IsUndefined(this_arg) || window_proxy_is(this_arg),
           "an entry of §8.7 Timers's map holds a step 1 `thisArg` that is neither a WindowProxy nor the "
           "`undefined` `run steps after a timeout` leaves — step 1's two arms are a WorkerGlobalScope global "
           "and \"the WindowProxy that corresponds to global\", and this engine installs §8.7's members on no "
           "worker global, so a third value means a writer of this map skipped timer_set");
    JS_FreeValue(docctx, e);
    JS_FreeValue(docctx, q);

    /* §8.7 Timers's STEP 12's COMPLETION STEP — "queues a global task on the timer task source given global
       to run TASK". What is queued is step 9's TASK and not the page's handler: the task is the thing step 11
       hangs the timer nesting level on, so a queue of the bare handler had nowhere to put it and step 3 had
       nothing to read. It is still the page's own code that ends up running, through the same flow machinery
       as before — the task's own substep 9.7 invokes the handler, and a step machine is how a C algorithm
       runs page code without the drive-to-completion this engine aborts on.
       IN THE REALM WHOSE MAP HELD IT, which is the GLOBAL half of the operation §8.1.7.2 Queuing tasks
       defines and step 12 invokes: the global is the one the timer was set on, never whichever realm the
       driver happens to be stepping. Minting the callee in `docctx` is the same statement one level down —
       a C function answers out of the realm that DEFINED it. */
    {
        JSValueConst targ[TT_ARG_N];
        JSValue task, lvl, idv, repv;

        DCHECK(g_task_stepid >= 0,
               "§8.7 Timers's step 9 task was queued before timer_init registered its machine — the machine "
               "is declared once per agent and every expiry goes through it");
        lvl = JS_NewInt32(docctx, nest);
        idv = JS_NewUint32(docctx, handle);
        repv = JS_NewBool(docctx, repeat != 0);
        targ[TT_ARG_HANDLER] = fn;
        targ[TT_ARG_THIS] = this_arg;
        targ[TT_ARG_NEST] = lvl;
        targ[TT_ARG_ID] = idv;
        targ[TT_ARG_REPEAT] = repv;
        targ[TT_ARG_TIMEOUT] = timeout;
        task = JS_NewCFunction2(docctx, NULL, "timerTask", TT_ARG_N, JS_CFUNC_step, g_task_stepid);
        CHECK(!JS_IsException(task),
              "timer: §8.7 Timers's step 9 task could not be allocated — a dropped task is a callback the "
              "page asked for and never gets, which is invisible from outside");
        JS_EnqueueCallTask(docctx, task, TT_ARG_N, targ);
        JS_FreeValue(docctx, task);
        JS_FreeValue(docctx, lvl);
        JS_FreeValue(docctx, idv);
        JS_FreeValue(docctx, repv);
    }
    JS_FreeValue(docctx, timeout);
    JS_FreeValue(docctx, this_arg);
    JS_FreeValue(docctx, fn);

    /* §8.7 Timers's `run steps after a timeout` STEP 4.5 — "Remove global's map of active timers[timerKey]",
       which the spec performs immediately after step 4.4 performs completionSteps, and step 12's completionStep
       is exactly the queue above. So the ARMING is gone the instant the task exists, while the entry itself —
       the map-of-setTimeout-and-setInterval-IDs record — stays for substeps 9.9-9.12 to find.
       WITHOUT THIS THE SAME TIMER FIRES TWICE. The driver reaches this function whenever it has nothing else
       runnable, and a handler parked on a reply the host still owes is precisely such a moment (see the
       paragraph above on why that debt must not be ahead of a due timer): a still-armed entry at a moment the
       clock has already reached would be picked again and queue a second task for one expiry.
       IT IS LOOKED UP RATHER THAN WRITTEN THROUGH `e`, because JS_EnqueueCallTask ran in between and the
       index the walk produced is a fact about the map as it stood BEFORE it. */
    {
        int at = timer_entry_index(docctx, handle);
        JSValue m;

        DCHECK(at >= 0,
               "the entry whose task was just queued is no longer in its global's map of setTimeout and "
               "setInterval IDs — queuing a task runs none of the page's code, and substep 9.12 is what "
               "removes it, from inside the task that has not started");
        m = timer_map(docctx);
        {
            JSValue ent = JS_GetPropertyUint32(docctx, m, (uint32_t)at);

            JS_SetPropertyUint32(docctx, ent, TE_WHEN, JS_UNDEFINED);
            JS_FreeValue(docctx, ent);
        }
        JS_FreeValue(docctx, m);
    }
    return 1;
}

/* HTML §8.7 Timers's TIMER INITIALIZATION STEPS, AS A STEP MACHINE — because step 4 is a COMPARISON and the
 * value it compares can be unknown external input.
 *
 * "If timeout is less than 0, then set timeout to 0" is a question with two real answers whenever the page
 * wrote `setTimeout(f, someUnknown)`, and §Solver forbids picking one: the false arm keeps the unknown and is
 * ordered against every other entry by the fork event_loop_before makes, while the TRUE arm concretizes to
 * the spec's own 0 — concretize-on-pin with the ALGORITHM doing the pinning rather than `x === 'admin'`.
 *
 * A PLAIN BODY COULD ASK THAT QUESTION AND COULD NOT ANSWER IT, WHICH IS THE WHOLE REASON THIS IS A MACHINE.
 * A fork prepares a sibling and needs a RESUME POINT for it. An interpreter at an OP_if clones its frame; a
 * walk between tasks (timer_earliest_in, above) re-reaches its own ask by re-running the flow's scheduler
 * step. A plain JSCFunction has NEITHER — it is already inside its own C activation — so the seam crashed there,
 * naming this operation, and the sibling that keeps the unknown could not be built at all. Which meant
 * the ORDER's concolic arm was unreachable: every OTHER writer of an entry's expiry writes a number
 * (the clock starts known, timer_after's `milliseconds` is the engine's own, an interval's re-arm is
 * arithmetic), so the ONE producer of an unknown expiry was the sibling this could not park.
 * As a machine the driver holds the resume point: step_fork_run snapshots the flow AT the ask, and the sibling
 * re-enters this body at this stage holding the other arm.
 *
 * IT IS A DECLARATION AND NOT A DISPATCH. Nothing asks at a call site which implementation to run — the member
 * is declared `idl_method_id_step` at timer_init and there is no other body for anything to select against.
 *
 * AND THE THREE INVOCATIONS ARE THREE DECLARATIONS OF ONE BODY, WHICH IS THE SAME SENTENCE. §8.7 states the
 * timer initialization steps ONCE and reaches them from three places — `setTimeout`, `setInterval`, and
 * substep 9.11's re-performance — differing only in `repeat` and in whether `previousId` was given. A magic is
 * exactly what a declaration carries for that ("one declaration serving two members is exactly what a magic is
 * for" — idl_args.h), so the re-arm is a third magic on TI_DECL and never a second implementation. The
 * alternative is what stood in timer_run_due: a hand-written re-speller of steps 5, 10, 11 and 13, which is
 * the dual system §Disposition forbids and which could not ask step 5's fork at all. */
#define TI_MAGIC_TIMEOUT   0   /* setTimeout: repeat false, previousId not given */
#define TI_MAGIC_INTERVAL  1   /* setInterval: repeat true, previousId not given */
#define TI_MAGIC_REARM     2   /* substep 9.11: repeat true, previousId given at position 2 */

#define TI_STAGES(X)                                                                                          \
    X(TI_THISARG,                                                                                             \
      "HTML §8.7 Timers timer initialization steps step 1 (let thisArg be global if that is a "                \
      "WorkerGlobalScope object; otherwise let thisArg be the WindowProxy that corresponds to global)")       \
    X(TI_NESTING,                                                                                             \
      "HTML §8.7 Timers timer initialization steps step 3 (if the surrounding agent's event loop's currently " \
      "running task is a task that was created by this algorithm, then let nestingLevel be the task's timer "  \
      "nesting level; otherwise let nestingLevel be 0)")                                                      \
    X(TI_TIMEOUT,                                                                                             \
      "HTML §8.7 Timers timer initialization steps step 4 (if timeout is less than 0, then set timeout to 0)") \
    X(TI_CLAMP,                                                                                               \
      "HTML §8.7 Timers timer initialization steps step 5 (if nestingLevel is greater than 5, and timeout is " \
      "less than 4, then set timeout to 4)")                                                                  \
    X(TI_SCHEDULE,                                                                                            \
      "HTML §8.7 Timers timer initialization steps steps 12-15 (the completion step that queues a global "     \
      "task on the timer task source, run steps after a timeout given that timeout, and return id)")
enum { IDL_STEP_STAGE_BASE(TI_STAGES) TI_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TI_STEPS[] = { TI_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* THE OPERATION HALF OF THE FORK'S KEY. step_fork_run keeps a BORROWED pointer to it on the header and the
   driver reads it AFTER this machine has returned, so it must outlive the return — a static literal does, and
   a stack buffer would dangle exactly where the constraint key is built. It names the SPEC STEP rather than
   this file, because decide.c keys a predicate by (source identity, operation) and two spellings of one
   question must compose to one key. */
static const char TI_STEP4_OP[] = "HTML §8.7 timer initialization steps step 4 (timeout < 0)";
/* AND STEP 5'S, WHICH IS A SECOND PREDICATE OVER THE SAME VALUE AND MUST NOT SHARE STEP 4'S KEY. `timeout < 0`
   and `timeout < 4` are different questions with different answer sets — a flow that answered the first FALSE
   has said nothing at all about the second — so folding them onto one name would let one arm's record decide
   its neighbour, which is exactly the "the flow's record of one predicate decides that predicate and never its
   neighbour" rule the constraint is keyed by (operation, operands) to keep. */
static const char TI_STEP5_OP[] = "HTML §8.7 timer initialization steps step 5 (timeout < 4)";

/* WHAT THE MACHINE HOLDS ACROSS THE FORK: step 4's RESULT, which is the only thing the stage below needs and
   the one thing it must not recompute — recomputing it would re-ask a question this flow has already answered.
   `placed` is the byte a machine needs and its stage cannot give it (the first stage IS the entry stage, so
   `stage == TI_THISARG` is true before anything has run): a zeroed JSValue is the INTEGER 0, not undefined
   (JS_TAG_INT is 0), so a visit walking `timeout` before its stage has written it would hand the fork a real
   value the page can see.
   `this_arg` IS STEP 1'S ANSWER AND `has_this` IS ITS OWN `placed`, for the same reason `timeout` has one: the
   value is OWNED (document_window_proxy answers a BORROWED reference and this takes its own, so the entry
   built four stages later cannot depend on a proxy the realm might have replaced in between), and a zeroed
   JSValue would be an integer the visit would hand a fork.
   `nest` IS STEP 3'S ANSWER AND `nested` IS ITS OWN `placed`, for the same reason and not for a weaker one: a
   zeroed `nest` is 0, which is a LEGITIMATE level (step 3's "Otherwise"), so a stage reading it before step 3
   ran would read a real answer to a question nobody asked and step 5 would silently never clamp. The two
   bytes are separate because the two stages are, and a machine re-entered at TI_CLAMP by a sibling must be
   able to say which of them its own arm has written. */
typedef struct {
    JSValue timeout;
    JSValue this_arg;
    int     nest;
    uint8_t placed;
    uint8_t nested;
    uint8_t has_this;
} TimerInitState;

static void ti_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    TimerInitState *s = st;

    if (s->has_this)
        v->val(ctx, &s->this_arg);
    if (s->placed)
        v->val(ctx, &s->timeout);
}

/* THERE IS NO `release`. Everything this state owns is the one JSValue the visit above names, so the teardown
   frees it through that ONE list — which is why no arm below frees `s->timeout` on its way out, including the
   string arm, which reaches a return without ever reading it. A free at one exit is the second ownership list
   this declaration exists to have only one of, and the arm that forgot it would leak while the arm that had it
   would double-free the sibling's copy. */

static int js_set_timer(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    TimerInitState *s = state;
    int magic = idl_step_magic(hdr);
    /* §8.7's `repeat` AND its `previousId`, WHICH ARE THE ONLY THINGS THE THREE INVOCATIONS DIFFER BY. The two
       page-facing members are step 2's "previousId was NOT given"; substep 9.11's re-performance is "given
       global, handler, timeout, arguments, TRUE, and ID" and declares the third position for it. */
    int repeat = (magic != TI_MAGIC_TIMEOUT);
    uint32_t prev_id = 0;

    (void)out_cb; (void)out_argc;
    /* This machine makes no request that delivers a value, so nothing below reads the answer to one. Freed on
       every entry, above the dispatch, because it belongs to no stage. */
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    /* §3.6's ARITY CHECK AND STEP 14.2's DEFAULT ARE THE DECLARATION'S, and both are asserted rather than
       re-derived. `idl_optional_from(1)` makes position 0 required, so `setTimeout()` is a TypeError before
       this body is entered — the `if (argc < 1) JS_ThrowTypeError` that stood here was a consumer restating
       its producer's contract. `idl_arg_default(1, IDL_DEFAULT_ZERO)` then guarantees position 1, so an
       `argc >= 2` test here was a hole that could never be taken.
       IT IS AN EQUALITY AND NOT A `>=`, so the day §8.7's `any... arguments` tail is declared (see timer_init,
       which names why it is not) this fires and names the two positions below that assume the count. */
    DCHECK(argc == (magic == TI_MAGIC_REARM ? 3 : 2),
           "§8.7's timer initialization steps reached their body with an argument count no declaration of them "
           "produces — the two page-facing members have position 0 required and position 1 carrying the IDL's "
           "`= 0`, and substep 9.11's door declares those two plus step 2's `previousId`");
    if (magic == TI_MAGIC_REARM) {
        double d = 0;

        JS_ToFloat64(ctx, &d, argv[2]);
        prev_id = (uint32_t)d;
        DCHECK(prev_id >= 1,
               "§8.7's substep 9.11 re-performed the timer initialization steps with a `previousId` below 1 — "
               "step 2 hands out identifiers \"greater than zero\", so 0 is the handle that names nothing and "
               "cannot be an id the task was queued with");
    }

    STEP_DISPATCH(TI_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(TI_THISARG);
    /* §8.7 step 1: "Let thisArg be global if that is a WorkerGlobalScope object; otherwise let thisArg be the
       WindowProxy that corresponds to global." Substep 9.7 invokes the handler "with callback this value set
       to thisArg", and this engine invoked it with `undefined` — which agreed with a browser only by accident,
       because a SLOPPY function's undefined receiver is replaced by ECMAScript §10.2.1.2 OrdinaryCallBindThis
       with the realm's globalThis, and almost every timer callback is sloppy. A strict one keeps what it is
       given, and answered `undefined` where a browser answers the WindowProxy.
       IT IS THE WindowProxy AND NOT THE GLOBAL, which is the half of step 1 that is easy to get wrong: `this`
       inside a timer callback is the object HTML §7.2.3 The WindowProxy exotic object defines and that `window`
       evaluates to, never the Window the realm's [[GlobalObject]] is. document_window_proxy answers exactly
       that for this realm and is BORROWED, so the reference this state keeps is its own.
       THE WORKER ARM IS ABSENT AND SAYS SO. §8.7's members are installed from core/platform.c's per-document
       column and this engine has no WorkerGlobalScope, so step 1's first clause is unreachable; the day a
       worker global gets them, the assert below is what fires. */
    {
        JSValueConst proxy = document_window_proxy(ctx);

        DCHECK(window_proxy_is(proxy),
               "§8.7's step 1 asked this realm for the WindowProxy that corresponds to its global and got "
               "something that is not one — every realm §8.7's members are installed on is a Window's, and a "
               "WorkerGlobalScope (step 1's other arm, which would be `thisArg` itself) is a global this "
               "engine does not build");
        s->this_arg = JS_DupValue(ctx, proxy);
        s->has_this = 1;
    }
    STEP_GOTO(hdr->stage, TI_NESTING, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(TI_NESTING);
    /* §8.7 step 3: "If the surrounding agent's event loop's currently running task is a task that was created
       by this algorithm, then let nestingLevel be the task's timer nesting level. Otherwise, let nestingLevel
       be 0." Both halves of that are ONE read here, because the level a task publishes IS 0 when it is not one
       of this algorithm's — see core/timing/event_loop.h on why 0 is the positive statement and not a default.
       IT IS ITS OWN STAGE BECAUSE IT IS ITS OWN STEP. Nothing here can fork or suspend, so the rest point it
       offers is declined at once; what it buys is that a parked machine can SAY it is standing at step 3, and
       that the arm below cannot quietly become "steps 3 and 4" the next time either of them grows. */
    s->nest = event_loop_timer_nesting(ctx);
    s->nested = 1;
    STEP_GOTO(hdr->stage, TI_TIMEOUT, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(TI_TIMEOUT);
    {
        double delay = 0;

        DCHECK(s->nested,
               "§8.7's timer initialization steps reached step 4 with no `nestingLevel` — step 3 is the only "
               "writer of it and the stage before this one is step 3, so an unwritten value means this "
               "machine was entered at a stage it did not leave");

        /* `timeout` is a Web IDL `long`, so §3.2.4.5's ConvertToInt(V, 32, "signed") already ran in the
           declaration — UNLESS the page passed unknown external input, which crosses an IDL boundary as itself
           so that opacity survives the coercion. The tag test that stood here asserted the first case and fired
           on the second: `setTimeout(f, timeout * n)` with an unknown `n` is a page's ordinary arithmetic, and
           §Offensive names a forced-exec flow meeting opaque input as the exploration surface rather than a
           broken invariant. idl_number_of answers both — the converted Number for a real value, and for an
           unknown the SAME §3.2.4.5 conversion run on that value's own EXAMPLE.
           THE EXAMPLE IS THE ORDERING KEY, AND THAT IS ALL IT IS. HTML §8.7 Timers orders one invocation
           against another in `run steps after a timeout` step 4.2: "Wait until any invocations of this
           algorithm that had the same global and orderingIdentifier, that started before this one, and whose
           milliseconds is less than or equal to this one's, have completed" — a comparison the ENGINE makes,
           never one the page wrote. This component realises it as (now + timeout, insertion order) on the
           virtual clock, so what it needs from the value is one number.
           AN UNKNOWN IS NOT A MISSING NUMBER, IT IS A FORK, and §8.7 states the fork itself.
           Step 4 is "If timeout is less than 0, then set timeout to 0" — a comparison over exactly this value,
           whose TRUE arm the SPEC then assigns a concrete 0 to. So one arm of every such call runs on a number
           nothing invented, and the other keeps the unknown and is ordered by event_loop_before's fork. This is
           the concretize-on-pin rule with the spec doing the pinning: `x === 'admin'` concretizes because the
           code determined the value, and step 4's true arm concretizes because the ALGORITHM determined it. */
        /* WHETHER STEP 4 FORKS IS A QUESTION ABOUT THE VALUE'S DOMAIN, AND ASKING IT OF THE VALUE'S EXAMPLE
           ANSWERED A DIFFERENT ONE. This test used to be `!idl_number_of(...)` — so an unknown that HAD an
           example never reached the fork at all: it fell into the arm below, which ran step 4 on the example
           and stored a plain Number, and the unknown was gone. That is the collapse §Solver-half forbids
           ("never collapse a modelable value to bare-concrete — that deletes the fork and its coverage"),
           performed silently, because what replaced the value was a REAL number the engine had computed and
           nothing downstream could tell it from a timeout the page wrote as a literal. The domain is what
           decides: §3.2.4.5's `long` spans [-2147483648, 2147483647] and ConvertToInt is TOTAL, so nothing has
           excluded either sign for ANY unknown, with an example or without one, and both completions of step 4
           are feasible over every one of them. So the fork is asked over the unknown, and the example is
           demoted to what it is everywhere else in this engine — the answer to WHICH ARM A REAL SESSION TAKES,
           not to whether there are two. */
        if (concolic_is(argv[1])) {
            /* §8.7 step 4 over an unknown: both outcomes are feasible, so BOTH arms run — this flow takes one
               and the driver snapshots the machine for the other.
               OUTCOME 0 IS THE FALSE ARM, which is step_fork_run's own rule read against this predicate: a
               run with no forking policy (the @S candidate re-fire) takes outcome 0, and the ordinary
               completion of "is this timeout negative" is that it is not. Numbering the spec's ASSIGNMENT
               first would divert every candidate re-fire onto an arm the page's own value does not take.
               `real` IS THIS MACHINE'S DECLARATION AND IT IS STEP 4'S OWN COMPARISON, RUN. HTML §8.7 Timers'
               timer initialization steps step 4 is "If timeout is less than 0, then set timeout to 0", and
               `delay` is that timeout: §3.2.4.5's ConvertToInt(V, 32, "signed") run by idl_number_of on this
               value's own EXAMPLE, through the one copy of that arithmetic. So `delay < 0` is the completion
               the operation reaches on the example — computed by performing the comparison, never by a rule
               predicting it — which is the same shape decide.c computes for a bytecode branch and is what
               makes one arm this flow's primary and the other FORCED.
               WITH NO EXAMPLE THERE IS NOTHING TO RUN IT ON, and JS_OUTCOME_REAL_UNSTATED is the positive
               statement of that rather than a guess: a `setTimeout(f, x)` whose delay is example-free has no
               observed completion, so neither arm may be called the real one and neither is marked forced.
               idl_number_of answers 0 for exactly that value, which is why its answer is read here as the
               presence of an example and no longer as the presence of a fork. */
            int have = idl_number_of(ctx, IDL_LONG, argv[1], &delay);
            int real = have ? (delay < 0) : JS_OUTCOME_REAL_UNSTATED;
            int arm = 0, rc = step_fork_run(ctx, hdr, argv[1], TI_STEP4_OP, 2, real, &arm);

            if (rc)
                return rc;
            DCHECK(arm == 0 || arm == 1,
                   "a two-armed outcome fork answered with an arm that is neither of them");
            /* The unknown is BORROWED from the machine's own converted-argument vector, which outlives this
               return and is what the sibling's clone dups — so the arm that keeps it takes its own reference
               and the vector keeps its. */
            s->timeout = (arm == 1) ? JS_NewFloat64(ctx, 0)   /* step 4's own assignment, on its own arm */
                                    : JS_DupValue(ctx, argv[1]);
        } else {
            int have = idl_number_of(ctx, IDL_LONG, argv[1], &delay);

            DCHECK(have,
                   "§3.2.4.5's conversion produced no number for a position this arm has already established "
                   "is NOT unknown external input — idl_number_of answers 0 only for an unknown carrying no "
                   "example, so a 0 here is a value that is neither a Number nor a concolic reaching a body "
                   "whose declaration converts every numeric argument");
            /* §3.2.4.5's own postcondition, which is what the tag test was reaching for and could not state:
               ConvertToInt takes the integer part modulo 2**32 and folds it into range, so a `long` is ALWAYS
               an integer in [-2147483648, 2147483647] — NaN and the infinities became +0 in the conversion. */
            DCHECK(delay >= -2147483648.0 && delay <= 2147483647.0 && delay == (double)(int32_t)delay,
                   "§3.2.4.5's `long` conversion answered something that is not a long — its result is the "
                   "integer part taken modulo 2**32 and folded into range, so a value outside it, or one with "
                   "a fraction, means this position was never converted by anything");
            /* §8.7 step 4: "If timeout is less than 0, then set timeout to 0." */
            if (delay < 0)
                delay = 0;
            s->timeout = JS_NewFloat64(ctx, delay);
        }
        s->placed = 1;
        STEP_GOTO(hdr->stage, TI_CLAMP, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(TI_CLAMP);
    /* §8.7 step 5: "If nestingLevel is greater than 5, and timeout is less than 4, then set timeout to 4."
     *
     * THE TWO CONJUNCTS ARE NOT ALIKE AND THE ORDER MATTERS. `nestingLevel` is a count this engine performed —
     * step 3 read it off a task this engine queued — so it is always a real integer and its half of the `and`
     * is DECIDED, never forked. `timeout` is the page's, and after step 4's false arm it can still be unknown
     * external input; so the second conjunct is a second predicate over that unknown, with its own key (see
     * TI_STEP5_OP) and its own two feasible arms. Asking it FIRST would mint a fork for every `setTimeout`
     * over an unknown delay whatever the nesting level, and five sixths of those arms would be worlds step 5
     * cannot enter — the short-circuit is the spec's own `and`, not an optimisation.
     *
     * AND WHAT THIS IS NOT IS A BOUND. §NO BOUNDS forbids a cap because a cap decides that work will not
     * HAPPEN; this decides only WHEN a timer becomes due. Every callback still runs, no flow is dropped,
     * starved, skipped, reordered or forgotten, and the frontier is the same set of work items either way.
     * What it removes is a STARVATION: with no clamp a `setTimeout(f, 0)` chain re-arms at the same virtual
     * moment for ever, timer_earliest picks it at every turn, and a `setTimeout(g, 1)` set beside it is never
     * reached at all. The clamp is what lets the other timer run — it is the opposite of a truncation. */
    DCHECK(s->nested && s->placed,
           "§8.7's timer initialization steps reached step 5 without both of the values it compares — step 3 "
           "writes `nestingLevel` and step 4 writes `timeout`, and both stages precede this one");
    if (s->nest > 5) {
        if (concolic_is(s->timeout)) {
            /* Step 5's second conjunct over an unknown: both completions are feasible over §3.2.4.5's whole
               `long`, so BOTH arms run — this flow takes one and the driver snapshots the machine for the
               other. Outcome 0 is the FALSE arm for the reason step 4's is: the ordinary completion of "is
               this timeout under 4" is that it is not, and a candidate re-fire (which has no forking policy
               and takes outcome 0) must not be diverted onto the spec's assignment. */
            double delay = 0;
            int have = idl_number_of(ctx, IDL_LONG, s->timeout, &delay);
            int real = have ? (delay < 4) : JS_OUTCOME_REAL_UNSTATED;
            int arm = 0, rc = step_fork_run(ctx, hdr, s->timeout, TI_STEP5_OP, 2, real, &arm);

            if (rc)
                return rc;
            DCHECK(arm == 0 || arm == 1,
                   "a two-armed outcome fork answered with an arm that is neither of them");
            if (arm == 1) {
                /* Step 5's own assignment, on its own arm — concretize-on-pin with the ALGORITHM doing the
                   pinning, exactly as step 4's true arm does. The unknown it replaces is released here
                   because ti_visit's ONE list names this field: the state keeps owning whatever stands in
                   it, so an overwrite that did not free would leak the value the sibling still holds a
                   reference to. */
                JS_FreeValue(ctx, s->timeout);
                s->timeout = JS_NewFloat64(ctx, 4);
            }
        } else {
            double delay = 0;
            /* OUTSIDE the assert, because a DCHECK's condition is compiled out in release and this one WRITES
               the number the line below compares — folded in, a release build would clamp every timeout at a
               nesting level past 5 to 4 on the strength of an uninitialised read. */
            int have = idl_number_of(ctx, IDL_LONG, s->timeout, &delay);

            DCHECK(have,
                   "§8.7's step 5 reached a `timeout` that is neither unknown external input nor a number — "
                   "step 4 writes one of exactly those two on every arm it has");
            if (delay < 4) {
                JS_FreeValue(ctx, s->timeout);
                s->timeout = JS_NewFloat64(ctx, 4);
            }
        }
    }
    STEP_GOTO(hdr->stage, TI_SCHEDULE, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(TI_SCHEDULE);
    DCHECK(s->placed && s->nested && s->has_this,
           "§8.7's timer initialization steps reached step 12 with no `timeout`, no `nestingLevel` or no "
           "`thisArg` — steps 4, 3 and 1 are their only writers and all three stages precede this one, so an "
           "unwritten value means this machine was entered at a stage it did not leave");

    /* THE STRING FORM IS A SCRIPT — compiled and run in global scope, which is the sink `setTimeout(str)` is
       known for. It is queued as a script flow (the path an injected <script> takes), never evaluated here:
       running the page's source inside this C activation is exactly what the flow machinery exists to avoid.
       It has no cancellable entry because it is no longer a timer at that point; it is a script. */
    /* WHICH ARM UNKNOWN EXTERNAL INPUT TAKES IS THIS ALGORITHM'S QUESTION, AND IT IS ASKED BEFORE IsCallable —
       which is the whole of what was wrong here, and it was invisible because both arms are real. §8.7's task
       substeps split on "If handler is a Function … Otherwise: … Assert: handler is a string", and Web IDL
       §3.2.25 Union types answers that for `(DOMString or Function)` with IsCallable(V). A concolic is an
       object carrying a [[Call]] — solver/concolic.c installs one so `document.cookie.indexOf(x)` yields
       another unknown instead of throwing — so IsCallable is TRUE over EVERY unknown external input, for a
       reason that is a fact about this engine's value class and not about the page's value. Asking it here
       therefore chose an arm from the model, which is the collapse §@S forbids, and it did not choose
       neutrally: the callback arm over an unknown callee runs no page code and emits nothing (concolic_call
       mints a derived unknown and returns), while the string arm is the code-execution sink this file exists to
       announce. Measured on the shipped artifact: `eval(location.hash.substr(1))` produced a fire-verified @S
       PoC and `setTimeout(location.hash.substr(1), 1)` on the same page shape produced no @S entry at all —
       the arm below had never been entered by an attacker value in production.
       SO UNKNOWN INPUT TAKES THE STRING ARM, and that is not a guess standing in for a fork: it is the only arm
       with an observable in it, and the @S search resolves it for real — a candidate run substitutes a
       CONCRETE breakout at this source, at which point IsCallable is false, this same test picks the same arm
       for the page's own reason, and the classic script below runs the marker. idl_args.c's own assert states
       the other half: the union may not be resolved over a concolic at the boundary either. */
    if (concolic_is(argv[0]) || !JS_IsFunction(ctx, argv[0])) {
        /* TimerHandler is `(DOMString or Function)`: the declaration converted the non-callable arm to a
           string already, so this reads one rather than running the page's toString from C. */
        const char *src;
        JSValue text;

        DCHECK(magic != TI_MAGIC_REARM,
               "§8.7 Timers's substep 9.11 re-performed the timer initialization steps over a handler that is "
               "not a Function — the string form of the algorithm never makes a map entry at all (it queues a "
               "script and returns a handle naming nothing), so no entry can carry one to the fire, and "
               "timer_run_due asserts the same fact from the other end");

        /* §8.7 step 4 ran at the stage above, where the spec puts it, and this arm makes no entry for its
           result to be the expiry of — it simply never reads it. Nothing is released here; ti_visit's one list
           is what discharges the value, on every exit there is.
           §8.7 Timers's STRING ARM OVER UNKNOWN EXTERNAL INPUT IS A CODE-EXECUTION SINK, NOT A BROKEN INVARIANT.
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
           value IS a string, the branch below queues it as §8.7 Timers requires, and a marker in it fires there.
           AND THE ANNOUNCEMENT IS THE SAME OPERATION AS THAT SUPPLY, WHICH IS THE FIX. The two were spelled as
           two arms of an `if`: the unknown handler was announced and queued nothing, the string handler was
           queued and announced nothing. Detection therefore worked and every candidate run after it was
           INVISIBLE — the search's context probe arrived here as a real string, nothing scanned it, no witness
           was learned, no §12 escape was derived, and a real page's string-bodied timer sink parked at
           probes == payloads for ever with the record honestly saying `witnessed:0`. So the program text comes
           OUT of the announcing operation (solve.h's solve_eval_sink_source) and there is no arm left that can
           queue bytes nothing looked at: `eval` and this share one detector, and now one ordering too. */
        DCHECK(JS_IsString(argv[0]) || concolic_is(argv[0]),
               "setTimeout's handler reached the body neither callable, nor a string, nor unknown external "
               "input — the TimerHandler union's conversion is the declaration's, not this body's");
        /* §8.7 Timers's substeps 9.8.2-9.8.3 — "Assert: handler is a string" and the
           EnsureCSPDoesNotBlockStringCompilation beside it, which is what makes these bytes a JS-context sink
           rather than an ordinary program. The answer is the text substep 9.8.7 creates a classic script from,
           or the absence of one. */
        text = solve_eval_sink_source(ctx, argv[0]);
        if (JS_IsUninitialized(text)) {
            *presult = JS_NewInt32(ctx, (int32_t)timer_next_handle(ctx));
            return JS_STEP_DONE;
        }
        src = JS_ToCString(ctx, text);
        JS_FreeValue(ctx, text);
        if (!src) {
            /* THE COVERAGE GOES WITH THE PROGRAM IT COVERED. This is the ONE line between the announcement and
               the queue that can leave without queueing, so it is the one place the host seam's latch can
               outlive the bytes it was raised for — and a latch that survives its own program is an assert
               that answers YES for whatever queues next, which is the assert lying rather than firing. */
            (void)solve_eval_sink_announced();
            return JS_STEP_ABRUPT;
        }
        DCHECK(g_script_sink != NULL,
               "setTimeout was given a STRING handler and this host registered no way to evaluate one — HTML "
               "§8.7 Timers evaluates it when the timer fires, and dropping it would lose whatever it was going to do");
        /* IN THIS REALM'S DOCUMENT — §8.7 Timers compiles the string with the entry global object's settings, which
           is the Window whose `setTimeout` was called and not the agent's root. */
        g_script_sink(document_doc(ctx), src);
        JS_FreeCString(ctx, src);
        /* §8.7 Timers still hands back a handle from THIS global's identifier, and it still names no entry — the
           script is queued, and there is nothing left for `clearTimeout` to find. */
        *presult = JS_NewInt32(ctx, (int32_t)timer_next_handle(ctx));   /* §8.7's return type is a `long` */
        return JS_STEP_DONE;
    }

    /* AN UNKNOWN PERIOD IS NOT REFUSED HERE ANY MORE, AND THE DELETION IS THE DIFF. What stood here was a
       DFAIL on `setInterval(f, someUnknownValue)` — a real shape in a real bundle, a poll whose period comes
       from a config field or a reply — and its reason was that substep 9.11 re-performs this algorithm at
       every fire, so step 5's comparison is a FORK over that unknown once per re-arm, and the re-arm ran in
       timer_run_due, a plain C function with no resume point to hand a sibling. The re-arm is now substep
       9.11's own call of THIS machine, from the task, so step 5 is asked at TI_CLAMP exactly as it is asked
       for the first arm — same body, same key, same two feasible outcomes, and the sibling parks on the
       task's frame like every other flow. There is nothing left to refuse: `timeout` is carried as a VALUE
       from here to the entry and from the entry to the task, and no arm of this file reads a number out of
       it. */
    /* BORROWED, NOT HANDED OVER: timer_set reads `timeout` through event_loop_moment_plus, which builds the expiry
       as a NEW value on both of its paths, so the state keeps owning this one and the teardown releases it.
       §8.7's `any... arguments` tail is not declared, so there are no extra arguments to pass — see timer_init,
       which names what declaring it costs.
       §8.7 STEP 10, "Increment nestingLevel by one", IS THE `+ 1` — and step 11 is what timer_set does with
       it. The increment is HERE and not inside timer_set because it is this algorithm's step: `timer_after`'s
       task is not one this algorithm created, so it passes step 3's own 0 through unincremented.
       §8.7 STEP 15 IS THE RETURN AND IT IS WHAT SUBSTEP 9.11 RELIES ON: "Return id", which for the re-arm is
       step 2's `previousId` back again, so the identifier the page is holding keeps naming this timer. The
       task's own DCHECK reads that from the other end. */
    *presult = JS_NewInt32(ctx, timer_set(ctx, s->timeout, repeat, s->nest + 1, prev_id, s->this_arg,
                                          argv[0], 0, NULL));
    return JS_STEP_DONE;
    /* nothing is queued: the driver asks when the clock may move */
}

static const IdlStepDecl TI_DECL = {
    js_set_timer, sizeof(TimerInitState), ti_visit, NULL,
    "HTML §8.7 Timers timer initialization steps", TI_STEPS
};

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
    /* AND ITS TASK IS NOT ONE THE TIMER INITIALIZATION STEPS CREATED, so the level it publishes is step 3's
       own "Otherwise": 0. `run steps after a timeout` is a DIFFERENT algorithm — §8.7's step 13 invokes it
       from the timer initialization steps rather than being them — so an engine algorithm scheduled through
       here must not make a `setTimeout` its completion steps perform look nested. */
    {
        JSValue msv = JS_NewFloat64(ctx, ms);
        /* `repeat` FALSE and `previousId` 0 — "§8.7 Timers's completion steps are performed once; there is no
           interval form of this", and this algorithm has no identifier map of its own to re-key into.
           AND `thisArg` UNDEFINED, which is this algorithm's own silence rather than a value withheld: step 1
           belongs to the timer initialization steps, and `run steps after a timeout` states no this value for
           the completionSteps it performs. Handing them the WindowProxy anyway would be a claim no standard
           makes, on the engine's own algorithms, in the one place nothing would ever notice it was wrong. */
        int key = timer_set(ctx, msv, 0, 0, 0, JS_UNDEFINED, steps, 0, NULL);
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

/* THE SMALLEST IDENTIFIER AT OR ABOVE `from` THAT THIS GLOBAL'S MAP OF setTimeout AND setInterval IDs HOLDS, or
   0 when it holds none at or above it — 0 being the answer §8.7 Timers already reserves for "names no entry",
   since its step 2 hands out integers "greater than zero".
   A SCAN AND NOT A COLLECTED LIST, and that is the design rather than a shortcut: a list of the live
   identifiers would be a C allocation whose head a context switch reverts while its nodes stay reachable from
   nothing — the leak §State-isolation names as the one the runtime's own GC walk cannot see. The map is a JS
   Array on the realm's record, so walking it reads the RUNNING FLOW's own COW state and leaves nothing behind
   to unwind. */
static uint32_t timer_id_from(JSContext *ctx, uint32_t from)
{
    JSValue q = timer_map(ctx);
    uint32_t i, n = arr_len(ctx, q), best = 0;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i);

        if (JS_IsObject(e)) {
            uint32_t h = (uint32_t)timer_entry_num(ctx, e, TE_HANDLE);

            DCHECK(h >= 1,
                   "an entry of §8.7 Timers's map of setTimeout and setInterval IDs is filed under the "
                   "identifier 0 — step 2 hands out integers \"greater than zero\", so 0 is the handle a page "
                   "gets back for nothing and the one this walk reads as \"no entry at or above the cursor\"");
            if (h >= from && (best == 0 || h < best))
                best = h;
        }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, q);
    return best;
}

/* HTML §8.7 "Timers"'s clearTimeout AND clearInterval, AS ONE STEP MACHINE — because the identifier the page
 * hands them can be unknown external input, and "which entry does it name" is a question with as many real
 * answers as this global has entries.
 *
 * §8.7 STATES THE TWO MEMBERS AS ONE ALGORITHM, WHICH IS WHY THIS IS ONE BODY AND ONE OPERATION STRING: "The
 * clearTimeout(id) and clearInterval(id) method steps are to remove this's map of setTimeout and setInterval
 * IDs[id]", and immediately after it, "Because clearTimeout() and clearInterval() clear entries from the same
 * map, either method can be used to clear timers created by setTimeout() or setInterval()". Those steps are
 * PROSE and not a numbered list, so there is no step number to cite and none is written. Two names for the one
 * question would be two constraint keys for one fact — the mistake TI_STEP4_OP and TI_STEP5_OP are kept apart
 * to avoid, read from the other end.
 *
 * IT IS THE MAP OF setTimeout AND setInterval IDs AND NOT THE MAP OF ACTIVE TIMERS, which the entry enum at the
 * top of this file already separates and which decides what the completions ARE. §8.7 gives a global both maps:
 * the map of IDs is keyed by "a positive integer, corresponding to the return value of a setTimeout() or
 * setInterval() call" — the number the page is holding — while the map of active timers is keyed by "a unique
 * internal value" no page ever sees. `clearTimeout` removes from the FIRST. An entry can be in the map of IDs
 * and out of the map of active timers (its task is in flight, `run steps after a timeout` step 4.5 having
 * removed its expiry), and clearing exactly that is what makes a `clearInterval(id)` inside the handler stop
 * the re-arm — substep 9.9 is the check that reads it. So an entry is a completion whether or not it is armed,
 * and TE_WHEN says nothing here.
 *
 * WHAT THE COMPLETIONS ARE. Over an unknown `id` this global's map admits one world per entry it holds — that
 * entry removed, every other left alone — plus the world in which `id` names none of them and nothing is
 * removed. Every one of them is reachable, which is a fact about the ARGUMENT's type: §3.2.4.5 "long"'s
 * ConvertToInt(V, 32, "signed") is TOTAL and folds modulo 2**32, so every uint32 identifier is denoted by some
 * `long` and so is a value that names nothing (0 itself, which §8.7 hands out to nobody).
 *
 * AND THEY ARE ASKED ONE IDENTIFIER AT A TIME, WHICH IS WHERE THIS DIFFERS FROM EVERY OTHER N-WAY FORK IN THE
 * ENGINE AND WHY. An N-way completion is N-1 binary decisions (solver/decide.c), and solver_outcome walks that
 * sequence for a machine that declares `n` — but it names each completion by its INDEX, composing the
 * constraint key out of `"%d"` of the position. A POSITION IS A FACT ABOUT THE OPERAND ONLY WHERE THE SET IS
 * THE MACHINE'S OWN AND FIXED AT ITS DEFINITION, which §18.4.4's registered hash names are and this global's
 * live entries are not: the page mutates the map, so index k names a different timer at two asks, and one
 * predicate's record then decides its neighbour. That is not a hypothetical. A flow that answers "`id` is the
 * entry at rank 0" clears it; at a second `clearTimeout(id)` the map is one shorter, the recorded "not rank 0,
 * yes rank 1" replays against the SHIFTED enumeration, and the flow removes a timer no answer of its own ever
 * named — silently, with every arm in range and every assert on the path satisfied.
 * So the completion carries its NAME instead of its position: the timer's own §8.7 identifier, written into the
 * operation string, which is half of what step_fork_run keys on. Each link is then an ordinary two-armed
 * question — "is `id` the timer with identifier H" — over the same unknown, recorded in one boolean slot,
 * forking ONE sibling, with the chain drawn lazily, one link per time the scheduler picks this machine up. That
 * is the SAME elimination sequence solver_outcome walks and not a second mechanism beside it; what changes is
 * only what names each question. It costs nothing and buys two things: every key is a fact about `id` alone and
 * stays true for ever, and `n` is 2 at every ask, so the return protocol's ceiling (solver/decide.h's
 * SOLVER_FORKED_BIT, 256 completions) is never approached by a page holding more live timers than that — which
 * an animation loop or a poll-per-widget bundle does hold.
 *
 * THE ORDER IS ASCENDING BY IDENTIFIER, AND THE PARENT IS ON THE EXAMPLE'S ARM AT EVERY LINK. §8.7's identifiers
 * are strictly monotone per global (timer_next_handle), so ascending order is the same sequence in a sibling's
 * snapshot, after a park, and in a session that resumes this flow from the cold tier. The parent answers each
 * question with what its own example says — NO at every identifier the example is not, YES at the one it is —
 * so §Learning-from-replies' "the example marks the real arm PRIMARY" holds at every link WITHOUT the
 * permutation solver_outcome needs to get it at one, and the forced sibling at each link is the world in which
 * `id` is that identifier.
 * WITH NO EXAMPLE THE PARENT WALKS THE WHOLE CHAIN AND REMOVES NOTHING, which is not a weaker answer but the
 * same rule with nothing observed to state: the seam asks about completion 0 first when a machine says nothing,
 * so the parent answers NO at every identifier and ends where a run with no forking policy ends, while one
 * sibling per identifier carries the removal. Neither arm is marked forced, because nothing was contradicted.
 *
 * OUTCOME 0 IS "NO" AT EVERY LINK, which is step_fork_run's one numbering rule read against this predicate: a
 * run with no forking policy — the @S candidate re-fire — answers NO all the way down and removes nothing. It
 * must. Clearing is the one operation in this file that DELETES scheduled work, so a numbering that put a
 * removal on the arm a re-fire takes would let the candidate cancel the very timer whose callback carries the
 * sink it is running to reach. */
#define CT_STAGES(X)                                                                                          \
    X(CT_REMOVE,                                                                                              \
      "HTML §8.7 Timers the clearTimeout(id) and clearInterval(id) method steps (remove this's map of "        \
      "setTimeout and setInterval IDs[id])")
enum { IDL_STEP_STAGE_BASE(CT_STAGES) CT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CT_STEPS[] = { CT_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* WHAT THE MACHINE HOLDS ACROSS A LINK OF THE CHAIN, and neither field is a JSValue.
   `next` is the smallest §8.7 identifier this flow has not yet asked about. It needs no `placed` byte beside
   it, unlike the setter's `timeout`: §8.7's identifiers are "greater than zero" and timer_next_handle asserts
   it, so 0 is not an identifier any entry carries and a zeroed cursor is unambiguously "before the first".
   `op` IS THE FORK'S OPERATION STRING AND IT LIVES HERE rather than in a C local, because step_fork_run keeps a
   BORROWED pointer to it on the header and the driver reads it AFTER this machine has returned — a stack buffer
   would dangle exactly where the constraint key is built (core/permissions/permission_status.c says the same of
   its own). It is rewritten at each link, which is safe for the same reason: the previous link's string was
   read by the driver before this body was re-entered.
   NOTHING IS OWNED, so the visit names nothing. It is declared rather than omitted because a machine with no
   `visit` cannot be forked and is refused at registration, and forking is the whole of what this one is for. */
typedef struct {
    uint32_t next;
    char     op[128];
} ClearTimerState;

static void ct_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* THERE IS NOTHING FOR THE COMPLETE-BEFORE-THE-FIRST-THROW RULE TO BE BROKEN BY HERE, and saying so is what
   makes that checkable rather than assumed. The rule costs a leak: a state must hold every owned field before
   any operation that can fail, because the failure path tears it down through a teardown that frees exactly
   what the state holds and nothing else. This state owns nothing at all — the visit above is the whole
   declaration, and it names no field — and no line of this body runs the page's code, so there is neither a
   field to hand over late nor a throw to hand it over after. */

static int js_clear_timer(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ClearTimerState *s = state;
    double id = 0;
    int have;

    (void)out_cb; (void)out_argc;
    /* This machine makes no request that delivers a value, so nothing below reads the answer to one. Freed on
       every entry, above everything else, because it belongs to no link of the chain. */
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == CT_REMOVE,
           "§8.7's clearTimeout/clearInterval resumed into a stage the algorithm does not have — it is ONE "
           "step, and the chain of questions it may ask is a cursor on this machine's own state rather than a "
           "stage apiece, so a second stage means a resume landed in another algorithm's numbering");
    /* THE COUNT IS THE DECLARATION'S, exactly as it is for the two setters above. `idl_optional_from(0)` makes
       every position optional and `idl_arg_default(0, IDL_DEFAULT_ZERO)` places §8.7's `= 0` whether or not
       the page reached it, so `clearTimeout()` arrives here with ONE argument holding that 0 — and §8.7's step
       is "remove this's map of setTimeout and setInterval IDs[id]", which for 0 removes nothing because step 2
       hands out integers "greater than zero". The `if (argc < 1) return` that stood here was a consumer
       restating its producer's contract; the two answers coincide only because handles start at 1, which is a
       coincidence a body should not be relying on and an assert is what stops it from starting to. */
    DCHECK(argc == 1,
           "§8.7's `clearTimeout`/`clearInterval` reached its body with an argument count its declaration does "
           "not produce — its one position is optional and carries the IDL's `= 0`, so §3.6 step 14.2 places a "
           "value at it on every call");

    /* THE EXAMPLE IS READ ONCE PER ENTRY AND ABOVE THE CHAIN, so that every question this activation asks is
       decided against ONE reading of it — taking it again inside the loop would be two reads of one example
       with nothing forcing them to agree, which is the statement decide.c makes of its own. `have` is 0 for an
       unknown carrying no example, which is a POSITIVE fact the chain reads as JS_OUTCOME_REAL_UNSTATED and
       never as a number to fall back on. */
    have = idl_number_of(ctx, IDL_LONG, argv[0], &id);

    /* `id` IS A WEB IDL `long`, converted by the declaration — or unknown external input crossing the boundary
       as itself, so that opacity survives the coercion. WHICH OF THE TWO IT IS decides whether there is a
       question at all, and asking that of the value's EXAMPLE would answer a different one: an unknown that HAS
       an example would fall into the arm below, clear one particular entry, and the fork would be gone. That is
       the collapse §Solver-half forbids, performed silently, and it is the correction TI_TIMEOUT carries in as
       many words. `concolic_is` is what decides. */
    if (!concolic_is(argv[0])) {
        DCHECK(have,
               "§3.2.4.5's conversion produced no number for a position this arm has already established is NOT "
               "unknown external input — idl_number_of answers 0 only for an unknown carrying no example, so a "
               "0 here is a value that is neither a Number nor a concolic reaching a body whose declaration "
               "converts its one numeric argument");
        /* §3.2.4.5 "long"'s own postcondition: §3.2.4.9 Abstract operations' ConvertToInt takes the integer
           part modulo 2**32 and folds it into range, so a `long` is ALWAYS an integer in [-2147483648,
           2147483647] — NaN and both infinities became +0 in the conversion. */
        DCHECK(id >= -2147483648.0 && id <= 2147483647.0 && id == (double)(int32_t)id,
               "§3.2.4.5's `long` conversion answered something that is not a long — its result is the integer "
               "part taken modulo 2**32 and folded into range, so a value outside it, or one with a fraction, "
               "means this position was never converted by anything");
        timer_clear(ctx, (uint32_t)(int32_t)id);
        return JS_STEP_DONE;
    }

    for (;;) {
        uint32_t h = timer_id_from(ctx, s->next);
        int arm = 0, real, rc, wrote;

        /* EVERY IDENTIFIER ELIMINATED. §8.7's removal of a key the map does not have is a no-op, so this world
           is the one in which `id` names no entry of this global's map of setTimeout and setInterval IDs and
           nothing is removed — the completion a run with no forking policy walks straight to. It is also the
           whole of the answer for a global whose map is EMPTY, which is why an empty map asks no question: one
           feasible completion is not a fork, it is the answer, and a seam asked to decide it would be handed a
           decision this body had already made. */
        if (h == 0)
            return JS_STEP_DONE;
        /* THE MACHINE'S SECOND DECLARATION — which completion this operation reaches on the operand's own
           EXAMPLE, computed by RUNNING the comparison rather than by a rule predicting it. §3.2.4.5's
           conversion has already been run on the example above, through the one copy of that arithmetic, and
           the cast is the same one the known arm performs: a negative `long` denotes the identifier its two's
           complement names, which is how identifiers at or above 2**31 stay reachable at all. */
        real = have ? ((uint32_t)(int32_t)id == h) : JS_OUTCOME_REAL_UNSTATED;
        wrote = snprintf(s->op, sizeof s->op,
                         "HTML §8.7 Timers clearTimeout/clearInterval (id is the timer with identifier %u)",
                         (unsigned)h);
        /* A TRUNCATED OPERATION STRING MERGES TWO PREDICATES, which is the defect the whole naming scheme above
           exists to avoid arriving through the back door: the identifier is the LAST thing in this string, so a
           buffer one byte short would file two identifiers under one key and let one link's record decide
           another's. snprintf reports what it WOULD have written, which is the only way to see it. */
        DCHECK(wrote > 0 && (size_t)wrote < sizeof s->op,
               "§8.7's elimination chain could not spell the identifier its question is about — the operation "
               "string is half the constraint key and the identifier is its tail, so a truncated one names a "
               "DIFFERENT timer's question and the flow answers it with this one's record");
        rc = step_fork_run(ctx, hdr, argv[0], s->op, 2, real, &arm);
        if (rc)
            return rc;
        DCHECK(arm == 0 || arm == 1,
               "a two-armed outcome fork answered with an arm that is neither of them");
        /* NAMED RESIDUAL — the DECISION is recorded and the value's DOMAIN is not, which is narrower than
           §Solver's concretize-on-pin and is not wrong: an unnarrowed value keeps arms, and keeping an arm is
           the sound direction. WHAT IS NOT COVERED: each link is an equality over `id` — the YES arm proves it
           IS this identifier and the NO arm proves it is NOT — and neither fact reaches the value. Only the
           decision vector holds them, and a vector answers the question it recorded and no other.
           WHAT THE NEXT DIFF BUILDS: the two halves decide.c already takes at a bytecode equality — the pin on
           the arm that determines the value and the exclusion on the arm that eliminates a token. It cannot be
           the same call: decide.c reads its subject off a comparison RESULT (concolic_cmp / concolic_cmp_subject),
           and this seam has no result to read one off, so the pin has to be taken over the OPERAND's own
           identity and that is the piece to write.
           HOW ITS ABSENCE SHOWS: a flow that has answered YES at some identifier and then reaches a second
           `clearTimeout(id)` mints a sibling for every OTHER identifier this global still holds — worlds its own
           path has already contradicted — and an @H parameter carrying this value renders with no domain beside
           it where the run observed one. */
        if (arm == 1) {
            /* THIS WORLD'S ANSWER: `id` IS this identifier, so §8.7's removal names this entry. The entry was
               found in the map by the walk at the top of this same iteration and nothing has run in between —
               step_fork_run runs none of the page's code and the driver only clones and re-enters — so a miss
               here is the map answering differently to two reads inside one link. */
            DCHECK(timer_entry_index(ctx, h) >= 0,
                   "§8.7's clearTimeout resolved `id` to an identifier this global's map of setTimeout and "
                   "setInterval IDs no longer holds — the identifier was read out of that map one line above "
                   "the fork, so the map has been mutated by something that ran while this flow was inside a "
                   "single link of its own elimination chain");
            timer_clear(ctx, h);
            return JS_STEP_DONE;
        }
        /* ELIMINATED: `id` is not this identifier, so the next question is about the next one this global
           holds. The cursor is advanced AFTER the answer and never before the ask, which is what makes the two
           arms agree: the sibling's snapshot was taken with the cursor still at or below `h`, so it recomputes
           the same `h` and answers the other way about the same timer. */
        DCHECK(h < UINT32_MAX,
               "§8.7's elimination chain answered NO at the largest identifier a uint32 can hold — advancing "
               "the cursor past it would wrap it to 0 and re-ask the whole chain for ever, and this global has "
               "handed out every identifier there is");
        s->next = h + 1;
    }
}

static const IdlStepDecl CT_DECL = {
    js_clear_timer, sizeof(ClearTimerState), ct_visit, NULL,
    "HTML §8.7 Timers the clearTimeout / clearInterval method steps", CT_STEPS
};

/* HTML §8.8 "Microtask queuing"'s queueMicrotask: a MICROTASK, which is the whole of what it is for — it runs inside the current
   checkpoint, ahead of every task, including a timer set for zero. */
static JSValue js_queue_microtask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "queueMicrotask requires a callback");
    JS_EnqueueCallJob(ctx, argv[0], 0, NULL);
    return JS_UNDEFINED;
}

/* THE MEMBERS' OWN IDL, declared rather than approximated. It is HTML §8.2 "The WindowOrWorkerGlobalScope
   mixin" that writes it — §8.7 Timers states the ALGORITHMS and the mixin states the signatures — and this
   is that block verbatim:
     typedef (DOMString or Function or TrustedScript) TimerHandler;
     long setTimeout(TimerHandler handler, optional long timeout = 0, any... arguments);
     undefined clearTimeout(optional long id = 0);
     long setInterval(TimerHandler handler, optional long timeout = 0, any... arguments);
     undefined clearInterval(optional long id = 0);
   `handler` is that three-armed union and `timeout` is a long, so BOTH can run the page's code — a toString on
   the non-callable arm, a valueOf on the delay — and neither is a string.
   THE `any...` TAIL IS NOT DECLARED, AND IT IS THEREFORE DROPPED — this said the opposite ("a position the
   IDL does not name is passed through as it is") and that is measurably false: a NON-variadic member's
   argument count is min(passed, declared) (idl_args.c), so this body can never see a third position and
   `setTimeout(f, 0, x)` invokes f with no arguments where §8.7 step 9's task invokes it "given arguments".
   Declaring it is not one call to idl_variadic: a variadic member repeats its LAST declared type for the
   tail, which here is IDL_LONG and would coerce every extra argument to a number, so the type list grows a
   third IDL_ANY entry and `idl_variadic()` is set beside `idl_optional_from` — the flag composes with the STEP
   declaration below, which the `idl_method_id_ext` this paragraph used to name does not (that entry builds a
   PLAIN-BODY member, and these two are machines). The body's `argc == 2` assert is what fires the day it lands.
   DECLARED ONCE PER AGENT, installed per realm: a declaration builds a pool entry and a member has ONE, and
   `idl_optional_from` names the member the LAST declaration made — so it belongs beside it, here.
   ALL FOUR PAGE-FACING MEMBERS ARE STEP MACHINES, and that is what each ALGORITHM asks rather than a policy:
   §8.7's step 4 compares `timeout`, and the clearers' one step indexes the map of setTimeout and setInterval
   IDs by `id`. Both operands can be unknown external input, and a fork asked over one needs the driver's
   snapshot to resume its sibling from — which is the one thing a plain JSCFunction, already inside its own C
   activation, has nowhere to put. The two SETTERS share one declaration under three magics because the timer
   initialization steps are one algorithm reached three ways; the two CLEARERS share one declaration under no
   magic at all, because §8.7 gives them literally one set of steps and there is nothing for a magic to tell
   apart. */
void timer_init(JSContext *ctx)
{
    static const IdlArgType SET_TIMER[2] = { IDL_STRING_UNLESS_CALLABLE, IDL_LONG };
    /* SUBSTEP 9.11's THREE ARGUMENTS. The first two are the members' own; the third is step 2's `previousId`,
       a `long` because §8.7's identifiers are the `long` the two setters return. All three are REQUIRED —
       no idl_optional_from below — because this door is never reached from a page and every caller of it is
       the task machine, which has all three or is not a repeat at all. */
    static const IdlArgType REARM_TIMER[3] = { IDL_STRING_UNLESS_CALLABLE, IDL_LONG, IDL_LONG };
    static const IdlArgType CLEAR_TIMER[1] = { IDL_LONG };

    DCHECK(!g_ready, "timer_init ran twice — §8.7 Timers's members are declared once per agent");
    /* THE `= 0` ABOVE IS PART OF THE DECLARATION, and it was written in the comment and not in the code.
       §3.6 step 14.2 gives an optional argument whose IDL writes `= …` THAT value, while a position with no
       declared default is ABSENT — so all four members reached their bodies with an undefined where the IDL
       guarantees a number, and each body would have had to invent the 0 its declaration owes it. */
    g_id_set_timeout = idl_method_id_step(ctx, SET_TIMER, 2, NULL, 0, &TI_DECL, TI_MAGIC_TIMEOUT);
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_ZERO, NULL);
    g_id_set_interval = idl_method_id_step(ctx, SET_TIMER, 2, NULL, 0, &TI_DECL, TI_MAGIC_INTERVAL);
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_ZERO, NULL);
    /* §8.7's SUBSTEP 9.11 DOOR — the same TI_DECL, and NOT installed on any global: it is reached only by
       js_timer_task_step, through idl_step_function, which is how autofocus.c reaches §6.6.7's flush and
       focus.c its own machines. Minting it that way rather than with a hand-written JS_NewCFunction2 is what
       keeps the pool's name on it, so a parked re-arm says which algorithm it is parked in. */
    g_id_rearm = idl_method_id_step(ctx, REARM_TIMER, 3, NULL, 0, &TI_DECL, TI_MAGIC_REARM);
    /* §8.7's TWO CLEARERS, ONE DECLARATION, NO MAGIC. They are two MEMBERS, so each takes its own pool entry
       and its own `= 0` default; they are ONE ALGORITHM, so both entries carry the same body and the same
       stage list, and neither passes a magic because the body has nothing to ask about which member it is. */
    g_id_clear_timeout = idl_method_id_step(ctx, CLEAR_TIMER, 1, NULL, 0, &CT_DECL, 0);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_ZERO, NULL);
    g_id_clear_interval = idl_method_id_step(ctx, CLEAR_TIMER, 1, NULL, 0, &CT_DECL, 0);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_ZERO, NULL);
    /* §8.7 Timers's STEP 9 TASK, declared once per agent like the four members above — its definition is
       static and outlives the runtime, which is what JS_RegisterStepDef borrows. */
    g_task_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &TT_DEF);
    CHECK(g_task_stepid >= 0,
          "timer: §8.7 Timers's step 9 task machine could not be registered — every expiry runs through it, so "
          "an agent without it has no way to fire a timer at all");
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
    agent_state_id("timer", &g_id_set_timeout, "§8.7 Timers's setTimeout declaration");
    agent_state_id("timer", &g_id_set_interval, "§8.7 Timers's setInterval declaration");
    agent_state_id("timer", &g_id_clear_timeout, "§8.7 Timers's clearTimeout declaration");
    agent_state_id("timer", &g_id_clear_interval, "§8.7 Timers's clearInterval declaration");
    agent_state_id("timer", &g_id_rearm, "§8.7 Timers's substep 9.11 re-performance declaration");
    agent_state_id("timer", &g_task_stepid, "§8.7 Timers's step 9 task machine");
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
    /* Web IDL §3.7.7 Operations' `length` — "the length of the shortest argument list in the entries in S",
       over the effective overload set computed "with argument count 0", and NOT the declared arity, which is
       what all four of these were.
       §8.2's `long setTimeout(TimerHandler handler, optional long timeout = 0, any... arguments)`: §2.5.8
       Overloading's step 5.9 loop walks back over the variadic tail (its step 5.9.1 breaks only for an
       argument that "is not marked as optional and is not a final, variadic argument") and then over
       `timeout`, and stops at the required `handler` — so the shortest entry is 1. `clearTimeout(optional long
       id = 0)` has nothing required at all, so the loop reaches i = 0 and step 5.9.5's note applies: S holds
       (X, « », « ») and the length is 0.
       Both numbers are already stated by the declarations in timer_init — `idl_optional_from(1)` for the two
       setters and `idl_optional_from(0)` for the two clearers — which is the whole of the defect: the fact was
       written twice and only one copy was §3.7.7's. The `any...` tail is not yet declared (see timer_init),
       and that does not move either number: the tail is optional under step 5.9.1 either way. */
    idl_install_method(ctx, g, "setTimeout", g_id_set_timeout);
    idl_install_method(ctx, g, "setInterval", g_id_set_interval);
    idl_install_method(ctx, g, "clearTimeout", g_id_clear_timeout);
    idl_install_method(ctx, g, "clearInterval", g_id_clear_interval);
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
    /* THE FOUR MEMBER DECLARATIONS, GIVEN BACK. They name entries in an id pool that goes with the agent, and
       `idl_install_method` reads them at every realm — so a carried one would install this agent's setTimeout
       from the last agent's pool entry. Nothing frees a pool id, which is exactly why nothing but this line
       and agent_state_check_released can notice one that stayed. */
    g_id_set_timeout = g_id_set_interval = g_id_clear_timeout = g_id_clear_interval = -1;
    /* §8.7's substep 9.11 door goes back with them — it is a FIFTH entry in the same pool, reached at every
       interval fire, so a carried one would re-perform this agent's timer initialization steps out of the
       last agent's declaration. */
    g_id_rearm = -1;
    /* §8.7 Timers's step 9 task machine goes back with them, and for the same reason: the id indexes a table
       that belongs to the RUNTIME, so a carried one would mint the next agent's tasks out of the last
       agent's entry. */
    g_task_stepid = -1;
    g_script_sink = NULL;
}
