/* Trusted Types §3.5 Get Trusted Type compliant string, §3.7 Process value with a default policy, §3.9 Get
 * Trusted Type compliant attribute value, §3.10 Get Trusted Type data for attribute, and the two CSP questions
 * §4.2.3 Does sink type require trusted types? and §4.2.5 Should Trusted Type policy creation be blocked by
 * Content Security Policy? — see trusted_types.h.
 *
 * A page under `require-trusted-types-for 'script'` throws at `el.innerHTML = someString`, and every branch
 * after that throw is reached only by modelling it: skipping the check explores an arm the real page never
 * takes and reports a markup sink as exploitable where the policy kills the assignment before any parse.
 *
 * Both directives are looked up in the list core/frame/csp_directive_list.h parsed (§2.2.1's strip, ASCII and
 * first-one-wins rules live there once); nothing here re-parses a policy.
 *
 * NAMED RESIDUAL. Not covered: §4.2.4's and §4.2.5's "Report a violation" — the decision is made, the
 * violation is never reported. Next diff: a Trusted Types entry in core/frame/csp_violation.c taking the
 * unconditional 40-character sample both algorithms set (not the 'report-sample'-gated one), called from both
 * walks. How its absence shows: a `securitypolicyviolation` listener never fires for a refused sink or policy
 * name, so a report-only policy's refusals are invisible to the page. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/frame/csp_directive_list.h"
#include "core/frame/policy_container.h"
#include "core/realm.h"
#include "core/html/trusted_type.h"
#include "core/html/trusted_type_policy.h"
#include "core/html/trusted_type_policy_factory.h"
#include "core/html/trusted_types.h"
#include "solver/concolic.h"

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
   soon as ANY policy requires them — the opposite quantifier from policy_allows_inline over the same list, for
   the same reason: a second policy can only narrow.
   A NULL list is a document with no Content-Security-Policy, which requires nothing; a directive with NO VALUE
   covers no group, because the value IS the set of groups it covers. */
static bool tt_list_requires(const CspList *list, const char *quoted_group, bool include_report_only)
{
    size_t i;

    if (!list) return false;
    for (i = 0; i < list->n_policies; i++) {
        const CspDirective *d = csp_policy_directive(&list->policies[i], "require-trusted-types-for");
        size_t j;
        if (!d) continue;
        for (j = 0; j < d->n_value; j++)
            if (csp_token_is(d->value[j], quoted_group)) {
                /* "Let enforced be true if policy's disposition is "enforce" … If enforced is true, return
                   true. If includeReportOnlyPolicies is true, return true." */
                if (csp_policy_is_enforced(&list->policies[i]) || include_report_only) return true;
                break;
            }
    }
    return false;
}

/* The global's CSP list: a Window realm's is its document's policy container's (built once with the
   Document, so this question, asked at every sink, never re-parses), and a worker realm's is its environment
   record's (core/dom/document.h, which states what that container does not yet cover). */
static const CspList *tt_global_csp_list(JSContext *ctx)
{
    return policy_container_csp_list(document_policy(ctx));
}

bool trusted_types_required(JSContext *ctx, TrustedTypeKind expected, bool include_report_only)
{
    return tt_list_requires(tt_global_csp_list(ctx), tt_sink_group(expected), include_report_only);
}

/* Whether a directive value holds a tt-policy-name token equal to `name`. Names compare exactly: §4.2.2's
   grammar gives them no case folding, unlike the quoted keywords csp_token_is matches. */
static bool tt_value_names(const CspDirective *d, const char *name, size_t name_len)
{
    size_t j;

    for (j = 0; j < d->n_value; j++)
        if (d->value[j].n == name_len && memcmp(d->value[j].p, name, name_len) == 0) return true;
    return false;
}

static bool tt_value_has_keyword(const CspDirective *d, const char *keyword)
{
    size_t j;

    for (j = 0; j < d->n_value; j++)
        if (csp_token_is(d->value[j], keyword)) return true;
    return false;
}

bool trusted_types_policy_creation_blocked(JSContext *ctx, const char *name, size_t name_len, bool created)
{
    const CspList *list = tt_global_csp_list(ctx);
    size_t i;

    DCHECK(name != NULL, "§4.2.5 was asked about a policy with no name");
    if (!list) return false;
    for (i = 0; i < list->n_policies; i++) {
        const CspDirective *d = csp_policy_directive(&list->policies[i], "trusted-types");
        bool create_violation = false;

        if (!d) continue;                                                       /* step 2.2 */
        if (d->n_value == 1 && csp_token_is(d->value[0], "'none'"))             /* step 2.4 */
            create_violation = true;
        if (created && !tt_value_has_keyword(d, "'allow-duplicates'"))          /* step 2.5 */
            create_violation = true;
        if (!tt_value_names(d, name, name_len) && !tt_value_has_keyword(d, "*")) /* step 2.6 */
            create_violation = true;
        if (create_violation && csp_policy_is_enforced(&list->policies[i]))     /* steps 2.7-2.12 */
            return true;
    }
    return false;
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
    /* The fixture's text is a `Content-Security-Policy` header, so its policies enforce. */
    csp_list_parse(&list, csp_text, strlen(csp_text), NULL, CSP_DISPOSITION_ENFORCE);
    required = tt_list_requires(&list, tt_sink_group(expected), false);
    csp_list_free(&list);
    return required;
}

