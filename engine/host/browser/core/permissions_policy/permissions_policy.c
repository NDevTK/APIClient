/* Permissions Policy §9 "Algorithms", over §4 "Framework"'s structures. See permissions_policy.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/fetch/structured_fields.h"
#include "core/frame/csp_source_list.h"
#include "core/permissions_policy/permissions_policy.h"
#include "core/url/url.h"

/* §4.1's SUPPORTED FEATURES as one table, built from the one X-list the enum is built from — so a row cannot
   exist in one and not the other, and the token and the §4.8 default allowlist of a feature are read from the
   same line that named it. */
typedef struct {
    const char                       *token;
    PermissionsPolicyDefaultAllowlist default_allowlist;
} PpFeatureRow;

#define PP_ROW(name, token, allowlist) { token, allowlist },
static const PpFeatureRow PP_FEATURES[PP_FEATURE_N] = { PERMISSIONS_POLICY_FEATURES(PP_ROW) };
#undef PP_ROW

/* §4.7's ALLOWLIST — "may be either: the special value *, which represents every origin, or a struct
 * containing: expressions … self-origin … src-origin".
 *
 * `*` IS A FIELD AND NOT A SENTINEL EXPRESSION, because §4.7's own first step reads it that way ("if the
 * allowlist is the special value *, then return true") and because the alternative is a string compare inside
 * the match loop, which is where the two spellings of `*` — §9.2's TOKEN and a Member Value STRING that
 * happens to be `"*"` — would silently become one answer.
 *
 * THE TWO ORIGIN FIELDS ARE WRITTEN BY DIFFERENT DELIVERIES AND THAT IS WHY THEY ARE TWO. §9.2 "Construct
 * policy from dictionary and origin" reads a HEADER and can only ever write `self-origin` — its vocabulary is
 * the tokens `*` and `self`. §9.3 "Parse policy directive" reads an `<iframe allow=…>` ATTRIBUTE and writes
 * BOTH, because that delivery has a second origin to name: the frame's own declared origin, which the keyword
 * `'src'` selects and which §9.3 step 2.9.1 ALSO installs for a directive whose target list is EMPTY. So
 * `allow="autoplay"` is not "autoplay with no allowlist" — it is autoplay for the origin the `src` attribute
 * names, which is §6.2's own sentence ("the default value for the allowlist is 'src'").
 *
 * `self_origin` AND `src_origin` ARE BORROWED AND MUST BE AGENT-LIFETIME. core/url/origin.h releases origin
 * records with the agent and never before ("every parked flow's delta names these records"), and a policy dies
 * with its Document, so the pointer cannot outlive its referent. */
typedef struct {
    bool          star;
    const Origin *self_origin;     /* §4.7's self-origin, or NULL */
    const Origin *src_origin;      /* §4.7's src-origin, or NULL */
    char        **expressions;     /* §4.7's ordered set of permissions-source-expression (owned, NUL-term) */
    int           n_expressions;
    int           cap_expressions;
} PpAllowlist;

/* §4.6 "Policy directives" — "an ordered map, mapping policy-controlled features to corresponding allowlists of
 * origins". ONE TYPE FOR BOTH OF ITS PRODUCERS: §9.2 builds one out of a header dictionary and §9.3 builds one
 * out of an `allow` attribute, and §4.2 makes the first of them the declarations of a DECLARED POLICY while
 * §4.5 makes the second a CONTAINER POLICY. Two spellings of one structure would be two places a feature can
 * go missing, and the shapes are identical because the section is one.
 *
 * THE PRESENCE BIT IS SEPARATE FROM THE ALLOWLIST because §9.7 step 5, §9.8 step 3 and §9.9 step 2 all begin by
 * asking whether the feature EXISTS in the map, and an absent entry is not an empty allowlist: absent falls
 * through to the DEFAULT allowlist (`Enabled` for a same-origin document), while `autoplay 'none'` is a present
 * allowlist that matches NOTHING. Reading a zeroed allowlist as absence would turn the second into the first
 * and hand back a feature the author explicitly withheld. */
typedef struct {
    bool        present[PP_FEATURE_N];
    PpAllowlist allowlist[PP_FEATURE_N];
} PpPolicyDirective;

/* §4.2's permissions policy — "a struct with the following items: inherited policy …, declared policy …", and
   §4.2's declared policy is «declarations, reporting configuration». THE REPORTING CONFIGURATION IS NOT A
   FIELD and permissions_policy.h states the residual: §9.11 is its only reader and §9.11's only callers are
   §9.13/§9.14, which do not exist here, so storing it would be a value with a writer and no reader. */
struct PermissionsPolicy {
    /* §4.3: "After a permissions policy has been initialized, its inherited policy will contain a value for
       each supported feature." A dense array is that sentence — there is no absent entry to answer for. */
    PermissionsPolicyValue inherited[PP_FEATURE_N];
    /* §4.2's DECLARATIONS — the §4.6 policy directive a response's own header declared. */
    PpPolicyDirective      declarations;
};

static void pp_check_feature(PermissionsPolicyFeature feature)
{
    DCHECK(feature >= 0 && feature < PP_FEATURE_N,
           "a permissions policy was asked about a feature index §4.1's supported-feature set has no row for — "
           "the set is the X-list in permissions_policy.h and the enum is built from it, so a value outside it "
           "is not an unsupported feature (§9.2 skips those at the parse) but a cast that invented one");
}

const char *permissions_policy_feature_token(PermissionsPolicyFeature feature)
{
    pp_check_feature(feature);
    return PP_FEATURES[feature].token;
}

PermissionsPolicyDefaultAllowlist permissions_policy_default_allowlist(PermissionsPolicyFeature feature)
{
    pp_check_feature(feature);
    return PP_FEATURES[feature].default_allowlist;
}

/* ---- §4.7 "Allowlists" ------------------------------------------------------------------------------------ */

static void pp_allowlist_free(PpAllowlist *a)
{
    int k;

    for (k = 0; k < a->n_expressions; k++)
        free(a->expressions[k]);
    free(a->expressions);
    memset(a, 0, sizeof *a);
}

static void pp_directive_free(PpPolicyDirective *d)
{
    int f;

    /* EVERY FEATURE, NOT THE PRESENT ONES. §4.7's expressions array is allocated by
       pp_allowlist_add_expression and `present` is what the ANSWERING steps read; tying a free to a flag whose
       meaning is about answers is the shape that leaks the day an allowlist is built and then not stored. */
    for (f = 0; f < PP_FEATURE_N; f++)
        pp_allowlist_free(&d->allowlist[f]);
    memset(d->present, 0, sizeof d->present);
}

static void pp_allowlist_add_expression(PpAllowlist *a, const char *s, size_t n)
{
    char *copy;

    if (a->n_expressions >= a->cap_expressions) {
        a->cap_expressions = a->cap_expressions ? a->cap_expressions * 2 : 4;
        a->expressions = realloc(a->expressions, (size_t)a->cap_expressions * sizeof *a->expressions);
        CHECK(a->expressions != NULL, "permissions policy: OOM growing §4.7's allowlist expressions");
    }
    copy = malloc(n + 1);
    CHECK(copy != NULL, "permissions policy: OOM copying a §5.1 permissions-source-expression");
    memcpy(copy, s, n);
    copy[n] = '\0';
    a->expressions[a->n_expressions++] = copy;
}

/* §4.1's "the policy-controlled feature identified by feature-name", and §9.2's / §9.3's shared answer for a
   name that identifies none: "if feature-name does not identify any recognized policy-controlled feature, then
   continue". PP_FEATURE_N IS THAT ANSWER — the sentinel the enum already spells — so an unsupported token is
   SKIPPED and never guessed at, which is §4.1's own "user agents are not required to support every feature".
   IT TAKES A LENGTH BECAUSE ONE OF ITS TWO CALLERS HAS NO NUL: §9.2's feature-name is a dictionary key that
   RFC 9651's parse already terminated, and §9.3's is a slice of an attribute value. Two entries would be two
   readings of §4.1's one sentence, and the day a feature-name grows a case rule they would disagree. */
static PermissionsPolicyFeature pp_feature_of_token(const char *name, size_t n)
{
    int f;

    for (f = 0; f < PP_FEATURE_N; f++)
        if (strlen(PP_FEATURES[f].token) == n && strncmp(PP_FEATURES[f].token, name, n) == 0)
            return (PermissionsPolicyFeature)f;
    return PP_FEATURE_N;
}

