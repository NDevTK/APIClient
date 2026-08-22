/* ABSENT GLOBALS — which unresolved name is unknown INPUT and which is a component this engine owes. */
#ifndef ENGINE_HOST_SOLVER_ABSENT_H
#define ENGINE_HOST_SOLVER_ABSENT_H
#include "quickjs.h"

/* Install as JSConcolicHooks.absent. */
JSValue absent_global_hook(JSContext *ctx, JSAtom name);

/* Is this global name one the WEB PLATFORM owns? Answered from the generated Web IDL table (every name
   [Exposed=Window]), not from anything a component or a person declares — see browser/platform_names.h. */
int absent_is_platform_name(const char *name);

#endif
