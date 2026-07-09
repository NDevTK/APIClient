/* Web Crypto — see crypto.h. Extracted from main.c. A MISSING crypto was a ReferenceError that killed every
 * bundle minting a token/UUID/id. getRandomValues(arr) FILLS + RETURNS the same typed array per spec (the bytes
 * are external randomness, so return the array as-is — its elements read opaque — and the fill-then-read idiom
 * works without throwing); randomUUID() is external randomness -> opaque (forks branches, shapes token/id URLs,
 * replay-sound); subtle is opaque (SubtleCrypto digest/encrypt results are externally-derived). */
#include "crypto.h"
#include "opaque.h"   /* g_opaque, js_opaque */

static JSValue js_crypto_getrandom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_DupValue(ctx, g_opaque);
}

JSValue js_crypto_make(JSContext *ctx) {
    JSValue cr = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cr, "randomUUID", JS_NewCFunction(ctx, js_opaque, "randomUUID", 0));
    JS_SetPropertyStr(ctx, cr, "getRandomValues", JS_NewCFunction(ctx, js_crypto_getrandom, "getRandomValues", 1));
    JS_SetPropertyStr(ctx, cr, "subtle", JS_DupValue(ctx, g_opaque));
    return cr;
}
