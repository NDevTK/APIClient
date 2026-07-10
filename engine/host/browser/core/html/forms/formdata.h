/* FormData — Blink core/html/forms/FormData. A real object recording append()/set() fields, so a
 * `fetch(url,{body:fd})` POST surfaces the REAL request params (name=value&…) instead of an opaque body; it
 * shares the concolic query serializer (js_sp_tostring) with URLSearchParams. See formdata.c. */
#ifndef ENGINE_HOST_BROWSER_FORMDATA_H
#define ENGINE_HOST_BROWSER_FORMDATA_H
#include "quickjs.h"
JSValue js_formdata_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new FormData() */
#endif
