/* ASYNC-AS-FLOW — a native async CALL, a settled-promise REACTION, and each fulfilled AWAIT are all first-class
 * BFS flows, so async recursion is a TREE of preemptible/parkable flows (each its own COW delta), never a
 * non-preemptible promise-reaction drain. This TU owns the async-flow HOOKS + the cross-session async-recipe
 * map; it reaches the scheduler through solver/scheduler.h (reg_add + the running-flow decision context). See
 * async_flow.c. The hooks are installed by the engine (JS_SetAsyncCallHook/ReactionHook/FlowAwaitHook). */
#ifndef ENGINE_HOST_SOLVER_ASYNC_FLOW_H
#define ENGINE_HOST_SOLVER_ASYNC_FLOW_H
#include "quickjs.h"
#include "solver/scheduler.h"   /* Flow */

int reg_add_async_call(JSContext *ctx, void *fs, JSValueConst func_obj, JSValueConst resolve, JSValueConst reject);   /* JSAsyncCallHook: a native async call -> a flow */
int await_decide(JSContext *ctx);   /* JSFlowAwaitHook: a fulfilled await forks a reject-replay sibling (0=fulfil,1=reject) */
int reaction_flow(JSContext *ctx, JSValueConst handler, JSValueConst value, int is_reject, JSValueConst resolve, JSValueConst reject);   /* JSReactionHook: a .then/.catch/.finally reaction -> a flow */

void flow_free_async_refs(JSContext *ctx, Flow *f);   /* release a flow's async refs (resolve/reject/await-promise/recipe) at teardown */
int  arec_attach(JSContext *ctx, Flow *f);            /* attach a matching parked async recipe to a fresh async flow (source-hash) */
void arec_park(uint32_t hash, signed char *dec, int dec_n, double val, int visits);   /* park an async flow's recipe (qjs_begin resume; takes dec ownership) */
void arec_free(void);                                  /* free unconsumed recipes (fresh session + teardown) */

#endif
