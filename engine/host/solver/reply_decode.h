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
 * WHERE IT IS CALLED FROM, AND WHY THERE. `engine_provide` is the ONE point every fetched reply crosses exactly
 * once — a URL two flows parked on is answered there once — so a reply is read for its content there and not
 * in the per-flow drain, where it would run once per waiter. What is learned is a
 * fact about the SERVER and not about a flow's world, so it is not per-flow state and takes no COW capture,
 * exactly as the endpoint surface does not.
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
   IT IS A PARAMETER AND NEVER READ HERE. This file runs on the reply-delivery path, OUTSIDE any flow, so
   `engine_prov_of_running_path` would answer about a path that is not standing — the grade belongs to the
   REQUEST and travels with the operation (§scheduler: "an operation that becomes a work item takes its inputs
   with it — anything it reads back off the object it acts on is read at the wrong TIME"). solver/engine.c's
   `engine_provide` composes it by joining the records the reply answers; see the fold there for the rule. */
void reply_decode_learn(JSContext *ctx, const char *method, const char *url, JSValueConst reply, int prov);

#endif