/* §4.7's "To determine whether an allowlist matches an origin origin", verbatim. */
static bool pp_allowlist_matches(const PpAllowlist *a, const Origin *origin)
{
    UrlRecord url;
    bool      ok;
    int       k;

    DCHECK(a != NULL && origin != NULL,
           "§4.7's allowlist matching was asked about nothing — an allowlist and an origin are both records, "
           "and the absence of either is a caller that lost one rather than an answer of `false`");
    /* Step 1: "If the allowlist is the special value *, then return true." The spec's own note is why there is
       no scheme test beside it: "We are not using the CSP variant of wildcard matching as it requires the
       HTTPS scheme." So `*` here is broader than `*` in a CSP source list, deliberately. */
    if (a->star)
        return true;
    /* Step 2: "If the allowlist's self-origin is not null and it is same origin-domain with origin, then
       return true." SAME ORIGIN-DOMAIN and not same origin — §7.1.1's other relation, which consults
       `document.domain`, and the two differ for exactly the pages that used that API. */
    if (a->self_origin != NULL && origin_same_origin_domain(a->self_origin, origin))
        return true;
    /* Step 3: "If the allowlist's src-origin is not null and it is same origin-domain with origin, then return
       true." SAME ORIGIN-DOMAIN again, and the field is §9.3's — an `allow` attribute's `'src'` keyword, or its
       own default for a directive that named no target at all. It is compared against the origin of the
       Document actually being created, so a frame REDIRECTED off the origin its `src` named does not match:
       that is the point of the keyword rather than a limitation of it. */
    if (a->src_origin != NULL && origin_same_origin_domain(a->src_origin, origin))
        return true;
    /* Step 4: "If origin is an opaque origin, return false." BEFORE the URL parse below, because §7.1.1 gives
       an opaque origin the serialization `null`, which is not a URL and would parse into something that is. */
    if (origin_is_opaque(origin))
        return false;
    /* Step 5: "Let url be the result of calling the url parser on the serialization of origin." */
    {
        const char *s = origin_serialized(origin);

        ok = url_parse(&url, s, strlen(s), NULL);
        DCHECK(ok, "§4.7 step 5 could not parse the serialization of a TUPLE origin as a URL — §7.1.1 "
                   "serializes a tuple origin as `scheme://host[:port]`, which the URL parser accepts by "
                   "construction, and step 4 has already refused the one origin that serializes to `null`");
        /* THE FREE IS ON THE FAILURE PATH TOO, which URL §4.4's entry states as its contract: "`out` is left
           initialised-and-empty on failure, so the caller frees it either way". Without it the release build —
           where the DCHECK above is compiled out and this branch is the only exit — would leak a record per
           call, on a path that runs once per source expression of every policy of every check. */
        if (!ok) {
            url_record_free(&url);
            return false;
        }
    }
    /* Step 6: "For each permissions-source-expression item in the allowlist's expressions: if the result of
       running CSP §6.7.2.8 `Does url match expression in origin with redirect count?` on url, item, origin,
       and 0 is true then return true."
       THE THIRD ARGUMENT IS `origin` AND NOT THE SELF-ORIGIN, which is what §4.7 says and is not the same
       thing: §6.7.2.8 reads it for the `'self'` keyword and for a SCHEMELESS host-source's scheme, and a
       permissions-source-expression is never `'self'` (§9.2 handles that token itself and
       csp_source_is_scheme_or_host_source refuses the keyword), so what the argument actually decides here is
       the scheme a bare `example.com` is measured against. THE REDIRECT COUNT IS 0 BECAUSE §4.7 PASSES 0 —
       there is no request here at all, so the path comparison §6.7.2.8 drops after a redirect always applies. */
    for (k = 0; k < a->n_expressions; k++) {
        CspToken e;

        e.p = a->expressions[k];
        e.n = strlen(a->expressions[k]);
        if (csp_source_match_url(e, &url, origin, 0) == CSP_MATCHES) {
            url_record_free(&url);
            return true;
        }
    }
    url_record_free(&url);
    /* Step 7: "Return false." */
    return false;
}

/* §9.8 step 3 / §9.9 step 2, which are the SAME two lines and are written once:
     "If feature is present in policy's declared policy:
        1. If policy's declared policy's declarations[feature] matches origin, then return `Enabled`.
        2. Otherwise return `Disabled`."
   `*decided` says whether the step FIRED, which is the whole of what the two callers need to know — §9.8 falls
   through to `Enabled` and §9.9 falls through to the default allowlist, and those are different answers. */
static bool pp_declared_decides(const PermissionsPolicy *policy, PermissionsPolicyFeature feature,
                                const Origin *origin, PermissionsPolicyValue *out)
{
    if (!policy->declarations.present[feature])
        return false;
    *out = pp_allowlist_matches(&policy->declarations.allowlist[feature], origin) ? PP_ENABLED : PP_DISABLED;
    return true;
}

/* §9.8 "Get feature value for origin".
 *   "1. Let policy be document's report-only permissions policy if report-only is True, or document's
 *    permissions policy otherwise." — the CALLER's choice, so it passes the policy rather than the document.
 *   "2. If policy's inherited policy for feature is `Disabled`, return `Disabled`."
 *   "3. If feature is present in policy's declared policy: …"
 *   "4. Return `Enabled`."
 *
 * IT DOES NOT CONSULT THE DEFAULT ALLOWLIST, and that is the whole difference from §9.9 one function down. An
 * implementation that shared one body between them would answer a parent's inheritance question with a
 * cross-origin child's `'self'` refusal and disable a feature at every nesting level.
 * `origin` is §9.8's third argument and is READ BY STEP 3 — which is now REACHABLE, because §9.6 fills the
 * declared policy from the response. It was already in the signature when the step was not, because the caller
 * is §9.7, which passes two DIFFERENT origins to two consecutive calls, and a signature that dropped it would
 * make those two calls identical and silently collapse §9.7 steps 2 and 3 into one — which is the same reason
 * it is right now, one step further along: those two calls are what ask an embedder's OWN declared allowlist
 * whether it reaches the child's origin. */
static PermissionsPolicyValue pp_get_feature_value_for_origin(const PermissionsPolicy *policy,
                                                              PermissionsPolicyFeature feature,
                                                              const Origin *origin)
{
    DCHECK(policy != NULL, "§9.8's get-feature-value-for-origin was asked of a Document with no permissions "
                           "policy — §4.3 says an initialized policy holds a value for every supported feature, "
                           "so the absence is a Document §9.5 never ran for and the caller is asking the wrong "
                           "document");
    DCHECK(origin != NULL, "§9.8 was asked to get a feature value for NO origin — its step 3 matches the "
                           "declared policy's allowlist against one, and an absent origin is not the opaque "
                           "origin §4.7 refuses (that is a real origin record) but a caller that had none");
    pp_check_feature(feature);
    if (policy->inherited[feature] == PP_DISABLED)
        return PP_DISABLED;                                   /* step 2 */
    {
        PermissionsPolicyValue declared;

        if (pp_declared_decides(policy, feature, origin, &declared))
            return declared;                                  /* step 3 */
    }
    return PP_ENABLED;                                        /* step 4 */
}

