/* URL + URLSearchParams objects — see urlobj.c. */
#ifndef ENGINE_HOST_URLOBJ_H
#define ENGINE_HOST_URLOBJ_H

#include "quickjs.h"

JSValue js_url_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv);     /* new URL(input, base) */
JSValue js_url_canparse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* URL.canParse */
JSValue js_searchparams_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv);
JSValue js_sp_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);    /* concolic query serializer (shared with FormData) */

/* The fetch request-building objects that share this TU (contiguous with URL/URLSearchParams). */
JSValue js_request_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv);   /* new Request(url, init) */
JSValue js_headers_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv);   /* new Headers(init) */
JSValue js_webobj_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv);    /* misc web-object stubs */

#endif
