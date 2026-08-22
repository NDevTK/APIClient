/* BroadcastChannel — HTML §9.5. See broadcast_channel.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_BROADCAST_CHANNEL_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_BROADCAST_CHANNEL_H
#include "quickjs.h"

/* §9.5's destination match is over the ORIGIN and the channel name, and every broadcast carries the origin's
   SERIALIZATION as `event.origin`. The origin is the AGENT'S and core/url/origin.c holds it — an instance is
   an origin-keyed agent cluster, so a per-component copy of that fact could only ever agree or be wrong. */
void broadcast_channel_init(JSContext *ctx);
/* §9.5's interface prototype object for ONE realm — declared into core/realm.h's list by the init above. */
void broadcast_channel_install_proto(JSContext *ctx);
void broadcast_channel_install(JSContext *ctx, JSValueConst global);
void broadcast_channel_free(JSRuntime *rt);

#endif
