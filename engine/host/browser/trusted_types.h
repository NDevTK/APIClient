/* Trusted Types — Blink core/trustedtypes (WICG Trusted Types). window.trustedTypes is a
 * TrustedTypePolicyFactory; trustedTypes.createPolicy(name, {createHTML,…}) RUNS to create a TrustedTypePolicy
 * whose createHTML wraps the page's own function into a TrustedHTML. Because the engine EXECUTES the bundle,
 * the @S Trusted-Types analysis observes the REAL reachable policies (is there a 'default' policy that
 * auto-applies to every sink? what does its createHTML do to a payload?) instead of regex-scanning minified JS.
 * See trusted_types.c. */
#ifndef ENGINE_HOST_BROWSER_TRUSTED_TYPES_H
#define ENGINE_HOST_BROWSER_TRUSTED_TYPES_H
#include "quickjs.h"
void trusted_types_init(JSContext *ctx);            /* register the TrustedTypePolicy class (qjs_init) */
JSValue js_trusted_types_make(JSContext *ctx);      /* window.trustedTypes = the TrustedTypePolicyFactory */
void tt_reset(void);                                /* per-document: clear observed-policy state */
int tt_default_exists(void);                        /* @S: a 'default' policy was created (auto-applies to every sink) */
int tt_any_policy(void);                            /* @S: any policy was created (a reachable createHTML exists) */
#endif
