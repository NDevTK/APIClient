/* EventTarget — Blink core/dom, the ROOT of the DOM interface inheritance spine. addEventListener/
 * removeEventListener/dispatchEvent live on ONE shared EventTarget.prototype; every DOM interface that "is an
 * EventTarget" (Node -> Document/Element, plus Window, XHR, ...) chains its prototype here, so the listener
 * methods are inherited, not duplicated per interface — the Blink structure the IDL inheritance describes. */
#ifndef ENGINE_HOST_BROWSER_EVENT_TARGET_H
#define ENGINE_HOST_BROWSER_EVENT_TARGET_H
#include "quickjs.h"
void event_target_init(JSContext *ctx, JSValue global);   /* create EventTarget.prototype + window.EventTarget (once, in qjs_init) */
void event_target_free(JSContext *ctx);                   /* drop the prototype singleton (teardown) */
JSValueConst event_target_proto(JSContext *ctx);          /* the shared EventTarget.prototype (BORROWED — do not free) to chain onto */
#endif
