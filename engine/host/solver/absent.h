/* ABSENT STATE — which read of a name the page cannot resolve is unknown INPUT, and which is a component this
 * engine owes. Two bases ask it: the GLOBAL OBJECT, and a RECORD THE DOCUMENT PUBLISHED onto it. */
#ifndef ENGINE_HOST_SOLVER_ABSENT_H
#define ENGINE_HOST_SOLVER_ABSENT_H
#include "quickjs.h"

/* Install as JSConcolicHooks.absent. `obj` is the base the read missed on — the global object, or a published
   record (the engine has already established it is one of the two). */
JSValue absent_read_hook(JSContext *ctx, JSValueConst obj, JSAtom name);

/* Install as JSConcolicHooks.publish. The engine has found `value` reachable from the global object through
   records the document's own inline scripts built, and states the PARENT it hangs off and the NAME it hangs
   under; this files the PATH the record's members are then read by. Parents arrive before their children, so
   a child's path is composed from a parent already filed — a child whose parent is not filed is the engine
   and this file disagreeing about what is published, which crashes rather than inventing a root. */
void absent_publish_hook(JSContext *ctx, JSValueConst parent, JSAtom name, JSValueConst value);

/* Give back the published-namespace registry. Called on the agent's release column, beside the source
   registry: a path names a record of a document that is gone. */
void absent_free(void);

/* Is this global name one the WEB PLATFORM owns? Answered from the generated Web IDL table (every name
   [Exposed=Window]), not from anything a component or a person declares — see browser/platform_names.h. */
int absent_is_platform_name(const char *name);

#endif
