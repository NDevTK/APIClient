/* THE POLICY CONTAINER — HTML §7.1.7 "Policy containers", over CSP's own policy/directive model.
 *
 * WHY IT EXISTS, AND WHY IT IS THE ANSWER TO how an about:blank child gets a policy — a question of this
 * tree's and not a sentence of §7.1.7's. A navigable created with no URL — `window.open()` with no argument,
 * `<iframe>` with no src — gets its initial `about:blank`
 * Document SYNCHRONOUSLY, and that Document has no response to take a policy from. HTML's answer is not a
 * special case: every Document has a POLICY CONTAINER, and §7.3.2.1 "Creating browsing contexts" says that when
 * creator is non-null, "set document's policy container to a clone of creator's policy container". So the child
 * inherits the parent's CSP by the ordinary rule, not by an inheritance rule written for CSP.
 *
 * AND WHICH CONTAINER A DOCUMENT IS CREATED WITH IS §7.1.7's OWN ALGORITHM, WHICH LIVES HERE ONCE. Two program
 * entries create a Document from a host's statement of a response and a creator, and a rule spelled at both is
 * two rules: determine-navigation-params-policy-container is one function at the bottom of this file, and its
 * predicate is the standard's `responseURL is local` rather than the approximation "the response carried no
 * policy" — which puts every third-party frame of a CSP-bearing page under its embedder's policy.
 *
 * WHAT IS IN ONE IS §7.1.7'S FIVE ITEMS AND NO OTHERS — a CSP list, an EMBEDDER POLICY, a referrer policy, an
 * integrity policy and a report-only integrity policy. This line said "and sandboxing flags", which is the
 * claim five predicates in this tree were once written against and which the standard does not make: a
 * Document's ACTIVE SANDBOXING FLAG SET is a row of §7.5.1's creation table BESIDE the container
 * (core/frame/sandboxing.h), and so is its OPENER POLICY. What the container contributes to the flag set is one
 * half of it, through the CSP `sandbox` directive alone — the function at the bottom of this file, and the only
 * connection between the two.
 *
 * THE EMBEDDER POLICY IS AN ITEM OF THE CONTAINER AND NOT A FACT ABOUT A RESPONSE, which is the whole reason
 * it lives here rather than beside §7.1.4's obtain. A Document created from no response has one (§7.1.7:
 * "initially a new embedder policy"), a Document created by a CREATOR inherits the creator's through the same
 * clone that moves the CSP list, and only a Document created from a RESPONSE gets the obtained one — three
 * answers to one item, all of which are the container's. `EmbedderPolicy` is OWNED here, so its two endpoint
 * strings are copied by the clone and freed by the free, and the two functions are written next to each other
 * because a field added to one and not the other is the defect that shape produces.
 *
 * AND THE CLONE CROSSES INSTANCES. A cross-origin creator and the child it clones from are not in the same
 * heap, so the clone is serialized as CSP TEXT and parsed again on the other side — which is exactly what
 * policy_container_clone does in one heap, so the two are one operation and the transport needs no second
 * representation. Nothing here may be shaped to avoid that transport; see policy_container.h.
 *
 * IT IS NOT YET PER-FLOW, and the place that will make it so is asserted rather than described. A flow that
 * NAVIGATES a frame replaces its policy container while a sibling that did not still holds the old one, so the
 * binding is per-flow state for the same reason the WindowProxy binding is. document_install CRASHES on a
 * second install rather than carrying a comment promising to handle it, and its `@WHY` names the record to
 * build (a COW record like ProxyData's PROXY_REC, captured in the accessor).
 *
 * AND THE INSTALL IS NO LONGER THE ONLY WRITE, WHICH IS WHAT MAKES THAT RECORD LOAD-BEARING RATHER THAN
 * HYPOTHETICAL. HTML §4.2.5.3 "Pragma directives" runs at a `<meta>` element's INSERTION, so a flow that
 * inserts one APPENDS a policy to a container the baseline built (policy_container_enforce_policy). That
 * append grows the container IN PLACE — there is one pointer, held by the Document, and a replacement would
 * leave that Document naming one container while the appending flow named another — so a sibling flow that
 * never ran the inserting script reads the grown list. That is the SAFE half of the two ways to be wrong:
 * §2.2 says "multiple policies can be applied to a single resource" and §4.1.2 blocks if ANY of them is
 * violated, so a policy the sibling did not earn can only make its verdict STRICTER — it suppresses a finding
 * rather than fabricating one, which is why the append lands ahead of the record instead of behind it. (The
 * narrowing is that QUANTIFIER's and is nowhere stated as a sentence of §2.2, which is what this line used to
 * put in quotation marks.) The residual is stated at the function, with what would show if it fired.
 *
 * THERE IS NO CSP PARSER IN THIS FILE ANY MORE, AND THAT IS THE POINT. It used to carry one: a `Directive`
 * struct of six booleans, a source-list scan that set them, and a `DCHECK` that ABORTED on any source
 * expression it did not model — a nonce, a hash, `https:`, `*.example.com`. That crash read as an honest gap
 * and was not one; it was naming a question this file had invented. CSP's own §6.7.3.2 decides whether inline
 * content runs by classifying a source list with exactly three tests and IGNORING every other expression, so
 * the real algorithm is total over every policy the web sends. The model those questions are now asked of is
 * core/frame/csp_directive_list.h (§2.2/§2.3) and core/frame/csp_source_list.h (§2.3.1/§6.7.3/§6.7.2), and
 * what is left here is HTML's container and the questions asked OF a container: §4.2.3's about inline content
 * of a type, §4.4.1's about string compilation, and CSP §4.1.2's about a URL — Fetch's main fetch step 7. All
 * three are here rather than in files of their own because they are the SAME walk over the same list with a
 * different question per policy, and because the quantifier that makes a second policy narrow — every policy
 * asked, content permitted only if EVERY one permits it — is then stated once. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
/* §6.7.1.1 step 1's gate is Fetch §2.2.5's SCRIPT-LIKE, and it is asked of Fetch — see fetch.h. */
#include "core/fetch/fetch.h"
#include "core/frame/csp_directive_list.h"
#include "core/frame/csp_source_list.h"
#include "core/frame/policy_container.h"
#include "core/frame/sandboxing.h"
/* NAMED RATHER THAN INHERITED THROUGH A NEIGHBOUR: §7.1.7's determine step reads Fetch §2.1's local-scheme
   answer and HTML §2.4.1's about:srcdoc match off a PARSED record, so this unit is a direct user of the URL
   parser and says so. */
