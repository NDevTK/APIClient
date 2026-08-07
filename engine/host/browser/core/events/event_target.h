/* EventTarget — DOM §2.7. See event_target.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#include "quickjs.h"

void event_target_init(JSContext *ctx);                          /* the private listener key (install time) */
/* Release that key. A component that mints a RUNTIME-LIFETIME value owns it, and this one did not free its
   Symbol — so every instance leaked it. It was invisible while only the ABI entry installed listeners, because
   nothing there runs the leak check; the moment the fixture harness installed the same components it ships
   with, JS_FreeRuntime's gc_obj_list assert named it. */
void event_target_free(JSContext *ctx);

/* WHO KNOWS A TARGET'S ANCESTORS. §2.9's propagation path walks up the tree, and the tree is the DOM's — so the
   DOM registers the walk rather than this file naming it. `ancestors` answers an Array of the target's
   ancestors, nearest first, and an empty one for a target that is in no tree. A host that registers none — a
   headless harness with no document — dispatches at the target and nowhere else, which is the whole of what
   §2.9 says for a target with no parent. */
void event_target_set_tree(JSValue (*ancestors)(JSContext *ctx, JSValueConst target));
/* add/removeEventListener. NOT dispatchEvent: §2.9 dispatch is SYNCHRONOUS and reports whether the default
   action was cancelled, and this engine runs a listener as its own FLOW (never a JS_Call from C), so a caller
   would be handed an answer before any listener had run. It is honestly absent until the dispatch is a step
   machine that drives its listeners through the trampoline — the page's own throw names it. */
void event_target_install(JSContext *ctx, JSValueConst target);

/* HTML §8.1.7.2 EVENT HANDLER IDL ATTRIBUTES — `onclick`, `onload`, `onabort`. Which set a target carries is
   which MIXIN its IDL includes, so the caller names the mixin rather than the members. */
enum { EH_GLOBAL = 1, EH_WINDOW = 2, EH_DOCUMENT = 4, EH_SIGNAL = 8, EH_PORT = 16 };
/* HTML §3.2.2 click() — "fire a synthetic pointer event named click", which IS §2.9 dispatch, so it is the same
   machine under a second entry rather than a second implementation of it. */
void event_target_install_click(JSContext *ctx, JSValueConst target);
void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask);
/* A handler attribute whose SETTER has a side effect. HTML has one: §9.4.2's `onmessage` on a MessagePort also
   starts the port, which is why assigning it is enough and addEventListener alone is not. The hook runs AFTER
   the handler is registered — start() delivers what is already queued, and delivering it first would fire at a
   target with no listener yet — and it is given the target and the attribute name so the registering component
   decides with its own brand test rather than this file knowing what a MessagePort is. */
void event_target_set_handler_hook(void (*after_set)(JSContext *ctx, JSValueConst target, const char *name));
/* The ENGINE firing its own event at `target`. One §2.9 dispatch, reached as a queued task because the callers
   are plain C the scheduler drives. The propagation path is derived from the target's ancestors — there is no
   `bubble_to` to pass, because the window is the document's parent and the spec already says so.
   It takes the EVENT, not a type and two flags: §2.9 dispatches an event, and a caller with a DERIVED one
   (PromiseRejectionEvent) has no way to hand it over if this mints its own. `ev` is CONSUMED. */
void event_target_fire(JSContext *ctx, JSValueConst target, JSValue ev);
/* THE SAME FIRE for a caller that can park — §2.9 is synchronous, and §3.2's `abort` is specified that way. One
   dispatch, two reaches: this is the REQUEST form, event_target_fire is the queued one. `phase` and `cb` belong
   to the calling machine and `cb` needs FOUR slots — pass it through STEP_CB so its capacity comes with it, as
   a forwarded buffer can no longer be measured. `ev` is the caller's too: the CALLER owns it and holds it across
   the suspension, because §2.9 dispatches an event that exists rather than one the dispatch invents. Returns
   JS_STEP_CALL (return it) or 0 when it has answered. */
int  event_target_fire_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst target,
                           JSValueConst ev, JSValue in,
                           bool *pnot_canceled, JSValue **out_cb, int *out_argc);
/* §7.6: the window the propagation path ends at. */
void event_target_set_window(JSContext *ctx, JSValueConst global);

#endif
