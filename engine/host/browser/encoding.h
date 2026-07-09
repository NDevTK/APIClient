/* Encoding — Blink core/encoding. TextEncoder.encode(str) -> Uint8Array (per IDL); TextDecoder.decode(buf) ->
 * DOMString. Real UTF-8 transforms for concrete input; source-tagged concolic for tainted input (never bare
 * opaque). See encoding.c. */
#ifndef ENGINE_HOST_BROWSER_ENCODING_H
#define ENGINE_HOST_BROWSER_ENCODING_H
#include "quickjs.h"
JSValue js_textencoder_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);
JSValue js_textdecoder_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);
#endif
