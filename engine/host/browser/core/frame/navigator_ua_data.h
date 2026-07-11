/* NavigatorUAData (navigator.userAgentData) — UA Client Hints, a core/frame interface (Blink's home for it, not
 * a module). `mobile`/`platform` and the high-entropy fields are CONCOLIC (desktop example, fork the branch) so
 * `if (navigator.userAgentData.mobile)` explores BOTH the mobile and desktop code paths (each ships its own
 * bundle/endpoints — the classic responsive moat split). getHighEntropyValues(hints) resolves to a concolic
 * detail object. Split out of navigator.c; unbuilt members (toJSON, ...) DFAIL via idl_dfail_wrap. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_UA_DATA_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_UA_DATA_H
#include "quickjs.h"

/* The navigator.userAgentData object (a NavigatorUAData instance). */
JSValue navigator_ua_data_make(JSContext *ctx);

#endif
