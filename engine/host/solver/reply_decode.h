/* WHAT A REPLY BODY TEACHES — the one component that reads a fetched response for its CONTENT rather than for
 * the promise it settles.
 *
 * WHY IT IS THE ENGINE'S. CLAUDE.md §Solver: "Learning from replies is the POINT, never suppressed... The
 * JS/JSON a server returns is the richest source of real example values." The host had that reading spread over
 * `extension/lib/response-decode.js`, `lib/protocol-parsers.js` and `lib/protobuf.js`, and
 * `extension/lib/discovery.js` held two of their callees: the
 * magic-byte classifier and the React Flight parser. TWO OF THOSE FOUR BELONG HERE AND THE OTHER TWO DO NOT,
 * and what separates them is the INPUT rather than the algorithm:
 * `lib/protocol-parsers.js` and `lib/protobuf.js` are the
 * codecs and they become this component; `lib/response-decode.js` is the LIVE-CAPTURE INTAKE — one
 * `handleResponseBody` the offscreen's chrome.runtime router hands every body intercept.js caught — and this
 * component has never been handed one of those. It reads a reply THE ENGINE FETCHED, so the intake's
 * aggregation belongs to the moat and its relay to the bridge. The classifier is `classifyResponseAsset` in
 * `extension/lib/discovery.js` and it STAYED there, because its input is a body intercept.js captured off the
 * live page and not one this engine fetched. What this file asks about the reply it IS handed is neither of
 * those: it READS the type the host stamped on the record — `computedType`, written by
 * `extension/lib/safe-fetch.js`, the zone that read the bytes, which CLAUDE.md §Architecture makes the only
 * source of sniffing. A renderer that sniffs for itself, or that re-derives a type from a raw header and calls
 * that the answer, can mine a cross-origin body a real renderer would never have been shown, and is in any
 * case a second voice on a question decided one hop earlier by the side that could see the body. The Flight
 * parser is here, because a wire protocol's framing is what this component is for.
 *
 * WHERE IT IS CALLED FROM, AND WHY THERE. `engine_provide` is the ONE point a reply to an ADDRESS-KEYED park
 * crosses exactly once — a URL two flows parked on is answered there once — so a reply is read for its content
 * there and not in the per-flow drain, where it would run once per waiter. What is learned is a
 * fact about the SERVER and not about a flow's world, so it is not per-flow state and takes no COW capture,
 * exactly as the endpoint surface does not.
 * THE RULE IS ONCE PER REPLY AND IT WAS NEVER ONCE PER FILE, which is the distinction a second caller makes
 * visible: core/xhr/xml_http_request.c's `xhr_take_reply` is the other one, and it is the same rule read on a
 * transport that has no address index to dedup through. An XMLHttpRequest is one request with one waiter, so
 * that site is crossed once per reply by construction — its two callers are the two arms of Fetch §4.1 main
 * fetch and a send takes one of them. What a caller owes is that property; what it may NOT do is ask a second
 * question here, which is why the Flight arm and the asset verdict below are shared rather than copied.
 *
 * IT USED TO SAY `EVERY FETCHED REPLY` AND THAT IS AN ABSOLUTE ONE GREP REFUTES, WHICH IS WHY IT IS NARROWED
 * RATHER THAN DELETED: a reader re-deriving the dedup argument re-derives the word `every` with it, and the
 * word is what makes the population below invisible. `engine_provide` walks solver/pending_index.h's
 * (method, url) set, and solver/pending.c tracks every kind into that set BUT ONE —
 * `if (kind != FLOW_PENDING_HOSTREQ) pending_index_track(e)` — so a synchronous host rendezvous is answered
 * through `engine_host_answer` by REQUEST ID and never reaches this file THROUGH THAT DOOR. That is still
 * exactly true of the index and is no longer true of the population: §3.5.6 "The send() method"'s rendezvous
 * now reaches this entry from the XHR machine itself, which holds the pair the index could not. The other
 * HOSTREQ kinds are not replies at all — a cross-instance operation, a document name, a number — so what is
 * still outside is a rendezvous that carries a BODY and has no caller here, and the honest form of `every` is
 * a claim about CALLERS rather than about the index.
 *
 * THE RESIDUAL THAT STOOD HERE IS RETIRED BY ITS OWN TERMS, AND THE ANSWER IS YES: AN XMLHttpRequest'S
 * JAVASCRIPT REPLY BODY IS A PROGRAM THIS ENGINE RUNS. core/xhr/xml_http_request.c's `xhr_take_reply` reads
 * the same `computedType` this file reads, decodes it with §3.6.6 "Response body"'s own final encoding, and
 * hands it to `engine_queue_fetched_script` — the ONE compile entry, which solver/engine.c's
 * FLOW_PENDING_RESOLVE delivery also calls, so there is no second door and no arm in this file.
 *
 * THE REASONING IS AT THAT SITE AND NOT HERE, because it is about a transport this file does not model. What
 * is kept here is the SHAPE OF THE WRONG ANSWER, because a reader who re-derives the question re-derives it:
 * the residual put the doubt on Fetch §2.2.5 "Requests"' DESTINATION — §3.5.6 "The send() method" step 6
 * states eleven request members and names no destination, so an XMLHttpRequest keeps the empty string, which
 * is outside that section's `script-like` set while a `<script src>`'s "script" is inside it. Every clause of
 * that is true and it settles nothing, because THE DOOR THAT ALREADY COMPILES IS `fetch()` AND ITS
 * DESTINATION IS THE EMPTY STRING TOO — §5.4 "Request class" only ever reads one back, §5.6 "Fetch methods"
 * does not mention one, and §2.2.5's own table puts `fetch()` and `XMLHttpRequest` in ONE ROW against
 * `<script>`'s. An axis on which two doors are identical cannot be why one of them refuses. The axis this
 * engine does discriminate on is at `engine_pending_resource_url` (solver/engine.h) and it is whether the
 * PAGE'S OWN CODE receives the bytes at all — preload, modulepreload and an image decode do not, and §3.6.10
 * "The responseText getter" does.
 *
 * WHAT IS STILL OPEN IS NOT ABOUT THIS TRANSPORT: a page whose CSP admits neither `eval` nor this reply's
 * origin as a script source cannot execute these bytes by any route, and nothing in this tree reads a policy
 * before EITHER compile. Refusing on it would refuse `fetch()` with it, so it is a question about the compile
 * entry; it is stated at the XHR route and is not a reason this door differs.
 * RETIREMENT: this record goes when a census counts the programs each reply door QUEUED, because a reader can
 * then watch both doors work instead of re-deriving from this file's silence which of them exists.
 *
 * IT HOLDS NO STATE. Everything it learns goes straight into solver/endpoint.c, so there is no table to
 * initialise, none to free, and no line for it in engine.h's release column. A component that kept its own copy
 * of the endpoints would be a second surface for the popup to reconcile with the first.
 *
 * IT NEVER SNIFFS A FORMAT OUT OF A BODY. §RUN, DON'T MATCH: "no regex/name/identifier matching, scoring,
 * heuristics". The protocol is read off the response's COMPUTED MIME TYPE — the host's ONE decision about
 * what this resource is, taken where the bytes were; the JS it replaces guessed React Flight from a body whose
 * first two lines matched `^[0-9a-f]+:` whenever the Content-Type was empty, which is the same shape as a JSON
 * object keyed by digits.
 */
