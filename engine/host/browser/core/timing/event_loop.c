/* THE EVENT LOOP'S OWN STATE — HTML §8.1.7. See event_loop.h for why it is a heap record and not three
   statics, and why it is per-agent while §8.7 Timers's map of active timers is per-global. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/timing/event_loop.h"
#include "solver/concolic.h"   /* a moment can be unknown external input, and its ordering is a predicate */
#include "solver/decide.h"     /* …asked as a restartable branch, and read back by this file's invariants */

/* THE RECORD, held for the AGENT. A module static is the CORRECT scope here and the wrong one for a prototype:
   §8.1.7 gives one event loop to a similar-origin window agent, so one record answering every realm IS the
   fact, where one prototype answering every realm is a defect. What must not be shared is the TIMELINE, and
   that is answered by the object's properties riding the per-flow COW delta rather than by a second object. */
static JSValue g_rec = JS_UNDEFINED;
static JSAtom  g_atom_now = JS_ATOM_NULL, g_atom_render = JS_ATOM_NULL, g_atom_seq = JS_ATOM_NULL;

/* A FIELD OF THE RECORD, as the VALUE it is. OWNED. The two moments may be unknown external input; the
   insertion counter may not, and el_num_get below is where that half is asserted. */
static JSValue el_get(JSContext *ctx, JSAtom key)
{
    JSValue v;

    DCHECK(JS_IsObject(g_rec),
           "the event loop's state was read before event_loop_init built it — the record has to be in the "
           "PRE-BOOT baseline, or the first flow to touch it creates it inside its own delta and every "
           "sibling then reads a clock that is not there");
    v = JS_GetProperty(ctx, g_rec, key);
    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "the event loop's state holds a value that is neither a moment on its clock nor unknown external "
           "input — every field of it is one of those or a counter, and every writer here writes one");
    return v;
}

/* BORROWS `v`. */
static void el_set(JSContext *ctx, JSAtom key, JSValueConst v)
{
    DCHECK(JS_IsObject(g_rec), "the event loop's state was written before event_loop_init built it");
    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "the event loop was asked to store something that is not a moment — a moment is a number or the "
           "unknown external input §8.7's `timeout` can carry into an expiry, and nothing else");
    JS_SetProperty(ctx, g_rec, key, JS_DupValue(ctx, v));
}

/* THE INSERTION COUNTER, which is the one field that can never be unknown: §8.1.7 hands out a real number at
   every set, and a tie broken by an unknown would be a tie broken by nothing. */
