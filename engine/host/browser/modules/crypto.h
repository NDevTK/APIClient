/* Web Crypto — Blink modules/crypto. window.crypto: getRandomValues / randomUUID / subtle. See crypto.c. */
#ifndef ENGINE_HOST_BROWSER_CRYPTO_H
#define ENGINE_HOST_BROWSER_CRYPTO_H
#include "quickjs.h"
JSValue js_crypto_make(JSContext *ctx);   /* window.crypto */
#endif