/* §9.9 "Check permissions policy", in full. Steps 3-4 are the DEFAULT ALLOWLIST steps §9.8 does not have. */
PermissionsPolicyValue permissions_policy_check(const PermissionsPolicy *policy,
                                                PermissionsPolicyFeature feature,
                                                const Origin *origin, const Origin *document_origin)
{
    DCHECK(policy != NULL, "§9.9's check-a-permissions-policy was asked of a Document with no permissions "
                           "policy — HTML §4.8.5's allowed-to-use refuses such a Document at its step 1 (\"if "
                           "document's browsing context is null, then return false\") and never reaches here");
    DCHECK(origin != NULL && document_origin != NULL,
           "§9.9 was asked to check a permissions policy against an absent origin — its step 4 compares the two "
           "with §7.1.1's same origin, which is a relation over two origin RECORDS");
    pp_check_feature(feature);
    /* Step 1: "If policy's inherited policy for feature is `Disabled`, return `Disabled`." */
    if (policy->inherited[feature] == PP_DISABLED)
        return PP_DISABLED;
    /* Step 2: "If feature is present in policy's declared policy: 1. If … declarations[feature] matches origin,
       then return `Enabled`. 2. Otherwise return `Disabled`." THIS IS THE STEP THE HEADER FILLS, and it is the
       one that makes a server's `Permissions-Policy` mean anything: a feature the response DECLARED is decided
       here and never reaches the default allowlist below. */
    {
        PermissionsPolicyValue declared;

        if (pp_declared_decides(policy, feature, origin, &declared))
            return declared;
    }
    /* Step 3: "If feature's default allowlist is *, return `Enabled`." */
    if (PP_FEATURES[feature].default_allowlist == PP_ALLOWLIST_STAR)
        return PP_ENABLED;
    DCHECK(PP_FEATURES[feature].default_allowlist == PP_ALLOWLIST_SELF,
           "a supported feature's §4.8 default allowlist is neither `*` nor `'self'` — §4.8 defines exactly "
           "those two values, so a third is a row of the X-list that spelled a keyword (`'none'`) an allowlist "
           "may be WRITTEN with and a default allowlist may not be");
    /* Step 4: "If feature's default allowlist is 'self', and origin is same origin with document origin,
       return `Enabled`." §7.1.1's same origin over records, so an OPAQUE origin is same origin with itself and
       a sandboxed top-level document is allowed its own 'self' features. */
    if (origin_same(origin, document_origin))
        return PP_ENABLED;
    /* Step 5: "Return `Disabled`." */
    return PP_DISABLED;
}

/* ---- §9.3 and §9.4, the `<iframe allow=…>` half ----------------------------------------------------------- */

/* Infra §4.6 "Code points"' ASCII WHITESPACE — "ASCII whitespace is U+0009 TAB, U+000A LF, U+000C FF, U+000D
   CR, or U+0020 SPACE" — which is the set Infra §4.7 "Strings"' split-on-ASCII-whitespace uses and therefore
   what §9.3 step 2.1 splits a serialized-declaration on. It is NOT the same set as §5.1's ABNF RWS, and the
   difference is deliberate on the standard's part: the grammar says what an AUTHOR should write and the
   algorithm says what a user agent must accept. Infra names the FF exclusion of XML/JSON/HTTP explicitly and
   says to prefer this definition, so the U+000C is a member and not an oversight. */
static bool pp_is_ascii_whitespace(char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

/* Infra §4.7 "Strings"' ASCII CASE-INSENSITIVE compare, over a slice — §9.3 steps 2.9.2.1 and 2.9.2.2 match
   `'self'` and `'src'` that way, QUOTES INCLUDED. The quotes are part of the keyword (§5.1's `allow-list-value` spells them)
   and a match that dropped them would resolve a bare host named `self` to the container's own origin. */
static bool pp_slice_eq_ascii_ci(const char *s, size_t n, const char *lit)
{
    size_t k;

    if (strlen(lit) != n)
        return false;
    for (k = 0; k < n; k++) {
        char a = s[k];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != lit[k])
            return false;
    }
    return true;
}

/* §9.3 step 2.9.2.3-2.9.2.4: "let result be the result of executing the URL parser on element. If result is not
 * failure: let target be the origin of result; if target is not an opaque origin, append the serialization of
 * target to allowlist's expressions."
 *
 * WHAT IS APPENDED IS AN ORIGIN'S SERIALIZATION AND NOT THE AUTHOR'S BYTES, which is the one place §9.3 and
 * §9.2 differ about what an expression IS. A header's element is a §5.1 permissions-source-expression and is
 * stored verbatim; an attribute's is a URL, and the standard normalizes it through §7.1.1's serializer before
 * storing — so `allow="autoplay https://a.example/path?q"` stores `https://a.example`, and §4.7 step 6 then
 * measures the checked origin against a host-source with no path to disagree about.
 *
 * NO ORIGIN IDENTITY IS MINTED. origin_of_url would keep an agent-lifetime record for every element of every
 * `allow` attribute of every navigable creation, for a question that never compares two origin RECORDS — the
 * opacity test is origin_tuple_url's NULL answer and the bytes are origin_serialize_of_url's, and core/url/
 * origin.h states that those two are the same one reading of URL §4.7 as origin_of_url. */
static void pp_append_url_origin(PpAllowlist *a, const char *s, size_t n)
{
    UrlRecord url, scratch;
    bool      opaque;

    if (!url_parse(&url, s, n, NULL)) {
        url_record_free(&url);                       /* URL §4.4's entry leaves `out` initialised on failure */
        return;
    }
    /* origin_tuple_url INITIALISES `scratch` on every path and answers NULL exactly where URL §4.7 gives the
       URL an OPAQUE origin, which is step 2.9.2.4's condition with no record minted to ask it. */
    opaque = origin_tuple_url(&url, &scratch) == NULL;
    url_record_free(&scratch);
    if (!opaque) {
        char *serialized = origin_serialize_of_url(&url);

        CHECK(serialized != NULL, "permissions policy: OOM serializing §9.3's target origin");
        pp_allowlist_add_expression(a, serialized, strlen(serialized));
        free(serialized);
    }
    url_record_free(&url);
}

/* §9.3 "Parse policy directive" — "given a string (value), an origin (container origin), and an optional origin
 * (target origin), this algorithm returns a policy directive".
 *
 * `target_origin` IS THE ELEMENT'S DECLARED ORIGIN AND IS ALWAYS GIVEN HERE, because §9.4 is this algorithm's
 * only caller in this build and §9.4 step 2 always passes one. The spec's optionality is real — §7.2's
 * `permissionsPolicy` object parses a directive with no element to take a declared origin from — so the
 * NULL arm is written rather than asserted away, and it is the arm in which `'src'` names nothing and an empty
 * target list yields an allowlist that matches NOTHING rather than the frame's own origin.
 *
 * A REPEATED FEATURE OVERWRITES, which is step 2.10's "set directive[feature] to allowlist" over an ORDERED
 * MAP: `allow="autoplay *; autoplay 'none'"` is `'none'`. That is the OPPOSITE of what a merge would give and
 * it is the restrictive-looking answer only by coincidence — `allow="autoplay 'none'; autoplay *"` is `*` —
 * so the old allowlist is FREED at the overwrite rather than leaked or unioned. */
