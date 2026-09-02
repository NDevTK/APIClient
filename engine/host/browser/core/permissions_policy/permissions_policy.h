/* PERMISSIONS POLICY — the W3C Permissions Policy framework (§4 "Framework") and the algorithms that answer
 * it (§9 "Algorithms"). See permissions_policy.c.
 *
 * WHAT THIS ANSWERS AND WHY IT IS ONE COMPONENT. HTML asks "is this Document ALLOWED TO USE feature X" in
 * several unrelated places — §7.2.2.6 "Script settings for Window objects" makes it the second conjunct of an
 * environment's CROSS-ORIGIN ISOLATED CAPABILITY, §6.6.6's allow focus steps make it the first clause of
 * whether a script may move focus — and each of those sites had WRITTEN THE ANSWER OUT FOR ITSELF as "is this
 * document same origin with the top-level traversable's active document". That approximation is not merely
 * duplicated, it is WRONG, and the shape it is wrong on is ordinary: A(top) → B(cross-origin) → C(same origin
 * as A). C is same origin with the top and its permissions policy is DISABLED, because §9.7's inheritance is a
 * CHAIN — every link must permit the feature, and B's link does not. One question asked in one place is what
 * makes that answerable at all; N copies of a shortcut is N chances to be wrong the same way.
 *
 * A POLICY IS PER-DOCUMENT STATE AND NEVER A MODULE STATIC. §9.5 creates one FOR A NAVIGABLE from the
 * navigable's container and the Document's origin, so two Documents of one agent legitimately hold different
 * answers — an `<iframe>` whose document is cross-origin with its parent is disallowed a 'self' feature its
 * parent is allowed. A static would answer the FIRST document's question for every later one, which is the
 * defect class CLAUDE.md names for per-realm facts. The one thing that IS static here is §4.1's set of
 * SUPPORTED FEATURES and their §4.8 default allowlists, which are constants of this user agent rather than
 * facts about a document.
 *
 * §4.1: "A user agent has a set of supported features, which is the set of features which it allows to be
 * controlled through policies. User agents are not required to support every feature." THAT SET IS THE X-LIST
 * BELOW, and it is a UNION OVER SPECIFICATIONS rather than one document's list: HTML §2.2 "Policy-controlled
 * features" defines `autoplay`, `cross-origin-isolated` and `focus-without-user-activation`, each "which has a
 * default allowlist of 'self'", and FULLSCREEN API §7 "Permissions Policy Integration" defines `fullscreen`
 * with the same default allowlist. A feature defined by a specification this build does not implement
 * (Geolocation's `geolocation`) is not in the supported set, and §9.2 and §9.3 both say what that means for
 * parsing — "if feature-name
 * does not identify any recognized policy-controlled feature, then continue" — so an unsupported token is
 * SKIPPED rather than guessed at. On the ASKING side there is no such softness and no runtime check either:
 * the question is an ENUM, so a component that needs to ask about a feature this build does not support does
 * not compile. That is the forcing function, and it is stronger than a crash because it fires before the
 * program runs. Adding a row here means stating the feature's default allowlist FROM ITS OWN SPECIFICATION —
 * §4.8's value is normative and inventing one is a security answer nobody measured.
 *
 * THE DECLARED POLICY IS §9.5'S «[], []» UNTIL §9.6 FILLS IT, AND §9.6 IS THE RESPONSE'S OWN HEADER. "Let
 * policy be a new permissions policy, with inherited policy inheritedPolicy and declared policy «[], []»" is
 * §9.5's last step, and §9.6 "Create a Permissions Policy for a navigable from response" is the one algorithm
 * that ever changes it: its step 2 runs §9.1 "Process response policy" over the response's
 * `Permissions-Policy` header, and its step 3 copies each resulting allowlist onto the policy for every
 * feature the INHERITED policy still permits.
 *
 * WHY THE SILENCE THAT PRECEDED IT WAS DANGEROUS AND NOT MERELY INCOMPLETE. §9.5's «[], []» is the MOST
 * PERMISSIVE declared policy there is: §9.9 step 2 is skipped entirely when a feature is absent from it, so
 * the answer falls through to §9.9's own default-allowlist steps (its steps 3 and 4, which §9.8 does not have)
 * and every restriction the server wrote is read as
 * silence. A build that parsed no header would therefore not be "missing a feature" — it would be answering
 * `Enabled` for documents a real browser answers `Disabled` for, which for `cross-origin-isolated` is a
 * capability decided by an absence. That is why this used to be a CRASH at the bytes rather than a default,
 * and why the crash is deleted in the same diff that makes the parse real.
 *
 * §9.2's REPORTING CONFIGURATION IS A NAMED RESIDUAL AND IS NOT STORED. §9.2 step 3 sets
 * `reporting-config[feature]` from a Member Value's `report-to` parameter, and the only algorithm that reads
 * one is §9.11 "Get the reporting endpoint for a feature", whose only callers are §9.13's and §9.14's
 * generate-a-report steps. Those steps do not exist here — §9.10's two DFAILs name them and the Reporting
 * §3.4.1 machinery under them — so a stored map would be a value with a computed writer and NO READER, which
 * is the mirror of the defect CLAUDE.md makes greppable and is harder to see because the value would be real.
 *   — WHAT IS NOT COVERED: a `report-to` parameter on a Member Value, which this build parses and discards.
 *   — WHAT THE NEXT DIFF BUILDS: §9.11 over a reporting-configuration map, landing WITH §9.13/§9.14's report
 *     bodies on Reporting §3.4.1, at which point the map has a reader on the same diff that writes it.
 *   — HOW ITS ABSENCE WOULD SHOW: it cannot move any §9.8, §9.9 or §9.10 ANSWER, because those are decided by
 *     an ALLOWLIST and a parameter never touches one. It shows the first time a report is generated and names
 *     no endpoint — and §9.10's two DFAILs stand in front of that, so it cannot be reached in silence.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_PERMISSIONS_POLICY_PERMISSIONS_POLICY_H
#define ENGINE_HOST_BROWSER_CORE_PERMISSIONS_POLICY_PERMISSIONS_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "core/url/origin.h"

/* §4.1's SUPPORTED FEATURES, with §4.8's DEFAULT ALLOWLIST of each. The allowlist is a column of the row rather
   than a fact the code assumes, because §4.8 admits two values and a `*` feature inherits by a different rule:
   it is "allowed in Documents in top-level traversables by default, as well as those in all child navigables",
   where 'self' stops at the first cross-origin link.
   EACH ROW STATES THE STANDARD THAT DEFINES IT, because §4.1's set is a UNION over specifications and not a
   list one document owns. The first three are HTML §2.2 "Policy-controlled features", each "which has a default
   allowlist of 'self'". The fourth is FULLSCREEN API §7 "Permissions Policy Integration", whose whole first
   sentence is the row: "This specification defines a policy-controlled feature identified by the string
   "fullscreen". Its default allowlist is 'self'." A row's default allowlist is NORMATIVE in its own
   specification and inventing one is a security answer nobody measured. */
