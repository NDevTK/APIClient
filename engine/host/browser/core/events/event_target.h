/* EventTarget — DOM §2.7. See event_target.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"

void event_target_init(JSContext *ctx);                          /* the private listener key (agent init) */
/* §2.7's PROTOTYPE FOR ONE REALM. Run it where a realm's other intrinsics are added — at the realm's creation,
   beside JS_AddIntrinsicDOMException — and exactly once per realm. The agent's first realm gets it from
   event_target_init, because every agent-scoped prototype in this engine chains to that realm's and so it has
   to exist before them; a child navigable's realm gets it from its host's realm builder. */
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
/* §2.7's INTERFACE PROTOTYPE OBJECT, where addEventListener, removeEventListener and dispatchEvent live.
   An interface that INHERITS EventTarget — Node, AbortSignal, MessagePort, BroadcastChannel, Window — chains
   its own prototype to this one; it does not install the three members again. That is not a saving, it is the
   spec: `EventTarget.prototype` is where they are declared, `Node.prototype` is not, and the corpus checks
   both. PER REALM — §3.7 gives each its own, and here that decides ANSWERS and not just identities, because a
   C member runs in the realm that defined it (see event_target.c). OWNED: the caller frees. */
JSValue event_target_proto(JSContext *ctx);
/* `interface X : EventTarget` — chain X's prototype to §2.7's, in `ctx`'s realm. Four interfaces declare it and
   each spelled it as a borrowed read fed straight to JS_SetPrototype; now that the read is per-realm and owned,
   the pair is written once here rather than four times, all four of them free-or-leak. */
void event_target_chain(JSContext *ctx, JSValueConst proto);
/* §2.7's interface object. CONSTRUCTIBLE — `new EventTarget()` is a plain event target, which is how a page
   gives an ordinary object a listener list. */
void event_target_install_interface(JSContext *ctx, JSValueConst global);

/* HTML §8.1.7.2 EVENT HANDLER IDL ATTRIBUTES — `onclick`, `onload`, `onabort`. Which set a target carries is
   which MIXIN its IDL includes, so the caller names the mixin rather than the members. */
enum { EH_GLOBAL = 1, EH_WINDOW = 2, EH_DOCUMENT = 4, EH_SIGNAL = 8, EH_PORT = 16 };
/* HTML §3.2.2 click() — "fire a synthetic pointer event named click", which IS §2.9 dispatch, so it is the same
   machine under a second entry rather than a second implementation of it. */
void event_target_install_click(JSContext *ctx, JSValueConst target);
void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask);
/* IS THIS THE NAME OF AN EVENT HANDLER CONTENT ATTRIBUTE? HTML §8.1.7.2 defines that set as the names of the
   event handler IDL attributes above, so it is answered from the one list rather than from a second copy.
   Trusted Types §3.8 step 2 is the caller: an event handler content attribute maps to TrustedScript. */
bool event_target_is_handler_attribute(const char *name);
/* A handler attribute whose SETTER has a side effect. HTML has one: §9.4.2's `onmessage` on a MessagePort also
   starts the port, which is why assigning it is enough and addEventListener alone is not. The hook runs AFTER
   the handler is registered — start() delivers what is already queued, and delivering it first would fire at a
   target with no listener yet — and it is given the target and the attribute name so the registering component
   decides with its own brand test rather than this file knowing what a MessagePort is. */
void event_target_set_handler_hook(void (*after_set)(JSContext *ctx, JSValueConst target, const char *name));

/* DOM §2.9's ACTIVATION BEHAVIOUR — what makes a click on an `<a href>` FOLLOW the link, on a `<form>`'s submit
   button submit, on a checkbox toggle it. It is not a listener and a page cannot register one: the dispatch
   picks an ACTIVATION TARGET while it builds the propagation path — the nearest entry, target first, that HAS
   one — and runs it AFTER the walk, only if nothing called preventDefault. That "only if" is the whole of what
   `preventDefault` means on a click, and with no activation behaviour at all it meant nothing: §2.9 ran its
   three legs and then dropped the event, so `<a href>` clicked navigated nowhere and `e.preventDefault()`
   suppressed something that was never going to happen.
   The two halves are declared by whoever owns the element, for the reason the tree walk is: this file does not
   know what an `<a>` is. `has` answers whether that element has one; `run` PERFORMS it, and it performs it as a
   STEP: §4.6.3's is a navigation, navigating fetches, and a fetch is a host-owed answer that suspends the
   asking flow. So `run` has a step's return contract — JS_STEP_YIELD to be re-entered, JS_STEP_DONE when it is
   finished — and its own two words of state on the dispatch machine, which is already a step machine and can
   therefore hold the suspension. A `void` hook could only ever reach a SYNCHRONOUS behaviour, which is the same
   ceiling `window.open` had while it was a plain C body. */
void event_target_set_activation(bool (*has)(JSContext *ctx, JSValueConst el),
                                 int (*run)(JSContext *ctx, JSValueConst el, JSValueConst ev,
                                            uint8_t *phase, uint32_t *req));
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

#endif