#include "core/url/url.h"
/* AND OF §7.1.1's SERIALIZATION, for the same reason: turning a live container into the form it crosses a seam
   in turns its CSP list's self-origin RECORD into the bytes that cross with it. */
#include "core/url/origin.h"

struct PolicyContainer {
    /* The serialized CSP list, comma-delimited, or NULL for none (OWNED). It is kept because it is what the
       container TRAVELS as — a clone across an instance or a session is this string, re-parsed — and because a
       report is worth nothing without the policy text it names. */
    char    *csp_text;
    /* §2.2's CSP LIST, parsed over `csp_text`. Every name and value token is a SLICE of it, so the two fields
       are one value with one lifetime: free them together and never reallocate the text under the list. */
    CspList  csp;
    char    *referrer_policy;    /* §7.1.7's referrer policy (owned) */
    /* §7.1.7's EMBEDDER POLICY item (owned — both of §7.1.4's endpoint strings). THE OWNED FIELDS OF THIS
       STRUCT ARE AN OBLIGATION AT THREE SITES AND NOWHERE ELSE: the constructor fills them, the clone copies
       them, the free releases them. Adding a field here without visiting all three is the defect a clone-site
       assert exists to catch, and there is one below. */
    EmbedderPolicy embedder;
};

PolicyContainer *policy_container_new(const char *csp_text, const Origin *self_origin,
                                      const char *referrer_policy, SerializedEmbedderPolicy embedder)
{
    PolicyContainer *p = calloc(1, sizeof *p);

    CHECK(p != NULL, "policy container: OOM");
    if (csp_text && *csp_text) {
        p->csp_text = strdup(csp_text);
        CHECK(p->csp_text != NULL, "policy container: OOM copying a policy");
    }
    /* THE PARSE RUNS WHETHER OR NOT THERE IS TEXT, because §2.2.2's last step does: a container with no
       policy still holds a CSP LIST, and every list has a self-origin. Running it unconditionally is what
       keeps ONE writer for that field — a container built with no text and one built with text are the same
       shape, so no reader has to ask which of the two it is holding. */
    csp_list_parse(&p->csp, p->csp_text, p->csp_text ? strlen(p->csp_text) : 0, self_origin);
    if (referrer_policy) {
        p->referrer_policy = strdup(referrer_policy);
        CHECK(p->referrer_policy != NULL, "policy container: OOM copying a referrer policy");
    }
    /* §7.1.4's ITEM, TAKING ITS OWN COPY OF BOTH ENDPOINT STRINGS. The caller's bytes belong to the operation
       creating this Document — a response's header list that is freed the moment the Document is installed, or
       a `navigable.create` record — and a container outlives every one of them. */
    embedder_policy_adopt(&p->embedder, embedder);
    return p;
}

PolicyContainer *policy_container_clone(const PolicyContainer *src)
{
    /* §7.1.7's "clone a policy container", and §7.3.2.1's answer to where an about:blank child's policy comes
       from. It is a DEEP copy: the child's policy is its own from the moment it exists, so a later navigation
       of the parent does not reach back and change what the child is allowed to do. Re-parsing rather than
       copying the parsed model is not laziness — the model's tokens point INTO the parent's text, so a
       structural copy would have to rebase every one of them, and re-parsing is the same operation the
       cross-instance and cross-session clones must perform anyway. */
    DCHECK(src != NULL, "a policy container was cloned from nothing — every document has one, including the "
                        "initial about:blank, which is the whole reason this operation exists");
    /* THE CLONE KEEPS THE SOURCE'S SELF-ORIGIN, WHICH IS THE POINT OF THE FIELD. §2.2's own note says it
       exists so that a document with an OPAQUE origin which inherited its policy still resolves `'self'`
       against the origin the policy came FROM — so re-deriving one here from the child would be the
       inheritance failing in exactly the case the field was added for. The RECORD travels, so the two
       containers share an identity as well as a tuple; where this clone crosses an instance instead, the
       origin crosses as its serialization like every other cross-boundary fact. */
    /* THE CLONE-SITE ASSERT §7.1.4'S ITEM CREATED, AND IT IS ABOUT THE OBLIGATION RATHER THAN ABOUT THIS
       PARTICULAR FIELD. A struct copied field-by-field must dup EVERY owned field, so an item added to
       `PolicyContainer` creates a duty at the constructor, at this clone and at the free — and the one of the
       three that fails SILENTLY is this one, because a clone that drops an item produces a container that is
       merely WRONG rather than one that crashes. §7.1.4 makes both endpoints strings whose absence is the
       EMPTY one, so a NULL here is a container whose embedder item its constructor never filled: the shape a
       new item takes on the day somebody adds it to the struct and not to policy_container_new. */
    DCHECK(src->embedder.endpoint != NULL && src->embedder.report_only_endpoint != NULL,
           "§7.1.7's clone-a-policy-container reached a container whose §7.1.4 EMBEDDER POLICY was never "
           "filled in — every container has one (initially a new embedder policy, whose endpoints are the "
           "EMPTY STRING and never null), so this container was built somewhere that does not go through "
           "policy_container_new, and the clone would carry an item the source does not have");
    return policy_container_new(src->csp_text, src->csp.self_origin, src->referrer_policy,
                                serialized_embedder_policy_of(&src->embedder));
}