#define PERMISSIONS_POLICY_FEATURES(X)                                                        \
    X(AUTOPLAY,                      "autoplay",                      PP_ALLOWLIST_SELF)      \
    X(CROSS_ORIGIN_ISOLATED,         "cross-origin-isolated",         PP_ALLOWLIST_SELF)      \
    X(FOCUS_WITHOUT_USER_ACTIVATION, "focus-without-user-activation", PP_ALLOWLIST_SELF)      \
    X(FULLSCREEN,                    "fullscreen",                    PP_ALLOWLIST_SELF)

typedef enum {
#define PP_ENUM_ROW(name, token, allowlist) PP_FEATURE_##name,
    PERMISSIONS_POLICY_FEATURES(PP_ENUM_ROW)
#undef PP_ENUM_ROW
    PP_FEATURE_N            /* also §9.2/§9.3's "does not identify any recognized policy-controlled feature" */
} PermissionsPolicyFeature;

/* §4.8 "Default Allowlists": "The default allowlist for a feature is one of these values: * … 'self' …".
   TWO VALUES AND NOT THREE — 'none' is a keyword an allowlist may be WRITTEN with (§4.7) and is not a default
   allowlist, so a row spelling one would be spelling a value §4.8 does not define. */
typedef enum { PP_ALLOWLIST_STAR, PP_ALLOWLIST_SELF } PermissionsPolicyDefaultAllowlist;

