/* @S sink entry — the destination every DOM/URL/JS sink host-edge calls when tainted input reaches it.
 *
 * solve_add records the reached sink and (for an opaque flow) spawns candidate-replay flows that drive
 * concrete breakout payloads through the real code to prove a working PoC. It's scheduler-coupled (it enqueues
 * flows), so it lives in main.c; this header lets the host-edges in other TUs (dom_element.c, ...) call it
 * without seeing the scheduler internals. `sctx` is the sink context: "html"/"htmls"/"url"/"js". */
#ifndef ENGINE_HOST_SOLVE_H
#define ENGINE_HOST_SOLVE_H

#include "quickjs.h"

void solve_add(JSContext *ctx, const char *sink, const char *sctx, JSValueConst val);

#endif