void policy_container_enforce_policy(PolicyContainer *p, const char *serialized_policy)
{
    /* HTML §4.2.5.3 "Pragma directives"' content security policy state step 5 — see policy_container.h for why
       an append is the whole of "enforce", and for the residual this shares with document_install's DCHECK. */
    const Origin *self_origin;
    size_t old_len, add_len, n_before;
    char *grown;

    DCHECK(p != NULL,
           "a policy was enforced upon a container that does not exist — CSP §2.2 \"Policies\" gives the list "
           "it would be appended to no home, so the policy would be delivered and silently unenforced; the "
           "caller that has no container is looking at a Document this engine gave none");
    DCHECK(serialized_policy != NULL && *serialized_policy,
           "an EMPTY policy was enforced — CSP §2.2.2 \"Parse response's Content Security Policies\" appends a "
           "policy only when its directive set is not empty, and joining nothing into the list here would emit "
           "a stray U+002C that re-parses the list as one policy longer than it is");
    /* U+002C IS §2.2's LIST DELIMITER AND §2.2.1 GIVES IT NO MEANING INSIDE A POLICY, so a comma here would be
       appended as TWO policies enforced independently — narrowing the Document by a directive it never
       declared. §2.3 "Directives"' grammar already excludes it (`directive-value = *( required-ascii-whitespace
       / ( %x21-%x2B / %x2D-%x3A / %x3C-%x7E ) )` — %x2C is not among them), so a comma arriving here is a
       producer that composed a LIST where this entry takes one policy. */
    DCHECK(strchr(serialized_policy, ',') == NULL,
           "a serialized CSP LIST reached the entry that appends ONE policy — CSP §2.2 makes U+002C the list "
           "delimiter and §2.3's `directive-value` grammar excludes it from a policy, so this would be "
           "appended as several policies enforced independently and the Document would be narrowed by a "
           "directive its own markup never declared");
    add_len = strlen(serialized_policy);
    /* §7.1.5 "Sandboxing"'s CSP-DERIVED SANDBOXING FLAGS ARE ALREADY SPENT BY THE TIME ANYTHING GETS HERE.
       §7.1.5 says a Document's active sandboxing flag set "is populated by the navigation algorithm", and
       §7.4.5 "Populating a session history entry" is where that happens — finalSandboxFlags is the union of
       the target's snapshot flags and the CSP list's CSP-derived ones, taken ONCE, before the Document exists.
       A `sandbox` directive appended afterwards is therefore a flag set with nothing left to recompute it, and
       the Document keeps running unsandboxed under a policy that sandboxes it. §4.2.5.3 step 4 REMOVES the
       directive from every meta-delivered policy, which is what makes this state unreachable rather than
       merely detected; the assert is here because this is now the only road a policy can take after creation. */
    DCHECK(policy_csp_derived_sandboxing_flags(serialized_policy, add_len) == 0,
           "a policy carrying a `sandbox` directive was enforced upon a Document that already exists — HTML "
           "§7.1.5 \"Sandboxing\" populates a Document's active sandboxing flag set from the navigation "
           "algorithm, which §7.4.5 \"Populating a session history entry\" runs ONCE before the Document is "
           "created, so this Document would keep running unsandboxed under a policy that sandboxes it; "
           "§4.2.5.3 \"Pragma directives\" step 4 strips the directive from every meta-delivered policy, so "
           "this one reached the list without going through that step");

    /* THE SELF-ORIGIN SURVIVES THE RE-PARSE BECAUSE IT IS THE LIST'S AND NOT THE TEXT'S. §2.2.2 states it from
       OUTSIDE the bytes, so it cannot be recovered by parsing them again — it is carried across by hand, which
       is the same reason csp_list_parse takes it as an argument at every other site. */
    self_origin = p->csp.self_origin;
    n_before = p->csp.n_policies;
    old_len = p->csp_text ? strlen(p->csp_text) : 0;
    /* THE PARSE GOES FIRST BECAUSE THE REALLOC BELOW MOVES THE BYTES IT POINTS INTO. Every name and value token
       of the old list is a SLICE of `csp_text` (csp_directive_list.h: "the parse borrows its text"), so the
       list is dropped while those slices are still valid rather than left naming a buffer that has moved. */
    csp_list_free(&p->csp);
    grown = realloc(p->csp_text, old_len + (old_len ? 1 : 0) + add_len + 1);
    CHECK(grown != NULL, "policy container: OOM enforcing a policy");
    p->csp_text = grown;
    /* §2.2's serialization of a policy LIST: comma-delimited. The existing bytes are left exactly where they
       are, which is what makes this an APPEND at the level of the text as well as of the list. */
    if (old_len) p->csp_text[old_len++] = ',';
    memcpy(p->csp_text + old_len, serialized_policy, add_len + 1);
    csp_list_parse(&p->csp, p->csp_text, old_len + add_len, self_origin);
    /* THE INVARIANT, AT ITS ORIGIN: the list GREW BY ONE. It is the whole of "every delivery appends and none
       replaces" made mechanical — a replace lands on `n_before`, a drop lands below it, and a policy that
       re-parsed as several lands above it. §2.2.2's "if policy's directive set is not empty, append policy"
       is what makes +1 exact: the caller's policy has at least one directive (an empty one serializes as NULL
       and never reaches here) and re-parsing the same bytes yields the same policies it did before. */
    DCHECK(p->csp.n_policies == n_before + 1,
           "enforcing a policy did not GROW this Document's CSP list by exactly one — CSP §2.2 makes the list "
           "a LIST and §2.2.2 appends to it, so a count that stood still is a delivery that replaced or was "
           "dropped and a count that jumped is one policy re-parsed as several; either way the Document is now "
           "judged under a policy set no delivery produced, which is the one input §@S measures every breakout "
           "against");
    DCHECK(p->csp.self_origin == self_origin,
           "a CSP list lost its §2.2 SELF-ORIGIN across an append — §2.2.2 states that origin from outside the "
           "bytes, so a re-parse cannot recover one and `'self'` would afterwards match nothing this Document "
           "could ever load");
}

