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
 * THERE IS NO src-origin FIELD, and its absence is a statement rather than an omission. §4.7 lists one, and
 * the ONLY algorithm that ever writes one is §9.3 "Parse policy directive"'s `'src'` keyword — which reads the
 * `<iframe allow=…>` attribute, not a header. §9.4 "Process permissions policy attributes" is not built (see
 * the DFAIL in pp_define_inherited, which crashes on an `allow` attribute rather than reading it as empty), so
 * a src-origin field here would be a field with a reader and NO WRITER: exactly the shape CLAUDE.md makes
 * greppable, and the direction that reads as a permissive answer nobody computed. The day §9.3 lands it lands
 * WITH the field, and the crash upstream is what makes that impossible to forget.
 *
 * `self_origin` IS BORROWED AND MUST BE AGENT-LIFETIME. core/url/origin.h releases origin records with the
 * agent and never before ("every parked flow's delta names these records"), and a policy dies with its
 * Document, so the pointer cannot outlive its referent. */
typedef struct {
    bool          star;
    const Origin *self_origin;     /* §4.7's self-origin, or NULL */
    char        **expressions;     /* §4.7's ordered set of permissions-source-expression (owned, NUL-term) */
    int           n_expressions;
    int           cap_expressions;
} PpAllowlist;

/* §4.2's permissions policy — "a struct with the following items: inherited policy …, declared policy …", and
   §4.2's declared policy is «declarations, reporting configuration». THE REPORTING CONFIGURATION IS NOT A
   FIELD and permissions_policy.h states the residual: §9.11 is its only reader and §9.11's only callers are
   §9.13/§9.14, which do not exist here, so storing it would be a value with a writer and no reader. */
struct PermissionsPolicy {
    /* §4.3: "After a permissions policy has been initialized, its inherited policy will contain a value for
       each supported feature." A dense array is that sentence — there is no absent entry to answer for. */
    PermissionsPolicyValue inherited[PP_FEATURE_N];
    /* §4.2's DECLARATIONS: "an ordered map from features to allowlists". THE PRESENCE BIT IS SEPARATE FROM THE
       ALLOWLIST because §9.8 step 3 and §9.9 step 2 both begin "if feature is PRESENT in policy's declared
       policy" and an ABSENT feature is not an empty allowlist: absent falls through to the DEFAULT allowlist
       (`Enabled` for a same-origin document), while `camera=()` is a present allowlist that matches NOTHING.
       Reading a zeroed allowlist as absence would turn the second into the first and hand back a feature the
       server explicitly withheld. */
    bool                   declared_present[PP_FEATURE_N];
    PpAllowlist            declared[PP_FEATURE_N];
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
    /* Step 3 is the src-origin, which this build never writes; see PpAllowlist. */
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
    if (!policy->declared_present[feature])
        return false;
    *out = pp_allowlist_matches(&policy->declared[feature], origin) ? PP_ENABLED : PP_DISABLED;
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
                                                  const char *container_allow, size_t container_allow_len,
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
       §9.4 builds that container policy by running §9.3's parse-policy-directive over the `allow` ATTRIBUTE,
       and neither §9.3 nor §4.7's allowlist matching is built. A present `allow` is therefore an answer this
       component cannot compute, and it is a crash rather than a default because the default is the PERMISSIVE
       direction: `<iframe allow="autoplay 'none'">` would be read as an iframe that said nothing.
       §9.4's OTHER attribute needs no branch: its step 3 sets `allowfullscreen` into the container policy under
       the `fullscreen` feature, which §4.1 leaves out of this user agent's supported-feature set, so it names
       nothing this loop iterates over. */
    if (container_allow != NULL && container_allow_len != 0)
        DFAIL("§9.4 \"Process permissions policy attributes\" reached an `<iframe allow=…>` and this build "
              "parses none. What is missing is §9.3 \"Parse policy directive\" (semicolon-split, per-directive "
              "token list, the `'self'`/`'src'` keywords resolved against the container document's origin and "
              "the element's declared origin) and §4.7 \"Allowlists\"' matches-an-origin, which §9.7 step 5 "
              "then asks of the created Document's origin. Until both exist this container's policy is being "
              "read as EMPTY, which grants the frame every feature the attribute was written to withhold — so "
              "build the two and delete this crash rather than letting the frame inherit a permissive default");

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
                                             const Origin *origin)
{
    PermissionsPolicy *policy;
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
    DCHECK(origin != NULL, "§9.5 was asked to create a permissions policy for a navigable with no origin — the "
                           "origin is the Document's own and every §9.7 default-allowlist step compares against "
                           "it");
    policy = calloc(1, sizeof *policy);
    CHECK(policy != NULL, "permissions policy: OOM creating §9.5's policy for a navigable");
    /* Steps 2-4: "let inherited policy be a new ordered map. For each feature supported, let isInherited be the
       result of running Define an inherited policy for feature in container at origin …; set inherited
       policy[feature] to isInherited." EVERY supported feature, which is what §4.3's "will contain a value for
       each supported feature" makes the array's density mean. */
    for (f = 0; f < PP_FEATURE_N; f++)
        policy->inherited[f] = pp_define_inherited((PermissionsPolicyFeature)f, container_document_policy,
                                                   container_document_origin, container_allow,
                                                   container_allow_len, origin);
    /* Step 5: "let policy be a new permissions policy, with inherited policy inherited policy and declared
       policy «[], []»." — the calloc above IS that empty declared policy: every `declared_present` is false,
       which is "the feature is not present in the declared policy" and not "the feature has an empty
       allowlist". §9.6 is the one algorithm that turns any of them true. */
    return policy;
}