static void pp_parse_policy_directive(const char *value, size_t len, const Origin *container_origin,
                                      const Origin *target_origin, PpPolicyDirective *out)
{
    size_t i = 0;

    DCHECK(container_origin != NULL,
           "§9.3 was asked to parse a policy directive with no CONTAINER ORIGIN — it is the second argument and "
           "is what the keyword `'self'` resolves to, so an absent one does not fail loudly, it silently "
           "narrows every `'self'` an author wrote to an allowlist that matches nothing");
    memset(out, 0, sizeof *out);
    if (value == NULL)
        return;                                      /* §9.4 step 2 over an element carrying no attribute */
    /* Step 2: "for each serialized-declaration returned by STRICTLY SPLITTING value on U+003B (;)" — Infra
       §4.7 "Strings"' strictly-split, so `a;;b` yields an EMPTY declaration between them, which step 2.2
       discards because splitting an empty string on ASCII whitespace is the empty list. */
    while (i <= len) {
        size_t decl_end = i;
        size_t t;
        const char           *feature_name;
        size_t                feature_n;
        PermissionsPolicyFeature feature;
        PpAllowlist           allowlist;
        bool                  any_target = false;

        while (decl_end < len && value[decl_end] != ';')
            decl_end++;
        /* Step 2.1: "let tokens be the result of splitting serialized-declaration on ASCII whitespace", read
           as a walk rather than materialised — the list is consumed exactly once, in order, and step 2.3 and
           step 2.6 are "the first element" and "the remaining elements". */
        t = i;
        while (t < decl_end && pp_is_ascii_whitespace(value[t])) t++;
        feature_name = value + t;
        while (t < decl_end && !pp_is_ascii_whitespace(value[t])) t++;
        feature_n = (size_t)(value + t - feature_name);
        if (feature_n == 0)
            goto next;                               /* step 2.2: an empty token list */
        feature = pp_feature_of_token(feature_name, feature_n);   /* steps 2.3-2.5 */
        if (feature == PP_FEATURE_N)
            goto next;                               /* step 2.4 */
        memset(&allowlist, 0, sizeof allowlist);     /* step 2.7: "let allowlist be a new allowlist" */
        /* Step 2.8: "if ANY element of targetlist is the string `*`, set allowlist to the special value *." It
           is asked over the WHOLE list before any element is stored, exactly as §9.2's is, because a `*` does
           not narrow an allowlist — it REPLACES one, so `allow="autoplay 'none' *"` is `*`. */
        {
            size_t u = t;

            while (u < decl_end) {
                size_t start;

                while (u < decl_end && pp_is_ascii_whitespace(value[u])) u++;
                start = u;
                while (u < decl_end && !pp_is_ascii_whitespace(value[u])) u++;
                if (u == start)
                    break;
                any_target = true;
                if (u - start == 1 && value[start] == '*')
                    allowlist.star = true;
            }
        }
        if (!allowlist.star) {
            /* Step 2.9.1: "if targetlist is EMPTY and target origin is given, let allowlist's src-origin be
               target origin." §6.2 states the same rule from the delivery's side: "the allowlist for the
               features named in the attribute may be empty; in that case, the default value for the allowlist
               is 'src'". So `allow="autoplay"` is autoplay FOR THE FRAME'S OWN DECLARED ORIGIN, which is the
               spelling every real embed uses and is the opposite of an empty allowlist. */
            if (!any_target && target_origin != NULL)
                allowlist.src_origin = target_origin;
            /* Step 2.9.2: "for each element in value". */
            while (t < decl_end) {
                size_t start;

                while (t < decl_end && pp_is_ascii_whitespace(value[t])) t++;
                start = t;
                while (t < decl_end && !pp_is_ascii_whitespace(value[t])) t++;
                if (t == start)
                    break;
                if (pp_slice_eq_ascii_ci(value + start, t - start, "'self'")) {
                    allowlist.self_origin = container_origin;      /* step 2.9.2.1 */
                    continue;
                }
                if (target_origin != NULL && pp_slice_eq_ascii_ci(value + start, t - start, "'src'")) {
                    allowlist.src_origin = target_origin;          /* step 2.9.2.2 */
                    continue;
                }
                /* Steps 2.9.2.3-2.9.2.4. THE KEYWORD `'none'` NEEDS NO BRANCH AND MUST NOT HAVE ONE: §9.3 has
                   no step for it, so it reaches the URL parser, fails there, and leaves the allowlist as step
                   2.7 made it — which is §4.7's "a struct with no self-origin, no src-origin and no
                   expressions", matching nothing. `allow="autoplay 'none'"` is therefore DISABLED because the
                   keyword parses as no origin at all, and a branch that special-cased it would have to say
                   what `allow="autoplay 'none' 'self'"` means, which the standard already answers. */
                pp_append_url_origin(&allowlist, value + start, t - start);
            }
        }
        /* Step 2.10: "set directive[feature] to allowlist." */
        if (out->present[feature])
            pp_allowlist_free(&out->allowlist[feature]);
        out->present[feature]   = true;
        out->allowlist[feature] = allowlist;
    next:
        i = decl_end + 1;
    }
}

/* §9.4 "Process permissions policy attributes" — "given an element (element), this algorithm returns a
 * container policy, which may be empty".
 *
 * ITS STEP 1 IS THE CALLER'S AND IS SPELLED AS AN ARGUMENT. "If element is not an `iframe` element, then return
 * an empty policy directive" is a question about the ELEMENT, and this component does not know what an element
 * is — core/dom/document.c does, and it states the answer by passing NULL for `allow` and for the declared
 * origin, which is the same shape §9.5's "null or an element" already travels in. An `<object>` or `<embed>`
 * container therefore gets step 1's empty directive with no branch here, and the assert below is what stops a
 * caller stating half of it.
 *
 * ITS STEP 3 IS THE LEGACY `allowfullscreen` DELIVERY OF ONE FEATURE. "If element's `allowfullscreen`
 * attribute is specified, and container policy does not contain an entry for the `fullscreen` feature", then
 * step 3.1 sets "container policy[fullscreen] = the special value *". It ran as nothing while §4.1's
 * supported-feature set here was HTML §2.2's three names; FULLSCREEN API §7 "Permissions Policy Integration"
 * defines the fourth, the X-list carries it, and the step is an assignment.
 *
 * ITS CONDITION IS ORDERED AFTER STEP 2 AND THAT ORDER IS OBSERVABLE. Step 2's parse of `allow` runs first, so
 * `<iframe allowfullscreen allow="fullscreen 'none'">` keeps the author's `'none'`: the entry EXISTS, step 3's
 * second conjunct is false, and the legacy attribute adds nothing. §6.3.1 "allowfullscreen" states the same
 * rule from the other end, twice: "If the `iframe` element has an `allow` attribute whose value contains the
 * token "fullscreen", then the `allowfullscreen` attribute must have no effect", and "If `allow="fullscreen"`
 * and `allowfullscreen` are both present on an `iframe` element, then the more restrictive allowlist of
 * `allow="fullscreen"` will be used". Reading the steps in the other order would let the legacy attribute
 * overrule the modern one, which is a security answer in the permissive direction.
 *
 * `*` IS §4.7's SPECIAL VALUE AND NOT AN ALLOWLIST OF EVERY ORIGIN. §4.7's own matching begins "If the
 * allowlist is the special value *, then return true", which is the `star` flag of PpAllowlist above; §9.7
 * step 5 asks pp_allowlist_matches, so a CROSS-ORIGIN child of an `allowfullscreen` frame is `Enabled`. §6.3.1
 * says that is deliberate and not a coincidence of the encoding: "This is different from the behaviour of
 * `<iframe allow="fullscreen">`, and is for compatibility with existing uses of `allowfullscreen`." */
static void pp_process_policy_attributes(const char *allow, size_t allow_len, bool allowfullscreen,
                                         const Origin *container_origin,
                                         const Origin *declared_origin, PpPolicyDirective *out)
{
    DCHECK(allow == NULL || declared_origin != NULL,
           "§9.4 step 2 was given an `allow` attribute value with no DECLARED ORIGIN — the step passes both to "
           "§9.3, and without the second the keyword `'src'` names nothing and a directive with an empty target "
           "list gets an allowlist that matches nothing, which is `<iframe allow=\"autoplay\">` read as the "
           "`'none'` its author did not write");
    pp_parse_policy_directive(allow, allow_len, container_origin, declared_origin, out);   /* step 2 */
    /* Step 3, and both conjuncts are read here: `allowfullscreen` specified, and no entry for the feature.
       THE ALLOWLIST IS WRITTEN AND NOT MERGED INTO. §4.6's map has one entry per feature and step 3.1 SETS it,
       so the slot must be the empty allowlist the caller's zeroed directive gives it — §9.3 fills a slot only
       on the path that also sets `present`, which the conjunct above has just refused. Asserted rather than
       re-zeroed: a non-empty allowlist under a false presence bit would mean §9.3 leaked one, and clearing it
       here would free nothing and hide that. */
    if (allowfullscreen && !out->present[PP_FEATURE_FULLSCREEN]) {
        DCHECK(out->allowlist[PP_FEATURE_FULLSCREEN].expressions == NULL
                   && out->allowlist[PP_FEATURE_FULLSCREEN].self_origin == NULL
                   && out->allowlist[PP_FEATURE_FULLSCREEN].src_origin == NULL
                   && !out->allowlist[PP_FEATURE_FULLSCREEN].star,
               "§9.4 step 3.1 found a non-empty allowlist in the `fullscreen` slot of a container policy that "
               "does not CONTAIN an entry for the feature — §4.6's presence bit and its allowlist are one "
               "entry, so a filled allowlist under a false bit is §9.3 having written half of one, and step "
               "3.1's assignment would then overwrite an owned pointer");
        out->allowlist[PP_FEATURE_FULLSCREEN].star = true;                                 /* step 3.1 */
        out->present[PP_FEATURE_FULLSCREEN] = true;
    }
}

/* §9.7 "Define an inherited policy for feature in container at origin" — "given a feature (feature), null or a
 * navigable container (container), an origin for a Document in that container (origin) … this algorithm
 * returns the inherited policy value for feature".
 *
 * `report_only` IS NOT A PARAMETER HERE, because the report-only arm of this algorithm has exactly one caller —
 * §9.12's check-potential-violation-of-permissions-policy-in-container — and §9.12 is the `iframe` load event's
 * REPORTING step, which needs the report queue this build does not have. The arm this function serves is
 * §9.5's, whose call passes the default False. */
