/* Event / CustomEvent — Blink core/dom/events/Event. `new Event(type, {bubbles,cancelable})` /
 * `new CustomEvent(type, {detail})`. See event.c. */
#ifndef ENGINE_HOST_BROWSER_EVENT_H
#define ENGINE_HOST_BROWSER_EVENT_H
#include "quickjs.h"
JSValue js_event_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);
#endif