/* §3.5 steps 1-3: true when they decide the answer, with *pout the compliant string (owned); false when step 4
   runs. */
static bool tt_compliant_decided(JSContext *ctx, TrustedTypeKind expected, JSValueConst input, const char *sink,
                                 JSValue *pout)
{
    DCHECK(sink != NULL, "a Trusted Types sink called §3.5 without naming itself, which is what a violation "
                         "report identifies");
    if (trusted_type_is(input, expected)) {                                     /* step 1 */
        *pout = trusted_type_data(ctx, input, expected);
        return true;
    }
    DCHECK(JS_IsString(input) || concolic_is(input),
           "a Trusted Types sink handed §3.5 a value that is neither the expected type nor a String — its "
           "declaration's union converts every other value to a DOMString first");
    if (!trusted_types_required(ctx, expected, /*include_report_only*/ true)) { /* steps 2-3 */
        *pout = JS_DupValue(ctx, input);
        return true;
    }
    return false;
}

/* §3.7 steps 1-2 as far as the callback: §3.3 Get Trusted Type policy value with throwIfMissing false answers
   null without calling anything when there is no default policy or it holds no function for the type. The
   function (owned), or JS_UNDEFINED for that null. */
static JSValue tt_default_policy_function(JSContext *ctx, TrustedTypeKind expected)
{
    JSValue policy = trusted_type_policy_factory_default_policy(ctx), fn = JS_UNDEFINED;

    if (!JS_IsNull(policy))
        fn = trusted_type_policy_function(ctx, policy, expected);
    JS_FreeValue(ctx, policy);
    return fn;
}

/* §3.5 step 6, after §3.7 answered null or undefined: §4.2.4 (step 6.1) answers "Allowed" unless an enforce
   policy requires the type, and step 6.2 then returns the stringified input; otherwise step 6.3 throws. */
static JSValue tt_compliant_unconverted(JSContext *ctx, TrustedTypeKind expected, JSValueConst input,
                                        const char *sink)
{
    if (!trusted_types_required(ctx, expected, /*include_report_only*/ false))
        return JS_DupValue(ctx, input);
    return JS_ThrowTypeError(ctx, "this document's Content-Security-Policy requires a %s at the %s sink and "
                                  "no default policy converted the value", trusted_type_name(expected), sink);
}

JSValue trusted_types_compliant_string(JSContext *ctx, TrustedTypeKind expected, JSValueConst input,
                                       const char *sink)
{
    JSValue out, fn;

    if (tt_compliant_decided(ctx, expected, input, sink, &out))
        return out;
    fn = tt_default_policy_function(ctx, expected);
    if (!JS_IsUndefined(fn))
        DFAIL("Trusted Types §3.7 reached a default policy whose callback must run, which a plain C call "
              "cannot do: make the sink a step machine and call trusted_types_compliant_string_run at its "
              "Trusted Types stage");
    JS_FreeValue(ctx, fn);
    return tt_compliant_unconverted(ctx, expected, input, sink);
}

void trusted_types_compliant_run_init(TrustedTypesCompliantRun *r)
{
    int i;

    r->stage = 0;
    r->phase = 0;
    r->call = JS_UNDEFINED;
    r->value = JS_UNDEFINED;
    STEP_CB_FOREACH(r->cb, i) r->cb[i] = JS_UNDEFINED;
}

void trusted_types_compliant_run_visit(JSContext *ctx, TrustedTypesCompliantRun *r, JSStepVisit *v)
{
    int i;

    v->val(ctx, &r->call);
    v->val(ctx, &r->value);
    STEP_CB_FOREACH(r->cb, i) v->val(ctx, &r->cb[i]);
}

enum { TTC_BEGIN = 0, TTC_CALL, TTC_STRING };

