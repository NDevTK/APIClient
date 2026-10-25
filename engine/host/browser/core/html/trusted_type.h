/* Trusted Types §2.2.1 TrustedHTML, §2.2.2 TrustedScript and §2.2.3 TrustedScriptURL: immutable wrappers over
 * a string, minted only by §3.2 create a trusted type (core/html/trusted_type_policy.c) and by the factory's
 * two empty values. The associated data is an own slot under a private key, so the object carries it from its
 * creation and the page has no way to set it. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPE_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/html/trusted_types.h"

void trusted_type_init(JSContext *ctx);
void trusted_type_free(JSRuntime *rt);

/* The interface's identifier, which is also §3.3's trustedTypeName and §3.7's "expectedType's type name". */
const char *trusted_type_name(TrustedTypeKind kind);
/* The class an instance of `kind`'s interface wears, for a `(TrustedHTML or DOMString)` declaration's brand. */
JSClassID trusted_type_class(TrustedTypeKind kind);

/* A new instance of `kind`'s interface in ctx's realm whose associated data is `data` (§3.2's last step).
   CONSUMES `data`, which is a String or unknown external input. OWNED. */
JSValue trusted_type_create(JSContext *ctx, TrustedTypeKind kind, JSValue data);
/* "input is an instance of expectedType" (§3.5 step 1). An instance has its data by construction. */
bool trusted_type_is(JSValueConst v, TrustedTypeKind kind);
/* The associated data of an instance of `kind` (asserted): its stringification. OWNED. */
JSValue trusted_type_data(JSContext *ctx, JSValueConst v, TrustedTypeKind kind);

#endif
