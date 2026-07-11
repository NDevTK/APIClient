/* Geolocation (navigator.geolocation) — the Geolocation API as a real Blink-style module (modules/geolocation/).
 * getCurrentPosition(success)/watchPosition(success) INVOKE the success callback as a driven BFS flow with a
 * concolic GeolocationPosition — the coordinates are permission-gated and genuinely unknowable headless, so
 * position.coords.latitude/longitude are concolic ({geolocation}) that FORK a feature branch AND carry taint to
 * an endpoint (`fetch('/api/nearby?lat='+p.coords.latitude+'&lng='+p.coords.longitude)` -> a geo-parameterized
 * endpoint the moat surfaces). A page using geolocation previously aborted on the unbuilt member; now its
 * location-gated code runs. Unbuilt members DFAIL via idl_dfail_wrap. */
#ifndef ENGINE_HOST_BROWSER_MODULES_GEOLOCATION_H
#define ENGINE_HOST_BROWSER_MODULES_GEOLOCATION_H
#include "quickjs.h"

/* The navigator.geolocation object (a Geolocation instance). */
JSValue geolocation_make(JSContext *ctx);

#endif