static double el_num_get(JSContext *ctx, JSAtom key)
{
    JSValue v = el_get(ctx, key);
    double d = 0;

    DCHECK(JS_IsNumber(v),
           "the event loop's task INSERTION ORDER is not a number — it is the counter that breaks a tie "
           "between two sources' tasks due at the same moment, so an unknown here is a tie decided by nothing");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static void el_num_set(JSContext *ctx, JSAtom key, double d)
{
    JSValue v = JS_NewFloat64(ctx, d);

    el_set(ctx, key, v);
    JS_FreeValue(ctx, v);
}

/* THE RELATION, NAMED BY THE CLAUSE IT COMES FROM. decide.c keys a predicate by the IDENTITY of the value a
   branch tests, and a relation's identity is composed from this name and BOTH operands — so this string is
   what makes two spellings of one question one key, and what would make one question two keys if a second
   component invented a name of its own. See event_loop.h's note on why there is exactly one of it. */
static const char EL_BEFORE[] = "HTML §8.1.7 Event loops: the event loop's clock, moment <";
static const char EL_SAME[]   = "HTML §8.1.7 Event loops: the event loop's clock, moment ==";

/* ARE THESE THE SAME MOMENT — by VALUE IDENTITY, which is decidable whether or not the moment is known and is
   therefore the only order question an invariant may ask. Two unknowns are the same moment when they are the
   same derivation; a value this engine cannot spell (concolic_ident_c NULL) is not the same as anything,
   which is the sound direction. */
static int el_identical(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    /* THE SAME VALUE, which is the one answer that holds even for a moment this engine cannot spell — and
       that case is the reason this test is here rather than folded into the identity compare below: a
       derivation with no spellable identity would otherwise fork `a < a`, an arm with no world in it. */
    if (JS_VALUE_GET_TAG(a) == JS_VALUE_GET_TAG(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b))
        return 1;
    if (concolic_is(a) != concolic_is(b))
        return 0;
    if (concolic_is(a)) {
        const char *ia = concolic_ident_c(a), *ib = concolic_ident_c(b);
        return ia != NULL && ib != NULL && strcmp(ia, ib) == 0;
    }
    {
        double x = 0, y = 0;
        JS_ToFloat64(ctx, &x, a);
        JS_ToFloat64(ctx, &y, b);
        return x == y;
    }
}

/* Both operands known: the comparison IS the answer, and no predicate is minted. */
static int el_num_before(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    double x = 0, y = 0;

    JS_ToFloat64(ctx, &x, a);
    JS_ToFloat64(ctx, &y, b);
    return x < y;
}

void event_loop_init(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_rec), "event_loop_init ran twice — §8.1.7 gives one event loop to an agent");
    g_atom_now = JS_NewAtom(ctx, "eventLoopNow");
    g_atom_render = JS_NewAtom(ctx, "lastRenderOpportunityTime");
    g_atom_seq = JS_NewAtom(ctx, "taskInsertionOrder");
    CHECK(g_atom_now != JS_ATOM_NULL && g_atom_render != JS_ATOM_NULL && g_atom_seq != JS_ATOM_NULL,
          "event loop: the record's own keys could not be interned");
    /* NULL-PROTOTYPED, like §8.12 Animation frames's map: this is the loop's own storage and never the page's object, so it
       carries no members a page could have replaced. */
    g_rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(g_rec), "event loop: the loop's own state could not be allocated");
    JS_SetProperty(ctx, g_rec, g_atom_now, JS_NewFloat64(ctx, 0));
    JS_SetProperty(ctx, g_rec, g_atom_render, JS_NewFloat64(ctx, 0));
    JS_SetProperty(ctx, g_rec, g_atom_seq, JS_NewFloat64(ctx, 0));
    /* THE CLOCK STARTS AT A KNOWN MOMENT and becomes unknown only where a task source's own due moment is —
       which is what makes HR-TIME §4's time origin, stamped from this clock at every realm's creation, a real
       number for every document created before the first such task fires. */
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The RECORD is also this init's
       own latch (`JS_IsUndefined(g_rec)` above), which is exactly the slot the header says must be given back:
       a second agent whose event_loop_init returned early would hold a clock object minted in a runtime that
       is gone, and every el_get on it reads that heap. */
    agent_state_value("event_loop", &g_rec,
                      "HTML §8.1.7 Event loops's own record — the virtual clock, §8.1.7.1's last render "
                      "opportunity time and the task insertion order — and this component's declaration latch");
    agent_state_atom("event_loop", &g_atom_now,
                     "HTML §8.1.7 Event loops's virtual-clock key on that record");
    agent_state_atom("event_loop", &g_atom_render,
                     "HTML §8.1.7.1 Definitions' last-render-opportunity-time key on that record");
    agent_state_atom("event_loop", &g_atom_seq,
                     "HTML §8.1.7 Event loops's task-insertion-order key on that record");
}

/* THE RUNTIME, NOT A REALM, and it is core/platform.c's release column that calls it — see core/platform.h.
   The record is a live JS object a C static holds for the AGENT, so what it is released against is the agent;
   taking a JSContext is what made it a line each of three hosts had to remember, and a host that forgot it
   would end on JS_FreeRuntime's gc_obj_list walk with this component's null-prototyped record still in it. */
void event_loop_free(JSRuntime *rt)
{
    /* NOT `if (JS_IsUndefined(g_rec)) return;`. The release is the inverse of the DECLARATION and rides the
       same row of core/platform.c's one list, whose declare pass is unconditional and whose table asserts that
       a release row has a declare — so an early return here would silently absorb a teardown reached without
       an init instead of naming it. */
    DCHECK(JS_IsObject(g_rec),
           "HTML §8.1.7 Event loops's record was released in an agent that never declared it — §8.1.7 gives one "
           "event loop to a similar-origin window agent and event_loop_init is what builds it");
    JS_FreeValueRT(rt, g_rec);
    g_rec = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_now);
    JS_FreeAtomRT(rt, g_atom_render);
    JS_FreeAtomRT(rt, g_atom_seq);
    g_atom_now = g_atom_render = g_atom_seq = JS_ATOM_NULL;
}

JSValue event_loop_now(JSContext *ctx) { return el_get(ctx, g_atom_now); }

