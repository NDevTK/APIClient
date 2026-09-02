/* CROSS-ORIGIN EMBEDDER POLICIES — HTML §7.1.4, and the half of cross-origin isolation a response states
 * about ITSELF.
 *
 * IT IS ONE OF §7.1.7'S FIVE POLICY-CONTAINER ITEMS, and that is the fact this file is written to serve. §7.1.7
 * Policy containers lists them one per item — a CSP list, an embedder policy, a referrer policy, an integrity
 * policy, a report-only integrity policy — and that enumeration is this file's own, assembled from the item
 * headings, so it is NOT written here as a quotation: the standard states each item in a sentence of its own
 * ("An embedder policy, which is an embedder policy. It is initially a new embedder policy."), and running the
 * five together into one comma list is a shape no sentence of §7.1.7 has. No sandboxing flag set and no opener
 * policy, which are two SEPARATE rows of §7.5.1's Document creation table — and §7.1.7's
 * create-a-policy-container-from-a-fetch-response builds the embedder policy item by "obtaining an embedder
 * policy given response and environment". That is the algorithm below.
 *
 * WHY A THREE-VALUED ENUM AND NOT A BOOLEAN. §7.1.4 gives `unsafe-none`, `require-corp` and `credentialless`,
 * and TWO of them are "compatible with cross-origin isolation" while the three differ in what they do to a
 * cross-origin no-CORS fetch (`require-corp` demands CORP or CORS; `credentialless` omits credentials
 * instead). A boolean would answer the isolation question and lose the fetch question, which is the same
 * collapse core/frame/agent_cluster.c had to undo for the three-valued isolation MODE.
 *
 * THE PROCESSING MODEL FAILS OPEN AND THAT IS LOAD-BEARING. §7.1.4.1: a header that cannot be parsed as a
 * token — including the LIST that two identical headers combine into — leaves the value at `unsafe-none`. So
 * this file never rejects a response; it answers with the policy the standard says that response has, and the
 * only way a value becomes non-default is a well-formed token that IS compatible with cross-origin isolation.
 * That is why `obtain` has no failure return: every response has an embedder policy.
 *
 * THE TWO ENDPOINT FIELDS ARE SHORTENED SPELLINGS OF §7.1.4'S TWO ITEMS, AND THE STANDARD DISTINGUISHES THEM.
 * §7.1.4 Cross-origin embedder policies writes the four items as "A value, which is an embedder policy value,
 * initially "unsafe-none". A reporting endpoint string, initially the empty string. A report-only value, which
 * is an embedder policy value, initially "unsafe-none". A report-only reporting endpoint string, initially the
 * empty string." — so `endpoint` here is the REPORTING ENDPOINT and `report_only_endpoint` is the REPORT-ONLY
 * REPORTING ENDPOINT, and each branch of obtain writes its own: the value branch sets policy's reporting
 * endpoint, the report-only branch sets policy's report-only reporting endpoint. They are two items, never one
 * written twice.
 * THIS PARAGRAPH USED TO SAY THE OPPOSITE, ON A QUOTATION THAT DOES NOT EXIST, AND THE ARGUMENT WAS ACTED ON.
 * It claimed §7.1.4's obtain reads "verbatim" a sentence setting "policy's endpoint" in BOTH branches, and
 * reasoned from it that implementing the collapse was the spec-faithful choice. The phrase `report only value`
 * occurs NOWHERE in HTML and no step of obtain writes a bare `endpoint`; the sentence was never anybody's. That
 * is CLAUDE.md's worst failure in this class stated exactly — a wrong number sends a reader to the wrong place
 * where they find out, and a fabricated quotation tells them not to go — and it stood here because every other
 * check passed: the number resolves, §7.1.4 is real, the algorithm does live there. Only the words were made
 * up, and only reading the section could say so. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_EMBEDDER_POLICY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_EMBEDDER_POLICY_H
#include <stdbool.h>

#include "core/fetch/headers.h"

/* §7.1.4's EMBEDDER POLICY VALUE — "one of three strings that controls the fetching of cross-origin resources
   without explicit permission from resource owners". */
