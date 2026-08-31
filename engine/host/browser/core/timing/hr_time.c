/* HIGH RESOLUTION TIME Level 3 §4. See hr_time.h for what this is and why the time origin is an environment
   field rather than a static, a document field or a concolic. */
#include <math.h>

#include "check.h"
#include "quickjs.h"
#include "cutils.h"            /* §4's WALL CLOCK — js__gettimeofday_us, the one real clock this engine reads */
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

/* §4's ESTIMATED MONOTONIC TIME OF THE UNIX EPOCH — "Each group of environment settings objects that could
 * possibly communicate in any way has an estimated monotonic time of the Unix epoch, a moment on the monotonic
 * clock". A GROUP, so it is AGENT state and not realm state: CLAUDE.md's §Security makes an instance an
 * origin-keyed agent cluster, which is exactly the set of settings objects §4's own note gestures at ("needs to
 * be specified better ... similar to familiar with but includes Workers"), and every realm this agent builds
 * shares one estimate. Two estimates in one agent would make `performance.timeOrigin` of a document and of its
 * same-origin child disagree about when 1970 was, which is the only thing this value is FOR.
 *
 * IT IS THE ONE PLACE THE WALL CLOCK ENTERS §4, and the flag is the latch rather than a sentinel value in the
 * double, because a legitimate estimate is a large NEGATIVE number (the monotonic clock starts at zero long
 * after 1970) and so no value of the double is free to mean "unset". core/agent_state.h has a kind per
 * PRE-INIT VALUE and none for a `double`; the flag is the state that decides whether the double means anything,
 * so the flag is what the release owes and what platform_agent_free checks.
 *
 * WHY A REAL CLOCK READ IS THE HONEST ANSWER HERE AND NOT ELSEWHERE IN THIS FILE. The MONOTONIC clock is the
 * event loop's virtual clock and must stay so — hr_time_unsafe_shared_current's residual says why, and a wall
 * clock there would make every timestamp a per-run disagreement that §Testing's solver differential reports as
 * a scheduling bug. The WALL clock is a different question: §4 reads it exactly ONCE per agent, to place the
 * monotonic clock's zero relative to the Unix epoch, and the alternative is not determinism but a FABRICATION —
 * an engine that answers `performance.timeOrigin` from the monotonic origin alone reports every document as
 * having been navigated to in January 1970, and `new Date(performance.timeOrigin + performance.now())` renders
 * it. That is a plausible datum rather than an absence, which is the one thing this tree refuses. This engine
 * already hands the page the same clock through ECMAScript's `Date.now()` (quickjs's js_Date_now over
 * js__gettimeofday_us) and through File §4.1's `lastModified`, so the entropy is not new and §9.2 Clock drift's
 * own comparison — `performance.timeOrigin` against `Date.now() - performance.now()` — is the relationship a
 * page can already make either way. What WOULD be new is the two disagreeing. */
static int    g_epoch_known;         /* §4's "whose value is initialized by the following steps", once per agent */
static double g_epoch_estimate_ms;   /* defined ONLY while g_epoch_known — see above for why no value can latch */

/* §4 STEP 3's FLOOR, run on a number. The one arithmetic in this file, so that the known path and the example
   of the unknown path cannot drift apart — and so that every call of `coarsen time` this component makes rounds
   the same way, on either of the two grids §4 step 1 and step 2 name. It is ABOVE the install because the
   install is now a caller and is the EARLIEST one; it sat below while its only caller was hr_time_coarsen,
   which nothing runs before a realm has been built. */