void policy_container_free(PolicyContainer *p)
{
    if (!p) return;
    csp_list_free(&p->csp);   /* frees the parse's arrays; the text it points into is freed next */
    free(p->csp_text);
    free(p->referrer_policy);
    /* §7.1.4's TWO ENDPOINT STRINGS — the other half of the obligation policy_container_new took on. This line
       and the adopt in the constructor are read together: an item owned by one and not released by the other
       is a leak that no gate names, because a container is freed once per Document and a document is not a
       loop. */
    embedder_policy_free(&p->embedder);
    free(p);
}

const char *policy_container_csp(const PolicyContainer *p) { return p ? p->csp_text : NULL; }

const CspList *policy_container_csp_list(const PolicyContainer *p) { return p ? &p->csp : NULL; }

const Origin *policy_container_self_origin(const PolicyContainer *p) { return p ? p->csp.self_origin : NULL; }

const EmbedderPolicy *policy_container_embedder(const PolicyContainer *p) { return p ? &p->embedder : NULL; }

SerializedPolicyContainer serialized_policy_container(const char *csp, const char *self_origin,
                                                     SerializedEmbedderPolicy embedder)
{
    SerializedPolicyContainer out;

    /* A CONTAINER THAT EXISTS HAS A CSP LIST AND EVERY CSP LIST HAS A SELF-ORIGIN (CSP §2.2). The one shape
       §2.2 forbids is a list of policies with no origin to resolve `'self'` against, and this is where it is
       refused — at the ONE constructor, rather than at each of the seams that would otherwise have to. A
       caller with no container calls serialized_policy_container_none, which is a different function so that
       "there is no container" cannot be reached by passing NULL to this one. */
    DCHECK(self_origin != NULL && *self_origin,
           "§7.1.7's policy container was built with no CSP §2.2 SELF-ORIGIN — every CSP list has one and it "
           "cannot be recovered from the policy text (§2.2.2 states it from outside the bytes), so a container "
           "without one resolves `'self'` against nothing and reports the document's own scripts as blocked by "
           "its own policy; the ABSENCE of a container is serialized_policy_container_none");
    out.csp = csp;
    out.self_origin = self_origin;
    out.embedder = embedder;
    return out;
}

SerializedPolicyContainer serialized_policy_container_none(void)
{
    SerializedPolicyContainer out;

    out.csp = NULL;
    out.self_origin = NULL;
    /* §7.1.4's "A NEW EMBEDDER POLICY" EVEN HERE, AND IT IS NOT A CONTRADICTION WITH "THERE IS NO CONTAINER".
       The absence is carried by the SELF-ORIGIN — the one field serialized_policy_container_exists reads — so
       every other member of this struct is only ever read after that question has been answered. Filling this
       one with the standard's own initial value rather than leaving it zeroed is what keeps the type free of a
       member whose bytes mean nothing: a reader that reached it would get `unsafe-none`, which is the answer
       §7.1.7 gives a container built from no response, and never an uninitialized pointer. */
    out.embedder = serialized_embedder_policy_new();
    return out;
}

SerializedPolicyContainer serialized_policy_container_or_none(const char *csp, const char *self_origin,
                                                              SerializedEmbedderPolicy embedder)
{
    /* THE ABSENCE IS AN ABSENT SELF-ORIGIN AND NEVER AN ABSENT POLICY. §2.2 gives every CSP list a self-origin
       whether or not it holds policies, so a relaying zone that has a container states one either way — and a
       POLICY arriving without one is that zone having stopped writing the field, which is a broken relay
       rather than "no container": read as an absence it would silently put the document under a container of
       its own, and read as a container it would resolve `'self'` against nothing. Neither is survivable, so it
       crashes here, at the boundary that produced it. */
    DCHECK(!(csp != NULL && *csp) || (self_origin != NULL && *self_origin),
           "a §7.1.7 policy container arrived from outside this engine with POLICIES and no CSP §2.2 "
           "SELF-ORIGIN — the two halves of one CSP list travel together (§2.2 makes the list a struct of "
           "both), so this is a relay that stopped writing the origin field and not a document with no "
           "container; `'self'` would resolve against this document's own address instead of the creator's");
    if (!(self_origin != NULL && *self_origin)) return serialized_policy_container_none();
    return serialized_policy_container(csp, self_origin, embedder);
}