typedef enum {
    EMBEDDER_POLICY_UNSAFE_NONE = 0,   /* §7.1.4's initial value, and what a fail-open parse leaves */
    EMBEDDER_POLICY_REQUIRE_CORP,
    EMBEDDER_POLICY_CREDENTIALLESS,
} EmbedderPolicyValue;

/* §7.1.4's EMBEDDER POLICY struct, all four items. The two endpoints are the empty string initially — §7.1.4
   says "a reporting endpoint STRING, initially the empty string", which is a different absence from the opener
   policy's null endpoint, so they are spelled differently here too. Owned. */
typedef struct {
    EmbedderPolicyValue value;
    char               *endpoint;
    EmbedderPolicyValue report_only_value;
    char               *report_only_endpoint;
} EmbedderPolicy;

/* §7.1.4's "a new embedder policy" — every field at its stated initial value. */
void embedder_policy_init(EmbedderPolicy *p);
void embedder_policy_free(EmbedderPolicy *p);

/* §7.1.4: "An embedder policy value is COMPATIBLE WITH CROSS-ORIGIN ISOLATION if it is `credentialless` or
   `require-corp`." Read by §7.1.3's obtain-an-opener-policy (which is how `same-origin` becomes
   `same-origin-plus-COEP`) and by §7.1.4.2's embedder policy checks. */
bool embedder_policy_compatible_with_cross_origin_isolation(EmbedderPolicyValue v);

/* §7.1.4's THREE STRINGS, IN BOTH DIRECTIONS — "an embedder policy value is one of three STRINGS", and these
   are that sentence read as a serialization. A value crosses a seam as the token the standard names it with
   and never as an integer: a record whose `1` a reader takes for `require-corp` because both sides happen to
   spell the enum in the same order is a contract nothing checks, and a `@WHY` naming `2` tells the reader of
   it nothing. `_of_token` is §7.1.4.1's "the valid token values are the embedder policy values" — false for a
   token that names none, which is the caller's fail-open. */
const char *embedder_policy_value_token(EmbedderPolicyValue v);
bool embedder_policy_value_of_token(const char *token, EmbedderPolicyValue *out);

/* §7.1.4'S EMBEDDER POLICY IN THE FORM §7.1.7'S CONTAINER CROSSES A SEAM IN — the same four items, with the
 * two endpoints BORROWED rather than owned, which is the only difference between this type and the one above.
 *
 * IT IS ITS OWN TYPE AND NOT `EmbedderPolicy` PASSED BY VALUE, for the reason every other field of a
 * SerializedPolicyContainer is `const`: a copy of an owning struct is a second name for memory it does not
 * own, so the day somebody frees through the copy the owner holds two dangling pointers and nothing said so.
 * The `const` is what makes that unspellable rather than merely discouraged.
 *
 * BUILT THROUGH A FUNCTION AND NEVER BY AN INITIALIZER, which is the same forcing mechanism
 * core/frame/policy_container.h states for the container that holds it: a designated initializer zero-fills
 * the item it does not name, and zero here is `unsafe-none` — a plausible policy for a response that opted
 * into isolation, which is exactly the silence this whole chain of work exists to end. */
typedef struct {
    EmbedderPolicyValue value;
    const char         *endpoint;               /* never NULL — §7.1.4's initial value is the EMPTY STRING */
    EmbedderPolicyValue report_only_value;
    const char         *report_only_endpoint;   /* never NULL, same sentence */
} SerializedEmbedderPolicy;

/* AN EMBEDDER POLICY, stating every item. Both endpoints are REQUIRED and may be empty but not NULL — §7.1.4
   spells their absence as the empty string, and a NULL would be a second absence with no meaning. */
SerializedEmbedderPolicy serialized_embedder_policy(EmbedderPolicyValue value, const char *endpoint,
                                                    EmbedderPolicyValue report_only_value,
                                                    const char *report_only_endpoint);

/* §7.1.4's "a NEW embedder policy" — every item at its stated initial value, which is what §7.1.7 gives a
   container that was not created from a response ("initially a new embedder policy") and what its
   create-a-policy-container-from-a-fetch-response sets the item to when there is no environment to obtain one
   for ("otherwise, set it to `unsafe-none`"). A real answer, not a placeholder. */