static PermissionsPolicyValue pp_define_inherited(PermissionsPolicyFeature feature,
                                                  const PermissionsPolicy *container_document_policy,
                                                  const Origin *container_document_origin,
                                                  const PpPolicyDirective *container_policy,
                                                  const Origin *origin)
{
    PermissionsPolicyValue by_default;

    /* Step 1: "If container is null, return `Enabled`." A top-level traversable's Document, which §4.3 states
       from the other end: "In a Document in a top-level traversable, the inherited policy is based on defined
       defaults for each feature." */
    if (container_document_origin == NULL)
        return PP_ENABLED;

    /* Steps 2 and 3: §9.8 over the CONTAINER DOCUMENT's own policy, twice — once at the container document's
       own origin and once at the origin of the Document being created. Either answering `Disabled` ends the
       algorithm, which is what makes inheritance a CHAIN rather than a comparison against the top: a document
       nested top → cross-origin → same-origin-as-top is refused HERE, at the middle link, and no comparison
       against the top-level traversable can see that. */
    if (pp_get_feature_value_for_origin(container_document_policy, feature,
                                        container_document_origin) == PP_DISABLED)
        return PP_DISABLED;
    if (pp_get_feature_value_for_origin(container_document_policy, feature, origin) == PP_DISABLED)
        return PP_DISABLED;

    /* Steps 4 and 5: "let container policy be the result of running Process permissions policy attributes on
       container. If feature exists in container policy: if the allowlist for feature in container policy
       matches origin, return `Enabled`; otherwise return `Disabled`."
       THE DIRECTIVE ARRIVES BUILT, and §9.5's caller is where step 4 runs — see permissions_policy_create.
       §9.4's OTHER attribute needs no branch HERE either, and for a different reason than it used to: its step
       3 sets `allowfullscreen` into the CONTAINER POLICY under the `fullscreen` feature, which pp_process_
       policy_attributes performs, so by the time this step reads the map the entry is an ordinary one and the
       `star` allowlist below answers it like any other. */
    DCHECK(container_policy != NULL,
           "§9.7 step 5 was asked whether a feature exists in a container policy that was never built — step 4 "
           "runs §9.4's process-permissions-policy-attributes on the container, and every container has one "
           "(an element carrying no `allow` attribute yields §9.4's EMPTY policy directive, which is a "
           "statement and not an absence)");
    if (container_policy->present[feature])
        return pp_allowlist_matches(&container_policy->allowlist[feature], origin) ? PP_ENABLED : PP_DISABLED;

    /* Steps 6, 7 and 8 — the DEFAULT ALLOWLIST, §4.8's two values and nothing else.
       "6. If feature's default allowlist is *, return `Enabled`.
        7. If feature's default allowlist is 'self', and origin is same origin with container's node document's
           origin, return `Enabled`.
        8. Otherwise return `Disabled`."
       STEP 7 COMPARES AGAINST THE CONTAINER DOCUMENT, NOT THE TOP. That is the sentence the two hand-written
       copies of this question got wrong: same origin with the top-level traversable's active document is
       neither necessary (a same-origin chain through a cross-origin middle frame satisfies it and is
       `Disabled`) nor what any step of this algorithm says. */
    if (PP_FEATURES[feature].default_allowlist == PP_ALLOWLIST_STAR) {
        by_default = PP_ENABLED;
    } else {
        DCHECK(PP_FEATURES[feature].default_allowlist == PP_ALLOWLIST_SELF,
               "a supported feature's §4.8 default allowlist is neither `*` nor `'self'`, which are the two "
               "values §4.8 defines");
        by_default = origin_same(origin, container_document_origin) ? PP_ENABLED : PP_DISABLED;
    }

    return by_default;
}

/* §9.5 "Create a Permissions Policy for a navigable". */
PermissionsPolicy *permissions_policy_create(const PermissionsPolicy *container_document_policy,
                                             const Origin *container_document_origin,
                                             const char *container_allow, size_t container_allow_len,
                                             bool container_allowfullscreen,
                                             const Origin *container_declared_origin,
                                             const Origin *origin)
{
    PermissionsPolicy *policy;
    PpPolicyDirective  container_policy;
    int f;

    /* Step 1: "Assert: If not null, container is a navigable container." The caller states the container by
       stating what §9.7 reads off it, so "there is no container" is the container document's POLICY and ORIGIN
       being absent TOGETHER — they are one navigable container's two facts and neither can arrive without the
       other. A caller holding a container it cannot read both of has one in ANOTHER WASM INSTANCE, which is
       answered by the instance that DOES hold the element running §9.5 there and sending its result
       (permissions_policy_serialize): passing half a container here would take §9.7 step 1 and enable every
       feature for a cross-origin child. */
    DCHECK((container_document_origin != NULL) == (container_document_policy != NULL),
           "§9.5 was given one half of a navigable container — a container document's origin without its "
           "permissions policy or the reverse. §9.7 reads both off ONE element: its steps 2 and 3 ask the "
           "policy and its step 7 compares the origin, and a missing policy is not an empty one (§4.3 gives an "
           "initialized policy a value for every supported feature)");
    DCHECK(container_allow == NULL || container_document_origin != NULL,
           "§9.5 was given an `allow` attribute with NO container — §9.4's process-permissions-policy-"
           "attributes reads that attribute off the navigable container itself, so an attribute without one is "
           "a caller that lost the element it came from");
    DCHECK(!container_allowfullscreen || container_document_origin != NULL,
           "§9.5 was told a container's `allowfullscreen` attribute is SPECIFIED with NO container — §9.4 step "
           "3 reads that attribute off the navigable container itself, so a specified attribute without one is "
           "a caller that lost the element it came from, and taking it at its word would give a TOP-LEVEL "
           "document a container policy §9.7 step 1 says it does not have");
    DCHECK(container_declared_origin == NULL || container_document_origin != NULL,
           "§9.5 was given a container's DECLARED ORIGIN with no container — §7.2's declared origin is computed "
           "from the element's own `sandbox`, `srcdoc` and `src` attributes and its node document, so one "
           "without a container document is a caller that lost the element it was read off");
    DCHECK(origin != NULL, "§9.5 was asked to create a permissions policy for a navigable with no origin — the "
                           "origin is the Document's own and every §9.7 default-allowlist step compares against "
                           "it");
    policy = calloc(1, sizeof *policy);
    CHECK(policy != NULL, "permissions policy: OOM creating §9.5's policy for a navigable");
    /* §9.7 STEP 4 RUNS ONCE, HERE, AND NOT PER FEATURE — which is the same answer and not an optimisation to
       be traded away later. §9.4 is a pure function of the ELEMENT (its `allow` attribute, its node document's
       origin, its declared origin), and §9.5's loop varies only `feature`, so a per-feature call would parse
       the same bytes PP_FEATURE_N times and build PP_FEATURE_N directives of which each call reads one entry.
       Hoisting it is also what makes the parse's OWNERSHIP statable: a directive owns heap allowlists, and
       there is exactly one of them here with exactly one free.
       IT DOES NOT RUN AT ALL FOR A NULL CONTAINER, because §9.7 RETURNS at its step 1 and step 4 is three steps
       further down: there is no element to process the attributes of, and §9.3's second argument — the
       container document's origin, which `'self'` resolves to — would be the absent one its own assert names. */
    memset(&container_policy, 0, sizeof container_policy);
    if (container_document_origin != NULL)
        pp_process_policy_attributes(container_allow, container_allow_len, container_allowfullscreen,
                                     container_document_origin, container_declared_origin, &container_policy);
    /* Steps 2-4: "let inherited policy be a new ordered map. For each feature supported, let isInherited be the
       result of running Define an inherited policy for feature in container at origin …; set inherited
       policy[feature] to isInherited." EVERY supported feature, which is what §4.3's "will contain a value for
       each supported feature" makes the array's density mean. */
    for (f = 0; f < PP_FEATURE_N; f++)
        policy->inherited[f] = pp_define_inherited((PermissionsPolicyFeature)f, container_document_policy,
                                                   container_document_origin, &container_policy, origin);
    /* THE CONTAINER POLICY DIES WITH THE ALGORITHM. §9.5 returns a permissions policy and §4.2 gives it an
       inherited policy and a declared policy — never a container policy, which §4.5 makes a fact about the
       CHILD NAVIGABLE and which §9.7 consults and discards. Keeping it on the record would be storing an input
       beside an answer, and every later reader would have to be told which of the two decides. */
    pp_directive_free(&container_policy);
    /* Step 5: "let policy be a new permissions policy, with inherited policy inherited policy and declared
       policy «[], []»." — the calloc above IS that empty declared policy: every `declarations.present` is
       false, which is "the feature is not present in the declared policy" and not "the feature has an empty
       allowlist". §9.6 is the one algorithm that turns any of them true. */
    return policy;
}