/* THE ORDER THIS CLOCK TAKES IN A SESSION THAT EXPLORES NOTHING — HTML §8.1.7.3 Processing model step 2.1's
 * own freedom, exercised on the only facts the moments carry.
 *
 * A session may declare that it does not fork (a conformance run measuring the browser half against a spec
 * oracle; §@S's candidate re-fire, which is ONE concrete path). The two arms of this order are then not two
 * worlds to explore but one world to be in, and the seam will not choose between them — see solver/decide.h.
 * What this file has to choose from is what a moment IS: a number, or unknown external input carrying the
 * concrete EXAMPLE the code computed for it (§8.7's `startTime plus milliseconds`, run by
 * event_loop_moment_plus as the real `+` over the operands' own examples). Where both moments have one, the
 * order is the REAL comparison on those two numbers — nothing is invented and nothing is picked, it is the
 * order the page's own values produce, which is exactly the order a real browser puts these tasks in. That is
 * the same rule as the interpreter's ordinary ToBool and a step machine's outcome 0: with one world available,
 * take the one the concrete says you are in.
 *
 * WHERE A MOMENT HAS NO EXAMPLE THERE IS NO ORDER, AND THIS SAYS SO RATHER THAN INVENTING ONE. An unknown with
 * no example is a moment NOTHING COMPUTED — its domain is the whole of §3.2.4.5's `long` and both orders are
 * feasible — so any answer here decides which of the page's two programs runs by nothing at all, and the tie
 * would not even be §8.1.7's insertion order, which breaks a tie between moments that are EQUAL rather than
 * unknown. SOLVER_NO_NONFORKING_ARM is the positive statement of that, and the seam crashes on it naming this
 * relation and both moments. The fix such a crash names is never here: it is whatever put a moment with no
 * example onto this clock in a session that cannot explore its way out of it. */
static int el_rel_nonforking(JSContext *ctx, JSValueConst a, JSValueConst b, const char *op)
{
    JSValue ea = concolic_is(a) ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue eb = concolic_is(b) ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    int r = SOLVER_NO_NONFORKING_ARM;

    if (JS_IsNumber(ea) && JS_IsNumber(eb)) {
        double x = 0, y = 0;

        JS_ToFloat64(ctx, &x, ea);
        JS_ToFloat64(ctx, &y, eb);
        /* `==` and not el_identical: two moments are being compared as NUMBERS here, which is what an example
           is, and el_identical's first test is a VALUE-identity one that answers a different question. */
        r = (op == EL_BEFORE) ? (x < y) : (x == y);
    }
    JS_FreeValue(ctx, ea);
    JS_FreeValue(ctx, eb);
    return r;
}

/* THE TWO HALVES OF THE ORDER QUESTION — the ask, which forks, and the read, which never does. They share the
   relation's NAME, which is the whole reason the read can discharge what the ask decided. */
static int el_rel(JSContext *ctx, JSValueConst a, JSValueConst b, const char *op, int ask)
{
    JSValue pred;
    int d;

    DCHECK(op == EL_BEFORE || op == EL_SAME,
           "the event loop's clock was asked a relation that is neither of the two its order is composed of — "
           "the name is what the constraint is keyed by, so a third one would be a third predicate over the "
           "same pair of moments");
    /* ASSERTED AT THE ORIGIN rather than left to a silent ToNumber. A moment is a number or unknown external
       input; anything else converts to NaN here, and a NaN comparison is FALSE for both orders at once — a
       pair of answers no arm could be standing on, and nothing downstream would ever report it. */
    DCHECK((JS_IsNumber(a) || concolic_is(a)) && (JS_IsNumber(b) || concolic_is(b)),
           "the event loop's clock was asked to order something that is not a moment — every operand of this "
           "order comes off its clock or out of §8.7's map of active timers, and both of those hold a number "
           "or unknown external input");
    /* Reflexivity first, and for both relations: a moment is not before itself and does coincide with itself,
       whatever it is. Asking either would mint a predicate with one feasible answer and — on the ask path —
       park a sibling exploring a world that cannot happen. */
    if (el_identical(ctx, a, b))
        return op == EL_SAME;
    if (!concolic_is(a) && !concolic_is(b))
        return op == EL_BEFORE ? el_num_before(ctx, a, b) : 0;   /* identical was handled above, so == is 0 */
    pred = concolic_new_rel(ctx, op, a, b);
    if (!ask) {
        d = decide_value_arm(pred);
        JS_FreeValue(ctx, pred);
        return d;                                               /* 1 / 0 / -1 not decided by this flow */
    }
    /* THE SEAM BUILDS THE SIBLING BEFORE ANSWERING, so the FORKED bit never comes back here — see decide.h,
       and see event_loop.h for why every caller of the ask is between a flow's tasks and can promise it. */
    d = solver_decide_restartable(ctx, pred, el_rel_nonforking(ctx, a, b, op));
    JS_FreeValue(ctx, pred);
    DCHECK(d >= 0,
           "the event loop's clock was asked to order two moments and the solver answered that neither is "
           "unknown — the branch seam answers -1 for an ordinary value and for a flow that is not running, and "
           "this clock is only ever ordered from inside a flow's own scheduler step");
    DCHECK(SOLVER_ARM(d) == 0 || SOLVER_ARM(d) == 1,
           "a two-armed decision answered with an arm that is neither of them");
    return SOLVER_ARM(d) == 1;
}

