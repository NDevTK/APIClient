/* HIGH RESOLUTION TIME Level 3 §4. See hr_time.h for what this is and why the time origin is an environment
   field rather than a static, a document field or a concolic. */
#include <math.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/frame/agent_cluster.h"
#include "core/realm.h"
#include "core/timing/event_loop.h"
#include "core/timing/hr_time.h"
#include "solver/concolic.h"   /* the clock is a MOMENT, so §4's operations run over unknown external input */

/* §4's TIME RESOLUTION, in the millisecond unit DOMHighResTimeStamp is measured in. Step 1: "let time
   resolution be 100 microseconds, or a higher implementation-defined value". Step 2: "if
   crossOriginIsolatedCapability is true, set time resolution to be 5 microseconds, or a higher
   implementation-defined value". Both steps permit a COARSER value and this engine takes neither offer: a
   higher value is a worse answer to every question a page asks about its own timing, and the page can see the
   difference. Which of the two applies is the ENVIRONMENT'S question and is asked below, not chosen here. */
#define HR_TIME_RESOLUTION_MS          0.1
#define HR_TIME_RESOLUTION_ISOLATED_MS 0.005

static int g_origin_slot = -1;

/* §4: "Performance measurements report a duration from a moment early in the initialization of a relevant
 * environment settings object. That moment is stored in that settings object's time origin." This runs from
 * the one call every realm goes through (core/realm.h), which is where this engine creates that environment —
 * so the moment the standard describes and the moment this reads are the same one.
 *
 * WHAT IS STORED IS THE RAW MOMENT, AND THE COARSENING HAPPENS AT THE READ. §4 stores a MOMENT here and names
 * `coarsen time` nowhere in this sentence; the algorithm that coarsens is `relative high resolution time`,
 * which runs when a timestamp is asked for. That is also the only ORDER in which the coarsening can be
 * correct, and the reason this is not a stylistic move: `coarsen time`'s resolution is decided by THE
 * ENVIRONMENT'S CROSS-ORIGIN ISOLATED CAPABILITY, and HTML §7.2.2.6 "Script settings for Window objects"
 * defines that capability over "window's associated DOCUMENT" — which does not exist yet at this point in the
 * realm's construction (core/platform.c installs `document` last, because §4.8.5's insertion steps create
 * child navigables and need every other component already standing). Coarsening here therefore asked an
 * environment field before the environment was set up, and it went unnoticed only for as long as the second
 * conjunct of that capability was a crash nobody reached: the first cross-origin-isolated page to load —
 * `Cross-Origin-Opener-Policy: same-origin` beside `Cross-Origin-Embedder-Policy: require-corp`, which is
 * every wasm application on the web — takes the `concrete` arm and asks the Document.
 *
 * THE ORIGIN IS STILL COARSENED BEFORE ANYTHING COMPARES IT, which is what hr_time_relative needs and why
 * hr_time_origin below does it rather than this. `relative high resolution coarse time` is "the duration from
 * the time origin to coarseTime", so an origin off the resolution grid would make that duration carry a
 * sub-resolution remainder — and would make it NEGATIVE for a moment equal to the origin, which is the one
 * comparison hr_time_relative asserts. Coarsening both ends at the read puts every page-visible timestamp on
 * the grid and keeps the duration a multiple of the resolution, which is what a browser reports, and the floor
 * is a pure function of the stored moment so no read can disagree with another. */
static void hr_time_install(JSContext *ctx)
{
    /* Running twice in one realm is asserted by realm_value_set, which is where the first moment is standing. */
    realm_value_set(ctx, g_origin_slot, event_loop_now(ctx));
}

JSValue hr_time_origin(JSContext *ctx)
{
    JSValue v = realm_value_get(ctx, g_origin_slot), coarse;

    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "an environment's TIME ORIGIN is neither a moment on the monotonic clock nor a derivation of one — "
           "HR-TIME §4 stores the clock's moment at this realm's creation, and the install that stamps it is "
           "the only thing that ever writes this slot");
    /* §4's coarsen time, at the read — see hr_time_install for why it cannot be at the write. The stored
       moment is BORROWED back from the slot by this call and released here; what leaves is the coarsening. */
    coarse = hr_time_coarsen(ctx, v);
    JS_FreeValue(ctx, v);
    return coarse;
}

/* §4 STEP 3's FLOOR, run on a number. The one arithmetic in this file, so that the known path and the example
   of the unknown path cannot drift apart. */
