/* ABSENT GLOBALS — which unresolved name is unknown INPUT and which is a component this engine owes. */
#ifndef ENGINE_HOST_SOLVER_ABSENT_H
#define ENGINE_HOST_SOLVER_ABSENT_H
#include "quickjs.h"

/* Install as JSConcolicHooks.absent. */
JSValue absent_global_hook(JSContext *ctx, JSAtom name);

/* Every name the web-platform surface installs, declared by the component that installs it. A name on this list
   is the ENGINE's to provide, so its absence is a gap to build and the page's ReferenceError says so. */
void absent_declare_platform(JSContext *ctx, const char *name);

#endif
