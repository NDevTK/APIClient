/* BroadcastChannel — HTML §9.5 "Broadcasting to other browsing contexts". See broadcast_channel.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_BROADCAST_CHANNEL_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_BROADCAST_CHANNEL_H
#include "quickjs.h"

/* §9.5's destination match is over the ORIGIN and the channel name, and every broadcast carries the origin's
   SERIALIZATION as `event.origin`. The origin is the AGENT'S and core/url/origin.c holds it — an instance is
   an origin-keyed agent cluster, so a per-component copy of that fact could only ever agree or be wrong. */
void broadcast_channel_init(JSContext *ctx);
/* HTML §9.5 Broadcasting to other browsing contexts' interface prototype object, its Web IDL §3.7.1 Interface
   object's interface object and the Web IDL §3.8 Platform objects implementing interfaces property reference
   for it — for ONE realm, declared into core/realm.h's list by the init above. ONE entry because Web IDL §3.8 Platform objects implementing
   interfaces' `define the global property references` is given "target" and "a realm realm" and its step 1
   population is "every interface that is exposed in realm": no Document appears in it. html.idl declares the
   interface `[Exposed=(Window,Worker)]`, so a WORKER realm owes the name — and while the interface object was
   installed from core/platform.c's per-document column, a worker realm, which reaches no
   platform_document_install, received neither. */
void broadcast_channel_install_realm(JSContext *ctx);
void broadcast_channel_free(JSRuntime *rt);

#endif
