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
/* add/removeEventListener. NOT dispatchEvent: §2.9 dispatch is SYNCHRONOUS and reports whether the default
   action was cancelled, and this engine runs a listener as its own FLOW (never a JS_Call from C), so a caller
   would be handed an answer before any listener had run. It is honestly absent until the dispatch is a step
   machine that drives its listeners through the trampoline — the page's own throw names it. */
void event_target_install(JSContext *ctx, JSValueConst target);

/* HTML §8.1.7.2 EVENT HANDLER IDL ATTRIBUTES — `onclick`, `onload`, `onabort`. Which set a target carries is
   which MIXIN its IDL includes, so the caller names the mixin rather than the members. */
enum { EH_GLOBAL = 1, EH_WINDOW = 2, EH_DOCUMENT = 4, EH_SIGNAL = 8 };
/* HTML §3.2.2 click() — "fire a synthetic pointer event named click", which IS §2.9 dispatch, so it is the same
   machine under a second entry rather than a second implementation of it. */
void event_target_install_click(JSContext *ctx, JSValueConst target);
void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask);
/* Fire `type` at `target` (bubbling to `bubble_to` when non-undefined, which is how a document event reaches
   window). Each listener runs as its own task on the RUNNING flow. Returns how many were scheduled. */
int  event_target_fire(JSContext *ctx, JSValueConst target, const char *type, JSValueConst bubble_to);

#endif