int trusted_types_compliant_string_run(JSContext *ctx, JSStepHdr *hdr, TrustedTypesCompliantRun *r,
                                       TrustedTypeKind expected, JSValueConst input, const char *sink,
                                       JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    JSValue out;
    int c;

    if (r->stage == TTC_BEGIN) {
        JSValue fn, args[3];

        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        if (tt_compliant_decided(ctx, expected, input, sink, pout))
            return 0;
        fn = tt_default_policy_function(ctx, expected);
        if (JS_IsUndefined(fn)) {
            *pout = tt_compliant_unconverted(ctx, expected, input, sink);
            return JS_IsException(*pout) ? -1 : 0;
        }
        /* §3.7 step 2: §3.3 with the stringified input as value and « trustedTypeName, sink » as arguments,
           whose step 7 invokes the function with « value » followed by them, and "rethrow". */
        args[0] = JS_DupValue(ctx, input);
        args[1] = JS_NewString(ctx, trusted_type_name(expected));
        args[2] = JS_NewString(ctx, sink);
        CHECK(!JS_IsException(args[1]) && !JS_IsException(args[2]),
              "a default policy's callback arguments could not be allocated");
        r->call = JS_NewBoundCall(ctx, fn, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, fn);
        for (c = 0; c < 3; c++) JS_FreeValue(ctx, args[c]);
        CHECK(!JS_IsException(r->call), "a default policy's callback call could not be allocated");
        r->stage = TTC_CALL;
    }
    if (r->stage == TTC_CALL) {
        c = step_call_run(ctx, &r->phase, STEP_CB(r->cb), r->call, JS_UNDEFINED, 0, NULL, in, &out,
                          out_cb, out_argc);
        if (c > 0)
            return c;
        in = JS_UNDEFINED;
        JS_FreeValue(ctx, r->call);
        r->call = JS_UNDEFINED;
        if (JS_IsException(out))
            return -1;                                                          /* §3.7 step 3, §3.5 step 5 */
        if (JS_IsNull(out) || JS_IsUndefined(out)) {                            /* §3.7 step 4 */
            *pout = tt_compliant_unconverted(ctx, expected, input, sink);
            return JS_IsException(*pout) ? -1 : 0;
        }
        r->value = out;
        r->stage = TTC_STRING;
    }
    /* §3.7 step 5's dataString: the callback's `DOMString?` (`USVString?` for a script URL) result converted
       by Web IDL §3.2, unknown external input crossing as itself. Step 6's new instance carries it, and §3.5
       step 7 returns that instance stringified, so the instance is never observed and is not made. */
    DCHECK(r->stage == TTC_STRING, "a Trusted Types §3.5 request resumed at a stage it does not have");
    if (concolic_is(r->value)) {
        JS_FreeValue(ctx, in);
        out = JS_DupValue(ctx, r->value);
    } else {
        c = step_tostring_run(ctx, hdr, r->value, in, &out, out_cb, out_argc);
        if (c > 0)
            return c;
        if (c < 0)
            return -1;
        if (expected == TRUSTED_TYPE_SCRIPT_URL) {
            out = JS_ToScalarValueString(ctx, out);
            if (JS_IsException(out))
                return -1;
        }
    }
    JS_FreeValue(ctx, r->value);
    r->value = JS_UNDEFINED;
    *pout = out;
    return 0;
}

/* THE FOUR NAMESPACES §3.10 NAMES, spelled as the URLs they are. They appear here because §3.10 tests element
   namespaces and one attribute namespace, and nothing else in this file needs to know what a namespace is. */
#define TT_NS_HTML   "http://www.w3.org/1999/xhtml"
#define TT_NS_SVG    "http://www.w3.org/2000/svg"
#define TT_NS_MATHML "http://www.w3.org/1998/Math/MathML"
#define TT_NS_XLINK  "http://www.w3.org/1999/xlink"

/* A NULL namespace and the empty string are the SAME namespace here, because §3.9 step 1 says so for the
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
    /* THE TABLE, as §3.10 writes it: (element interface, attribute namespace, attribute local name) ->
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
           "§3.10 was asked for attribute data with nowhere to put it");
    DCHECK(element_local != NULL && attr_local != NULL,
           "§3.10 was asked about an element or an attribute with no local name");

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

/* NAMED RESIDUAL. Not covered: the attribute and script sinks (setAttribute, setAttributeNS, setAttributeNode,
   the reflected `srcdoc` and script `src`, script text) declare a DOMString, so a policy-made value is
   stringified before §3.9 sees it and §3.5 step 1 never matches; only the markup sinks declare the union. Next
   diff: their `(TrustedType or DOMString)` and `(TrustedScriptURL or USVString)` unions, with §3.9 step 3.3's
   unwrap. How its absence shows: under an enforcing `require-trusted-types-for 'script'`, an attribute sink
   handed a TrustedScriptURL or TrustedHTML throws the TypeError a browser does not. */
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
    /* STEPS 4-6: the mapped type and sink, straight into §3.5 with 'script' as the sink group — which is the
       only group the standard defines, and which trusted_types_required already answers for all three types. */
    return trusted_types_compliant_string(ctx, kind, value, sink);
}
