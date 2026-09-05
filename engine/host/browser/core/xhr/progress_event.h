/* ProgressEvent — XMLHttpRequest Standard §5. See progress_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_XHR_PROGRESS_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_XHR_PROGRESS_EVENT_H

#include "quickjs.h"

void progress_event_init(JSContext *ctx);
/* XHR §5's prototype, its Web IDL §3.7.1 interface object and the §3.8 property reference for it — for ONE
   realm, declared into core/realm.h's list and run exactly once per realm. ONE entry because Web IDL §3.8
   `define the global property references` is given "target" and "a realm realm" and its step 1 population is
   "every interface that is exposed in realm": no Document appears in it. XHR §5 declares
   `[Exposed=(Window,Worker)]`, so a worker realm is owed the name — and while the interface object was
   installed from core/platform.c's per-document column, reached from `xhr_install` because §5 has no row of
   its own, a worker realm reaches no platform_document_install and received neither. */
void progress_event_install_realm(JSContext *ctx);
void progress_event_free(JSRuntime *rt);

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