static double hr_floor(double m, double resolution)
{
    return floor(m / resolution) * resolution;
}

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
    JSValue moment = event_loop_now(ctx);

    /* §4's FOUR STEPS THAT INITIALIZE THE GROUP'S ESTIMATED MONOTONIC TIME OF THE UNIX EPOCH, run at the FIRST
     * realm this agent builds and at no other. §4 states the estimate as a property of the group and gives no
     * moment for it; the first realm's install is the earliest moment at which both clocks are readable, and
     * doing it here rather than in hr_time_init is forced — hr_time_init runs in core/platform.c's DECLARE pass,
     * which is where the event loop's own record is BUILT, so a monotonic read there would ask a clock that
     * does not exist yet. A LATER moment would be worse than merely late: `event_loop_now` is PER FLOW, so an
     * estimate initialized on first read would be fixed out of whichever flow happened to touch
     * `performance.timeOrigin` first and would then be an agent-wide fact minted inside one flow's timeline.
     * The first realm's install runs before any flow does, which is what makes this a group fact rather than
     * one flow's. */
    if (!g_epoch_known) {
        double monotonic_ms = 0, wall_ms;

        /* §4 step 2: "Let monotonic time be the monotonic clock's unsafe current time." It is the moment read
           at the top of this function, which is also the one this realm's time origin is stamped with — one
           read and not two, so the group's estimate and the first environment's origin cannot be placed against
           each other by a clock that moved in between. The read is therefore BEFORE step 1's below rather than
           after, and that reordering is unobservable here for the reason hr_time_unsafe_shared_current states:
           this monotonic clock advances only when a task source becomes due, so it holds one moment for the
           whole of an install.
           Before any flow has run, the loop has reached no task source, so this is the agent's starting moment
           and is KNOWN. The assert is what says so: the day a realm is first built after an unknown timeout has
           moved the clock (core/timing/event_loop.h's §8.7 `timeout` reaching the map of active timers), the
           estimate would be a derivation over unknown input and every `performance.timeOrigin` in the agent
           would fork. */
        DCHECK(JS_IsNumber(moment),
               "HR-TIME §4's estimated monotonic time of the Unix epoch was initialized from a monotonic "
               "moment this run does not KNOW — the estimate is one fact for the whole agent, so a derivation "
               "here would hand every realm's `performance.timeOrigin` an unknown and fork every branch over "
               "it. BUILD the group fact where the group is created rather than at the first realm: carry the "
               "agent's starting moment on the agent (core/agent_state.h) and initialize this from that");
        JS_ToFloat64(ctx, &monotonic_ms, moment);
        /* §4 step 1: "Let wall time be the wall clock's unsafe current time." The ONE wall-clock read in this
           file — see the estimate's declaration above for why it is here and nowhere else. */
        wall_ms = (double)js__gettimeofday_us() / 1000.0;
        /* §4 step 3: "Let epoch time be monotonic time - (wall time - Unix epoch)". The Unix epoch is "the
           moment on the wall clock corresponding to 1 January 1970 00:00:00 UTC", which is the moment this
           clock reports as zero, so `wall time - Unix epoch` is the reading itself.
           §4 step 4: "Initialize the estimated monotonic time of the Unix epoch to the result of calling
           coarsen time with epoch time." NOTE THE ARGUMENT LIST: step 4 passes NO crossOriginIsolatedCapability,
           so `coarsen time`'s optional second argument takes its declared default of false and that algorithm's
           OWN step 1 — 100 microseconds — is the grid. It is also the only grid available here: a group has no
           environment whose capability could be asked, and the first realm's Document does not exist yet. That
           is why this is hr_floor at the ordinary resolution and NOT hr_time_coarsen, whose whole contract is
           to ask an environment (and whose non-negative assertion is about moments of THIS clock; §4's epoch
           time is legitimately far in this clock's past, which is the entire point of it). */
        g_epoch_estimate_ms = hr_floor(monotonic_ms - wall_ms, HR_TIME_RESOLUTION_MS);
        g_epoch_known = 1;
    }
    /* Running twice in one realm is asserted by realm_value_set, which is where the first moment is standing. */
    realm_value_set(ctx, g_origin_slot, moment);
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

/* HR-TIME §4 Time Origin's UNSAFE SHARED CURRENT TIME — verbatim: "must return the unsafe current time of the
 * monotonic clock". IT IS ITS OWN OPERATION AND NOT A LINE INSIDE `current high resolution time`, because §4
 * gives it two readers (`current high resolution time` and `coarsened shared current time`) and because it is
 * the ONE place in this engine that reads the monotonic clock — the day the clock's own definition changes,
 * there is one site to change and no second answer to disagree with it.
 *
 * WHICH CLOCK IT IS. There is no wall clock in a headless run, and a second time source would order a timestamp
 * against the queue that runs the listeners observing it differently from the queue itself. So the monotonic
 * clock here IS the event loop's virtual clock (core/timing/event_loop.h), which is per-flow, time-travels with
 * the flow that reads it, and never decreases — HR-TIME §2.1 Clocks' one hard requirement on the monotonic
 * clock ("The monotonic clock's unsafe current time never decreases"), asserted at its own origin by
 * event_loop_advance_to.
 *
 * NAMED RESIDUAL — THE CLOCK DOES NOT COUNT WHILE A TASK RUNS.
 * WHAT IS NOT COVERED: §2.1's other sentence — "All clocks on the web platform attempt to count 1 millisecond
 *   of clock time per 1 millisecond of real-world time" — is narrower here than in a real UA. This clock moves
 *   only when a task source becomes due (a timer's expiry, a rendering opportunity), so every moment read
 *   inside ONE task is the SAME moment and every duration between two of them is exactly zero. That is CORRECT
 *   for everything this engine currently orders by time, which is why this is a residual and not a crash: a
 *   task's timestamps genuinely all belong to the moment the task ran, the value is a real duration from a real
 *   time origin, and §4's coarsening below is performed rather than skipped.
 * WHAT THE NEXT DIFF BUILDS: the clock advances as a function of WORK THE RUNNING FLOW PERFORMED, in
 *   core/timing/event_loop.c, driven from the per-opcode attention check that is already the one thing counting
 *   a flow's own progress. It must NOT be a wall clock: a real clock is not a function of the flow's path, so
 *   every timestamp becomes a disagreement §Testing's solver differential reports as a scheduling bug, and a
 *   resume stops being byte-identical. Work-derived it stays deterministic, per-flow and COW-captured, which is
 *   the only shape both halves of this project accept.
 * HOW ITS ABSENCE SHOWS: a page that waits for time to pass inside one task never leaves the loop —
 *   `do { e2 = new MouseEvent('t'); } while (e2.timeStamp - e1.timeStamp === 0)` has no exit, and
 *   wpt dom/events/Event-timestamp-safe-resolution.html is that loop written out. The flow is preemptible
 *   bytecode, so the scheduler is not violated and nothing is capped; the flow simply never emits again and the
 *   harness's CPU backstop is what reports it, which is a signal about the harness rather than about the page. */
