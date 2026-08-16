/* WHAT A REPLY BODY TEACHES — the one component that reads a fetched response for its CONTENT rather than for
 * the promise it settles.
 *
 * WHY IT IS THE ENGINE'S. CLAUDE.md §Solver: "Learning from replies is the POINT, never suppressed... The
 * JS/JSON a server returns is the richest source of real example values." The host had that reading spread over
 * `extension/lib/response-decode.js`, `lib/protocol-parsers.js` and `lib/protobuf.js` — jsaudit step 2 names
 * THIS FILE as what all three become — and `extension/lib/discovery.js` (step 1) held two of their callees: the
 * magic-byte classifier and the React Flight parser. The classifier became its own standard
 * (browser_process/network/mime_sniff.c, WHATWG MIME Sniffing) — and that standard is the NETWORK SERVICE'S,
 * which is now a program of its own rather than a directory of this one, so this file
 * does not call it: §7 sniffs bytes, CORB gates on the result, and a renderer that sniffs for itself can mine a
 * cross-origin body a real renderer would never have been shown. What this file asks instead is
 * Fetch §4's extract a MIME type, the server's own STATEMENT, which the renderer legitimately parses because the same
 * record is what `Blob.type` and `accept` matching are read off. The Flight parser is here, because a wire
 * protocol's framing is what this component is for.
 *
 * WHERE IT IS CALLED FROM, AND WHY THERE. `engine_provide` is the ONE point every fetched reply crosses exactly
 * once — a URL two flows parked on is answered there once — so a reply is read for its content beside
 * `req2proto_learn` and not in the per-flow drain, where it would run once per waiter. What is learned is a
 * fact about the SERVER and not about a flow's world, so it is not per-flow state and takes no COW capture,
 * exactly as the endpoint surface does not.
 *
 * IT HOLDS NO STATE. Everything it learns goes straight into solver/endpoint.c, so there is no table to
 * initialise, none to free, and no line for it in engine.h's release column. A component that kept its own copy
 * of the endpoints would be a second surface for the popup to reconcile with the first.
 *
 * IT NEVER SNIFFS A FORMAT OUT OF A BODY. §RUN, DON'T MATCH: "no regex/name/identifier matching, scoring,
 * heuristics". The protocol is read off the response's COMPUTED MIME TYPE, which is a statement the server made
 * and a standard interpreted; the JS it replaces guessed React Flight from a body whose first two lines matched
 * `^[0-9a-f]+:` whenever the Content-Type was empty, which is the same shape as a JSON object keyed by digits.
 */
#ifndef ENGINE_HOST_SOLVER_REPLY_DECODE_H
#define ENGINE_HOST_SOLVER_REPLY_DECODE_H

#include "quickjs.h"

/* READ ONE REPLY FOR WHAT IT TEACHES. `url` is the address that was fetched — the BASE every relative address
   inside the body resolves against, which is why it is a parameter and not something rederived from the record.
   `reply` is the host's reply record (`{status, statusText, headers:[[name,value]…], body, urlList}`) or
   JS_NULL for a network error, handed over exactly as it arrived: "this address answered nothing" is a positive
   answer and not an engine invariant.
   A reply whose computed MIME type is an image, a media stream, a font or an archive teaches nothing here and
   is not an error — CLAUDE.md §Attacker sources: "Static assets are NEVER endpoints (magic-byte + content-type,
   not URL suffix) but still drive the code path", and the driving is the flow's, not this file's. */
void reply_decode_learn(JSContext *ctx, const char *url, JSValueConst reply);

#endif
