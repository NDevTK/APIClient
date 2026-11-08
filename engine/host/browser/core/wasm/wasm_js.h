/* The WebAssembly JavaScript Interface's machines over the core (module_record.h, instance.h, interpreter.h),
 * each a host step machine (quickjs-step.h) that rests at every step of the core's own machines, charged as
 * engine work, and forks where the core leaves an outcome to the environment (§4.7.1's growtable and growmem).
 * Refusals and traps throw §5.10's CompileError, LinkError and RuntimeError, the realm's (quickjs.h).
 * Named residual. Not covered: the WebAssembly namespace and its interface objects; a module with imports (JS
 * API §5 "read the imports"); a host call from the start function. Next diff builds: the namespace with "read
 * the imports". Absence shows as: `typeof WebAssembly` is "undefined"; a module with an import throws a
 * TypeError. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_WASM_JS_H
#define ENGINE_HOST_BROWSER_CORE_WASM_WASM_JS_H

#include "quickjs.h"

/* The machines' registration, once per agent (core/platform.c's `wasm_js` row), and its release. */
void wasm_js_init(JSContext *ctx);
void wasm_js_free(void);
/* A callable of one argument, source bytes (a Uint8Array): JS API §5.1's `new Module(bytes)` and then §5.2's
   `new Instance(module)` with no import object, into the store `store_owner` owns (store.h), answering the new
   instance's store address. */
JSValue wasm_js_instantiate_closure(JSContext *ctx, JSValueConst store_owner);

#endif