SerializedPolicyContainer serialized_policy_container_of(const PolicyContainer *p)
{
    /* THE ONE PLACE A LIVE CONTAINER BECOMES BYTES, which is what makes §7.1.7's clone one operation rather
       than one per seam. `p` must exist: every Document has a container (document_install builds one for the
       initial about:blank too), so a NULL here is a caller holding something that is not a document. */
    DCHECK(p != NULL,
           "§7.1.7's policy container was serialized from nothing — every Document has one, including the "
           "initial about:blank §7.3.2.1 clones the creator's into, which is why the clone is an ordinary rule "
           "rather than an inheritance rule written for CSP; the ABSENCE is serialized_policy_container_none");
    /* ASKED OF THE RECORD, BEFORE IT IS SERIALIZED. origin_serialized asserts its own argument, and its message
       is about an origin — the reader of that `@WHY` would be standing at a URL component rather than at the
       container whose CSP list has no self-origin, which is the fact that is actually wrong. */
    DCHECK(policy_container_self_origin(p) != NULL,
           "a policy container's CSP list has no CSP §2.2 SELF-ORIGIN to serialize — every list has one and "
           "policy_container_new parses it in whether or not there is policy text, so a container without one "
           "was built somewhere that did not state it");
    return serialized_policy_container(policy_container_csp(p),
                                       origin_serialized(policy_container_self_origin(p)),
                                       serialized_embedder_policy_of(policy_container_embedder(p)));
}

bool serialized_policy_container_exists(SerializedPolicyContainer c)
{
    /* §7.1.7's `is not null`, from the field that answers it — see the type. */
    return c.self_origin != NULL && *c.self_origin;
}

SerializedPolicyContainer policy_container_determine_navigation_params(const char *response_url,
                                                                      SerializedPolicyContainer response,
                                                                      SerializedPolicyContainer inherited)
{
    UrlRecord rec;
    bool parsed, local, srcdoc;
    bool has_inherited = serialized_policy_container_exists(inherited);

    DCHECK(response_url != NULL && *response_url,
           "§7.1.7's determine-navigation-params-policy-container was asked without a responseURL — its third "
           "step turns on whether that URL is LOCAL (Fetch §2.1), so without it there is no algorithm to run");
    /* STEP 4'S RESPONSE CONTAINER IS ALWAYS THERE, INCLUDING WHEN THERE WAS NO RESPONSE. Step 5's "new policy
       container" is a container too, and the CSP list it holds still needs §2.2's self-origin — which for a
       Document created at this address is that address's origin, the same answer §2.2.2 would have given. So
       the absence a caller can have is an absence of POLICIES, never of this container. */
    DCHECK(serialized_policy_container_exists(response),
           "a Document was created with no origin for its own CSP list — §2.2.2 makes that origin the list's "
           "self-origin and §7.1.7 step 5's new container needs one just as a response-delivered list does, so "
           "without it a `'self'` in this document's own policy matches nothing it could ever load");

    url_record_init(&rec);
    parsed = url_parse(&rec, response_url, strlen(response_url), NULL);
    /* A `CHECK` AND NOT A `DCHECK`: every caller of this has already parsed this same address to derive the
       Document's principal, so a failure here is not an unbuilt capability — it is the two parses disagreeing,
       and continuing would judge the Document under whichever container the `else` arm happens to name. */
    CHECK(parsed, "policy container: the address a Document is being created at does not parse, so §7.1.7's "
                  "local-URL step has nothing to ask");
    local  = url_scheme_is_local(rec.scheme);
    /* §7.1.7's SECOND STEP IS SEPARATE FROM ITS THIRD and asserts rather than falls through: "If responseURL is
       about:srcdoc: Assert: parentPolicyContainer is not null. Return a clone of parentPolicyContainer." A
       srcdoc Document has no response and no creator-less spelling — it exists only inside a parent — so a
       srcdoc address arriving here with nothing to inherit is a caller that did not carry the parent's
       container, and the `else` arm below would silently judge it under an empty list of its own. */
    srcdoc = url_matches_about(&rec, "srcdoc", true);
    url_record_free(&rec);

    DCHECK(!srcdoc || has_inherited,
           "§7.1.7 step 2 reached an `about:srcdoc` Document with no parent policy container — the standard "
           "ASSERTS one is there, because a srcdoc Document is created by its parent and has no response of "
           "its own; the caller that named this address did not carry the container that goes with it");

    /* §7.1.7 STEP 3 — "If responseURL is local and initiatorPolicyContainer is not null, then return a clone of
       initiatorPolicyContainer" — which step 2's srcdoc case reaches the same answer through here, since the
       parent IS the creator this seam carries. Everything else is step 4's responsePolicyContainer, whose CSP
       list is §2.2.2's pair; a document with no response at all has an empty one, which is step 5's new
       container and needs no arm of its own. THE WHOLE CONTAINER IS RETURNED, not a container assembled from
       one arm's policy and the other's remainder: §7.1.7 returns a clone of ONE of them. */
    return (local && has_inherited) ? inherited : response;
}

