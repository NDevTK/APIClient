/* TRUSTED TYPES §3.4, §3.7, §3.8 and §4.2.3 — see trusted_types.h for what is here and what is honestly absent.
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
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
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

/* §4.2.3 over the serialized list. CSP §2.2: the policies are enforced INDEPENDENTLY, so trusted types are
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
    DCHECK(sink != NULL, "a Trusted Types sink called §3.4 without naming itself — the sink name is what a "
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

/* THE FOUR NAMESPACES §3.8 NAMES, spelled as the URLs they are. They appear here because §3.8 tests element
   namespaces and one attribute namespace, and nothing else in this file needs to know what a namespace is. */
#define TT_NS_HTML   "http://www.w3.org/1999/xhtml"
#define TT_NS_SVG    "http://www.w3.org/2000/svg"
#define TT_NS_MATHML "http://www.w3.org/1998/Math/MathML"
#define TT_NS_XLINK  "http://www.w3.org/1999/xlink"

/* A NULL namespace and the empty string are the SAME namespace here, because §3.7 step 1 says so for the
   attribute and because a caller reading one off an element gets the empty string where the standard writes
   null. Stated once so no row in the table has to. */
static bool tt_ns_is(const char *ns, const char *want)
{
    if (!ns || !*ns) return want == NULL;
    return want != NULL && strcmp(ns, want) == 0;
}

bool trusted_types_attribute_data(const char *element_ns, const char *element_local,
                                  const char *attr_ns, const char *attr_local,
                                  TrustedTypeKind *kind, char *sink, size_t sink_cap)
{
    /* THE TABLE, as §3.8 writes it: (element interface, attribute namespace, attribute local name) ->
       (Trusted Type, sink name). The interface column is expressed as the (namespace, local name) pair that
       DECIDES it — HTMLIFrameElement is an `iframe` in the HTML namespace and nothing else can be one — so
       the row is checkable against the standard line by line without this file knowing what an interface is. */
    static const struct {
        const char     *el_ns, *el_local, *attr_ns, *attr_local;
        TrustedTypeKind kind;
        const char     *sink;
    } ROWS[] = {
        { TT_NS_HTML, "iframe", NULL,         "srcdoc", TRUSTED_TYPE_HTML,       "HTMLIFrameElement srcdoc" },
        { TT_NS_HTML, "script", NULL,         "src",    TRUSTED_TYPE_SCRIPT_URL, "HTMLScriptElement src" },
        { TT_NS_SVG,  "script", NULL,         "href",   TRUSTED_TYPE_SCRIPT_URL, "SVGScriptElement href" },
        { TT_NS_SVG,  "script", TT_NS_XLINK,  "href",   TRUSTED_TYPE_SCRIPT_URL, "SVGScriptElement href" },
    };
    size_t i;

    DCHECK(kind != NULL && sink != NULL && sink_cap > 0,
           "§3.8 was asked for attribute data with nowhere to put it");
    DCHECK(element_local != NULL && attr_local != NULL,
           "§3.8 was asked about an element or an attribute with no local name");

    /* STEP 1: let data be null. STEP 2: the EVENT HANDLER rule, which comes BEFORE the table and returns
       immediately — an `onclick` on an HTML, SVG or MathML element is a TrustedScript sink whatever the
       element is, and the sink name is built from the attribute's own name. Its namespace must be null: an
       `onclick` in some other namespace is an ordinary attribute. */
    if (tt_ns_is(attr_ns, NULL) &&
        (tt_ns_is(element_ns, TT_NS_HTML) || tt_ns_is(element_ns, TT_NS_SVG) ||
         tt_ns_is(element_ns, TT_NS_MATHML)) &&
        event_target_is_handler_attribute(attr_local)) {
        int n = snprintf(sink, sink_cap, "Element %s", attr_local);
        CHECK(n > 0 && (size_t)n < sink_cap,
              "a Trusted Types sink name did not fit the buffer its caller declared");
        *kind = TRUSTED_TYPE_SCRIPT;
        return true;
    }

    /* STEP 3: find the row. STEP 4: return data. */
    for (i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        if (!tt_ns_is(element_ns, ROWS[i].el_ns) || strcmp(element_local, ROWS[i].el_local) != 0) continue;
        if (!tt_ns_is(attr_ns, ROWS[i].attr_ns) || strcmp(attr_local, ROWS[i].attr_local) != 0) continue;
        {
            int n = snprintf(sink, sink_cap, "%s", ROWS[i].sink);
            CHECK(n > 0 && (size_t)n < sink_cap,
                  "a Trusted Types sink name did not fit the buffer its caller declared");
        }
        *kind = ROWS[i].kind;
        return true;
    }
    return false;
}

JSValue trusted_types_compliant_attribute_value(JSContext *ctx, const char *element_ns, const char *element_local,
                                                const char *attr_ns, const char *attr_local, JSValueConst value)
{
    TrustedTypeKind kind;
    char sink[96];

    /* STEP 1 is inside tt_ns_is: the empty string IS the null namespace. STEP 2: get Trusted Type data for
       attribute. STEP 3: a null answer returns the value as it stands — this is nearly every attribute the
       platform has, and it is why setAttribute is not a Trusted Types sink in general. */
    if (!trusted_types_attribute_data(element_ns, element_local, attr_ns, attr_local, &kind, sink, sizeof(sink)))
        return JS_DupValue(ctx, value);
    /* STEPS 4-6: the mapped type and sink, straight into §3.4 with 'script' as the sink group — which is the
       only group the standard defines, and which trusted_types_required already answers for all three types. */
    return trusted_types_compliant_string(ctx, kind, value, sink);
}