/* Permissions Policy §4.2 "Policies": an inherited policy is "an ordered map from features to `Enabled` or
   `Disabled`", and every §9 algorithm returns one of the same two words. They are the SAME type because the
   algorithms compose — §9.10's result IS §9.9's return value — and a `bool` at any joint would let
   "Disabled" and "false" be read for each other by a caller that meant one of them. */
typedef enum { PP_DISABLED = 0, PP_ENABLED = 1 } PermissionsPolicyValue;

/* Permissions Policy §4.2's PERMISSIONS POLICY — "a struct with the following items: inherited policy …,
   declared policy …". OPAQUE AND HEAP-ALLOCATED, owned by the Document §9.5 created it for, exactly like
   §7.1.7's policy container beside it on that record. A `PermissionsPolicy *` that is NULL is a Document
   that HAS no permissions policy — a Document with no browsing context, which HTML §4.8.5's "allowed to use"
   step 1 refuses outright — and is never a policy whose contents are unknown. */
typedef struct PermissionsPolicy PermissionsPolicy;

/* §4.1's token for a feature, and §4.8's default allowlist of it. The token is what §9.2 and §9.3 match a
   header or an `allow` attribute against, and what a report's `featureId` would name. */
const char *permissions_policy_feature_token(PermissionsPolicyFeature feature);
PermissionsPolicyDefaultAllowlist permissions_policy_default_allowlist(PermissionsPolicyFeature feature);