static JSValue hr_time_unsafe_shared_current(JSContext *ctx)
{
    return event_loop_now(ctx);
}

/* HR-TIME §2.2 Moments and Durations' DURATION FROM ONE MOMENT TO ANOTHER, which is the shape of every value
 * §4 returns: `relative high resolution coarse time` is "the duration from ... time origin to coarseTime" and
 * `get time origin timestamp` is "the duration from the estimated monotonic time of the Unix epoch to
 * timeOrigin". TWO callers, ONE subtraction, because the operand ORDER is the half of this that is easy to get
 * backwards and a second copy is a second chance to get it backwards independently.
 *
 * THE SUBTRACTION IS ECMAScript §13.8.2 The Subtraction Operator ( - ), RUN — which §13.15.3
 * ApplyStringOrNumericBinaryOperator performs, the same abstract operation `+` reaches. Its result's identity is
 * composed from the operator and BOTH operands, which is why it goes through the operator rather than through a
 * derivation named for one clause: two environments subtracting two different origins from one coarsened moment
 * must not compose one key. The hook's stack effect is the interpreter's — both operands freed, the result
 * placed in sp[-2] — so both are handed over here. BOTH ARGUMENTS ARE CONSUMED either way. */
static JSValue hr_duration(JSContext *ctx, JSValue from, JSValue to)
{
    JSValue sp[2];

    if (!concolic_is(from) && !concolic_is(to)) {
        double f = 0, t = 0;

        JS_ToFloat64(ctx, &f, from);
        JS_ToFloat64(ctx, &t, to);
        JS_FreeValue(ctx, from);
        JS_FreeValue(ctx, to);
        return JS_NewFloat64(ctx, t - f);
    }
    sp[0] = to;
    sp[1] = from;
    if (!concolic_arith_hook(ctx, sp + 2, JS_CARITH_SUB, 2))
        DFAIL("§13.8.2 The Subtraction Operator ( - ) declined an operand this component has already "
              "established is UNKNOWN — the "
              "concolic value semantics are not installed in this host, so every operator over an unknown "
              "falls through to the ordinary-object path and the next coercion throws out of an expression "
              "the page never wrote (solver/concolic.h: concolic_install_hooks)");
    return sp[0];
}

/* HR-TIME §4 Time Origin's GET TIME ORIGIN TIMESTAMP. §7.2's `timeOrigin` is its only reader, and this is why
 * hr_time.h has said since it was written that the day this engine has a `performance` object it becomes §4's
 * second reader of the origin — it is a DIFFERENT operation over the same stored moment, not the same one.
 *
 * IT TAKES THE STORED MOMENT AND NOT hr_time_origin's COARSENING, and the two-line difference is the spec's.
 * §4 step 1 is "Let timeOrigin be global's relevant settings object's time origin" — the moment as stored, with
 * no coarsening in this algorithm at all — because the coarsening that protects this value has ALREADY happened
 * at the other end: step 4 of the estimate's own initialization coarsened it, so the duration is a moment minus
 * a value on the 100-microsecond grid. hr_time_origin coarsens because ITS reader (`relative high resolution
 * coarse time`) subtracts the origin from a COARSENED moment and both ends have to be on one grid; that reason
 * does not exist here and borrowing its answer would be applying one algorithm's step inside another. */
