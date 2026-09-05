/* IdleDeadline — Cooperative Scheduling of Background Tasks §4.3 The IdleDeadline interface.
   See idle_deadline.h for why this is its own component and what §4.3's two associated concepts are. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/scheduling/idle_deadline.h"
#include "core/timing/hr_time.h"
#include "solver/concolic.h"

/* §4.3's TWO ASSOCIATED CONCEPTS, as the two fields of one internal-slot record. Named constants rather than
   array indices for core/idl_slots.h's reason: the record has a null prototype, so these are own properties
   nothing on Object.prototype can intercept, and the names are what a dump reads back. */
static JSClassID g_class;
static JSValue   g_state_key = JS_UNDEFINED;
static JSAtom    g_atom_state = JS_ATOM_NULL;
static JSAtom    g_atom_deadline = JS_ATOM_NULL;   /* what `get deadline time` answers */
static JSAtom    g_atom_timeout  = JS_ATOM_NULL;   /* §4.3's `timeout`, initially false */
static int       g_id_remaining = -1;
static int       g_ready;

/* THE RECEIVER'S RECORD, or a TypeError. Web IDL §3.7.6's brand test as a REAL throw and never a DCHECK: the
   receiver is PAGE-SUPPLIED INPUT — `IdleDeadline.prototype.timeRemaining.call(null)` is one line, and a
   FORCING solver writes unusual receivers constantly — so asserting here would hand any page an abort switch
   for the engine. OWNED on success. */
static JSValue idle_deadline_state(JSContext *ctx, JSValueConst v)
{
    JSValue s;

    if (JS_GetClassID(v) != g_class)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (JS_GetOwnSlot(ctx, &s, v, g_atom_state) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s),
           "an IdleDeadline carries no record — §4.3's two associated concepts are written before the object "
           "exists (idle_deadline_new_for_idle_period), so one without them was built somewhere that is not "
           "this file");
    return s;
}

/* THE EXAMPLE §4.3 STEPS 3 AND 4 COMPUTE, run for real on the operands' own examples — the contract
   concolic_new_derived states for its `example` argument, which is that the CALLER performs the operation and
   never predicts it.
   IT ANSWERS JS_UNDEFINED WHERE EITHER OPERAND HAS NO NUMBER, which is a positive statement and not a hole to
   default: @H never invents, so a deadline or a clock reading that carries no concrete leaves the derived
   value with none either. `deadline` and `now` are BORROWED. */
static JSValue idle_deadline_remaining_example(JSContext *ctx, JSValueConst deadline, JSValueConst now)
{
    JSValue de = concolic_is(deadline) ? concolic_example(ctx, deadline) : JS_DupValue(ctx, deadline);
    JSValue ne = concolic_is(now)      ? concolic_example(ctx, now)      : JS_DupValue(ctx, now);
    JSValue out = JS_UNDEFINED;
    double d = 0, n = 0;

    /* Numbers ONLY, and asked of the EXAMPLE rather than of the value: an example that is itself unknown is
       not a concrete this could subtract, and JS_ToFloat64 over one owes C a number it cannot have. */
    if (!concolic_is(de) && !concolic_is(ne) && JS_IsNumber(de) && JS_IsNumber(ne)
        && JS_ToFloat64(ctx, &d, de) == 0 && JS_ToFloat64(ctx, &n, ne) == 0) {
        double r = d - n;                          /* §4.3 step 3 */

        if (r < 0) r = 0;                          /* §4.3 step 4 */
        out = JS_NewFloat64(ctx, r);
    }
    JS_FreeValue(ctx, de);
    JS_FreeValue(ctx, ne);
    return out;
}

