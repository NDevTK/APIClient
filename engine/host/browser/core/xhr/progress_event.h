/* ProgressEvent — XMLHttpRequest Standard §5. See progress_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_XHR_PROGRESS_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_XHR_PROGRESS_EVENT_H

#include "quickjs.h"

void progress_event_init(JSContext *ctx);
/* §5's PROTOTYPE FOR ONE REALM — run where a realm's other intrinsics are added, exactly once per realm. */
void progress_event_install_proto(JSContext *ctx);
void progress_event_install(JSContext *ctx, JSValueConst global);
void progress_event_free(JSContext *ctx);

/* `ProgressEvent.prototype`, OWNED. Per realm, for the reason event.h gives: a C member runs in the realm that
   DEFINED it, so one shared prototype answers every document out of whichever realm built it first. */
JSValue progress_event_proto(JSContext *ctx);

/* §5.1 "fire a progress event named `type` at target, given transmitted and length" — the MINT half.
   The DISPATCH half is the caller's, and deliberately: §2.9 runs the page's listeners, so it belongs to a step
   machine that can suspend at each of them (event_target_fire_run), and a helper that dispatched from here
   would be a JS_Call out of C with no flow base under it.
   §5.1 states the mapping exactly: `loaded` is transmitted; a length of 0 leaves `lengthComputable` false and
   `total` 0, and any other length sets both. The event does not bubble, is not cancelable, and IS trusted. */
JSValue progress_event_new(JSContext *ctx, const char *type, double transmitted, double length);

#endif
