/* EVENT-HANDLER REGISTRY — the page's registered event listeners (addEventListener), kept so the forced-exec
 * orphan driver can FIRE the ones that never fire on their own (the unused gated surface) and drive a
 * 'message' listener with the synthetic {pm} attacker event. This is event-handling, distinct from the WFQ
 * scheduler: js_add_listener is the addEventListener host-edge; the registry is a list the solver force-fires.
 * State is exposed (the scheduler dispatch + the attacker-session iterate it) but owned here. See
 * handler_registry.c. Depends on the boot-replay flags (scheduler.h) to know when NOT to grow the registry. */
#ifndef ENGINE_HOST_HANDLER_REGISTRY_H
#define ENGINE_HOST_HANDLER_REGISTRY_H
#include "quickjs.h"

extern JSValue g_handlers;      /* JS array of registered listener functions (borrowed refs also in g_msg_handlers) */
extern int g_handler_n;         /* count in g_handlers */
extern void *g_msg_handlers[128]; extern int g_msg_handler_n;   /* 'message' listener ptrs -> drive with the {pm} event */
extern JSValue *g_replay_handlers; extern int g_replay_handler_n, g_replay_handler_cap;   /* candidate-boot-replay re-registered (closure) handlers */
extern void *g_replay_msg[128]; extern int g_replay_msg_n;      /* re-registered 'message' handler ptrs (candidate closure) */

void handlers_init(JSContext *ctx);   /* create g_handlers (qjs_init) */
void handlers_free(JSContext *ctx);   /* free g_handlers + g_replay_handlers + reset counts (teardown) */
JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* addEventListener host-edge (xhr.c borrows it too) */
int is_handler(JSContext *ctx, JSValueConst fn);   /* is fn a registered listener? (drive it with a real Event, not opaque) */
int is_msg_handler(JSValueConst h);                /* is h a 'message' listener? (drive it with the {pm} event) */
void replay_handlers_clear(JSContext *ctx);        /* drop the transient candidate-closure handlers after a drive */

#endif
