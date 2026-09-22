/* MIXED CONTENT — the three algorithms Fetch integrates: Mixed Content §4.3 "Does settings prohibit mixed security
 * contexts?", §4.4 "Should fetching request be blocked as mixed content?" (Fetch §4.1 "Main fetch" step 7's
 * SECOND disjunct) and Mixed Content §4.1 "Upgrade a mixed content request to a potentially trustworthy URL, if
 * appropriate" (Fetch §4.1 step 6). See mixed_content.c.
 *
 * THERE IS A FOURTH ALGORITHM THIS STANDARD OWNS AND THIS COMPONENT DOES NOT HAVE, AND IT IS A NAMED
 * RESIDUAL. WHAT IS NOT COVERED: Mixed Content §4.5 "Should response to request be blocked as mixed
 * content?", the RESPONSE-side mirror of §4.4. Fetch asks it at §4.1 "Main fetch" step 20 — "should
 * internalResponse to request be blocked as mixed content" — and this engine does not ask that step at all,
 * so the header line above it enumerates the three algorithms this component HAS rather than the four Fetch
 * integrates.
 * THE OTHER THREE DISJUNCTS OF THAT STEP ARE NOT THIS COMPONENT'S AND ONE OF THEM IS ALREADY ANSWERED
 * ELSEWHERE, which is stated here so the next diff does not build a second copy of it: the step's MIME-type
 * and nosniff pair is a question about whether a body may be READ AS CODE, and the dangerous direction of it
 * is answered outside this engine entirely, by the trusted zone's CORB gate at its network chokepoint
 * (SECURITY.md). That is a different question asked at a different layer and it is NOT this step; what is
 * absent is the step.
 * WHAT THE NEXT DIFF BUILDS: §4.5 here beside §4.4, and a step-20 entry at core/fetch/fetch.h beside
 * fetch_main_blocked that asks it of the delivered response — ONE component with N callers and not a check
 * per delivery site, for the reason that header already gives about step 7: four hand-written copies is what
 * step 7 was before it was one component, and the copy nobody remembered to write is the one that ran no
 * check at all.
 * HOW ITS ABSENCE WOULD SHOW: a response fetched over `http` is delivered to a document this engine answers
 * PROHIBITS for, and nothing between the two refuses it — where a browser sets that response to a network
 * error. Observe it at the delivery seam rather than at a member: compare the scheme of the address a
 * response was actually fetched from against mixed_content_settings_prohibit for the document receiving it,
 * and the pair that a browser would have refused arrives here carrying a body.
 *
 * THIS STANDARD IS INDEXED AND EVERY CITATION HERE IS CHECKED — engine/specindex/mixedcontent.json, at the
 * editor's draft, carrying all twenty of that document's numbered sections. The paragraph that stood here
 * said the opposite and ended by telling its reader to read any §4.x finding in this file as the auditor
 * answering about Fetch. THAT INSTRUCTION IS WHY IT IS REWRITTEN RATHER THAN DELETED: a note telling a reader
 * to DISCOUNT a channel outlives the gap it was written for, nothing reports that it has, and its only effect
 * afterwards is to make a real finding here look like a known artefact.
 *
 * WHY A MISSING ROW ACCUSED RATHER THAN WENT SILENT, WHICH IS THE PART THAT GENERALISES AND IS WHY THE ROW
 * WAS WORTH FETCHING. This standard's numbers are §4.1, §4.3 and §4.4, and Fetch HAS a §4.1, a §4.3 and a
 * §4.4, all different algorithms. A citation naming an UNINDEXED standard resolves to nothing, so a file vote
 * placed these at Fetch and the quotation channel compared this standard's words against Main fetch and
 * Scheme fetch — a fabrication charge manufactured by the ABSENCE of a row, landing on the sites whose author
 * had pasted from the fetched draft. Naming the standard in the citation did not help, because the name
 * resolved to nothing and the vote won anyway. A missing row is SILENT only where the numbers collide with
 * nothing; where they collide with an indexed standard's it is ACCUSING, and the accused sites are the
 * carefully-written ones because a citation carrying no quotation has nothing to fail on.
 * RETIREMENT: this note goes when a coverage claim here carries the command that read engine/specindex.
 *
 * THE STEP THAT REWRITES RUNS BEFORE THE STEP THAT JUDGES, AND THAT ORDER IS THE WHOLE DESIGN. Fetch §4.1
 * puts the upgrade at step 6 and the disjunction at step 7, so §4.4 is asked of an address §4.1 may already
 * have changed. Asking them in the other order — or asking §4.4 of the address a caller started with — makes
 * this engine REFUSE an `<img src="http://…">` on an https page that a browser upgrades and loads, which is a
 * wrong answer in the coverage-losing direction and shows up as the element firing `error` instead of `load`.
 * That is why the upgrade is its OWN entry and not a second return value from the predicate: two steps, two
 * components, called in the standard's order by every site that builds a request.
 *
 * THE THREE RETURN TOKENS ARE MODELLED AS BOOLEANS BECAUSE THE STANDARD SPELLS THEM THREE WAYS. §4.3's prose
 * says it returns "Prohibits Mixed Security Contexts" or "Does Not Prohibit Mixed Security Contexts", its own
 * step 3 returns "Does Not Restrict Mixed Security Contexts", and Mixed Content §4.1 step 1.3 tests for
 * "Does Not Restrict Mixed Security Contents". Three spellings of two states: a `const char *` domain here
 * would be a fourth,
 * and every comparison against it a chance to pick the wrong one silently. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_MIXED_CONTENT_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_MIXED_CONTENT_H
#include <stdbool.h>

#include "quickjs.h"
#include "core/url/url.h"

/* §4.3 "Does settings prohibit mixed security contexts?", over the environment settings object `ctx` IS.
 * TRUE is the section's "Prohibits Mixed Security Contexts".
 *
 * WHAT IS NOT COVERED, AND IT IS THE STEP THIS ENGINE'S SHAPE MAKES NEARLY EMPTY. Step 2.2 walks the
 * document's ANCESTOR NAVIGABLES and returns Prohibits if any ancestor's active document has a potentially
 * trustworthy origin. An instance here is an origin-keyed agent cluster (SECURITY.md), so every ancestor IN
 * THIS INSTANCE shares this document's origin and is already answered by step 1 — the step can only add an
 * answer for an ancestor in a PEER instance, whose origin this heap cannot read without a cross-instance
 * suspend that a settings test has no flow to make. WHAT THE NEXT DIFF BUILDS: the ancestors' origins carried
 * on the navigable the way HTML §7.1.7 "Policy containers"' items now are, stated by the instance that
 * holds them.
 * HOW ITS ABSENCE SHOWS: an http document framed by an https one answers "does not restrict" here and
 * "prohibits" in a browser, so its own http subresources load where a browser blocks them — an
 * UNDER-block, which is what this engine did for every document before this component existed.
 * HTML §3.1.3's ancestor-origins list is NOT that input and must not be used as one: it is a different
 * algorithm with its own same-origin masking, built for `location.ancestorOrigins`. */