/* §9.5 "Create a Permissions Policy for a navigable" — "given null or an element (container) and an origin
 * (origin) this algorithm returns a new Permissions Policy": one §9.7 inherited-policy value per supported
 * feature, and §9.5's own «[], []» declared policy.
 *
 * §9.7 IS THE WHOLE OF WHAT THE ARGUMENTS ARE FOR, and they are the five things it reads off the container:
 * `container_document_policy` is "container's node document's" permissions policy (its §9.7 steps 2 and 3),
 * `container_document_origin` is "container's node document's origin" (its step 7 — and §9.4 step 2's second
 * argument, which is what the keyword `'self'` resolves to inside an `allow` attribute), `container_allow`/
 * `container_allow_len` are the value of the `allow` attribute, `container_allowfullscreen` is whether the
 * element's `allowfullscreen` attribute is SPECIFIED (§9.4 step 3's condition — see below), and
 * `container_declared_origin` is §7.2 "The
 * permissionsPolicy object"'s DECLARED ORIGIN of the element — §9.4 step 2's third argument, and what the
 * keyword `'src'` resolves to. A TOP-LEVEL DOCUMENT passes NULL for all of them, which is §9.7 step 1's "if
 * container is null, return `Enabled`" — the absence of a container is a POSITIVE statement about this
 * navigable and never a missing argument, which is why they are absent TOGETHER and asserted to be. The
 * attribute arrives as BYTES AND A LENGTH because that is how the DOM holds an attribute value; a container
 * element that carries no `allow` attribute at all passes NULL, which §9.4 step 2 reads as the empty directive
 * rather than as a missing input.
 *
 * §9.4 STEP 1 IS THE CALLER'S, AND IT IS SPELLED AS AN ABSENT ATTRIBUTE. "If element is not an `iframe`
 * element, then return an empty policy directive" is a question about what an element IS, and this component
 * does not know what an element is; the caller that does states the answer by passing NULL for `container_allow`
 * and for `container_declared_origin`, and false for `container_allowfullscreen`. So an `<object>` or `<embed>`
 * container reaches §9.7 steps 6-8 with no
 * branch here, and the declared origin — which §7.2 computes for any Element — is not read for a container
 * whose attribute the standard does not honour.
 *
 * §9.4 STEP 3 IS A SECOND, NARROWER DELIVERY OF ONE FEATURE AND IT IS A BOOLEAN AND NOT AN ALLOWLIST. "If
 * element's `allowfullscreen` attribute is specified, and container policy does not contain an entry for the
 * `fullscreen` feature", then its step 3.1 sets "container policy[fullscreen] = the special value `*`". So the
 * attribute carries no VALUE into the policy — the value is fixed by the step — and what the caller states is
 * only whether the attribute is THERE, which is why this is a `bool` beside the `allow` attribute's bytes
 * rather than a second string. It is a BOOLEAN CONTENT ATTRIBUTE and therefore SPECIFIED-or-not, never
 * true-or-false — HTML §2.3.2 "Boolean attributes": "The presence of a boolean attribute on an element
 * represents the true value, and the absence of the attribute represents the false value" — so
 * `allowfullscreen="false"` is a conforming-invalid spelling of TRUE and never of false.
 *   THE STEP IS ORDERED AFTER THE PARSE AND ITS CONDITION IS WHY. `allow` is processed first, so
 * `<iframe allowfullscreen allow="fullscreen 'none'">` keeps the `'none'` the author wrote: the entry EXISTS,
 * so step 3's second conjunct is false and the attribute adds nothing. FULLSCREEN §7 states the same rule from
 * the other end — "Unless overridden by the `allow` attribute, setting `allowfullscreen` on an `iframe` is
 * equivalent to `<iframe allow="fullscreen *">`" — and reading the two steps in the other order would make the
 * legacy attribute silently overrule the modern one.
 *
 * THE DECLARED ORIGIN IS THE ELEMENT'S AND NOT THE CREATED DOCUMENT'S, and they legitimately differ: §7.2
 * computes it from the `src` attribute the embedder WROTE, so a frame that redirected elsewhere, or that
 * inherited an `about:blank`, is checked against `origin` (the Document's own) and does not match a `'src'`
 * allowlist. That is the keyword meaning what it says rather than a narrowing.
 *
 * THE CONTAINER DOCUMENT'S POLICY AND ORIGIN ARRIVE TOGETHER OR NOT AT ALL, and that is asserted. They are two
 * facts about one element, so half of them is not a partial container — it is a container in ANOTHER WASM
 * INSTANCE (SECURITY.md keys an instance on `(browsing context group, origin)`, so a cross-origin container
 * document's policy is not a pointer this heap holds), and reading that as "no container" takes §9.7 step 1 and
 * enables every feature for the one child that must not have them. THIS FUNCTION IS NOT WHERE THAT CASE IS
 * ANSWERED and it has no branch that could quietly take the other road: the instance whose heap holds the
 * element runs §9.5 there and its ANSWER crosses (the serialization below), so the instance that holds the
 * navigable never calls this function for such a container at all — it reads the answer back.
 *
 * `origin` is the origin of the Document being created in the navigable. OWNED by the caller: free with
 * permissions_policy_free. */
PermissionsPolicy *permissions_policy_create(const PermissionsPolicy *container_document_policy,
                                             const Origin *container_document_origin,
                                             const char *container_allow, size_t container_allow_len,
                                             bool container_allowfullscreen,
                                             const Origin *container_declared_origin,
                                             const Origin *origin);
void permissions_policy_free(PermissionsPolicy *policy);