static double hr_floor(double m, double resolution)
{
    return floor(m / resolution) * resolution;
}

/* THE OPERATION'S NAME, one per resolution, because §4 takes the resolution as an argument and two
   environments coarsening the same moment on different grids produce two different values. decide.c keys a
   predicate by the identity of the value a branch tests, and a derivation's identity is composed from its
   operand and this name — so one name for both grids would make a narrowing decided in an isolated
   environment decide the same branch in one that is not. */
static const char HR_COARSEN_OP[] = "HR-TIME §4 Time Origin coarsen time (100us)";
static const char HR_COARSEN_OP_ISOLATED[] = "HR-TIME §4 Time Origin coarsen time (5us)";

JSValue hr_time_coarsen(JSContext *ctx, JSValueConst unsafe_moment)
{
    /* §4 steps 1-2, decided from THE ENVIRONMENT'S OWN cross-origin isolated capability. §4 does not name a
       constant: it takes `crossOriginIsolatedCapability` as an argument, and every caller passes "global's
       relevant settings object's cross-origin isolated capability" — HTML §7.2.2's field, computed by
       core/frame/agent_cluster.h. `ctx` IS that global, which is why this is asked per call and per realm and
       never once for the agent.
     *
     * IT IS A COMPONENT'S ANSWER AND NOT A PROBE OF THE GLOBAL. This asked `realm_awaits(ctx,
     * "crossOriginIsolated", ...)` while the capability had no C answer, and the member's arrival made the
     * probe fire on the first coarsen of every run — the stale-assertion failure, one turn after it was
     * written. Asking the component instead also removes the ordering hazard the probe carried: the answer
     * exists before any realm intrinsic is installed, so `hr_time_install` can coarsen the time origin itself. */
    int isolated = agent_cluster_cross_origin_isolated(ctx);
    double resolution = isolated ? HR_TIME_RESOLUTION_ISOLATED_MS : HR_TIME_RESOLUTION_MS;

    /* §4 step 3: "in an implementation-defined manner, coarsen AND POTENTIALLY JITTER timestamp such that its
       resolution will not exceed time resolution". This engine takes the coarsening and declines the jitter —
       see hr_time.h for why that is a design constraint and not a shortcut. Flooring onto the grid is then the
       whole of step 3, and it is MONOTONE (division, floor and multiplication all are), which is what makes
       hr_time_relative's non-negative duration an invariant rather than a hope. */
    if (!concolic_is(unsafe_moment)) {
        double m = 0;

        DCHECK(JS_IsNumber(unsafe_moment),
               "HR-TIME §4's coarsen time was given something that is neither a moment on the monotonic clock "
               "nor unknown external input — its argument is the event loop's virtual clock or a moment taken "
               "off it, and every producer of one writes one of the two");
        JS_ToFloat64(ctx, &m, unsafe_moment);
        DCHECK(m >= 0.0,
               "HR-TIME §4's coarsen time was given a moment BEFORE the agent's clock started — the unsafe "
               "shared current time in this engine is the event loop's virtual clock, which begins at zero and "
               "never runs backwards (core/timing/event_loop.h asserts the second half at every move)");
        return JS_NewFloat64(ctx, hr_floor(m, resolution));
    }
    /* §4 STEP 3 OVER AN UNKNOWN MOMENT IS THE SAME OPERATION, PERFORMED. It is a DERIVATION: the result names
       §4's algorithm as what produced it and carries the moment's own provenance, so a page branching on a
       coarsened moment branches on a value whose identity is (this operation, that moment) and not on a
       number this file chose. The EXAMPLE is the REAL floor run on the moment's own example — the engine
       running the actual operation on the concrete, which is what §Solver-half means by the example
       propagating, and not a label a hook derived. Where the moment has no example the derivation has none
       either: @H never invents. Nothing here is clamped and nothing reads the example to DECIDE anything. */
    {
        JSValue ex = concolic_example(ctx, unsafe_moment), out = JS_UNDEFINED;

        if (JS_IsNumber(ex)) {
            double m = 0;

            JS_ToFloat64(ctx, &m, ex);
            out = JS_NewFloat64(ctx, hr_floor(m, resolution));
        }
        JS_FreeValue(ctx, ex);
        return concolic_builtin_hook(ctx, unsafe_moment, isolated ? HR_COARSEN_OP_ISOLATED : HR_COARSEN_OP,
                                     out);
    }
}

