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

/* An interface backed by a NATIVE EXOTIC CLASS (an internal [[slot]] the page cannot see): the member table
   becomes the shared PROTOTYPE (methods; opaque attrs become concolic prototype props) and the finalizer frees
   the slot. This is how a spec interface with private state (Blob's [[bytes]], a FileReader's stream) is
   GENERATED from its IDL instead of hand-assembled per instance — the class id is returned so the interface's
   own make() sets the slot via JS_SetOpaque and its per-instance readonly attributes. */
typedef void JSClassFinalizerFn(JSRuntime *rt, JSValue val);
typedef struct { const char *name; const IDLMember *members; int n; JSClassFinalizerFn *finalizer; } IDLInterface;
JSClassID idl_define_class(JSContext *ctx, const IDLInterface *iface);

/* ── GENERATED-SHAPE binding (the Blink model: SHAPE from canonical Web IDL, BEHAVIOR from C) ────────────────
   idl_generated.h (produced by engine/idlgen.mjs from @webref/idl) declares one IdlGenMember[] per interface —
   the spec-exact member list. idl_bind installs it, matching each member to the component's IdlImpl BEHAVIOR by
   name: an operation with no impl is a spec-present noop; a readonly attribute with no getter is the concolic
   unknown (EXISTS + typed, VALUE forks); a writable attribute with no impl is a plain settable property. So the
   member list can never drift from the spec, and a member we haven't modelled is honest, not missing. */
typedef enum { IDL_GEN_OP, IDL_GEN_ATTR } IdlGenKind;
typedef struct { const char *name; IdlGenKind kind; int readonly; int arg; } IdlGenMember;   /* the SHAPE (from IDL) */
/* the BEHAVIOR (from the C component); magic>=0 -> magic-dispatched get/set (one fn serving many by index). */
typedef struct { const char *name; JSCFunction *get; JSCFunction *set; JSCFunction *op; int magic; } IdlImpl;
/* install_attrs: 1 = install operations AND attributes (a small interface fully IDL-driven); 0 = operations
   ONLY (for a big interface like Element whose reflected/magic attributes are still installed separately). */
void idl_bind(JSContext *ctx, JSValueConst target, const IdlGenMember *shape, int shape_n, const IdlImpl *impls, int impl_n, int install_attrs);

#endif