/* §4.3: "When the timeRemaining() method is invoked on an IdleDeadline object it MUST return the remaining
 * duration before the deadline expires as a DOMHighResTimeStamp". FIVE STEPS, no nesting:
 *   1. Let now be a DOMHighResTimeStamp representing current high resolution time in milliseconds.
 *   2. Let deadline be the result of calling IdleDeadline's get deadline time algorithm.
 *   3. Let timeRemaining be deadline - now.
 *   4. If timeRemaining is negative, set it to 0.
 *   5. Return timeRemaining.
 *
 * WHAT THIS ANSWERS, AND THE ARGUMENT FOR IT. Step 2's `deadline` is not fixed by the standard: §5.1 takes
 * `getDeadline` as a PARAMETER — "given Window window and getDeadline, an algorithm that returns a
 * DOMHighResTimeStamp" — so the number is the user agent's to choose, and THIS user agent cannot choose one
 * honestly. Its flows are preempted by RANK and resumed byte-identically, never against a time budget, so
 * there is no interval a callback is being given; and on the shipped host there is no CPU clock at all, so a
 * figure derived from wall time is an artifact of machine load rather than a fact about the run. Handing the
 * page such a number would put a load-dependent value into the page's OWN control flow — `while
 * (deadline.timeRemaining() > 0)` would spin a number of times decided by whatever else the machine was doing.
 *
 * SO THE DEADLINE IS UNKNOWN AND THE UNKNOWN IS MINTED AT THE DEADLINE — see
 * idle_deadline_new_for_idle_period. This member is then §4.3's arithmetic over it and nothing else: the
 * subtraction is DERIVED (concolic_new_derived), so the result is opaque for control flow and carries the
 * concrete the steps actually computed. `timeRemaining() > 0` therefore FORKS rather than deciding, which is
 * what makes the loop above explore its body instead of spinning: the example marks the exit arm primary and
 * the forced sibling runs the body, each iteration its own parkable flow, ordered by the one WFQ and bounded
 * by nothing.
 *
 * AND WHERE NOTHING IS UNKNOWN IT IS THE PLAIN NUMBER. concolic_new_derived answers JS_UNINITIALIZED when no
 * operand is unknown — the conformance host installs no source overlay, so this is the ordinary case there —
 * and the value the steps computed is then returned as itself. */
static JSValue js_idle_deadline_time_remaining(JSContext *ctx, JSValueConst this_val, int argc,
                                               JSValueConst *argv, int magic)
{
    JSValue s = idle_deadline_state(ctx, this_val), now, deadline, out, ex;
    JSValueConst ops[2];

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(s)) return s;

    now = hr_time_current(ctx);                                    /* step 1 */
    deadline = JS_GetProperty(ctx, s, g_atom_deadline);            /* step 2 */
    JS_FreeValue(ctx, s);
    ops[0] = deadline;
    ops[1] = now;
    ex = idle_deadline_remaining_example(ctx, deadline, now);      /* steps 3 and 4, on the examples */
    out = concolic_new_derived(ctx, "IdleDeadline.timeRemaining", ops, 2, ex);
    if (JS_VALUE_GET_TAG(out) == JS_TAG_UNINITIALIZED) {
        /* Neither operand is unknown, so the steps above already produced the answer and there is nothing to
           derive. It is a real DOMHighResTimeStamp either way; the example IS the value here. */
        out = idle_deadline_remaining_example(ctx, deadline, now);
        DCHECK(JS_IsNumber(out),
               "§4.3's timeRemaining had two known operands and still computed no number — steps 1 and 2 answer "
               "a DOMHighResTimeStamp apiece, so a non-number here is one of the two answering something else");
    }
    JS_FreeValue(ctx, deadline);
    JS_FreeValue(ctx, now);
    return out;
}

/* §4.3: "Each IdleDeadline has an associated timeout, which is initially false. The didTimeout getter MUST
   return timeout." Nothing but §5.3's invoke idle callback timeout algorithm ever sets it — see
   idle_callback.c's residual for why this build has no writer for it yet. */
static JSValue js_idle_deadline_did_timeout(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue s = idle_deadline_state(ctx, this_val), t;

    (void)magic;
    if (JS_IsException(s)) return s;
    t = JS_GetProperty(ctx, s, g_atom_timeout);
    JS_FreeValue(ctx, s);
    DCHECK(JS_IsBool(t), "§4.3's timeout is not a boolean — it is written once at the object's creation and "
                         "by §5.3, and neither writes anything else");
    return t;
}

JSValue idle_deadline_new_for_idle_period(JSContext *ctx)
{
    JSValue obj, rec, deadline, ex;

    DCHECK(g_ready, "an IdleDeadline was built before §4.3's interface was declared");
    obj = JS_NewObjectClass(ctx, g_class);
    if (JS_IsException(obj)) return obj;
    rec = idl_slots_new(ctx);
    CHECK(!JS_IsException(rec), "§4.3's associated-concept record could not be allocated");

    /* §4.3's `get deadline time`, FOR AN IDLE PERIOD, AS THE ONE UNKNOWN IN THIS FILE. The standard leaves the
       number to the user agent (§5.1 takes it as `getDeadline`) and this one has no honest answer — see
       js_idle_deadline_time_remaining for the whole argument. What it CAN state is the moment the period
       began, which is a real reading of its own clock, so that is the EXAMPLE: §4.3 steps 3 and 4 over it give
       a remaining time of zero at the instant the callback starts, which is the standard's own initial state
       ("The deadline is initially set to return zero") reached by computing rather than by choosing.
       ZERO IS THE RIGHT EXAMPLE AND NOT A PESSIMISM. It marks the arm a page takes when its budget is spent —
       reschedule, or fall through — as PRIMARY, so the flow the example follows is the one that terminates,
       while the forced sibling explores the work body. An example naming a large budget would put the
       SPINNING arm on the primary path, which is the one shape of this member that cannot make progress. */
    ex = hr_time_current(ctx);
    CHECK(!JS_IsException(ex), "§4.3's deadline example could not be read from the clock");
    deadline = concolic_new(ctx, "{IdleDeadline.deadline}", "IdleDeadline.deadline", ex);
    CHECK(!JS_IsException(deadline), "minting §4.3's deadline failed");
    JS_SetProperty(ctx, rec, g_atom_deadline, deadline);
    JS_SetProperty(ctx, rec, g_atom_timeout, JS_FALSE);   /* §4.3: "initially false" */
    JS_DefinePropertyValue(ctx, obj, g_atom_state, rec, 0);
    return obj;
}

