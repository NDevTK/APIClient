/* ATTACKER SESSION flow — fire ALL registered handlers (+ boot-executed readers) in seed order over ONE
 * accumulating COW delta, modeling an attacker firing a sequence of events. Handler A's tainted write to shared
 * state persists to handler B (no revert between them), so a cross-handler sink — a source stored by A, sunk by
 * B — is reached. This is a forced-execution FLOW TYPE (not boot, not replay): it never re-runs the page's boot,
 * it drives the handler fire-list as a self-hosted bytecode loop (__driveSession) so it yields per-opcode like
 * any flow. Split out of the deleted boot_flow.c — sessions are their own component, distinct from the removed
 * boot-replay machinery. The scheduler enqueues an exploratory session (reg_add_session) from a fork; the engine
 * registers js_session_fns/js_session_drain as the session loop's host globals. */
#ifndef ENGINE_HOST_SOLVER_SESSION_H
#define ENGINE_HOST_SOLVER_SESSION_H
#include "quickjs.h"

int  reg_add_session(JSContext *ctx, signed char *dec, int dec_n);   /* enqueue an exploratory attacker SESSION that forks */
JSValue js_session_fns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);    /* the session fire-list [fn,event,this] */
JSValue js_session_drain(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);  /* drive the fire-list as a flow */

#endif