bool mixed_content_settings_prohibit(JSContext *ctx);

/* Mixed Content §4.4 "Should fetching request be blocked as mixed content?" — TRUE is its step 2's "blocked".
 * `url` is the request's current URL PARSED, and it is the address AFTER Fetch §4.1 step 6 has run: a caller
 * that passes the pre-upgrade address gets the wrong answer for every upgradeable destination.
 * `destination` is Fetch §2.2.5's destination string, read by step 1.4 alone.
 * `has_parent` is Mixed Content §4.4 step 1.4's "request's target browsing context has no parent browsing
 * context", inverted at
 * the call so the caller states a fact about its own navigable rather than this component guessing one. */
bool mixed_content_should_block_fetching(JSContext *ctx, const UrlRecord *url, const char *destination,
                                         bool has_parent);

/* §4.1 "Upgrade a mixed content request to a potentially trustworthy URL, if appropriate" — Fetch §4.1 step 6.
 *
 * RETURNS AN OWNED SERIALIZATION OF THE UPGRADED ADDRESS, OR NULL FOR "RETURN WITHOUT MODIFYING REQUEST".
 * NULL is the standard's step 1 and is not a failure: five conditions each end the algorithm with the request
 * untouched, and a caller reads NULL as "keep the address you have" rather than as an error. Returning the
 * address unchanged instead would make a caller free and re-take a string on every ordinary fetch and would
 * hide which of the two things happened.
 * `initiator` is Fetch §2.2.5's initiator. Mixed Content §4.1 step 1.5 is its only reader here —
 * "request's destination is `image` and request's initiator is `imageset`",
 * the one historical exemption. NULL is a request whose creating algorithm sets no
 * initiator, which is §2.2.5's own initial value and is a real answer rather than an absence. */
char *mixed_content_upgrade_url(JSContext *ctx, const char *url, const char *destination,
                                const char *initiator);

#endif