/* ---- §9.6's RESPONSE HALF ---------------------------------------------------------------------------------
 *
 * WHAT A RESPONSE STATES ABOUT A DOCUMENT'S PERMISSIONS POLICY, IN THE FORM IT TRAVELS. HTML §7.5.1 "Shared
 * document creation infrastructure" runs "creating a permissions policy from a response" given
 * "navigationParams's navigable's container, navigationParams's origin, and navigationParams's response", and
 * §10.1 "Changes to the HTML specification" inserts the SAME call with report-only True beside it. So the
 * response is an input to DOCUMENT CREATION, which happens later than the fetch — and CLAUDE.md's rule (an
 * operation that becomes a work item takes its inputs WITH it) is why this is a value carried from
 * core/frame/navigation_params.c rather than a header list read back at the install.
 *
 * IT IS THE TWO FIELD VALUES AND NOT THE HEADER LIST, because §9.1 reads exactly one name out of that list per
 * call and Fetch §2.2.2 step 2's "get" is what turns repeated headers into the one string it parses. Carrying
 * the got values IS carrying the response as far as §9.1 can see it, and it is what lets the response be freed
 * at the fetch — which this engine does, deliberately, so that nothing the bundle runs can ask for a header
 * again.
 *
 * BOTH MAY BE NULL AND THAT IS A POSITIVE STATEMENT: a response carrying neither header, and a Document
 * created from NO response at all (§7.3.2.1's initial about:blank), agree — §9.1 step 3 returns an empty
 * ordered map for both, and §9.6's merge then has nothing to copy, so the policy is §9.5's. There is no third
 * state to spell and therefore no second constructor: an absent header and a header declaring nothing are the
 * same declared policy, which is Fetch's own note about a structured field that is missing or malformed.
 *
 * IT IS BUILT THROUGH A FUNCTION AND NEVER BY AN INITIALIZER, for SerializedPolicyContainer's reason: a
 * designated initializer zero-fills what it does not name, so the day a third delivery lands (§6.2's `allow`
 * attribute does not travel this way, but a `<meta>` delivery would) every producer stops compiling until it
 * states one, instead of silently carrying a NULL nobody chose. */
typedef struct {
    const char *enforced;      /* §6.1's `Permissions-Policy` field value, or NULL */
    const char *report_only;   /* §8.1's `Permissions-Policy-Report-Only` field value, or NULL */
} SerializedResponsePermissionsPolicy;

SerializedResponsePermissionsPolicy serialized_response_permissions_policy(const char *enforced,
                                                                          const char *report_only);

/* §9.6 "Create a Permissions Policy for a navigable from response", STEPS 2 THROUGH 4 — over a policy its step
 * 1 has already produced.
 *
 * STEP 1 IS THE CALLER'S BECAUSE IT HAS TWO IMPLEMENTATIONS AND ONLY THE CALLER KNOWS WHICH. "Let policy be
 * the result of running Create a Permissions Policy for a navigable given container and origin" is
 * permissions_policy_create for a navigable whose §7.3.1.3 container is an element in THIS heap, and it is
 * permissions_policy_deserialize for one whose container is in another WASM instance — where §9.5 ran in the
 * instance that HELD the element and its answer crossed. Folding step 1 in here would mean this function
 * taking either an element's three facts or a peer's record, which is one function answering two questions.
 * Steps 2-4 are identical for both, which is the half that must not be written twice.
 *
 * `header_value` is the field value §9.1 step 2 gets — the enforced or the report-only one, chosen by the
 * CALLER, which is §9.1 step 1 ("let header name be `Permissions-Policy-Report-Only` if report-only is True,
 * or `Permissions-Policy` otherwise"). NULL is a response that carried none.
 * `origin` is §9.2's origin — the origin of the Document being created — and is what `self` resolves to. It is
 * BORROWED and must be an AGENT-LIFETIME record (core/url/origin.h releases these with the agent and never
 * before), because §4.7's self-origin is stored on the allowlist and outlives this call by the life of the
 * Document. */
void permissions_policy_apply_response(PermissionsPolicy *policy, const char *header_value,
                                       const Origin *origin);