SandboxFlags policy_csp_derived_sandboxing_flags(const char *serialized_csp_list, size_t len)
{
    /* §7.1.5's "Every CSP list cspList has CSP-derived sandboxing flags", verbatim: collect the `sandbox`
       directive of every ENFORCE policy, and if there are any, parse a sandboxing directive over the LAST
       one. Not the union of them — the last one WINS, which is the one place in CSP where a later policy does
       something other than narrow, and is why this cannot be answered from a per-container flag.
       IT TAKES TEXT rather than a container because the caller that needs it is the NAVIGATION and the
       container it would read does not exist yet: §7.5.1 hands the new Document its policy container and its
       final sandboxing flag set in the same breath. The parse BORROWS these bytes for the length of the call.
       WHAT THE `sandbox` DIRECTIVE'S VALUE IS: not a source list. §7.1.5's sandboxing-directive parse reads
       the whole value as a token set, which is why the model keeps the raw remainder beside the split — and
       why an ABSENT `sandbox` and a `sandbox` with an EMPTY value are not the same thing. The empty value is
       the most restrictive form there is; absence is an empty flag set. */
    CspList list;
    SandboxFlags out = 0;
    size_t i;

    memset(&list, 0, sizeof list);
    /* NO SELF-ORIGIN, and that is a statement rather than an omission: this list is read for ONE directive's
       raw value and is never asked to match a URL, so there is no `'self'` here to resolve. §6.7.2.7 asserts
       the origin is present, which is what makes this the only shape of list allowed to lack one. */
    csp_list_parse(&list, serialized_csp_list, len, NULL);
    for (i = 0; i < list.n_policies; i++) {
        const CspDirective *d = csp_policy_directive(&list.policies[i], "sandbox");
        if (d)
            out = sandbox_parse_directive(d->value_text.p, d->value_text.n);
    }
    csp_list_free(&list);
    return out;
}

/* §4.4.1's EnsureCSPDoesNotBlockStringCompilation, for ONE policy. Its directive lookup is written out in the
   algorithm itself and is NOT §6.8's fallback machinery: "if policy contains a directive whose name is
   script-src, set source-list to that directive's value; otherwise if policy contains default-src, that one."
   §6.1.10 says why in as many words — 'unsafe-eval' acts as a global page flag, so the granular script-src-elem
   and script-src-attr forms are never consulted for it.
   'trusted-types-eval' is the other expression that can permit compilation, and it cannot be reached here: it
   applies only when the code string arrived as a TrustedScript, and §2's three types do not exist in this
   engine, so no value can be one. That is the same step-is-decided-not-skipped reading core/html/
   trusted_types.c already relies on for §3.4 step 1, and it is why an eval sink under
   `require-trusted-types-for 'script'` is reported through the trusted-types answer rather than this one. */
static bool policy_permits_compilation(const CspPolicy *policy)
{
    const CspDirective *d = csp_policy_directive(policy, "script-src");

    if (!d) d = csp_policy_directive(policy, "default-src");
    /* A policy with neither says nothing about compilation, which is not the same as forbidding it. */
    if (!d) return true;
    return csp_source_list_contains(d, "'unsafe-eval'");
}

/* §4.2.3's INNER LOOP FOR ONE POLICY, and the collapse of it is an identity rather than a shortcut — the same
   one policy_blocks_request makes below, read on the inline side. §4.2.3 runs every directive's INLINE CHECK
   in turn; the SEVEN directives that HAVE one — default-src (§6.1.3.3), script-src (§6.1.10.3),
   script-src-elem (§6.1.11.3), script-src-attr (§6.1.12.1), style-src (§6.1.13.3), style-src-elem (§6.1.14.3)
   and style-src-attr (§6.1.15.1) — each open with the SAME two lines: take §6.8.2's effective directive name
   for the type, and return "Allowed" unless §6.8.4 says THIS directive is the one that executes for that
   name. The six granular ones then end in the SAME §6.7.3.3, and default-src's delegates to whichever of them
   §6.8.2 named "using this directive's value", which is the same thing done from the other side. §6.8.4
   answers Yes for at most one directive of a policy, and csp_policy_governing_directive is that walk. Every
   other directive defines no inline check, so §4.2.3 leaves its result untouched for them. */
static bool policy_permits_inline(const CspPolicy *policy, CspInlineType type,
                                  const lxb_dom_element_t *element, const char *source, size_t source_len)
{
    const CspDirective *d =
        csp_policy_governing_directive(policy, csp_effective_directive_for_inline_checks(type));

    /* A policy that carries no directive governing this check says NOTHING about it: `img-src 'none'` alone
       blocks no handler. §4.2.3's loop leaves its result at "Allowed". */
    if (!d) return true;
    return csp_element_match_source_list(d, element, type, source, source_len) == CSP_MATCHES;
}

/* THE ONE WALK, ASKED ONE OF TWO QUESTIONS. §2.2: the policies in a list are enforced INDEPENDENTLY, so
   content runs only if EVERY one permits it — the opposite quantifier from core/html/trusted_types.c's
   question over the same list, and for the same reason: a second policy can only narrow. A document with no
   policy allows everything, which is the overwhelmingly common case and is what "no Content-Security-Policy
   header" means. */
bool policy_allows_inline(const PolicyContainer *p, CspInlineType type, const lxb_dom_element_t *element,
                          const char *source, size_t source_len)
{
    size_t i;

    if (!p) return true;
    for (i = 0; i < p->csp.n_policies; i++)
        if (!policy_permits_inline(&p->csp.policies[i], type, element, source, source_len)) return false;
    return true;
}

bool policy_allows_string_compilation(const PolicyContainer *p)
{
    size_t i;

    if (!p) return true;
    for (i = 0; i < p->csp.n_policies; i++)
        if (!policy_permits_compilation(&p->csp.policies[i])) return false;
    return true;
}

