/* Trusted Types' sink half and its two CSP questions. See trusted_types.c.
 *
 * Every HTML and script sink starts by calling "get trusted type compliant string": HTML §8.5.4's innerHTML
 * setter step 1, §8.5.5's outerHTML setter step 1, §8.5.6's insertAdjacentHTML step 1. A sink that skips it
 * answers differently from a browser for every document carrying a `require-trusted-types-for` policy.
 *
 * Here: §3.5 Get Trusted Type compliant string (what a markup sink calls), §3.9 Get Trusted Type compliant
 * attribute value (what an attribute sink calls), §3.10 Get Trusted Type data for attribute (which attributes
 * are sinks), §4.2.3 Does sink type require trusted types? and §4.2.5 Should Trusted Type policy creation be
 * blocked by Content Security Policy?. The types themselves are core/html/trusted_type.h, the policies
 * core/html/trusted_type_policy.h and the factory core/html/trusted_type_policy_factory.h. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPES_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TRUSTED_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include "quickjs.h"

/* §2.2's three types. The kind is what a sink names; it decides the brand §3.5 step 1 tests and the sink group
   §4.2.3 asks the CSP about, which is "script" for all three (the only group the standard defines). */
typedef enum {
    TRUSTED_TYPE_HTML = 0,     /* TrustedHTML — the HTML sinks */
    TRUSTED_TYPE_SCRIPT,       /* TrustedScript */
    TRUSTED_TYPE_SCRIPT_URL,   /* TrustedScriptURL */
} TrustedTypeKind;

/* §3.5 "get trusted type compliant string" for this realm's global, over the value the sink's
   `(TrustedHTML or DOMString)` union converted: an instance of `expected` answers its data, anything else is a
   String or unknown external input. `sink` is the standard's sink name ("Element innerHTML"), which a violation
   report names. Returns the compliant string OWNED, or JS_EXCEPTION having thrown step 6.3's TypeError. Unknown
   input is returned unchanged: it is the value. Step 4 runs no page code until a default policy carries a
   callback for the type, which is asserted (trusted_types.c says what to build). */
JSValue trusted_types_compliant_string(JSContext *ctx, TrustedTypeKind expected, JSValueConst input,
                                       const char *sink);

/* §4.2.3 "Does sink type require trusted types?" over this global's CSP list. Exposed because
   it is a question about the DOCUMENT rather than about one sink, and because a report that says "sink REAL,
   CSP blocks" has to be able to ask it.
   `include_report_only` is §4.2.3's includeReportOnlyPolicies: true counts a report-only policy that requires
   the type, which §3.5 step 2 asks; false counts enforce policies only, which is the question of whether the
   sink throws. */
bool trusted_types_required(JSContext *ctx, TrustedTypeKind expected, bool include_report_only);

/* §4.2.5 "Should Trusted Type policy creation be blocked by Content Security Policy?" for this global, true for
   "Blocked". `name` is policyName's bytes (`name_len` of them) and `created` is whether the factory's created
   policy names already holds it. */
bool trusted_types_policy_creation_blocked(JSContext *ctx, const char *name, size_t name_len, bool created);

/* §3.10 "Get Trusted Type data for attribute" — WHICH attributes are Trusted Type sinks at all. Answers false
   for the overwhelming majority (§3.9 step 3 then returns the value unchanged); answers true having set
   `*kind` and written the standard's own sink name into `sink` for the four table rows and for every event
   handler content attribute.
   THE ELEMENT IS NAMED BY ITS (namespace, local name), not by an interface pointer: the table's first column
   is an interface, and which interface an element implements is decided by exactly that pair — so the mapping
   from pair to row lives beside the table instead of at the caller, and this file needs to know nothing about
   Lexbor. `element_ns` and `attr_ns` are namespace URLs, NULL for the null namespace. */
bool trusted_types_attribute_data(const char *element_ns, const char *element_local,
                                  const char *attr_ns, const char *attr_local,
                                  TrustedTypeKind *kind, char *sink, size_t sink_cap);

/* §3.9 "Get Trusted Type compliant attribute value" — DOM §4.9 setAttribute step 3 and setAttributeNS step 2.
   Returns the compliant value OWNED (a dup of `value` when §3.10 maps nothing), or JS_EXCEPTION having thrown.
   The empty string for `attr_ns` is the null namespace, which is §3.9 step 1 and not a caller's convention. */
JSValue trusted_types_compliant_attribute_value(JSContext *ctx, const char *element_ns, const char *element_local,
                                                const char *attr_ns, const char *attr_local, JSValueConst value);

/* THE SAME QUESTION OVER A SERIALIZED CSP LIST, which is what makes the directive lookup exercisable with one
   fixture and no document. It is the ONLY entry that parses: a Document's policy container parses its list
   once when the Document is created, and the half above reads that, because this question is asked at every
   HTML and script sink the platform has. `csp_text` may be NULL, which is what "no Content-Security-Policy"
   is. */
bool trusted_types_required_by(const char *csp_text, TrustedTypeKind expected);

#endif
