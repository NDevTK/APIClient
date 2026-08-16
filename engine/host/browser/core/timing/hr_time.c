/* HIGH RESOLUTION TIME Level 3 §4. See hr_time.h for what this is and why the time origin is an environment
   field rather than a static, a document field or a concolic. */
#include <math.h>

#include "check.h"
#include "quickjs.h"
#include "core/realm.h"
#include "core/timing/event_loop.h"
#include "core/timing/hr_time.h"

/* §4's TIME RESOLUTION for an environment WITHOUT the cross-origin isolated capability: "let time resolution
   be 100 microseconds, or a higher implementation-defined value". In the millisecond unit DOMHighResTimeStamp
   is measured in, that is 0.1. The 5-microsecond branch belongs to the capability this build does not have,
   and `hr_time_coarsen` asserts its absence rather than assuming it. */
#define HR_TIME_RESOLUTION_MS 0.1

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
    DCHECK(unsafe_moment >= 0.0,
           "HR-TIME §4's coarsen time was given a moment BEFORE the agent's clock started — the unsafe shared "
           "current time in this engine is the event loop's virtual clock, which begins at zero and never runs "
           "backwards (core/timing/event_loop.h asserts the second half at every move)");
    /* THE CROSS-ORIGIN ISOLATED CAPABILITY, asked where §4 asks it. `crossOriginIsolated` is the member whose
       arrival would mean an environment can answer true, and the resolution is then 5 microseconds instead of
       100 — a page measuring its own timing sees the difference directly, so this is a behaviour change and not
       a constant. The probe is HERE, at the one place the resolution is decided, rather than once per realm at
       install: a realm's intrinsics are built in declaration order, so an install-time probe would run before
       whichever component eventually installs the member and would be silent forever. */
    realm_awaits(ctx, "crossOriginIsolated",
                "HR-TIME §4's coarsen time makes the time resolution 5 microseconds instead of 100 when the "
                "environment has the CROSS-ORIGIN ISOLATED CAPABILITY — this build now has a realm that can "
                "answer that question, so the capability must become an environment settings object field and "
                "this resolution must be decided from it");
    return floor(unsafe_moment / HR_TIME_RESOLUTION_MS) * HR_TIME_RESOLUTION_MS;
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
