/* Self-hosted Array iteration builtins — see array_iteration.h.
 *
 * WHY self-hosted (not the native C builtins): a C iteration builtin (js_array_every backing
 * forEach/map/reduce) holds its loop counter + accumulator on the C stack between callback calls,
 * so a forced-exec flow CANNOT be preempted mid-iteration — the loop is not bytecode, there is no
 * back-edge opcode to yield at, and the callback runs in a nested non-generator JS_CallInternal.
 * Re-expressing the loop as BYTECODE makes it a heap TrampFrame whose OP_goto back-edge preempts
 * via the deep-preempt path, and the callback dispatches through the already-trampolined OP_call —
 * so iteration suspends/resumes at any depth like every other loop. This is the standard browser
 * approach (V8/Blink self-host builtins); it lives in a host COMPONENT, never as strings in the
 * quickjs fork. Spec-faithful ECMAScript semantics (holes skipped, thisArg, length read once). */
#include "browser/array_iteration.h"
#include "check.h"
#include <string.h>

/* One spec-faithful source per builtin. Loops use `for` (OP_goto back-edge) so each is preemptible. */
static const char *ARRAY_ITER_SRC =
"(function(){"
"  var A = Array.prototype;"
"  A.forEach = function(cb, thisArg) {"
"    var O = this, len = O.length >>> 0;"
"    for (var i = 0; i < len; i++) { if (i in O) cb.call(thisArg, O[i], i, O); }"
"  };"
"  A.map = function(cb, thisArg) {"
"    var O = this, len = O.length >>> 0, out = new Array(len);"
"    for (var i = 0; i < len; i++) { if (i in O) out[i] = cb.call(thisArg, O[i], i, O); }"
"    return out;"
"  };"
"  A.filter = function(cb, thisArg) {"
"    var O = this, len = O.length >>> 0, out = [], k = 0;"
"    for (var i = 0; i < len; i++) { if (i in O) { var v = O[i]; if (cb.call(thisArg, v, i, O)) out[k++] = v; } }"
"    return out;"
"  };"
"  A.reduce = function(cb, init) {"
"    var O = this, len = O.length >>> 0, i = 0, acc;"
"    if (arguments.length > 1) { acc = init; }"
"    else { while (i < len && !(i in O)) i++; if (i >= len) throw new TypeError('Reduce of empty array with no initial value'); acc = O[i++]; }"
"    for (; i < len; i++) { if (i in O) acc = cb(acc, O[i], i, O); }"
"    return acc;"
"  };"
"})();";

void array_iteration_install(JSContext *ctx) {
    JSValue r = JS_Eval(ctx, ARRAY_ITER_SRC, strlen(ARRAY_ITER_SRC), "<array_iteration>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(r), "array_iteration: self-hosted install threw — the source is malformed");
    JS_FreeValue(ctx, r);
}
