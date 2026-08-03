/* THE WEB IDL ARGUMENT COERCION, as one machine every member shares — see idl_args.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#define ENGINE_HOST_BROWSER_CORE_IDL_ARGS_H
#include "quickjs.h"

/* A member's body, run once its declared arguments are real strings. Same shape as JS_CFUNC_generic_magic, so
   an existing body becomes one by taking a magic it may ignore. */
typedef JSValue (*IdlBody)(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

/* DECLARE a member: which of its arguments the spec coerces (bit i = argv[i], IDL_ALL_STRINGS for a variadic
   run of them) and the body to run once they are strings. Returns the step id, which the caller CACHES.
   Registration and installation are separate on purpose: Element's members are installed on every wrapper
   object the tree hands out, so registering there would mint a definition per element. Declare once, install
   as often as there are objects. */
#define IDL_ALL_STRINGS 0xFFFFFFFFu
int  idl_string_method_id(JSContext *ctx, uint32_t strmask, IdlBody body, int magic);

/* Install a declared member on `target`. The coercion is a request, so a page's `toString` — loop, await and
   all — suspends and resumes at the exact argument it was on. */
void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid);

#endif