/* §6.7.2.1 "does request violate policy?" for ONE policy, over the FETCH DIRECTIVES — which is every directive
 * whose pre-request check can answer "Blocked".
 *
 * WHY THE SPEC'S LOOP OVER EVERY DIRECTIVE COLLAPSES TO ONE LOOKUP, and it is an identity rather than a
 * shortcut. §6.7.2.1 runs each directive's pre-request check; the fourteen that HAVE one — thirteen of §6.1
 * "Fetch Directives"' fifteen, plus §6.2.2's worker-src — each open with the SAME two lines: take §6.8.1's
 * effective directive name for the request, and return "Allowed" immediately unless §6.8.4 says THIS directive
 * is the one that executes for that name. §6.8.4 answers Yes for at most one directive of a policy, and
 * csp_policy_governing_directive is that walk. Every other directive of a policy — base-uri, form-action,
 * frame-ancestors, sandbox, webrtc, report-to, and the two *-attr forms — defines no pre-request check at all,
 * so §6.7.2.1 leaves `violates` untouched for them.
 *
 * WHICH CHECK RUNS IS DECIDED BY §6.8.1's EFFECTIVE DIRECTIVE NAME AND NEVER BY THE GOVERNING DIRECTIVE'S
 * OWN NAME. That is the one thing about this collapse which is not obvious, and it is the thing it was got
 * wrong on. §6.1.3.1 "default-src Pre-request check" ends "Return the result of executing the pre-request
 * check for the directive whose name is name on request, policy, and self-origin, using this directive's value
 * for the comparison", and §6.1.1.1 "child-src Pre-request check" says the same — so a policy whose only
 * script directive is `default-src` runs SCRIPT-SRC-ELEM's check over default-src's VALUE. A dispatch on the
 * governing directive's own spelling gets this wrong in both directions at once: it says nothing about
 * `default-src 'nonce-x'`, which is a whole check silently skipped, while firing for spellings whose check it
 * has misnamed. The name §6.8.1 returned is what the standard dispatches on, and it is already in hand.
 *
 * AND ONLY TWO OF §6.8.1's NAMES RUN ANYTHING BEFORE §6.7.2.5. §6.1.11.1 "script-src-elem Pre-request check"
 * hands its whole answer to §6.7.1.1 "Script directives pre-request check". §6.1.14.1 "style-src-elem
 * Pre-request Check" honours the request's NONCE and then goes to §6.7.2.5 — it has no integrity arm and no
 * 'strict-dynamic' arm, and neither does §6.1.13.1 "style-src Pre-request Check", which is why a crash naming
 * §6.7.1.1 for a style directive names an algorithm that does not govern it and asks for two mechanisms it
 * would never use. Every other name §6.8.1 can return — connect-src, manifest-src, object-src, frame-src,
 * media-src, font-src, img-src, and §6.2.2.1's worker-src — is §6.7.2.5 alone.
 *
 * WHAT OF THOSE TWO IS ANSWERED HERE, AND WHAT CRASHES. Each preliminary step is settled by the SOURCE LIST
 * before the request is looked at: §6.7.2.3 step 3 loops over the list's nonce-source expressions, §6.7.2.4
 * steps 2-3 say a list with no hash-source is "Does Not Match" outright, and step 1.3's 'strict-dynamic' arm
 * does not exist unless the keyword is in the list. A policy carrying none of the three therefore reaches
 * §6.7.2.5 BY THE ALGORITHM and not by an approximation of it — which is the shape most real policies are
 * written in, and is why `script-src 'self' 'unsafe-inline' <hosts>` is answered here rather than refused. A
 * policy that DOES carry one needs a field of Fetch §2.2.5's request record that this engine does not carry to
 * the check, and each arm crashes naming its own field. */
