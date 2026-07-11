/* MediaDevices (navigator.mediaDevices) — the Media Capture API as a real Blink-style module
 * (modules/mediastream/). enumerateDevices() resolves to a concolic device list whose per-device deviceId/label
 * are permission-gated attacker/unknown sources ({mediaDevices}) — `devices.forEach(d => fetch('/api/dev?id='+
 * d.deviceId))` surfaces a device-parameterized endpoint; getUserMedia()/getDisplayMedia() resolve to a concolic
 * MediaStream; getSupportedConstraints() is concolic so a capability feature-check forks. A page using camera/mic
 * previously aborted on the unbuilt member; now its media-gated code runs. Unbuilt members DFAIL via idl_dfail_wrap. */
#ifndef ENGINE_HOST_BROWSER_MODULES_MEDIASTREAM_MEDIA_DEVICES_H
#define ENGINE_HOST_BROWSER_MODULES_MEDIASTREAM_MEDIA_DEVICES_H
#include "quickjs.h"

/* The navigator.mediaDevices object (a MediaDevices instance). */
JSValue media_devices_make(JSContext *ctx);

#endif