#ifndef ENGINE_HOST_SOLVER_REPLY_DECODE_H
#define ENGINE_HOST_SOLVER_REPLY_DECODE_H

#include "quickjs.h"

/* READ ONE REPLY FOR WHAT IT TEACHES. `url` is the address that was fetched — the BASE every relative address
   inside the body resolves against, which is why it is a parameter and not something rederived from the record.
   `reply` is the host's reply record (`{status, statusText, headers:[[name,value]…], body, urlList,
   computedType}`) or JS_NULL for a network error, handed over exactly as it arrived: "this address answered
   nothing" is a positive answer and not an engine invariant.
   `method` IS THE OTHER HALF OF THE REQUEST'S NAME, and it is a parameter because the asset verdict below is
   REPORTED and not merely acted on: the reply register is keyed on the (method, url) pair, so a verdict that
   named only the address would mark a POST to it on the strength of what a GET returned.
   A reply whose computed MIME type is an image, a media stream, a font or an archive teaches nothing here and
   is not an error — CLAUDE.md §Attacker sources: "Static assets are NEVER endpoints (magic-byte + content-type,
   not URL suffix) but still drive the code path", and the driving is the flow's, not this file's. THAT
   SENTENCE HAS TWO CLAUSES AND THIS FILE USED TO ACT ON THE SECOND ONLY: it returned without learning, which
   is right, and told nobody, so the address stayed on the @H surface that the first clause is a rule about.
   It calls solver/endpoint.c's `endpoint_mark_asset` now, which is what makes the classification a mechanism
   rather than a computation with no reader.
   `prov` IS WHAT THIS REPLY IS EVIDENCE OF — one of solver/pending.h's PROV_* — and it is the fact that makes
   everything below reportable at all. CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE: "a forced reply's values
   are learned and CARRIED AS FORCED, never merged into the observed pool", and the reason it gives is that
   the danger is not uselessness but PLAUSIBILITY — "a 401 body parses as JSON and yields fields that exist
   nowhere, an error envelope becomes a config, and the fabrication then PROPAGATES, since one invented field
   is the example that shapes the next endpoint". A chunk address mined out of a route reached only on a
   forced arm has bytes indistinguishable from one the document's own parser fetched, so an address emitted
   with its grade silent is read as the second, which is the fabrication performed on the @H surface.
   IT IS A PARAMETER AND NEVER READ HERE, AND THE REASON IS THE REQUEST RATHER THAN THE CALLER. The grade
   belongs to the REQUEST and travels with the operation (§scheduler: "an operation that becomes a work item
   takes its inputs with it — anything it reads back off the object it acts on is read at the wrong TIME"), so
   a reply is evidence at the grade its request was FIRED at and never at whatever path happens to be standing
   when the bytes land. That holds on both doors and is why this is a parameter on both: solver/engine.c's
   `engine_provide` runs OUTSIDE any flow, where `engine_prov_of_running_path` would answer about a path that
   is not standing at all, and composes the grade by joining the records the reply answers (see the fold there
   for the rule); core/xhr/xml_http_request.c runs INSIDE the flow that sent, on a LATER turn than the one
   §3.5.6 "The send() method" step 6 composed the request on, and carries the grade it took at that step on
   its own record. A caller that asked the running path here would be the second answer that rule forbids —
   and on the XHR door it would be a different one from the grade the trusted zone made its firing decision
   from, which is one exchange graded twice. */
void reply_decode_learn(JSContext *ctx, const char *method, const char *url, JSValueConst reply, int prov);

#endif