/* ---- §9.1, §9.2 and §9.6's response half ----------------------------------------------------------------- */

/* §5.2's TOKEN `*` and TOKEN `self`, asked of ONE bare item. The KIND is half the question and dropping it is
   the trap: `camera="self"` is a STRING whose text is `self` and §5.2 makes a String "a String containing the
   ASCII permissions-source-expression", so reading it as the keyword would resolve an allowlist to the
   document's own origin because of a pair of quotes. */
static bool pp_bare_is_token(const SfBareItem *b, const char *token)
{
    return b->kind == SF_TOKEN && b->text != NULL && !strcmp(b->text, token);
}

/* §9.2's inner-list scan for the token `*`: "or if value is a list which contains the token *". It is asked
   BEFORE any element is stored, because it does not narrow an allowlist — it REPLACES one, so
   `camera=(self *)` is `*` and not "self plus a wildcard". */
static bool pp_member_list_has_star(const SfMember *m)
{
    int k;

    if (!m->inner_list)
        return false;
    for (k = 0; k < m->n_items; k++)
        if (pp_bare_is_token(&m->items[k].item, "*"))
            return true;
    return false;
}

/* §9.2 "Construct policy from dictionary and origin" — "given an ordered map (dictionary) and an origin
 * (origin), this algorithm will return a declared policy".
 *
 * IT WRITES A §4.6 POLICY DIRECTIVE RATHER THAN RETURNING A MAP, because that IS what a declared policy's
 * declarations are and a second representation to copy out of would be a second place a feature can go missing.
 * `out` starts EMPTY, which is §9.2 step 1's "let declarations be an empty ordered map" — the same type §9.3
 * builds out of an attribute, because §4.6 is one section and not two.
 *
 * §9.2's REPORTING CONFIGURATION (its step 2 and its step 3's `report-to`) IS PARSED AND DISCARDED — the named
 * residual permissions_policy.h states, and the reason a `report-to` parameter is not a crash: it cannot move
 * any answer, because every answer is decided by an ALLOWLIST. */
static void pp_construct_from_dictionary(const SfDictionary *dict, const Origin *origin,
                                         PpPolicyDirective *out)
{
    int i;

    for (i = 0; i < dict->n_members; i++) {                     /* step 3: "for each feature-name → (value, params)" */
        const SfMember          *m = &dict->members[i].value;
        PermissionsPolicyFeature feature = pp_feature_of_token(dict->members[i].key,
                                                               strlen(dict->members[i].key));
        PpAllowlist              allowlist;

        if (feature == PP_FEATURE_N)                            /* step 3.1 */
            continue;
        memset(&allowlist, 0, sizeof allowlist);                /* step 3.4: "let allowlist be a new allowlist" */
        /* Step 3.5: "if value is the token *, or if value is a list which contains the token *, set allowlist
           to the special value *." */
        if ((!m->inner_list && pp_bare_is_token(sf_member_bare(m), "*")) || pp_member_list_has_star(m)) {
            allowlist.star = true;
        } else if (!m->inner_list) {
            /* Step 3.6.1: "if value is the token self, let allowlist's self-origin be origin."
               A BARE ITEM THAT IS NEITHER TOKEN LEAVES THE ALLOWLIST EMPTY, AND THAT IS WHAT §9.2 SAYS. Its
               otherwise-arm has exactly two branches — the token `self`, and "otherwise if value is a LIST" —
               so a Member Value that is a bare STRING (`camera="https://a.example"`, which §5.2 admits) falls
               out of both and §9.2 stores the empty allowlist it made at step 3.4. That is the RESTRICTIVE
               direction and it is written verbatim rather than repaired here: guessing the other reading would
               be this file inventing an allowlist entry no algorithm produced, which is precisely what §9.2's
               own "does not identify any recognized feature ⇒ continue" refuses to do one line up. */
            if (pp_bare_is_token(sf_member_bare(m), "self"))
                allowlist.self_origin = origin;
        } else {
            int k;

            /* Step 3.6.2: "otherwise if value is a list, then for each element in value: …" */
            for (k = 0; k < m->n_items; k++) {
                const SfBareItem *e = &m->items[k].item;

                if (pp_bare_is_token(e, "self")) {              /* step 3.6.2.1 */
                    allowlist.self_origin = origin;
                    continue;
                }
                /* Step 3.6.2.2: "if element is a valid permissions-source-expression, append element to
                   allowlist's expressions." §5.1: `permissions-source-expression = scheme-source / host-source`
                   — CSP §2.3.1's two productions, recognised by the one reading of that grammar this tree has
                   (core/frame/csp_source_list.h). §5.2's "any other items inside of an Inner List will be
                   ignored by the processing steps" is the same sentence read from the other side, which is why
                   an unrecognised element is skipped rather than failing the member. */
                if (e->text != NULL) {
                    CspToken t;

                    t.p = e->text;
                    t.n = strlen(e->text);
                    if (csp_source_is_scheme_or_host_source(t))
                        pp_allowlist_add_expression(&allowlist, e->text, t.n);
                }
            }
        }
        /* Step 3.7: "set declarations[feature] to allowlist." A REPEATED FEATURE CANNOT ARRIVE HERE — RFC 9651
           §4.2.2 step 2.4 overwrote the earlier member in the dictionary — so this is a set and never a merge,
           and the assert is what says so rather than a free that would quietly accept one. */
        DCHECK(!out->present[feature],
               "§9.2 reached one policy-controlled feature TWICE from a single dictionary — RFC 9651 §4.2.2's "
               "ordered map overwrites a duplicate key in place ('all but the last instance are ignored'), so "
               "two members naming one feature is a dictionary parse that APPENDED where the section says it "
               "must overwrite, and the allowlist about to be dropped is the one the server sent last");
        out->present[feature]   = true;
        out->allowlist[feature] = allowlist;
    }
}

/* §9.1 "Process response policy" — "given a response (response), an origin (origin), and a boolean
 * (report-only), this algorithm returns a declared policy".
 *
 * ITS STEP 1 AND HALF OF ITS STEP 2 HAPPENED AT THE RESPONSE, and that is the split permissions_policy.h
 * argues: step 1 picks the header NAME from `report-only`, and step 2's "get a structured field value" begins
 * with Fetch §2.2.2 step 2's join of every header of that name. Both are questions about a header LIST, which
 * exists only while the response does — so core/frame/navigation_params.c performs them and what arrives here
 * is the one field value. What is left is the half that is about the GRAMMAR and the ORIGIN, and neither of
 * those is available at the fetch: the origin is not decided until §7.5.1.
 *
 * STEP 3 IS THE ONE THAT MAKES A MALFORMED HEADER SAFE: "if parsed header is null, return an empty ordered
 * map." Fetch's get returns null for a value that does not parse as well as for an absent header, so a server
 * sending `Permissions-Policy: ((((` gets the same declared policy as one sending nothing — which is the
 * platform-wide uniformity Fetch's own note demands, and is why sf_parse_dictionary failing here is a VALUE
 * and never a crash. */
static void pp_process_response_policy(const char *header_value, const Origin *origin,
                                       PpPolicyDirective *out)
{
    SfDictionary dict;

    memset(out, 0, sizeof *out);                                 /* step 3's empty ordered map */
    if (header_value == NULL)
        return;                                                  /* no such header: step 3's empty map */
    if (!sf_parse_dictionary(header_value, strlen(header_value), &dict))
        return;                                                  /* step 3, for a value that did not parse */
    pp_construct_from_dictionary(&dict, origin, out);            /* step 4 */
    sf_dictionary_free(&dict);
}

