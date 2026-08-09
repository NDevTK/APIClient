/* TRUSTED TYPES — the SINK half of the standard. See trusted_types.c.
 *
 * Every HTML and script sink in the platform is DEFINED to start by calling "get trusted type compliant
 * string": HTML §8.5.4's innerHTML setter step 1, §8.5.5's outerHTML setter step 1, §8.5.6's
 * insertAdjacentHTML step 1. Not a Chrome-ism and not optional — it is step 1 of the algorithm, and a sink
 * that skips it answers differently from a browser for every document that carries a
 * `require-trusted-types-for` policy.
 *
 * WHAT IS HERE IS §4.2 AND §4.4 — the algorithm the sinks call, and the CSP question it turns on. §2's three
 * TrustedHTML/TrustedScript/TrustedScriptURL objects and §3's TrustedTypePolicyFactory (`window.trustedTypes`)
 * are honestly ABSENT: a page that calls `trustedTypes.createPolicy` gets a ReferenceError, which is the
 * forcing function for building them. Their absence does not make this algorithm approximate. It makes two of
 * its steps DECIDED: nothing can be an instance of the expected type, and there is no default policy — so a
 * document under `require-trusted-types-for 'script'` gets the TypeError §4.2 step 6 specifies, which is
 * exactly what a real browser does for a page whose policy creation did not happen. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPES_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPES_H
#include <stdbool.h>
#include "quickjs.h"

/* §2's three types. The KIND is what a sink names, and it decides both the brand step 1 tests and the sink
   GROUP §4.4 asks the CSP about — which is "script" for all three, because that is the only sink group the
   standard defines. */
typedef enum {
    TRUSTED_TYPE_HTML = 0,     /* TrustedHTML — the HTML sinks */
    TRUSTED_TYPE_SCRIPT,       /* TrustedScript */
    TRUSTED_TYPE_SCRIPT_URL,   /* TrustedScriptURL */
} TrustedTypeKind;

/* §4.2 "get trusted type compliant string" for THIS document, over the value the IDL union already converted.
   `sink` is the standard's own sink name ("Element innerHTML"), which is what a violation report names and
   what makes two sinks distinguishable in one report.
   Returns the compliant string OWNED, or JS_EXCEPTION having thrown the TypeError step 6 specifies. A CONCOLIC
   input is returned unchanged: it is the value, and stringifying one is what the sink's own solver hooks are
   for. NO PAGE CODE runs in any step this can currently reach — §3's default policy is the one step that does,
   and it does not exist yet — which is why a caller needs no request here and why the stage that calls it must
   exist anyway: that stage is where the machine will rest once the policy callback can run. */
JSValue trusted_types_compliant_string(JSContext *ctx, TrustedTypeKind expected, JSValueConst input,
                                       const char *sink);

/* §4.4 "Does sink type require trusted types?" over this document's §7.2.6 policy container. Exposed because
   it is a question about the DOCUMENT rather than about one sink, and because a report that says "sink REAL,
   CSP blocks" has to be able to ask it. */
bool trusted_types_required(JSContext *ctx, TrustedTypeKind expected);

#endif
