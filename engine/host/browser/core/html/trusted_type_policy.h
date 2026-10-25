/* Trusted Types §2.3.3 TrustedTypePolicy, with §3.2 Create a Trusted Type and §3.3 Get Trusted Type policy
 * value behind its three create* members. A policy is minted only by the factory's createPolicy
 * (core/html/trusted_type_policy_factory.c). */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPE_POLICY_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPE_POLICY_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/html/trusted_types.h"

void trusted_type_policy_init(JSContext *ctx);
void trusted_type_policy_free(JSRuntime *rt);

/* §3.1 Create a Trusted Type Policy steps 4-6: a new TrustedTypePolicy in ctx's realm whose name is `name` and
   whose options are the converted TrustedTypePolicyOptions dictionary `options` (or undefined for `{}`). Both
   borrowed. OWNED. */
JSValue trusted_type_policy_create(JSContext *ctx, JSValueConst name, JSValueConst options);
bool trusted_type_policy_is(JSValueConst v);
/* §3.3 step 2's "policy's options[functionName]" for `kind`'s function name: the callback, or JS_UNDEFINED
   where the options held none. OWNED. */
JSValue trusted_type_policy_function(JSContext *ctx, JSValueConst policy, TrustedTypeKind kind);

#endif