void idle_deadline_init(JSContext *ctx)
{
    JSClassDef d = { "IdleDeadline", NULL, NULL, NULL, NULL };

    DCHECK(!g_ready, "idle_deadline_init ran twice — §4.3's class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);

    g_state_key = JS_NewSymbol(ctx, "idleDeadlineState", false);
    CHECK(!JS_IsException(g_state_key), "§4.3's record slot key could not be allocated");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    g_atom_deadline = JS_NewAtom(ctx, "deadline");
    g_atom_timeout = JS_NewAtom(ctx, "timeout");
    CHECK(g_atom_state != JS_ATOM_NULL && g_atom_deadline != JS_ATOM_NULL && g_atom_timeout != JS_ATOM_NULL,
          "§4.3's record keys could not be interned");

    g_id_remaining = idl_method_id(ctx, NULL, 0, js_idle_deadline_time_remaining, 0);
    g_ready = 1;
    realm_declare_intrinsic(idle_deadline_install_proto);

    agent_state_flag("idle_deadline", &g_ready, "the declaration latch");
    agent_state_class("idle_deadline", &g_class, "§4.3's IdleDeadline class");
    agent_state_value("idle_deadline", &g_state_key, "§4.3's associated-concept record key");
    agent_state_atom("idle_deadline", &g_atom_state, "§4.3's record key, interned");
    agent_state_atom("idle_deadline", &g_atom_deadline, "the record's `get deadline time` field name");
    agent_state_atom("idle_deadline", &g_atom_timeout, "the record's §4.3 `timeout` field name");
    agent_state_id("idle_deadline", &g_id_remaining, "§4.3's timeRemaining declaration");
}

void idle_deadline_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_class != 0, "a realm asked for IdleDeadline.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "idle_deadline_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IdleDeadline.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IdleDeadline");
    idl_install_method(ctx, proto, "timeRemaining", g_id_remaining);
    /* `readonly attribute boolean didTimeout` — an ATTRIBUTE, so Web IDL §3.7.6 puts an accessor on the
       prototype; -1 is "no setter", which is what `readonly` means. */
    idl_install_accessor(ctx, proto, "didTimeout", js_idle_deadline_did_timeout, 0, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

void idle_deadline_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto;

    DCHECK(g_ready, "IdleDeadline's Web IDL §3.7.1 interface object was installed before the interface was "
                    "declared");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "IdleDeadline was installed in a realm that never ran its proto build");
    /* §4.3 declares `[Exposed=Window] interface IdleDeadline` — no constructor and no exposure condition, so
       this is Web IDL §3.7.1's interface object for an interface that declares none (the installer mints over
       idl_illegal_ctor) and IDL_EXPOSED is the IDL carrying no condition rather than a default chosen here.
       [Exposed=Window] itself is not this argument: the member is installed from the WINDOW's install and a
       host with no Window never reaches it. */
    idl_install_interface_object_exposed(ctx, global, "IdleDeadline", proto, IDL_EXPOSED);
    JS_FreeValue(ctx, proto);
}

void idle_deadline_free(JSRuntime *rt)
{
    DCHECK(g_ready, "§4.3 was released in an agent that never declared it");
    g_ready = 0;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_state);
    JS_FreeAtomRT(rt, g_atom_deadline);
    JS_FreeAtomRT(rt, g_atom_timeout);
    g_atom_state = g_atom_deadline = g_atom_timeout = JS_ATOM_NULL;
    /* The member declaration names an entry in a pool idl_args_pool_free restarts at 0 (core/agent_state.h). */
    g_id_remaining = -1;
}