SerializedResponsePermissionsPolicy serialized_response_permissions_policy(const char *enforced,
                                                                          const char *report_only)
{
    SerializedResponsePermissionsPolicy r;

    r.enforced    = enforced;
    r.report_only = report_only;
    return r;
}

void permissions_policy_apply_response(PermissionsPolicy *policy, const char *header_value,
                                       const Origin *origin)
{
    PpPolicyDirective d;
    int               f;

    DCHECK(policy != NULL,
           "§9.6's steps 2-4 were asked to apply a response's declared policy to NOTHING — its step 1 has "
           "already run §9.5 and returned a policy, so a NULL here is a caller that did not perform step 1");
    DCHECK(origin != NULL,
           "§9.6 was given no ORIGIN. HTML §7.5.1 passes navigationParams's origin, §9.2 makes it what the "
           "token `self` resolves to, and an allowlist whose self-origin is absent refuses the document's own "
           "origin — so a missing origin does not fail loudly, it silently narrows every `self` a server wrote");
    for (f = 0; f < PP_FEATURE_N; f++)
        DCHECK(!policy->declarations.present[f],
               "§9.6 is applying a response to a policy that ALREADY carries a declared policy — its step 1 "
               "returns §9.5's policy, whose declared policy is «[], []», so a policy with declarations here "
               "has had a response applied to it once already and the second application would be a document "
               "judged under two responses' headers at once");
    /* Step 2: "let d be the result of running Process response policy given response, origin and
       report-only." The report-only choice is the CALLER's, spelled as WHICH field value it passed. */
    pp_process_response_policy(header_value, origin, &d);
    /* Step 3: "for each feature → allowlist of d's declarations: if policy's inherited policy[feature] is true,
       then set policy's declared policy's declarations[feature] to allowlist."
       "TRUE" IS §4.2's `Enabled` — the section defines an inherited policy as a map to `Enabled` or `Disabled`
       and §9.6 writes the boolean spelling of it — and the test is what stops a response from re-granting a
       feature its EMBEDDER already refused: a cross-origin child whose parent disallowed `autoplay` cannot
       declare it back, and the declaration is DROPPED rather than stored-and-overruled, which is the difference
       §9.11's endpoint would later be able to see. */
    for (f = 0; f < PP_FEATURE_N; f++) {
        if (d.present[f] && policy->inherited[f] == PP_ENABLED) {
            policy->declarations.present[f]   = true;
            policy->declarations.allowlist[f] = d.allowlist[f];
            memset(&d.allowlist[f], 0, sizeof d.allowlist[f]);   /* moved: `d` owns it no longer */
        }
    }
    /* WHAT STEP 3 DID NOT TAKE IS FREED HERE AND NOT AT THE BRANCH, because the ownership statement is about
       `d` as a whole: every allowlist step 3 moved out has been emptied above, so this frees exactly the
       declarations the inherited policy refused. */
    pp_directive_free(&d);
    /* Step 4: "set policy's declared policy[feature]'s reporting configuration to d's reporting configuration."
       THE STEP IS OUTSIDE THE LOOP IN THE SPEC AND NAMES A `feature` THAT IS OUT OF SCOPE THERE — an editorial
       defect in §9.6, whose intent §4.2 settles: a declared policy is «declarations, reporting configuration»,
       so what is set is the declared policy's ONE reporting configuration. This build stores none; see the
       named residual in permissions_policy.h. */
}

/* §4.2's TWO WORDS. They are the vocabulary a value CROSSES in, and they are the same two the standard uses
   everywhere else in §9 — an inherited policy is "an ordered map from features to `Enabled` or `Disabled`". A
   record spelling this build's enum integers instead would be read by whichever build received it against its
   own declaration order, which is precisely the substitution the feature TOKEN beside it exists to prevent. */
static const char *pp_value_token(PermissionsPolicyValue value)
{
    DCHECK(value == PP_ENABLED || value == PP_DISABLED,
           "§4.2's inherited policy holds a value that is neither `Enabled` nor `Disabled` — the section "
           "defines an inherited policy as a map to exactly those two, so a third is a cast that invented one");
    return value == PP_ENABLED ? "Enabled" : "Disabled";
}

/* The reverse. It stands inside a `CHECK` at its one call site rather than beside one, which is sound for the
   same reason that assert is a CHECK: a CHECK evaluates its condition in EVERY build, so the parse it performs
   is a parse the shipped build runs. The same expression under a DCHECK would be a parse that vanishes. */
static bool pp_value_of_token(const char *s, size_t n, PermissionsPolicyValue *out)
{
    if (n == 7 && strncmp(s, "Enabled", 7) == 0)  { *out = PP_ENABLED;  return true; }
    if (n == 8 && strncmp(s, "Disabled", 8) == 0) { *out = PP_DISABLED; return true; }
    return false;
}

char *permissions_policy_serialize(const PermissionsPolicy *policy)
{
    size_t n = 1;
    char *out;
    int f;

    DCHECK(policy != NULL,
           "§4.2's permissions policy was serialized for a peer instance from NOTHING — a NULL policy is a "
           "Document §9.5 never ran for, and the record that provisions an instance states what its "
           "navigable's container DID answer, never that nobody asked");
    /* THE ASSERT THAT SAYS WHAT THIS RECORD IS, and it replaces a `sizeof` equality that stood here while the
       struct held only the inherited map. That size check was a PROXY for this sentence and it has now been
       cashed: the struct DID grow a declared policy, and the growth is not the defect the check was written
       against — what would be is a policy carrying DECLARATIONS being sent where §9.5's answer belongs.
       §9.6 splits across the boundary (permissions_policy.h says how): step 1 needs the CONTAINER, which is in
       the creator's heap, and steps 2-4 need the RESPONSE, which only the child's own instance ever fetches.
       So what crosses is §9.5's result, whose declared policy is empty by construction, and the receiver runs
       permissions_policy_apply_response over ITS response. A record with declarations on it would be one
       instance applying its own response headers to a document that never received them. */
    for (f = 0; f < PP_FEATURE_N; f++)
        DCHECK(!policy->declarations.present[f],
               "§4.2's permissions policy is being serialized for a peer instance with a DECLARED POLICY on it "
               "— every policy that crosses is §9.5's own result and §9.5's declared policy is «[], []», so "
               "declarations here are a §9.6 result (this instance's response applied) being sent where a §9.5 "
               "answer belongs. The receiver runs §9.6's steps 2-4 over the response IT fetched; send it the "
               "policy BEFORE permissions_policy_apply_response ran, not after");
    for (f = 0; f < PP_FEATURE_N; f++)
        n += strlen(PP_FEATURES[f].token) + 1 + strlen(pp_value_token(policy->inherited[f])) + 1;
    out = malloc(n);
    CHECK(out != NULL, "permissions policy: OOM serializing §4.2's inherited policy for a peer instance");
    out[0] = '\0';
    for (f = 0; f < PP_FEATURE_N; f++) {
        if (f != 0) strcat(out, ";");
        strcat(out, PP_FEATURES[f].token);
        strcat(out, "=");
        strcat(out, pp_value_token(policy->inherited[f]));
    }
    return out;
}

bool permissions_policy_serialized_has_container(const char *text)
{
    /* A `CHECK` FOR THE REASON THE DESERIALIZER'S ARE: this is the field's presence, read off a record that
       crossed an instance boundary, and it decides a security capability. It is also the only guard between a
       host that stated nothing and a `strcmp` on NULL, which a DCHECK would leave in place for release. */
    CHECK(text != NULL && *text != '\0',
          "§9.5's container was read off a record that states NOTHING for it — the algorithm takes \"null or "
          "an element\" and both are answers, so an absent field is a host that stopped writing one rather "
          "than a navigable with no container, and reading it as the second grants a cross-origin child every "
          "feature its embedder holds");
    return strcmp(text, PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER) != 0;
}