/* ---- §4.2's PERMISSIONS POLICY IN THE FORM IT CROSSES AN INSTANCE BOUNDARY --------------------------------
 *
 * WHY A POLICY CROSSES AT ALL, WHEN §9.5 TAKES AN ELEMENT. SECURITY.md keys a WASM instance on
 * `(browsing context group, origin)`, so a cross-origin `<iframe>` is the ROOT of a PEER instance and the
 * HTML §7.3.1.3 "Child navigables" CONTAINER of that peer's navigable is an element in ANOTHER HEAP. §9.5's
 * two arguments are that element and the origin of the Document being created in the navigable, and BOTH of
 * them belong to the CREATOR: the element is in its tree, and the child's origin is what its own create
 * computed and already puts on the record that provisions the peer. So §9.5 runs ONCE, in the heap where its
 * arguments are, and what crosses is its RESULT.
 *
 * IT IS THE RESULT AND NOT THE INPUTS, and that is not a preference between two carriers. §9.7's steps 2 and
 * 3 run §9.8 over the CONTAINER DOCUMENT's own permissions policy at TWO different origins, its step 5 matches
 * §9.4's container policy against the child's origin, and its step 7 compares the two origins — so "the
 * inputs" is that document's WHOLE policy (its inherited map AND its declared policy's allowlists), its
 * origin, and the `allow` attribute's bytes, and a peer handed them would be a SECOND site evaluating §9.7.
 * One question answered in two places is the defect the top of this file is written against.
 *
 * AND WHAT CROSSES IS §9.5'S RESULT, WHOSE DECLARED POLICY IS EMPTY BY CONSTRUCTION — which is why this record
 * carries the INHERITED MAP ALONE and why that is not the half-a-policy it would otherwise be. §9.6 splits into
 * a step 1 that needs the CONTAINER (the creator's heap) and steps 2-4 that need the RESPONSE (the child's own,
 * fetched by the child's own instance), and the two live on opposite sides of the boundary. So the creator
 * sends §9.5's answer and the RECEIVER runs permissions_policy_apply_response over the response IT fetched;
 * neither side evaluates the other's half. A record that carried declarations would therefore be a §9.6 result
 * sent where a §9.5 answer belongs — one instance applying another instance's response headers to a document
 * that never received them — and permissions_policy_serialize ASSERTS against exactly that rather than
 * against the struct's size, because the size was only ever a proxy for this sentence.
 *
 * THE GRAMMAR IS §4.1's TOKENS AND §4.2's TWO WORDS — `<token>=<Enabled|Disabled>`, ';'-joined. The standard's
 * own vocabulary rather than this build's declaration order, for the reason HTML §7.1.4's values cross their
 * own record as tokens: a record written by a build whose X-list has since moved would otherwise resolve to a
 * DIFFERENT feature with nothing anywhere to say so. EVERY supported feature appears EXACTLY ONCE and the
 * reader REFUSES a record that omits one — §4.3 makes an initialized policy hold "a value for each supported
 * feature", so a missing feature is not a permissive default to fill in, it is a record that does not describe
 * a policy. Neither ';' nor '=' nor a TAB can occur in a §4.1 token (they are the `allow`-attribute and
 * header vocabulary) or in `Enabled`/`Disabled`, which is what makes this safe as ONE field of the
 * tab-delimited notice that provisions a peer instance.
 *
 * `permissions_policy_serialize` returns a malloc'd record the caller frees. */
char *permissions_policy_serialize(const PermissionsPolicy *policy);

/* §9.5's "null or an element (container)", spelled in that same record — the POSITIVE statement that the
   navigable this record provisions has NO §7.3.1.3 container, which is exactly what an AUXILIARY navigable is
   (§7.3.1.7 step 8 creates one out of a target name, with no element anywhere in the algorithm). It holds no
   '=', so no reader can mistake it for a pair, and it is not the empty string, which a reader could default.
   Asked through this predicate rather than compared inline, so that every seam answers §9.5's "container is
   null" from one place — the same rule core/frame/policy_container.h states for §7.1.7's `is not null`. */