static bool policy_blocks_request(const CspPolicy *policy, const UrlRecord *url, const char *effective,
                                  const char *destination, const Origin *self_origin, int redirect_count)
{
    const CspDirective *d = csp_policy_governing_directive(policy, effective);

    /* A policy carrying none of §6.8.3's fallback chain says NOTHING about this request — §6.8.4 answers No
       for every directive of it, so §6.7.2.1 returns "Does Not Violate" and §4.1.2 leaves its result
       "Allowed". `img-src 'none'` blocks no `fetch()`. */
    if (!d)
        return false;
    if (!strcmp(effective, "script-src-elem")) {
        /* §6.7.1.1 step 1's GATE, and its step 2 for everything the gate refuses: a request whose destination
           is not script-like never reaches the source list at all through a script directive — step 1.4 is
           INSIDE step 1, so "Return Allowed" is the whole of the algorithm for it. Fetch §2.2.5's script-like
           set holds three of the four destinations §6.8.1 routes to `script-src-elem`; the fourth is `xslt`,
           which Fetch excludes deliberately and says so in a note. The predicate is Fetch's own and is ASKED of
           Fetch rather than restated here — core/fetch/fetch.h says why a second copy of a moving enumeration
           is a question the two halves of one program answer differently. */
        if (!fetch_is_script_like(destination))
            return false;
        DCHECK(!csp_source_list_has_nonce_source(d),
               "§6.7.1.1 step 1.1 asks §6.7.2.3 whether the REQUEST's cryptographic nonce metadata matches this "
               "source list, and the list carries a nonce-source, so §6.7.2.3's own step 3 does not settle it — "
               "the answer is the request's to give and this engine does not carry it to the check. Fetch "
               "§2.2.5 gives every request a cryptographic nonce metadata and HTML §4.2.4.3's create a link "
               "request sets it (\"set request's cryptographic nonce metadata to options's cryptographic nonce "
               "metadata\"); carry that field into policy_should_block_request and run §6.7.2.3 here. Matching "
               "the source list alone reports a `<script nonce=x>` under `script-src 'nonce-x'` — one of the "
               "two shapes modern CSP is written in — as blocked, which is a script every browser LOADS");
        DCHECK(!csp_source_list_has_hash_source(d),
               "§6.7.1.1 step 1.2 asks §6.7.2.4 whether the REQUEST's integrity metadata matches this source "
               "list, and the list carries a hash-source, so §6.7.2.4's step 3 does not settle it — the answer "
               "needs the request's integrity metadata and SRI's parse metadata over it. HTML §4.2.4.3's create "
               "a link request sets that field from the element's `integrity`; carry it into "
               "policy_should_block_request and run §6.7.2.4 here. Matching the source list alone reports a "
               "subresource a browser LOADS as blocked");
        DCHECK(!csp_source_list_contains(d, "'strict-dynamic'"),
               "§6.7.1.1 step 1.3 decides this request by the REQUEST's parser metadata ALONE once the list "
               "carries 'strict-dynamic' — Blocked when it is \"parser-inserted\", Allowed otherwise, with the "
               "source list never consulted either way. This engine does not carry Fetch §2.2.5's parser "
               "metadata to the check; HTML sets it \"parser-inserted\" for an element the tokenizer created and "
               "\"not-parser-inserted\" for one a script inserted. Carry it and answer step 1.3 here. Falling "
               "through to the source list INVERTS the directive: 'strict-dynamic' exists precisely to allow "
               "the script-inserted loads a host list refuses, and to block the parser-inserted ones it "
               "permits");
        /* Step 1.4 is the §6.7.2.5 below, which is §6.7.2.7 over the request's current URL. */
    } else if (!strcmp(effective, "style-src-elem")) {
        DCHECK(!csp_source_list_has_nonce_source(d),
               "§6.1.14.1 \"style-src-elem Pre-request Check\" step 3 asks §6.7.2.3 whether the REQUEST's "
               "cryptographic nonce metadata matches this source list, and the list carries a nonce-source, so "
               "the answer is the request's to give and this engine does not carry it to the check. It is the "
               "SAME field §6.7.1.1 step 1.1 needs, and it is the ONLY preliminary step a style directive has: "
               "§6.1.13.1 and §6.1.14.1 have no integrity arm and no 'strict-dynamic' arm, so carrying the nonce "
               "finishes this directive's check outright. Matching the source list alone reports a `<link "
               "rel=stylesheet nonce=x>` under `style-src 'nonce-x'` as blocked");
        /* Step 4 is the §6.7.2.5 below. */
    }
    if (csp_source_list_match_url(d, url, self_origin, redirect_count) == CSP_MATCHES)
        return false;
    /* §4.1.2 STEP 3.3.1 — "execute §5.5 Report a violation on the result of executing §2.4.2 Create a
       violation object for request, and policy" — WHICH THIS ENGINE DOES NOT PERFORM, and here is where that
       becomes visible rather than merely absent. A violation has exactly two observables: a
       `securitypolicyviolation` event at the Document, and a report POSTed to the endpoints a policy names.
       The second is DECLARED IN THE POLICY ITSELF, so it is a gap this function can see coming and refuse. */
    DCHECK(!csp_policy_directive(policy, "report-uri") && !csp_policy_directive(policy, "report-to"),
           "a request was BLOCKED by a policy that declares a reporting endpoint, and §4.1.2 step 3.3.1's "
           "report is not built — the page's server is owed a report it will never receive, and a test that "
           "waits for one waits forever. Build CSP §2.4.2's violation object and §5.5's report a violation, "
           "whose two observables are the `securitypolicyviolation` event fired at the Document and the POST "
           "to the endpoints named by `report-to`/`report-uri`");
    return true;
}

CspRequestVerdict policy_should_block_request(const PolicyContainer *p, const UrlRecord *url,
                                              const char *destination, int redirect_count)
{
    const char *effective;
    size_t i;

    /* §4.1.2 step 1 reads the request's policy container's CSP list, and a document with no container has no
       policies — the overwhelmingly common case, and §4.1.2's own "let result be Allowed" for it. */
    if (!p)
        return CSP_REQUEST_ALLOWED;
    effective = csp_effective_directive_for_request(destination);
    /* §6.8.1's "report" row: a violation report upload is governed by no fetch directive, so every policy's
       pre-request check returns Allowed and there is nothing to walk. */
    if (!effective)
        return CSP_REQUEST_ALLOWED;
    /* §4.1.2 step 3: for each policy. This build parses only ENFORCE policies (see csp_directive_list.h), so
       the step's "if policy's disposition is report, skip" is vacuous rather than skipped.
       THE QUANTIFIER IS THE SAME ONE `policy_allows_inline` RUNS, from the other end: §4.1.2 sets result to Blocked
       if ANY policy is violated, which is "allowed only if EVERY policy permits it". */
    for (i = 0; i < p->csp.n_policies; i++)
        if (policy_blocks_request(&p->csp.policies[i], url, effective, destination, p->csp.self_origin,
                                  redirect_count))
            return CSP_REQUEST_BLOCKED;
    return CSP_REQUEST_ALLOWED;
}
