/* EventTarget — DOM §2.7. See event_target.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_TARGET_H
#include "quickjs.h"

void event_target_init(JSContext *ctx);                          /* the private listener key (install time) */
void event_target_install(JSContext *ctx, JSValueConst target);  /* add/removeEventListener + dispatchEvent */
/* Fire `type` at `target` (bubbling to `bubble_to` when non-undefined, which is how a document event reaches
   window). Each listener runs as its own task on the RUNNING flow. Returns how many were scheduled. */
int  event_target_fire(JSContext *ctx, JSValueConst target, const char *type, JSValueConst bubble_to);

#endif