#define PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER "null"
bool permissions_policy_serialized_has_container(const char *text);

/* The policy such a record describes, minted for the Document being created in that navigable. OWNED by the
   caller: free with permissions_policy_free. The text must describe a container (the predicate above), because
   the absence is §9.7 step 1's answer and is a decision its READER takes rather than a policy to build. */
PermissionsPolicy *permissions_policy_deserialize(const char *text);

/* §4.2's EMPTY PERMISSIONS POLICY IS GONE FROM THIS INTERFACE, and the deletion is the point rather than a
   simplification. It existed as the stand-in for §10.1's REPORT-ONLY permissions policy while no header could
   be parsed — one constant handed to every §9.10 check — and §9.6's report-only arm now gives every Document
   a real one, built by §9.5 over its own navigable and narrowed by its own `Permissions-Policy-Report-Only`.
   With that, nothing read the constant: a value with a writer and no reader is not a mechanism, and keeping
   one that ANSWERS A SECURITY QUESTION would leave a second, more permissive report-only policy available to
   whichever caller reached for it next. §4.2's empty policy is not a value this user agent needs a name for —
   it is what a Document with no navigable would have, and §4.8.5's allowed-to-use refuses such a Document at
   its step 1 without ever asking a policy. */

/* §9.9 "Check permissions policy" — "given permissions policy (policy), a feature (feature), an origin
   (origin) and another origin (document origin), this algorithm returns `Disabled` if feature should be
   considered disabled, and `Enabled` otherwise".
   IT IS NOT §9.8, and the two differ in the step that matters: §9.8 "Get feature value for origin" ends at
   `Enabled` when the feature is absent from the declared policy, while §9.9 goes on to consult the feature's
   DEFAULT ALLOWLIST. §9.8 is the question the INHERITANCE asks of a parent, §9.9 is the question a USE asks of
   its own document, and swapping them silently widens or narrows every cross-origin frame. */
PermissionsPolicyValue permissions_policy_check(const PermissionsPolicy *policy,
                                                PermissionsPolicyFeature feature,
                                                const Origin *origin, const Origin *document_origin);

/* §9.10 "Is feature enabled in document for origin?" — "given a feature (feature), a Document object
 * (document), an origin (origin), and an optional boolean (report), with a default value of True".
 *
 * IT TAKES THE DOCUMENT'S TWO POLICIES RATHER THAN THE DOCUMENT, because the algorithm's own first two steps
 * are "let policy be document's permissions policy" and "let report-only policy be document's report-only
 * permissions policy" and nothing after them reads the Document again except to generate a report. A component
 * that reached for a Document would be a permissions-policy file that knows what a Document is; the caller
 * that HAS one (core/dom/document.c, which owns HTML §4.8.5's "allowed to use") passes the two.
 *
 * `report` IS THE SPEC'S PARAMETER AND IS REAL. Its True arm reaches §9.13/§9.14's generate-and-queue-a-report,
 * which this build cannot perform — Reporting §3.4.1 "Generate report of type with data" notifies
 * ReportingObservers BEFORE any endpoint is consulted, so a violation is observable whether or not a policy
 * named an endpoint, and neither the observer nor the queue exists here. That arm therefore CRASHES by name.
 * §9.10's own note is what a caller uses to stay off it honestly: "if a call to this algorithm is performed
 * just to query the state of a feature, and does not represent an actual attempted use of the feature, then
 * report should be set to False." HTML §4.8.5's "allowed to use" is not such a call and passes True. */
PermissionsPolicyValue permissions_policy_is_feature_enabled_in_document(const PermissionsPolicy *policy,
                                                                        const PermissionsPolicy *report_only,
                                                                        PermissionsPolicyFeature feature,
                                                                        const Origin *origin,
                                                                        const Origin *document_origin,
                                                                        bool report);

#endif
