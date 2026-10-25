/* Trusted Types §2.3.2 TrustedTypePolicyFactory and §4.1.1's `trustedTypes` attribute: one factory per realm,
 * holding the default policy and the created policy names that §3.1 Create a Trusted Type Policy reads and
 * writes. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPE_POLICY_FACTORY_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPE_POLICY_FACTORY_H

#include "quickjs.h"

void trusted_type_policy_factory_init(JSContext *ctx);
void trusted_type_policy_factory_free(JSRuntime *rt);

/* "global's trusted type policy factory's default policy" for ctx's global (§3.7 step 1): the policy, or
   JS_NULL. OWNED. */
JSValue trusted_type_policy_factory_default_policy(JSContext *ctx);

#endif
