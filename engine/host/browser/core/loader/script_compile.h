/* HTML §8.1.4.3 "Creating scripts"' compile of a classic script's source text, held across the rests the parse
 * hands the thread back at — Blink core/script/classic_script.cc's role for a source the caller already holds.
 *
 * A step machine that compiles page-chosen source (an inserted `<script>`, a timer's string handler) owns one
 * of these on its state: the source and base URL ride the state as Strings, and the parse in flight is a cache
 * the machine declares, which a fork sheds (the sibling parses again from the String) and a teardown drops. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_SCRIPT_COMPILE_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_SCRIPT_COMPILE_H
#include <stdbool.h>
#include <stddef.h>
#include "quickjs.h"
#include "quickjs-step.h"

typedef struct {
    JSValue src, base;   /* owned; undefined when no compile is held */
    void   *rec;         /* the parse in flight (a cache over `src` and `base`) */
} ScriptCompile;

void script_compile_init(ScriptCompile *c);
/* Whether a compile begun on `c` has not yet answered. */
bool script_compile_pending(const ScriptCompile *c);
/* Takes copies of `src` (UTF-8, `n` bytes) and the base URL the program is named by. */
void script_compile_begin(JSContext *ctx, ScriptCompile *c, const char *src, size_t n, const char *base);
/* One stint of the parse, as a global program whose closure runs on the caller's trampoline
   (JS_FlowCompileStep's flags; `eval_flags` adds JS_EVAL_FLAG_STRICT, JS_EVAL_FLAG_INLINE_SCRIPT or
   JS_EVAL_FLAG_FUNCTION_CTOR).
   JS_STEP_YIELD while the parse is parked (the caller returns it and is re-entered here), otherwise 0 with
   *prog the closure (owned) or JS_EXCEPTION with the SyntaxError pending; `c` is then released. */
int  script_compile_run(JSContext *ctx, ScriptCompile *c, int eval_flags, JSValue *prog);
void script_compile_visit(JSContext *ctx, ScriptCompile *c, JSStepVisit *v);

#endif
