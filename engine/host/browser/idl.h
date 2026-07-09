/* Web IDL binding driver — a declarative interface member table -> a native object, the way a browser engineer
 * builds Web APIs (Blink generates its bindings from `.idl`). The table IS the interface's IDL and the single
 * spec-traceable source of its SHAPE; it replaces hand-rolled JS_SetPropertyStr-per-method assembly (which
 * drifts from the spec and leaves unlisted members reading undefined). See idl.c. */
#ifndef ENGINE_HOST_BROWSER_IDL_H
#define ENGINE_HOST_BROWSER_IDL_H
#include "quickjs.h"

/* An IDL member: an operation (METHOD) or a readonly attribute whose VALUE is unknown headless (ATTR_OPAQUE ->
   a getter returning the concolic unknown, so the attribute still EXISTS + has its type but a gate on it FORKS). */
typedef enum { IDL_METHOD, IDL_ATTR_OPAQUE } IDLMemberKind;
typedef struct { const char *name; IDLMemberKind kind; JSCFunction *fn; int length; } IDLMember;

JSValue idl_instance(JSContext *ctx, const IDLMember *members, int n);   /* build a native instance from an IDL table */

#endif