/* ---- §9.1, §9.2 and §9.6's response half ----------------------------------------------------------------- */

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
   SKIPPED and never guessed at, which is §4.1's own "user agents are not required to support every feature". */
static PermissionsPolicyFeature pp_feature_of_token(const char *name)
{
    int f;

    for (f = 0; f < PP_FEATURE_N; f++)
        if (!strcmp(PP_FEATURES[f].token, name))
            return (PermissionsPolicyFeature)f;
    return PP_FEATURE_N;
}

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
 * IT WRITES THE TWO ARRAYS RATHER THAN RETURNING A MAP, because the declared policy IS those two arrays on the
 * policy record (see the struct) and a second representation to copy out of would be a second place a feature
 * can go missing. `present`/`declared` are PP_FEATURE_N-wide and start empty, which is §9.2 step 1's "let
 * declarations be an empty ordered map".
 *
 * §9.2's REPORTING CONFIGURATION (its step 2 and its step 3's `report-to`) IS PARSED AND DISCARDED — the named
 * residual permissions_policy.h states, and the reason a `report-to` parameter is not a crash: it cannot move
 * any answer, because every answer is decided by an ALLOWLIST. */
static void pp_construct_from_dictionary(const SfDictionary *dict, const Origin *origin,
                                         bool *present, PpAllowlist *declared)
{
    int i;

    for (i = 0; i < dict->n_members; i++) {                     /* step 3: "for each feature-name → (value, params)" */
        const SfMember          *m = &dict->members[i].value;
        PermissionsPolicyFeature feature = pp_feature_of_token(dict->members[i].key);
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
        DCHECK(!present[feature],
               "§9.2 reached one policy-controlled feature TWICE from a single dictionary — RFC 9651 §4.2.2's "
               "ordered map overwrites a duplicate key in place ('all but the last instance are ignored'), so "
               "two members naming one feature is a dictionary parse that APPENDED where the section says it "
               "must overwrite, and the allowlist about to be dropped is the one the server sent last");
        present[feature]   = true;
        declared[feature]  = allowlist;
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
                                       bool *present, PpAllowlist *declared)
{
    SfDictionary dict;

    if (header_value == NULL)
        return;                                                  /* no such header: step 3's empty map */
    if (!sf_parse_dictionary(header_value, strlen(header_value), &dict))
        return;                                                  /* step 3, for a value that did not parse */
    pp_construct_from_dictionary(&dict, origin, present, declared);   /* step 4 */
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
    bool        present[PP_FEATURE_N];
    PpAllowlist declared[PP_FEATURE_N];
    int         f;

    DCHECK(policy != NULL,
           "§9.6's steps 2-4 were asked to apply a response's declared policy to NOTHING — its step 1 has "
           "already run §9.5 and returned a policy, so a NULL here is a caller that did not perform step 1");
    DCHECK(origin != NULL,
           "§9.6 was given no ORIGIN. HTML §7.5.1 passes navigationParams's origin, §9.2 makes it what the "
           "token `self` resolves to, and an allowlist whose self-origin is absent refuses the document's own "
           "origin — so a missing origin does not fail loudly, it silently narrows every `self` a server wrote");
    for (f = 0; f < PP_FEATURE_N; f++) {
        DCHECK(!policy->declared_present[f],
               "§9.6 is applying a response to a policy that ALREADY carries a declared policy — its step 1 "
               "returns §9.5's policy, whose declared policy is «[], []», so a policy with declarations here "
               "has had a response applied to it once already and the second application would be a document "
               "judged under two responses' headers at once");
        present[f] = false;
        memset(&declared[f], 0, sizeof declared[f]);
    }
    /* Step 2: "let d be the result of running Process response policy given response, origin and
       report-only." The report-only choice is the CALLER's, spelled as WHICH field value it passed. */
    pp_process_response_policy(header_value, origin, present, declared);
    /* Step 3: "for each feature → allowlist of d's declarations: if policy's inherited policy[feature] is true,
       then set policy's declared policy's declarations[feature] to allowlist."
       "TRUE" IS §4.2's `Enabled` — the section defines an inherited policy as a map to `Enabled` or `Disabled`
       and §9.6 writes the boolean spelling of it — and the test is what stops a response from re-granting a
       feature its EMBEDDER already refused: a cross-origin child whose parent disallowed `autoplay` cannot
       declare it back, and the declaration is DROPPED rather than stored-and-overruled, which is the difference
       §9.11's endpoint would later be able to see. */
    for (f = 0; f < PP_FEATURE_N; f++) {
        if (present[f] && policy->inherited[f] == PP_ENABLED) {
            policy->declared_present[f] = true;
            policy->declared[f]         = declared[f];
        } else {
            pp_allowlist_free(&declared[f]);
        }
    }
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
        DCHECK(!policy->declared_present[f],
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
    int f;

    if (policy == NULL)
        return;
    /* THE DECLARED POLICY'S ALLOWLISTS ARE THE ONLY OWNED THING ON THIS RECORD, and the loop runs over every
       feature rather than over the present ones: §4.7's expressions array is allocated by
       pp_allowlist_add_expression, `declared_present` is what §9.8/§9.9 READ, and an allowlist built and then
       dropped by §9.6 step 3 is freed at its site. Reading the presence bit here would tie a FREE to a flag
       whose meaning is about ANSWERS — the shape that leaks the day a policy is stored without being present. */
    for (f = 0; f < PP_FEATURE_N; f++)
        pp_allowlist_free(&policy->declared[f]);
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