SerializedEmbedderPolicy serialized_embedder_policy_new(void);

/* THE SERIALIZATION OF A LIVE ONE. The bytes are BORROWED from `p`, so they live as long as it does. */
SerializedEmbedderPolicy serialized_embedder_policy_of(const EmbedderPolicy *p);

/* §7.1.7's clone step 3 — "set clone's embedder policy to a COPY of policyContainer's embedder policy" — and
   the arrival of one over a seam, which are ONE operation because a container crossing a seam and a container
   cloned in this heap are one operation (core/frame/policy_container.h). `out` is FILLED from `s`, taking its
   own copy of both endpoint strings; the caller frees it with embedder_policy_free. */
void embedder_policy_adopt(EmbedderPolicy *out, SerializedEmbedderPolicy s);

/* §7.1.4's "obtain an embedder policy from a response `response` and an environment `environment`", over the
 * response's HEADER LIST — which is the whole of what the algorithm reads from the response.
 *
 * `secure_context` IS THE ENVIRONMENT, reduced to the one question step 2 asks of it: "if environment is a
 * NON-SECURE CONTEXT, then return policy". A non-secure page gets the default policy no matter what it sends,
 * which is why an embedder policy cannot be a pure function of a header list. HTML §8.1.3.5 answers it over
 * the environment's TOP-LEVEL CREATION URL (core/frame/secure_context.h), and the caller asks there because
 * this algorithm runs BEFORE the realm whose environment it is exists.
 *
 * `out` is written from step 1's "let policy be a NEW embedder policy" onward — it is filled, never merged
 * into, and the caller frees it. */
void embedder_policy_obtain(EmbedderPolicy *out, const HeaderList *headers, bool secure_context);

/* HTML §7.1.4.2 "Embedder policy checks"' CHECK A NAVIGATION RESPONSE'S ADHERENCE TO ITS EMBEDDER POLICY, over
 * the two policies its steps 3-6 read. TRUE is the standard's "the response adheres"; FALSE is §7.4.5
 * "Populating a session history entry" BLOCKING the navigation before its Document is created — the frame gets
 * §7.5.7 "Loading a document for inline content that doesn't have a DOM"'s Document, made unsalvageable, and
 * never the response's own.
 *
 * ITS STEPS 1 AND 2 ARE THE CALLER'S, WHICH IS THE SPLIT core/frame/opener_policy.h ALREADY MAKES. Step 1 asks
 * whether the navigable is a CHILD NAVIGABLE — §7.3.1.3 "Child navigables": a navigable "is a child navigable",
 * "which means that its parent is non-null" — and step 2 reads parentPolicy off "navigable's CONTAINER
 * DOCUMENT's policy container". Both are questions about the navigable TREE, and this file holds none of it:
 * §7.1.4 is a policy, a parse and a decision, exactly as §7.1.3.2's group-switch decisions take a navigable's
 * `isInitialAboutBlank` from the site that has the navigable. A caller whose navigable has no parent does not
 * call this at all; step 1's `return true` IS that absence.
 *
 * parentPolicy IS READ LIVE AT THE CHECK AND NEVER CARRIED FROM THE ENQUEUE. §7.4.5 runs this where the
 * response arrives and hands it the NAVIGABLE, so the container document it dereferences is the one presenting
 * that navigable at that moment. The load job standing beside this call carries an INITIATOR's policy container
 * (§7.1.7's clone, which the operation decided), and the two name different documents the moment a THIRD
 * document navigates a frame — `frames[0].location = …`, a form with a `target` — because the initiator is
 * whoever's script ran while the container document is whoever's element presents the navigable. Substituting
 * the carried one is CLAUDE.md's work-item defect with the operands reversed: a real policy, belonging to a real
 * document, answering a different question, and identical to the right answer in every case anyone tests first.
 *
 * ITS REPORTING ARMS CRASH — see embedder_policy.c, which is where what has to be built is named. */
bool embedder_policy_check_navigation_response(SerializedEmbedderPolicy parent_policy,
                                               SerializedEmbedderPolicy response_policy);

#endif