JSValue hr_time_origin_timestamp(JSContext *ctx)
{
    JSValue origin = realm_value_get(ctx, g_origin_slot);

    DCHECK(g_epoch_known,
           "HR-TIME §7.2's `timeOrigin` was read before §4's estimated monotonic time of the Unix epoch was "
           "initialized — the estimate is stamped by the FIRST realm this agent builds and this attribute is "
           "reachable only THROUGH a realm, so the two cannot come apart unless the install stopped running");
    DCHECK(JS_IsNumber(origin) || concolic_is(origin),
           "an environment's TIME ORIGIN is neither a moment on the monotonic clock nor a derivation of one — "
           "HR-TIME §4 stores the clock's moment at this realm's creation, and the install that stamps it is "
           "the only thing that ever writes this slot");
    /* §4 step 3: "Return the duration from the estimated monotonic time of the Unix epoch to timeOrigin." */
    return hr_duration(ctx, JS_NewFloat64(ctx, g_epoch_estimate_ms), origin);
}

JSValue hr_time_relative(JSContext *ctx, JSValueConst unsafe_moment)
{
    JSValue coarse, origin;

    /* §4's argument is "an unsafe moment FROM THE MONOTONIC CLOCK", and §2.1 Clocks says what a clock reports:
       "the unsafe current time that an algorithm step is executing". A moment this clock has not reached was
       reported by nothing, so it is not a moment of this clock at all — it is a moment some caller COMPUTED
       (§8.1.7.3's next rendering opportunity is one) and handed to the wrong operation, and the timestamp that
       came back would let a page observe an occurrence before the event loop reached it.
       READ, NEVER ASKED, for the reason the negative-duration invariant below is: over an unknown moment a
       comparison is an arm to take rather than a fact to test, so this fires on a DECIDED contradiction and
       stays silent on uncertainty. The clock READ is inside the guard because it is the assertion's operand
       and nothing else's — DCHECKs are free in release, and one that left a property read behind would not be. */
#if APICLIENT_DEV
    {
        JSValue now = hr_time_unsafe_shared_current(ctx);

        DCHECK(event_loop_before_decided(ctx, now, unsafe_moment) != 1,
               "HR-TIME §4's relative high resolution time was given a moment the monotonic clock HAS NOT "
               "REACHED — §2.1 Clocks reports the moment an algorithm step is executing, so a later moment is "
               "one this caller computed rather than one the clock handed it, and the timestamp would date an "
               "occurrence into the event loop's future");
        JS_FreeValue(ctx, now);
    }
#endif

    coarse = hr_time_coarsen(ctx, unsafe_moment);
    origin = hr_time_origin(ctx);

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
    return hr_duration(ctx, origin, coarse);
}

JSValue hr_time_current(JSContext *ctx)
{
    /* §4's CURRENT HIGH RESOLUTION TIME, in its own words: "the result of relative high resolution time given
       unsafe shared current time and current global". Both halves are the named operations above, so this
       clause is the composition the standard writes and holds no arithmetic of its own. */
    JSValue now = hr_time_unsafe_shared_current(ctx), r = hr_time_relative(ctx, now);

    JS_FreeValue(ctx, now);
    return r;
}

void hr_time_init(JSContext *ctx)
{
    DCHECK(g_origin_slot < 0, "hr_time_init ran twice — the TIME ORIGIN's slot is declared once per AGENT");
    DCHECK(!g_epoch_known, "hr_time_init ran twice — §4's estimated monotonic time of the Unix epoch is the "
                           "GROUP's, so a second agent starting on the first one's estimate would place its "
                           "clock's zero against a wall-clock reading taken in a process that is gone");
    g_origin_slot = realm_value_declare(ctx, "HR-TIME §4 the environment settings object's time origin");
    agent_state_id("hr_time", &g_origin_slot, "HR-TIME §4's time-origin realm slot");
    agent_state_flag("hr_time", &g_epoch_known,
                     "HR-TIME §4's estimated monotonic time of the Unix epoch, and the latch that says the "
                     "first realm's install has stamped it");
    realm_declare_intrinsic(hr_time_install);
}

void hr_time_free(void)
{
    /* The moments are the REALMS' — each is released with its context. What the agent holds is the slot, and a
       slot id is a class id in a runtime that is going away with it. */
    g_origin_slot = -1;
    /* §4's estimate is the GROUP's and this group is over. The double goes back with its latch rather than
       being left to be read under a `g_epoch_known` a later agent has re-raised: it names a moment on a
       monotonic clock that no longer runs, and the pair is one fact. */
    g_epoch_known = 0;
    g_epoch_estimate_ms = 0.0;
}
