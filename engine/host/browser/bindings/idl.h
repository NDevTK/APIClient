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

/* Wrap a partially-modeled interface object so any member NOT installed on it DFAILs (dev) naming
   <iface>.<member> as an unbuilt browser feature — the IDL-gap forcing function, never an opaque/undefined
   shrug. Used by every split sub-interface (navigator.clipboard=Clipboard, .credentials=CredentialsContainer, …)
   so each module gets the audit trap without re-implementing it. CONSUMES `obj`. */
JSValue idl_dfail_wrap(JSContext *ctx, JSValue obj, const char *iface);

/* HISTORY + CORRECTED POLICY: the runtime shape-driver (idl_bind + IdlGenMember/IdlImpl) was REMOVED — a
   table-driven driver that installed concolic/noop stubs for unmodelled members is exactly the banned-stub
   anti-pattern. It is NOT replaced by codegen: GENERATING a C binding and filling unimplemented members with a
   noop/DCHECK/concolic default is STILL a stub (CLAUDE.md's corrected policy). So `engine/idlgen.mjs` is a gap
   AUDITOR, never a generator — it DIFFS each component's installed members against the canonical `.idl` and
   prints what is MISSING, so we implement each at the root. idl_define_class / idl_instance are therefore
   PERMANENT helpers (not transitional, not pending codegen, this file is not slated for deletion): they build a
   native EXOTIC-CLASS interface with a private [[slot]] (Blob's [[bytes]], Response's [[body]]) whose members
   are REAL per-member impls, the browser-engineer way to build an interface the spec gives private state. */

#endif
