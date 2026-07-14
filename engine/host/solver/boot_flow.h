/* BOOT-AS-FLOW + CANDIDATE-REPLAY + ATTACKER SESSION — see boot_flow.c. The flow types beyond a plain orphan.
 * The scheduler dispatch (scheduler_run) calls boot_replay / boot_replay_candidate / resolve_replayed_handler
 * during a boot/candidate flow; branch_decide calls reg_add_boot/reg_add_session to fork one; the engine
 * registers js_session_fns/js_session_drain as the self-hosted attacker-session loop's globals. */
#ifndef ENGINE_HOST_SOLVER_BOOT_FLOW_H
#define ENGINE_HOST_SOLVER_BOOT_FLOW_H
#include "quickjs.h"

/* The canonical BOOT COW DELTA: the page's boot mutations, captured once by the engine after the initial boot,
   READ by candidate/opaque flows to reach the pre-boot baseline (JS_CowSeedBootInverse). Owned here; the engine
   captures it (qjs_init) and frees it (teardown), the scheduler + boot_replay_candidate read it. */
extern void *g_boot_delta; extern int g_boot_delta_n, g_boot_delta_cap;

void boot_replay(JSContext *ctx);                       /* re-run the page's inline scripts (candidate boot replay) */
void boot_delta_merge_active(JSContext *ctx);           /* merge a post-boot chunk's captured baseline globals into g_boot_delta */
int  reg_add_boot(JSContext *ctx, signed char *dec, int dec_n);      /* enqueue a forking BOOT flow */
void boot_replay_candidate(JSContext *ctx);             /* re-run boot under the running flow's candidate */
JSValue resolve_replayed_handler(JSContext *ctx, JSValueConst orig); /* re-resolve a candidate-closure handler by source identity */

#endif
