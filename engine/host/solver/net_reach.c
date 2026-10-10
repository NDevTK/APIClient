/* See net_reach.h. */
#include "solver/net_reach.h"
#include "solver/flow.h"
#include "check.h"

#include <string.h>

void net_reach_census(JSContext *ctx, NetReachCensus *out) {
    Flow *f;
    int i;

    DCHECK(ctx != NULL && out != NULL, "the request-site reach census was asked with no runtime or no record");
    memset(out, 0, sizeof(*out));
    JS_NetSiteCensus(JS_GetRuntime(ctx), &out->sites);
    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        JSNetSiteAhead a;

        out->members++;
        if (!f->frame)
            continue;
        JS_FlowNetSiteAhead((const JSValue *)f->frame, &a);
        DCHECKF(a.top_ahead <= a.frames_ahead && a.frames_ahead <= a.frames,
                "a member's stack reads %d frames ahead of a site over %d frames with top %d",
                a.frames_ahead, a.frames, a.top_ahead);
        if (a.frames == 0)
            continue;
        out->framed++;
        if (a.frames_ahead)
            out->ahead_stack++;
        if (!a.top_ahead)
            continue;
        if (out->ahead_top == 0 || (long)a.top_bytes < out->top_min)
            out->top_min = (long)a.top_bytes;
        if ((long)a.top_bytes > out->top_max)
            out->top_max = (long)a.top_bytes;
        out->top_sum += a.top_bytes;
        out->ahead_top++;
    }
    DCHECKF(out->members == flow_count(), "the reach walk visited %ld members of a %d-member frontier",
            out->members, flow_count());
    DCHECKF(out->ahead_top <= out->ahead_stack && out->ahead_stack <= out->framed && out->framed <= out->members,
            "reach member rows out of order: top %ld, stack %ld, framed %ld, members %ld", out->ahead_top,
            out->ahead_stack, out->framed, out->members);
}
