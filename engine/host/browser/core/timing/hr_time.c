/* HIGH RESOLUTION TIME Level 3 §4. See hr_time.h for what this is and why the time origin is an environment
   field rather than a static, a document field or a concolic. */
#include <math.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/agent_cluster.h"
#include "core/realm.h"
#include "core/timing/event_loop.h"
#include "core/timing/hr_time.h"

/* §4's TIME RESOLUTION, in the millisecond unit DOMHighResTimeStamp is measured in. Step 1: "let time
   resolution be 100 microseconds, or a higher implementation-defined value". Step 2: "if
   crossOriginIsolatedCapability is true, set time resolution to be 5 microseconds, or a higher
   implementation-defined value". Both steps permit a COARSER value and this engine takes neither offer: a
   higher value is a worse answer to every question a page asks about its own timing, and the page can see the
   difference. Which of the two applies is the ENVIRONMENT'S question and is asked below, not chosen here. */
#define HR_TIME_RESOLUTION_MS          0.1
#define HR_TIME_RESOLUTION_ISOLATED_MS 0.005

static int g_origin_slot = -1;

/* §4: "a moment early in the initialization of a relevant environment settings object". This runs from the one
   call every realm goes through (core/realm.h), which is where this engine creates that environment — so the
   moment the standard describes and the moment this reads are the same one.
 *
 * THE ORIGIN IS ITSELF COARSENED, and that is not tidiness. `relative high resolution coarse time` is "the
 * duration from the time origin to coarseTime", so an origin off the resolution grid would make that duration
 * carry a sub-resolution remainder — and would make it NEGATIVE for a moment equal to the origin, which is the
 * one comparison hr_time_relative asserts. Coarsening both ends puts every page-visible timestamp on the grid
 * and keeps the duration a multiple of the resolution, which is what a browser reports. */
static void hr_time_install(JSContext *ctx)
{
    /* Running twice in one realm is asserted by realm_value_set, which is where the first moment is standing. */
    realm_value_set(ctx, g_origin_slot, JS_NewFloat64(ctx, hr_time_coarsen(ctx, event_loop_now(ctx))));
}

double hr_time_origin(JSContext *ctx)
{
    JSValue v = realm_value_get(ctx, g_origin_slot);
    double t = 0.0;

    DCHECK(JS_IsNumber(v),
           "an environment's TIME ORIGIN is not a number — HR-TIME §4 makes it a moment on the monotonic "
           "clock, and the install that stamps it is the only thing that ever writes this slot");
    JS_ToFloat64(ctx, &t, v);
    JS_FreeValue(ctx, v);
    return t;
}

double hr_time_coarsen(JSContext *ctx, double unsafe_moment)
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
    double resolution = agent_cluster_cross_origin_isolated(ctx) ? HR_TIME_RESOLUTION_ISOLATED_MS
                                                                 : HR_TIME_RESOLUTION_MS;

    DCHECK(unsafe_moment >= 0.0,
           "HR-TIME §4's coarsen time was given a moment BEFORE the agent's clock started — the unsafe shared "
           "current time in this engine is the event loop's virtual clock, which begins at zero and never runs "
           "backwards (core/timing/event_loop.h asserts the second half at every move)");
    /* §4 step 3: "in an implementation-defined manner, coarsen AND POTENTIALLY JITTER timestamp such that its
       resolution will not exceed time resolution". This engine takes the coarsening and declines the jitter —
       see hr_time.h for why that is a design constraint and not a shortcut. Flooring onto the grid is then the
       whole of step 3, and it is MONOTONE (division, floor and multiplication all are), which is what makes
       hr_time_relative's non-negative duration an invariant rather than a hope. */
    return floor(unsafe_moment / resolution) * resolution;
}

double hr_time_relative(JSContext *ctx, double unsafe_moment)
{
    double coarse = hr_time_coarsen(ctx, unsafe_moment);
    double origin = hr_time_origin(ctx);

    /* §4: "the duration from global's relevant settings object's time origin to coarseTime". A NEGATIVE
       duration would be a moment this environment can observe that precedes the environment's own creation,
       and there is exactly one way for that to arise here: core/frame/window_proxy.h DEFERS building a child
       navigable's realm until something reaches it, so a Window §7.4 created at one moment gets its time
       origin at a LATER one. The day this fires, the fix is the one core/html/user_activation.c already names
       for the same deferral — carry the creation moment on the NAVIGABLE and hand it to the realm when it
       materializes — and never a clamp here, which would report a page's own first frame as moment zero. */
    DCHECK(coarse >= origin,
           "HR-TIME §4's relative high resolution time is NEGATIVE — this environment was asked about a moment "
           "earlier than its own TIME ORIGIN, which means the origin was stamped when the realm materialized "
           "rather than when HTML §7.4 created the Window");
    return coarse - origin;
}

double hr_time_current(JSContext *ctx)
{
    /* §4: "the result of relative high resolution time given UNSAFE SHARED CURRENT TIME and current global".
       There is no wall clock in a headless run and a second time source would order a timestamp against the
       queue that runs the listeners observing it differently from the queue itself, so the unsafe shared
       current time is the event loop's one virtual clock. */
    return hr_time_relative(ctx, event_loop_now(ctx));
}

void hr_time_init(JSContext *ctx)
{
    DCHECK(g_origin_slot < 0, "hr_time_init ran twice — the TIME ORIGIN's slot is declared once per AGENT");
    g_origin_slot = realm_value_declare(ctx, "HR-TIME §4 the environment settings object's time origin");
    realm_declare_intrinsic(hr_time_install);
}

void hr_time_free(void)
{
    /* The moments are the REALMS' — each is released with its context. What the agent holds is the slot, and a
       slot id is a class id in a runtime that is going away with it. */
    g_origin_slot = -1;
}
