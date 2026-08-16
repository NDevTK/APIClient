/* TRUSTED TYPES — the SINK half of the standard. See trusted_types.c.
 *
 * Every HTML and script sink in the platform is DEFINED to start by calling "get trusted type compliant
 * string": HTML §8.5.4's innerHTML setter step 1, §8.5.5's outerHTML setter step 1, §8.5.6's
 * insertAdjacentHTML step 1. Not a Chrome-ism and not optional — it is step 1 of the algorithm, and a sink
 * that skips it answers differently from a browser for every document that carries a
 * `require-trusted-types-for` policy.
 *
 * WHAT IS HERE IS §3.4, §3.7, §3.8 AND §4.2.3 — the algorithm a markup sink calls, the algorithm an ATTRIBUTE
 * sink calls, the table that decides which attributes are sinks at all, and the CSP question all of them turn
 * on. (The section numbers were §4.2 and §4.4 in this file's first draft; the standard numbers "get trusted
 * type compliant string" §3.4 and "Does sink type require trusted types?" §4.2.3, and §4.4 does not exist. A
 * citation that names the wrong section is the same defect as a stage resting at the wrong step.)
 *
 * §2's three TrustedHTML/TrustedScript/TrustedScriptURL objects and §3's TrustedTypePolicyFactory
 * (`window.trustedTypes`) are honestly ABSENT: a page that calls `trustedTypes.createPolicy` gets a
 * ReferenceError, which is the forcing function for building them. Their absence does not make this algorithm
 * approximate. It makes two of its steps DECIDED: nothing can be an instance of the expected type, and there
 * is no default policy — so a document under `require-trusted-types-for 'script'` gets the TypeError §3.4
 * step 6 specifies, which is exactly what a real browser does for a page whose policy creation did not
 * happen. */
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

/* §3.4 "get trusted type compliant string" for THIS document, over the value the IDL union already converted.
   `sink` is the standard's own sink name ("Element innerHTML"), which is what a violation report names and
   what makes two sinks distinguishable in one report.
   Returns the compliant string OWNED, or JS_EXCEPTION having thrown the TypeError step 6 specifies. A CONCOLIC
   input is returned unchanged: it is the value, and stringifying one is what the sink's own solver hooks are
   for. NO PAGE CODE runs in any step this can currently reach — §3's default policy is the one step that does,
   and it does not exist yet — which is why a caller needs no request here and why the stage that calls it must
   exist anyway: that stage is where the machine will rest once the policy callback can run. */
JSValue trusted_types_compliant_string(JSContext *ctx, TrustedTypeKind expected, JSValueConst input,
                                       const char *sink);

/* §4.2.3 "Does sink type require trusted types?" over this document's §7.2.6 policy container. Exposed because
   it is a question about the DOCUMENT rather than about one sink, and because a report that says "sink REAL,
   CSP blocks" has to be able to ask it.
   §3.4 step 2 passes `includeReportOnlyPolicies` = true, and this engine's policy container holds the ENFORCED
   list only — a Content-Security-Policy-Report-Only header is not parsed into it — so the argument has no
   value to distinguish yet and is not taken. The step it would change is §3.4 step 6.1, which returns the
   string instead of throwing when only a report-only policy required the type; that arm becomes reachable at
   the same moment the container learns a second list, and it lands in §3.4 rather than here. */
bool trusted_types_required(JSContext *ctx, TrustedTypeKind expected);

/* §3.8 "Get Trusted Type data for attribute" — WHICH attributes are Trusted Type sinks at all. Answers false
   for the overwhelming majority (§3.7 step 3 then returns the value unchanged); answers true having set
   `*kind` and written the standard's own sink name into `sink` for the four table rows and for every event
   handler content attribute.
   THE ELEMENT IS NAMED BY ITS (namespace, local name), not by an interface pointer: the table's first column
   is an interface, and which interface an element implements is decided by exactly that pair — so the mapping
   from pair to row lives beside the table instead of at the caller, and this file needs to know nothing about
   Lexbor. `element_ns` and `attr_ns` are namespace URLs, NULL for the null namespace. */
bool trusted_types_attribute_data(const char *element_ns, const char *element_local,
                                  const char *attr_ns, const char *attr_local,
                                  TrustedTypeKind *kind, char *sink, size_t sink_cap);

/* §3.7 "Get Trusted Type compliant attribute value" — DOM §4.9 setAttribute step 3 and setAttributeNS step 2.
   Returns the compliant value OWNED (a dup of `value` when §3.8 maps nothing), or JS_EXCEPTION having thrown.
   The empty string for `attr_ns` is the null namespace, which is §3.7 step 1 and not a caller's convention. */
JSValue trusted_types_compliant_attribute_value(JSContext *ctx, const char *element_ns, const char *element_local,
                                                const char *attr_ns, const char *attr_local, JSValueConst value);

/* THE SAME QUESTION OVER A SERIALIZED CSP LIST, which is what makes the directive lookup exercisable with one
   fixture and no document. It is the ONLY entry that parses: a Document's policy container parses its list
   once when the Document is created, and the half above reads that, because this question is asked at every
   HTML and script sink the platform has. `csp_text` may be NULL, which is what "no Content-Security-Policy"
   is. */
bool trusted_types_required_by(const char *csp_text, TrustedTypeKind expected);

#endif
