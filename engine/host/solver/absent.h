/* ABSENT STATE — which read of a name the page cannot resolve is unknown INPUT, and which is a component this
 * engine owes. Two bases ask it: the GLOBAL OBJECT, and a RECORD THE DOCUMENT PUBLISHED onto it. */
#ifndef ENGINE_HOST_SOLVER_ABSENT_H
#define ENGINE_HOST_SOLVER_ABSENT_H
#include "quickjs.h"

/* Install as JSConcolicHooks.absent. `obj` is the base the read missed on — the global object, or a published
   record (the engine has already established it is one of the two). */
JSValue absent_read_hook(JSContext *ctx, JSValueConst obj, JSAtom name);

/* Install as JSConcolicHooks.present — THE SAME QUESTION FOR A MEMBER THE RECORD HOLDS. A published record's
   extent was chosen against THIS visitor's credentials, so `__FLAGS.admin === false` is a fact about this
   session and not about the program: it is unknown for control flow exactly as a missing member is, and it
   additionally KNOWS what it concretely is. So this mints the same derivation the read hook does, with the
   held value as the EXAMPLE — which is the whole of §solver's "a loaded `features.admin:false` must NOT
   concretize the gate" in one call. `holder` is the record the slot was found on and `value` is borrowed. */
JSValue absent_present_hook(JSContext *ctx, JSValueConst holder, JSAtom name, JSValueConst value);

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
