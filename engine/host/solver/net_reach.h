/* Request-site reach: how much of the code that names a network door the frontier has executed, and how far
 * each parked member stands from the nearest site it has not. A diagnostic published beside the WFQ census
 * (solver/result.c's `reach*` rows); it decides nothing and no term of flow_weight reads it. quickjs.h's
 * JS_NetSiteCensus states what a site is and what the census cannot see. A static census of call sites is a
 * measurement of reach only: it never names an endpoint, and no address or value is read from it.
 */
/* Named residual: request sites a global resolution cannot spell.
 * Not covered: a door whose entry is a member of some other receiver (`navigator.sendBeacon`, an element's
 * `src`/`href`/`action`, `form.submit()`) or no name at all (`import()`); a site is only a read of an entry
 * that solver/endpoint.c's edges declare (`fetch`, `XMLHttpRequest`), and an XHR site is its constructor.
 * Next diff: an edge declares a member entry and the compiler reports the field get that names it on any
 * receiver, beside the global report, so those reads join each body's site table.
 * Absence shows as: @H rows arriving through the `beacon`, `image-element`, `form-submit` or `module-import`
 * doors while `reachSites` counts only bodies that spell `fetch` or `XMLHttpRequest`. */
#ifndef ENGINE_HOST_SOLVER_NET_REACH_H
#define ENGINE_HOST_SOLVER_NET_REACH_H

#include "quickjs.h"

/* One instant. `sites` is the runtime's census (quickjs.h). The member rows fold JS_FlowNetSiteAhead over the
   frontier: `framed` members have a frame executing a body; `ahead_top` of those stand in a body holding an
   unreached site ahead of the deepest frame's pc, `ahead_stack` in any frame of their stack. The distance
   rows are bytecode bytes over the `ahead_top` members and read 0 when there are none. All member rows are
   gauges; `ahead_top <= ahead_stack <= framed <= members` is asserted. */
typedef struct {
    JSNetSiteCensus sites;
    long members, framed, ahead_top, ahead_stack;
    long top_min, top_max;
    long long top_sum;
} NetReachCensus;

/* Reads `ctx`'s runtime and every frontier member. Pure: it steps, allocates and weighs nothing. */
void net_reach_census(JSContext *ctx, NetReachCensus *out);

#endif
