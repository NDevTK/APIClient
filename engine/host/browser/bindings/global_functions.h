#ifndef ENGINE_HOST_BROWSER_BINDINGS_GLOBAL_FUNCTIONS_H
#define ENGINE_HOST_BROWSER_BINDINGS_GLOBAL_FUNCTIONS_H
#include "quickjs.h"
/* Global-scope JS function bindings the engine WRAPS to feed the scheduler (Blink bindings/core/v8): `eval` and
 * `new Function` are code-execution @S sinks — an opaque/candidate body is RECORDED via solve_add, never run
 * blind; `structuredClone` is identity for forced-exec. install_ wires all three onto the global (capturing the
 * real Function so `x instanceof Function` holds and a non-attacker body still compiles). Pure browser/binding
 * edges: they call into the solver, they hold no scheduler control flow. */
void install_js_global_functions(JSContext *ctx, JSValueConst global);
void js_global_functions_free(JSContext *ctx);
#endif
