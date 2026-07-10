/* XMLHttpRequest emulation — open/send/setRequestHeader capture the method/url/headers/body and funnel the
 * request through the shared @H sink (record_endpoint), so XHR-based apps are learned exactly like fetch ones.
 * The response is opaque (external input). Borrows the @H recording helpers from main.c (the scheduler side). */
#ifndef ENGINE_HOST_XHR_H
#define ENGINE_HOST_XHR_H

#include "quickjs.h"

/* `new XMLHttpRequest()` -> an object with open/send/setRequestHeader/... wired to the @H recorder. */
JSValue js_xhr_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv);

#endif