PermissionsPolicy *permissions_policy_deserialize(const char *text)
{
    PermissionsPolicy *policy;
    bool seen[PP_FEATURE_N];
    const char *p;
    int f, missing = PP_FEATURE_N;

    /* EVERY REFUSAL BELOW IS A `CHECK` AND NOT A `DCHECK`, which is the one design decision in this function.
       This record crosses an INSTANCE boundary, and SECURITY.md makes the WASM instance on the other side of it
       UNTRUSTED — so a malformed record is not "the engine's own logic being wrong", it is check.h's other
       case exactly: a SECURITY / AUTHORIZATION boundary, decided from bytes this build did not write. A
       DCHECK would also be a hole rather than a judgement call, because it is compiled out: the feature-token
       search below yields an index, and in release an unmatched token would index the arrays past their end.
       THE FIRST OF THEM IS THE FIELD'S PRESENCE, and it is here rather than at each caller for the reason the
       rest are here: a host that made no statement at all arrives with nothing, and a consumer that caught it
       with a DCHECK would have the message in dev and a segfault in release. */
    CHECK(text != NULL && *text != '\0',
          "§9.5's answer was read back from a record that carries no field for it — a navigable's container "
          "either answered something or there is no container, both are facts a provisioning record states, "
          "and an absent field is the one thing that is neither");
    DCHECK(permissions_policy_serialized_has_container(text),
           "§9.5's answer was read back from a record that says there is NO container — \"container is null, "
           "return `Enabled`\" is step 1 of §9.7 and is a decision the READER of this record takes, not a "
           "policy to build: a caller that reached here has not asked whether its navigable has a container");
    policy = calloc(1, sizeof *policy);
    CHECK(policy != NULL, "permissions policy: OOM rebuilding §9.5's answer for a navigable's Document");
    for (f = 0; f < PP_FEATURE_N; f++) seen[f] = false;
    for (p = text; *p != '\0'; ) {
        const char *end = strchr(p, ';');
        const char *eq;
        PermissionsPolicyValue value = PP_DISABLED;

        if (end == NULL) end = p + strlen(p);
        /* THE SEGMENT IS FOUND BEFORE THE '=' IS, and the order is the whole of why this cannot read across a
           malformed pair: a `strchr` for '=' over the rest of the record would happily reach into the NEXT
           pair and mint a feature name spanning a ';'. */
        eq = memchr(p, '=', (size_t)(end - p));
        CHECK(eq != NULL,
              "§4.2's inherited policy arrived with a segment that is not a `<feature>=<value>` pair — this "
              "record's grammar is ';'-joined pairs and every one of them names a §4.1 supported feature, so "
              "a segment without an '=' is a producer and a consumer holding two different grammars");
        for (f = 0; f < PP_FEATURE_N; f++)
            if (strlen(PP_FEATURES[f].token) == (size_t)(eq - p) &&
                strncmp(PP_FEATURES[f].token, p, (size_t)(eq - p)) == 0)
                break;
        CHECK(f < PP_FEATURE_N,
              "§4.2's inherited policy names a feature §4.1's supported-feature set has no row for — the two "
              "instances on either side of this record are the same build, so a token neither of them shares "
              "is not §9.2's \"does not identify any recognized policy-controlled feature\" (that is a PARSE "
              "of an author's header) but a record from a build whose X-list has moved, or a peer naming a "
              "capability this user agent does not have");
        CHECK(!seen[f],
              "§4.2's inherited policy states one feature TWICE — the map has one value per feature and the "
              "second would silently win, so a repeat is a record that does not describe a policy");
        CHECK(pp_value_of_token(eq + 1, (size_t)(end - (eq + 1)), &value),
              "§4.2's inherited policy holds a value that is neither `Enabled` nor `Disabled` — those are the "
              "two words the section defines, and anything else read as one of them would decide a "
              "cross-origin child's capabilities from a byte nobody wrote");
        seen[f] = true;
        policy->inherited[f] = value;
        p = *end != '\0' ? end + 1 : end;
    }
    /* §4.3: "After a permissions policy has been initialized, its inherited policy will contain a value for
       each supported feature." A feature this record did not state is therefore not a gap to fill with a
       default — and either spelling of that default is a capability decided by an absence: `calloc` leaves
       PP_DISABLED, which refuses a feature the container allowed, and the other way round grants one it
       refused. */
    for (f = 0; f < PP_FEATURE_N; f++)
        if (!seen[f]) { missing = f; break; }
    CHECK(missing == PP_FEATURE_N,
          "§4.2's inherited policy arrived without a value for a supported feature — §4.3 makes an initialized "
          "policy hold one for EVERY feature, so a record that omits one does not describe a policy and the "
          "value this build would otherwise carry for it is whichever way its zero happens to point");
    return policy;
}

void permissions_policy_free(PermissionsPolicy *policy)
{
    if (policy == NULL)
        return;
    /* THE DECLARED POLICY'S DECLARATIONS ARE THE ONLY OWNED THING ON THIS RECORD, and pp_directive_free runs
       over every feature rather than over the present ones — see its own comment. */
    pp_directive_free(&policy->declarations);
    free(policy);
}

/* §9.10 "Is feature enabled in document for origin?", in full. */
PermissionsPolicyValue permissions_policy_is_feature_enabled_in_document(const PermissionsPolicy *policy,
                                                                        const PermissionsPolicy *report_only,
                                                                        PermissionsPolicyFeature feature,
                                                                        const Origin *origin,
                                                                        const Origin *document_origin,
                                                                        bool report)
{
    PermissionsPolicyValue result, report_only_result;

    DCHECK(report_only != NULL,
           "§9.10 was given no REPORT-ONLY permissions policy — §10.1 \"Changes to the HTML specification\" "
           "gives EVERY Document one and its insertion into HTML §7.5.1 sets it to §9.6 run with report-only "
           "True, so a Document that has an enforced policy has this one too: they are built together, from "
           "one navigable and one response, and a caller holding only the first has taken half of a creation");
    /* Steps 3 and 4: §9.9 against each of the two policies, both at the DOCUMENT's origin for the second
       argument — §9.10 passes "document's origin" as §9.9's `document origin` in both calls. */
    result = permissions_policy_check(policy, feature, origin, document_origin);
    report_only_result = permissions_policy_check(report_only, feature, origin, document_origin);
    /* Step 5: "If report is True: …". Its two arms both end in §9.13's generate-report-for-violation, which
       ends in Reporting §3.4.1 "Generate report of type with data" — and §3.4.1 runs §4.2 "Notify reporting
       observers on scope" BEFORE it looks at the endpoint, so the report is observable to a page holding a
       ReportingObserver whether or not any policy named an endpoint. Neither exists here, so both arms crash
       rather than silently swallowing a report the standard owes the page. The same reading, and the same
       refusal to gate on an endpoint, is written out at core/frame/embedder_policy.c's two queue steps. */
    if (report && result == PP_DISABLED)
        DFAIL("§9.10's report arm reached GENERATE REPORT FOR VIOLATION OF PERMISSIONS POLICY with disposition "
              "\"Enforce\" — this Document is not allowed to use a policy-controlled feature and the standard "
              "owes it a `permissions-policy-violation` report. The ANSWER is unaffected (§9.10 returns "
              "`Disabled` either way and the caller has already been refused correctly); what is missing is the "
              "observable. Build §9.13's generate-report-for-violation-of-permissions-policy-on-settings (a "
              "PermissionsPolicyViolationReportBody carrying the feature's token, the script's source position "
              "and the disposition) on top of Reporting §3.4.1 \"Generate report of type with data\" and §4.1's "
              "ReportingObserver — §9.11's endpoint is null for every policy here and is NOT what makes this "
              "unreachable, because §3.4.1 notifies observers before consulting one");
    if (report && result == PP_ENABLED && report_only_result == PP_DISABLED)
        DFAIL("§9.10's report arm reached GENERATE REPORT FOR VIOLATION OF PERMISSIONS POLICY with disposition "
              "\"Report\" — this Document's REPORT-ONLY permissions policy disallows a feature its enforced "
              "policy allows. The two policies share an inherited map and differ only in their DECLARED one, so "
              "reaching this means the response sent a `Permissions-Policy-Report-Only` that narrows a feature "
              "its `Permissions-Policy` left alone — exactly what that header is for, and exactly the case this "
              "build cannot deliver on. The ANSWER is unaffected (§9.10 returns the ENFORCED result and the "
              "caller has already been allowed correctly); what is missing is the observable: §9.14's "
              "generate-report-for-POTENTIAL-violation body on Reporting §3.4.1 \"Generate report of type with "
              "data\" plus §4.1's ReportingObserver, and §9.11's endpoint over a reporting configuration §9.2 "
              "currently parses and discards");
    /* Step 6: "Return result." */
    return result;
}