int event_loop_before(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return el_rel(ctx, a, b, EL_BEFORE, 1);
}

int event_loop_before_decided(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return el_rel(ctx, a, b, EL_BEFORE, 0);
}

int event_loop_coincident(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return el_rel(ctx, a, b, EL_SAME, 1);
}

JSValue event_loop_moment_plus(JSContext *ctx, JSValueConst moment, JSValueConst delta)
{
    JSValue sp[2];

    if (!concolic_is(moment) && !concolic_is(delta)) {
        double m = 0, d = 0;

        DCHECK(JS_IsNumber(moment) && JS_IsNumber(delta),
               "the event loop was asked to add something that is neither a moment nor a duration — both "
               "operands of §13.15.3's `+` here are numbers on this clock or unknown external input");
        JS_ToFloat64(ctx, &m, moment);
        JS_ToFloat64(ctx, &d, delta);
        return JS_NewFloat64(ctx, m + d);
    }
    sp[0] = JS_DupValue(ctx, moment);
    sp[1] = JS_DupValue(ctx, delta);
    /* The hook's stack effect is js_add_slow's: both operands freed, the result placed in sp[-2]. */
    if (!concolic_add_hook(ctx, sp + 2, JS_CONCOLIC_ADD_PLUS))
        DFAIL("§13.15.3's `+` declined an operand this component has already established is UNKNOWN — the "
              "concolic value semantics are not installed in this host, so every operator over an unknown "
              "falls through to the ordinary-object path and the next coercion throws out of an expression "
              "the page never wrote (solver/concolic.h: concolic_install_hooks)");
    return sp[0];
}

void event_loop_advance_to(JSContext *ctx, JSValueConst when, JSValueConst due)
{
    JSValue now = el_get(ctx, g_atom_now);

    /* READ, NEVER RE-EVALUATED. Over an unknown a comparison is an arm to take, and an assertion that forks
       would mint a sibling whose only content is the violated invariant. The caller reached this move by
       ASKING event_loop_before, so the answer is already in this flow's constraint; -1 (never asked) is
       uncertainty and keeps quiet, exactly as an undecided branch keeps both arms everywhere else. */
    DCHECK(event_loop_before_decided(ctx, when, now) != 1,
           "the event loop's clock was asked to move BACKWARDS — a moment already passed is a task that "
           "should have run before whatever is asking, and on a PER-FLOW clock it is also how a flow reading "
           "another timeline's clock announces itself");
    DCHECK(JS_IsUndefined(due) || event_loop_before_decided(ctx, due, when) != 1,
           "the event loop's clock was moved PAST a task source that is already due — a source that steps over "
           "an earlier one runs the page's work in an order the page did not write, which is exactly what "
           "virtual time makes easy to get wrong (a long timeout landing in the middle of the work it was set "
           "to outlast)");
    JS_FreeValue(ctx, now);
    el_set(ctx, g_atom_now, when);
}

JSValue event_loop_last_render(JSContext *ctx) { return el_get(ctx, g_atom_render); }

void event_loop_set_last_render(JSContext *ctx, JSValueConst when)
{
    JSValue now = el_get(ctx, g_atom_now);

    DCHECK(el_identical(ctx, when, now),
           "§8.1.7.1 Definitions' last render opportunity time was set to a moment the clock does not stand "
           "at — §8.1.7.3 Processing model's in-parallel loop says \"Set eventLoop's last render opportunity "
           "time to THE UNSAFE SHARED CURRENT TIME\", which in this engine is this clock, so the two are ONE "
           "moment and not two that happen to compare; the `when <= now` that stood here admitted a state no "
           "caller can produce and could not be evaluated at all once a moment may be unknown");
    JS_FreeValue(ctx, now);
    el_set(ctx, g_atom_render, when);
}

double event_loop_task_seq_peek(JSContext *ctx) { return el_num_get(ctx, g_atom_seq); }

double event_loop_task_seq(JSContext *ctx)
{
    double seq = el_num_get(ctx, g_atom_seq);

    el_num_set(ctx, g_atom_seq, seq + 1);
    return seq;
}
