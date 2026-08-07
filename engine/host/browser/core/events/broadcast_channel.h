/* BroadcastChannel — HTML §9.5. See broadcast_channel.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_BROADCAST_CHANNEL_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_BROADCAST_CHANNEL_H
#include "quickjs.h"

/* `origin` is this document's serialized origin — the value every broadcast carries as `event.origin`, and
   half of what §9.5's destination match is over (the other half being the channel name). */
void broadcast_channel_init(JSContext *ctx, const char *origin);
void broadcast_channel_install(JSContext *ctx, JSValueConst global);
void broadcast_channel_free(JSContext *ctx);

#endif
