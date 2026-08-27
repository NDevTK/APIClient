/* Permissions Policy §9 "Algorithms", over §4 "Framework"'s structures. See permissions_policy.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/permissions_policy/permissions_policy.h"

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

/* §4.2's permissions policy. THE DECLARED POLICY IS NOT A FIELD, and permissions_policy.h says why: §9.5 gives
   every policy this build creates the declared policy «[], []», the only algorithm that would give it another
   is §9.6, and the response header §9.6 reads crashes upstream (core/frame/navigation_params.c). A field whose
   sole value is the empty map would be a map no writer fills and no reader could distinguish from a hole. */
struct PermissionsPolicy {
    /* §4.3: "After a permissions policy has been initialized, its inherited policy will contain a value for
       each supported feature." A dense array is that sentence — there is no absent entry to answer for. */
    PermissionsPolicyValue inherited[PP_FEATURE_N];
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

/* §9.8 "Get feature value for origin", steps 2-4 — the whole of it for a policy whose declared policy is
 * «[], []».
 *   "1. Let policy be document's report-only permissions policy if report-only is True, or document's
 *    permissions policy otherwise." — the CALLER's choice, so it passes the policy rather than the document.
 *   "2. If policy's inherited policy for feature is `Disabled`, return `Disabled`."
 *   "3. If feature is present in policy's declared policy: …" — absent for every policy here, by §9.5.
 *   "4. Return `Enabled`."
 *
 * IT DOES NOT CONSULT THE DEFAULT ALLOWLIST, and that is the whole difference from §9.9 one function down. An
 * implementation that shared one body between them would answer a parent's inheritance question with a
 * cross-origin child's `'self'` refusal and disable a feature at every nesting level.
 * `origin` is §9.8's third argument and is READ BY STEP 3 ALONE — the step this build's declared policy makes
 * unreachable. It stays in the signature because the caller is §9.7, which passes two DIFFERENT origins to two
 * consecutive calls, and a signature that dropped it would make those two calls identical and silently collapse
 * §9.7 steps 2 and 3 into one. */
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
    return PP_ENABLED;                                        /* step 4 — step 3 is «[], []» here */
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
    /* Step 2: "If feature is present in policy's declared policy: …" — «[], []» here; see the struct. */
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
       policy «[], []»." — the empty declared policy is the absence of the field; see the struct. */
    return policy;
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
    /* THE FORCING ASSERT FOR THE ITEM ADDED NEXT. §4.2 makes a permissions policy a struct of an inherited
       policy AND a declared policy, and this build's struct holds only the first because §9.5 gives every
       policy it creates the declared policy «[], []» (see the struct above). The day §9.6's header parse lands
       and a declared policy becomes a field, this serializer would carry HALF a policy across an instance
       boundary and the peer would read the half it got as the whole — a cross-origin child silently judged
       under its container's inheritance and none of its own response's declarations. The size equality is what
       makes that impossible to add quietly: it fails the moment the struct grows. */
    DCHECK(sizeof *policy == sizeof policy->inherited,
           "§4.2's permissions policy has grown an item beside its inherited policy and this serializer still "
           "carries only the inherited map — a peer instance would then be provisioned with half a policy and "
           "no way to know it. Carry the new item on this record too (it crosses as TEXT, in the standard's own "
           "vocabulary, like everything else here) and read it back in permissions_policy_deserialize");
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
    /* NOT the constant below: §4.2's empty policy is a value of this user agent and belongs to no Document, so
       a Document release that reached it would be freeing something it never owned. */
    DCHECK(policy != permissions_policy_empty(),
           "§4.2's EMPTY permissions policy was passed to a free — it is one constant borrowed by every §9.10 "
           "report-only check, not a policy §9.5 created for a Document");
    free(policy);
}

const PermissionsPolicy *permissions_policy_empty(void)
{
    /* §4.2: "An empty permissions policy is a permissions policy that has an inherited policy which contains
       `Enabled` for every supported feature …". PP_ENABLED is 1, so this cannot be a zero-initialized static;
       it is written out, and the assertion below is what keeps that true if the enum's spelling ever moves. */
    static PermissionsPolicy empty;
    static bool built;
    int f;

    if (!built) {
        for (f = 0; f < PP_FEATURE_N; f++)
            empty.inherited[f] = PP_ENABLED;
        built = true;
    }
    DCHECK(empty.inherited[0] == PP_ENABLED,
           "§4.2's empty permissions policy does not contain `Enabled` for its first supported feature — an "
           "empty policy that reads `Disabled` reports a report-only violation for every feature of every "
           "document");
    return &empty;
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
           "gives EVERY Document one (\"which is a permissions policy, which is initially empty\"), so the "
           "absence is a caller that has not been told which policy it holds rather than a document without "
           "one; §4.2's empty policy is what a Document whose response declared none has");
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
              "policy allows. §10.1 makes a Document's report-only policy initially EMPTY and §9.6's "
              "report-only arm (the `Permissions-Policy-Report-Only` header, §9.1 with report-only True) is the "
              "only thing that ever narrows it, so reaching this means that header is now parsed and the "
              "report it demands is not built: Reporting §3.4.1 plus §9.14's body");
    /* Step 6: "Return result." */
    return result;
}
