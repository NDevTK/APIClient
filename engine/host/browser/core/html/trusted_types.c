/* TRUSTED TYPES §3.4, §3.7, §3.8 and §4.2.3 — see trusted_types.h for what is here and what is honestly absent.
 *
 * WHY THE SINK CANNOT SKIP THIS. A page under `require-trusted-types-for 'script'` is a page on which
 * `el.innerHTML = someString` THROWS, and every branch after that throw is code a solver reaches only by
 * modelling it. Skipping step 1 does not make the engine permissive, it makes it WRONG in the direction that
 * matters most here: it explores an arm the real page never takes, and it reports an innerHTML sink as
 * exploitable on a document whose policy kills the assignment before the markup is ever parsed. §S says the
 * same thing about CSP for the PoC; this is the same rule one algorithm earlier.
 *
 * THE DIRECTIVE IS ASKED OF CSP'S OWN MODEL, and this file used to carry a SECOND parser instead. It had its
 * own ASCII-whitespace predicate, its own case-insensitive token compare and its own two-level split of the
 * serialized list, standing beside a third scan of the same grammar in core/frame/policy_container.c. Two
 * readings of one grammar is one reading too many: §2.2.1 has a strip rule, an ASCII rule and a
 * FIRST-ONE-WINS duplicate rule, and a scanner written per question implements whichever of them its author
 * remembered — this one implemented none of the three, so a `require-trusted-types-for` repeated in one policy
 * was read LAST-one-wins and a non-ASCII byte anywhere in a directive did not discard it. That parser is
 * deleted; core/frame/csp_directive_list.h is the one parse, and this is a lookup in its result. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/frame/csp_directive_list.h"
#include "core/frame/policy_container.h"
#include "core/html/trusted_types.h"

/* THE SINK GROUP a type belongs to, spelled as the directive's value spells it. The ONLY group the standard
   defines is "script" and all three types belong to it; this is a switch rather than a constant so that a
   second group added to the standard lands here, at the one place that decides it, instead of being a string
   spelled at every sink. It is QUOTED because the value writes a sink group as a quoted keyword and the
   comparison is against the value token as parsed — an unquoted `require-trusted-types-for script` therefore
   names no group at all. */
static const char *tt_sink_group(TrustedTypeKind expected)
{
    switch (expected) {
    case TRUSTED_TYPE_HTML:
    case TRUSTED_TYPE_SCRIPT:
    case TRUSTED_TYPE_SCRIPT_URL:
        return "'script'";
    }
    DFAIL("a sink asked whether trusted types are required for a type the standard does not define");
    return NULL;
}

/* §4.2.3 over a parsed CSP list. The policies are enforced INDEPENDENTLY, so trusted types are required as
   soon as ANY policy requires them — the opposite quantifier from policy_allows over the same list, and for
   the same reason: a second policy can only narrow.
   A NULL list is a document with no Content-Security-Policy, which requires nothing; a directive with NO VALUE
   covers no group, because the value IS the set of groups it covers. */
static bool tt_list_requires(const CspList *list, const char *quoted_group)
{
    size_t i;

    if (!list) return false;
    for (i = 0; i < list->n_policies; i++) {
        const CspDirective *d = csp_policy_directive(&list->policies[i], "require-trusted-types-for");
        size_t j;
        if (!d) continue;
        for (j = 0; j < d->n_value; j++)
            if (csp_token_is(d->value[j], quoted_group)) return true;
    }
    return false;
}

bool trusted_types_required(JSContext *ctx, TrustedTypeKind expected)
{
    /* THE DOCUMENT PATH NEVER RE-PARSES. This question is asked at every HTML and script sink the platform
       has, so it reads the list the container parsed once when the Document was created rather than the text
       it also keeps. Only the fixture entry below has bytes and no container. */
    return tt_list_requires(policy_container_csp_list(document_policy(ctx)), tt_sink_group(expected));
}

bool trusted_types_required_by(const char *csp_text, TrustedTypeKind expected)
{
    CspList list;
    bool required;

    if (!csp_text) return false;
    memset(&list, 0, sizeof list);
    /* NO SELF-ORIGIN: `require-trusted-types-for` names SINK GROUPS, not URLs, so this list is never asked to
       match one and there is no `'self'` here to resolve. §6.7.2.7 asserts the origin is present, which is
       what keeps that absence a statement rather than a hole. */
    csp_list_parse(&list, csp_text, strlen(csp_text), NULL);
    required = tt_list_requires(&list, tt_sink_group(expected));
    csp_list_free(&list);
    return required;
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
