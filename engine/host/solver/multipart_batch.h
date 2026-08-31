/* THE SUB-REQUESTS A REQUEST BODY NAMES — a multipart body whose parts carry embedded HTTP request messages
 * (RFC 2046 §5.1's multipart framing over RFC 9112 §3's request-line), read for the endpoints they address.
 *
 * WHY THIS EXISTS. A batch API takes N calls in ONE request: `POST /batch` with
 * `Content-Type: multipart/mixed; boundary=b` and a body whose parts are `GET /v1/animals/pony HTTP/1.1`,
 * `POST /v1/farms HTTP/1.1`, … Those N addresses are endpoints the PAGE'S OWN CODE composed — the strongest
 * possible provenance, since forced execution watched them being built — and the engine was recording exactly
 * one endpoint for the batch and discarding every one of them. CLAUDE.md §What-the-tool-produces is the whole
 * of the reason: the surface this tool emits is the set of endpoints the bundle CAN address, and a batch body
 * is a place where twenty of them are written down in full.
 *
 * WHY IT IS THE ENGINE'S, AND WHY NOW. It was `parseMultipartBatchRequest` in
 * `extension/lib/protocol-parsers.js` — where a JS copy still runs, over a different input — and it is the ONE
 * capability in that file whose consumer already exists in C. Everything else there (protobuf/JSPB trees,
 * gRPC-Web frames, SSE, NDJSON, GraphQL, batchexecute) decodes a body into a SCHEMA, and schema inference is
 * `moat_schema.c`'s, which is not written; this one decodes a body into an ADDRESS, and `solver/endpoint.c`
 * has taken addresses since before this file existed. THE TEST IS THE INPUT, NOT THE LANGUAGE: this decodes a
 * body THE ENGINE'S OWN `fetch()` composed, mid-flow, where the answer must fork per arm and park with the
 * flow. The JS copy stayed because it is handed a different input — bodies `intercept.js` captured off live
 * traffic, and bodies the user edits in the popup's multipart panel — and two inputs are not one duplicated
 * algorithm. (This paragraph used to derive the same conclusion from a MIGRATION QUEUE's ordering rule; that
 * queue, `extension/jsaudit.mjs`, is deleted, because the only trajectory it could have had was a migration
 * CLAUDE.md §Architecture now forbids.)
 *
 * WHERE IT IS CALLED FROM. `core/fetch/fetch.c`, on the line that already records the outer request's own
 * endpoint — the one point that holds the request's header list, the extracted body bytes and §5.1's body
 * Content-Type together, and the point every `fetch()` the page performs converges on. It is not called from a
 * reply path: this reads what the page SENDS, which is why it is not in `solver/reply_decode.c` ("what a reply
 * body teaches") even though both end at `endpoint_record`.
 *
 * NOTHING HERE IS FIRED. §Attacker sources: "A state-mutating request is NEVER fired to learn." A sub-request
 * is RECORDED, exactly as the outer one is, and the only thing a recorded endpoint can start is
 * `endpoint.c`'s discovery seeding, which is GET-only structurally. A batch naming a `DELETE` teaches the
 * reviewer that the endpoint exists; it never asks the server anything.
 *
 * THE BYTES ARE THE PAGE'S, WHICH DECIDES EVERY DISPOSITION IN HERE. A body composed from `location.hash` is
 * attacker-shaped input, and CLAUDE.md §Offensive-programming names that case explicitly as NOT a `@WHY`: a
 * malformed boundary, a part with no blank line, a first line that is not a request-line, a field line with no
 * colon are all ordinary FACTS ABOUT DATA and each ends in "record nothing", never in an abort and never in a
 * tolerant partial result that records half a request line. The DCHECKs in the implementation are about this
 * component's OWN logic — that the walk advances, that a span stays inside the body it came from — which is
 * the only thing an assert here could be about.
 */
#ifndef ENGINE_HOST_SOLVER_MULTIPART_BATCH_H
#define ENGINE_HOST_SOLVER_MULTIPART_BATCH_H

#include "quickjs.h"
#include "core/fetch/headers.h"

/* READ ONE REQUEST BODY FOR THE SUB-REQUESTS IT NAMES.
 *
 * `hdrs` is the request's own header list as §5.3 built it, or NULL for a request that carries none.
 * `body_mime` is §5.1's Content-Type for the extracted body (a JS string, or undefined for a body arm that has
 * none). Both are taken because §5.3 step 37.4 states the request's type in exactly those two places and in
 * that order — the header list when it names one, the body's own arm otherwise — so asking only one of them
 * would read a `Blob`-bodied batch as typeless. This is the spec's own rule, not a default filling a hole.
 * `body`/`body_n` are the extracted bytes. A NULL or empty body is the spec's null body and teaches nothing.
 *
 * A body that is not multipart, or whose parts carry no HTTP request messages, records nothing and is not an
 * error — most request bodies are neither. That is why there is no return value to test. */
/* `prov` is what the OUTER request is evidence of (solver/pending.h's PROV_*), handed down rather than read
 * here: a sub-request written inside a body is evidence of exactly what the request carrying it is, and the
 * caller has already asked the running path once for the act they are both part of (core/fetch/fetch.c). */
void multipart_batch_learn(JSContext *ctx, const HeaderList *hdrs, JSValueConst body_mime,
                           const char *body, size_t body_n, int prov);

#endif
