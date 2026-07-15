/* Self-hosted Array iteration builtins (forEach/map/filter/reduce) — bytecode, so their loops are
 * preemptible mid-iteration (the native C builtins are not). See array_iteration.c for the why. */
#ifndef ENGINE_HOST_BROWSER_ARRAY_ITERATION_H
#define ENGINE_HOST_BROWSER_ARRAY_ITERATION_H

#include "quickjs.h"

/* Replace Array.prototype.{forEach,map,filter,reduce} with self-hosted bytecode versions. Call once
 * per context at init, before boot runs. */
void array_iteration_install(JSContext *ctx);

#endif