JSValue hr_time_relative(JSContext *ctx, JSValueConst unsafe_moment)
{
    JSValue coarse = hr_time_coarsen(ctx, unsafe_moment);
    JSValue origin = hr_time_origin(ctx);
    JSValue sp[2];

    /* §4: "the duration from global's relevant settings object's time origin to coarseTime". A NEGATIVE
       duration would be a moment this environment can observe that precedes the environment's own creation,
       and there is exactly one way for that to arise here: core/frame/window_proxy.h DEFERS building a child
       navigable's realm until something reaches it, so a Window §7.4 created at one moment gets its time
       origin at a LATER one. The day this fires, the fix is the one core/html/user_activation.c already names
       for the same deferral — carry the creation moment on the NAVIGABLE and hand it to the realm when it
       materializes — and never a clamp here, which would report a page's own first frame as moment zero.
       IT IS READ FROM THE CLOCK'S OWN ORDER AND NOT RE-COMPUTED. Over two known moments this is exactly the
       `coarse >= origin` that stood here; over an unknown one a comparison is an arm to take rather than a
       fact to test, so the invariant asks what this flow ALREADY DECIDED (event_loop_before_decided never
       forks) and fires on a decided contradiction. Re-evaluating it would mint a sibling flow whose only
       content is the violated invariant, which is the assertion manufacturing the world in which it holds. */
    DCHECK(event_loop_before_decided(ctx, coarse, origin) != 1,
           "HR-TIME §4's relative high resolution time is NEGATIVE — this environment was asked about a moment "
           "earlier than its own TIME ORIGIN, which means the origin was stamped when the realm materialized "
           "rather than when HTML §7.4 created the Window");
    if (!concolic_is(coarse) && !concolic_is(origin)) {
        double c = 0, o = 0;

        JS_ToFloat64(ctx, &c, coarse);
        JS_ToFloat64(ctx, &o, origin);
        JS_FreeValue(ctx, coarse);
        JS_FreeValue(ctx, origin);
        return JS_NewFloat64(ctx, c - o);
    }
    /* THE SUBTRACTION IS ECMAScript §13.8.2 The Subtraction Operator ( - ), RUN — which §13.15.3
       ApplyStringOrNumericBinaryOperator performs, the same abstract operation `+` reaches. Its result's
       identity is composed from the operator
       and BOTH operands, which is why it goes through the operator rather than through a second derivation
       named for this clause: two environments subtracting two different origins from one coarsened moment
       must not compose one key. The hook's stack effect is the interpreter's — both operands freed, the
       result placed in sp[-2] — so both are handed over here. */
    sp[0] = coarse;
    sp[1] = origin;
    if (!concolic_arith_hook(ctx, sp + 2, JS_CARITH_SUB, 2))
        DFAIL("§13.8.2 The Subtraction Operator ( - ) declined an operand this component has already "
              "established is UNKNOWN — the "
              "concolic value semantics are not installed in this host, so every operator over an unknown "
              "falls through to the ordinary-object path and the next coercion throws out of an expression "
              "the page never wrote (solver/concolic.h: concolic_install_hooks)");
    return sp[0];
}

JSValue hr_time_current(JSContext *ctx)
{
    /* §4: "the result of relative high resolution time given UNSAFE SHARED CURRENT TIME and current global".
       There is no wall clock in a headless run and a second time source would order a timestamp against the
       queue that runs the listeners observing it differently from the queue itself, so the unsafe shared
       current time is the event loop's one virtual clock. */
    JSValue now = event_loop_now(ctx), r = hr_time_relative(ctx, now);

    JS_FreeValue(ctx, now);
    return r;
}

void hr_time_init(JSContext *ctx)
{
    DCHECK(g_origin_slot < 0, "hr_time_init ran twice — the TIME ORIGIN's slot is declared once per AGENT");
    g_origin_slot = realm_value_declare(ctx, "HR-TIME §4 the environment settings object's time origin");
    agent_state_id("hr_time", &g_origin_slot, "HR-TIME §4's time-origin realm slot");
    realm_declare_intrinsic(hr_time_install);
}

void hr_time_free(void)
{
    /* The moments are the REALMS' — each is released with its context. What the agent holds is the slot, and a
       slot id is a class id in a runtime that is going away with it. */
    g_origin_slot = -1;
}
