/* TRUSTED TYPES §4.2 and §4.4 — see trusted_types.h for what is here and what is honestly absent.
 *
 * WHY THE SINK CANNOT SKIP THIS. A page under `require-trusted-types-for 'script'` is a page on which
 * `el.innerHTML = someString` THROWS, and every branch after that throw is code a solver reaches only by
 * modelling it. Skipping step 1 does not make the engine permissive, it makes it WRONG in the direction that
 * matters most here: it explores an arm the real page never takes, and it reports an innerHTML sink as
 * exploitable on a document whose policy kills the assignment before the markup is ever parsed. §S says the
 * same thing about CSP for the PoC; this is the same rule one algorithm earlier.
 *
 * THE DIRECTIVE IS READ OFF THE SERIALIZED CSP LIST, not off a parsed directive set, because the policy
 * container exports its list as text and its parse models the four SCRIPT questions a breakout turns on —
 * `require-trusted-types-for` is a fifth, and the parser that answers it belongs beside those four in
 * core/frame/policy_container.c the moment that file gains a directive this component can ask for. Reading the
 * serialized list here is the same grammar over the same bytes (CSP §2.2: policies comma-delimited, directives
 * `;`-delimited within a policy, a directive is a name followed by ASCII-whitespace-separated values), so the
 * answer is the standard's, not an approximation of it. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/frame/policy_container.h"
#include "core/html/trusted_types.h"

/* ASCII whitespace, as CSP §2.2 splits on it. */
static bool tt_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* An ASCII-case-insensitive token compare — a CSP directive name is matched case-insensitively. */
static bool tt_token_is(const char *tok, size_t n, const char *name)
{
    size_t k = strlen(name), i;

    if (n != k) return false;
    for (i = 0; i < n; i++) {
        char a = tok[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != name[i]) return false;
    }
    return true;
}

/* Does ONE `;`-delimited directive of one policy require trusted types for `group`? The directive is
   `require-trusted-types-for` and its value is a list of sink groups, each written as a quoted keyword —
   `require-trusted-types-for 'script'`. A directive with no value requires nothing, which is what the grammar
   says and not a lenient reading: the value IS the set of groups it covers. */
static bool tt_directive_requires(const char *d, size_t len, const char *group)
{
    size_t i = 0, start;
    bool named = false;

    while (i < len && tt_space(d[i])) i++;
    start = i;
    while (i < len && !tt_space(d[i])) i++;
    if (!tt_token_is(d + start, i - start, "require-trusted-types-for")) return false;
    while (i < len) {
        while (i < len && tt_space(d[i])) i++;
        start = i;
        while (i < len && !tt_space(d[i])) i++;
        if (i > start) {
            const char *v = d + start;
            size_t n = i - start;
            /* The grammar writes a sink group as a quoted keyword; the quotes are part of it, so they are
               stripped here rather than matched, and an UNQUOTED value is not the keyword. */
            if (n >= 2 && v[0] == '\'' && v[n - 1] == '\'' && tt_token_is(v + 1, n - 2, group))
                named = true;
        }
    }
    return named;
}

/* §4.4 over the serialized list. CSP §2.2: the policies are enforced INDEPENDENTLY, so trusted types are
   required as soon as ANY policy requires them — the opposite quantifier from policy_allows, and for the same
   reason: a second policy can only narrow. */
static bool tt_list_requires(const char *csp, const char *group)
{
    size_t i = 0, n;

    if (!csp) return false;
    n = strlen(csp);
    while (i < n) {
        size_t pol_end = i;
        while (pol_end < n && csp[pol_end] != ',') pol_end++;
        {
            size_t j = i;
            while (j < pol_end) {
                size_t d_end = j;
                while (d_end < pol_end && csp[d_end] != ';') d_end++;
                if (tt_directive_requires(csp + j, d_end - j, group)) return true;
                j = d_end + 1;
            }
        }
        i = pol_end + 1;
    }
    return false;
}

bool trusted_types_required(JSContext *ctx, TrustedTypeKind expected)
{
    return trusted_types_required_by(policy_container_csp(document_policy(ctx)), expected);
}

bool trusted_types_required_by(const char *csp_text, TrustedTypeKind expected)
{
    /* The ONLY sink group the standard defines is "script", and all three types belong to it. This is a switch
       rather than a constant so that a second group added to the standard lands here, at the one place that
       decides it, instead of being a string spelled at every sink. */
    const char *group;

    switch (expected) {
    case TRUSTED_TYPE_HTML:
    case TRUSTED_TYPE_SCRIPT:
    case TRUSTED_TYPE_SCRIPT_URL:
        group = "script";
        break;
    default:
        DFAIL("a sink asked whether trusted types are required for a type the standard does not define");
        return false;
    }
    return tt_list_requires(csp_text, group);
}

JSValue trusted_types_compliant_string(JSContext *ctx, TrustedTypeKind expected, JSValueConst input,
                                       const char *sink)
{
    DCHECK(sink != NULL, "a Trusted Types sink called §4.2 without naming itself — the sink name is what a "
                         "violation report identifies, and two sinks that report the same name are one sink "
                         "to whoever reads it");
    /* STEP 1: "if input is an instance of expectedType, return its stringification." §2's three types do not
       exist in this engine, so NO value can be an instance of one — the step is decided, not skipped, and it
       becomes a brand test the moment §2 lands. */

    /* STEPS 2-3: a document that requires no trusted types at this sink group gets the value it was given.
       This is the overwhelmingly common case and it is what "no Content-Security-Policy" means. */
    if (!trusted_types_required(ctx, expected))
        return JS_DupValue(ctx, input);

    /* STEP 4: "process value with a default policy", whose step 1 is the factory's default policy — §3, which
       does not exist, so there is no default policy and the algorithm returns null. STEP 6: a null
       convertedInput is a violation report and then a TypeError. That is not this engine failing to model
       something: it is what a real browser does for a page that enforces trusted types and never created a
       default policy, and it is the throw such a page's own code is written to expect. */
    return JS_ThrowTypeError(ctx, "this document's Content-Security-Policy requires a Trusted Type at the %s "
                                  "sink and no default policy converted the value", sink);
}
