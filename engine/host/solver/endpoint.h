/* @H ENDPOINT SURFACE — the deduped set of endpoints the forced execution learned, rebuilt clean.
 *
 * Every request host-edge (fetch/XHR/...) funnels one endpoint into record_endpoint; identical (method, url)
 * pairs dedup on the way in. A concolic URL contributes its SHAPE (`/api/region/{state}.region`), a concrete
 * one its literal. endpoint_result assembles the harness `@H` structure (fetchCallSites) for JSON emit.
 *
 * A REQUEST HAS THREE PLACES A VALUE CAN LAND and this surface names all three: the PATH (a `{hole}` the code
 * interpolated into the address, whose example is recovered by aligning the concolic's concrete URL against
 * its shape), the QUERY STRING, and the BODY (read in the body's own content-type). It named one of them for
 * the whole life of the file — the query — while every consumer branched on a `location` field nothing wrote,
 * so the path-parameter registration and the entire request-body schema had never run once and both read as
 * live. See path_scan for why a hole is re-spelled around the whole segment.
 *
 * RESIDUAL: a hole's NAME is the segment's shape with the braces moved, and lib/merge.js round-trips the URL
 * through `new URL()` before matching it. WHATWG URL's path percent-encode set is the query set plus `?`, `^`,
 * backtick, `{` and `}` — so `.`, `[` and `]` survive that trip and offscreen-brain.js's `_decHoles` restores
 * the braces, while a member name holding a SPACE or a `#` would come back percent-encoded and its hole would
 * stop matching. Not yet handled anywhere; it is page data, so it is a residual rather than an assert. */
#ifndef ENGINE_HOST_SOLVER_ENDPOINT_H
#define ENGINE_HOST_SOLVER_ENDPOINT_H

#include "quickjs.h"

void    endpoint_init(void);
void    endpoint_free(void);
void    endpoint_suppress(int on);   /* 1 during a candidate/verify re-run: its requests are @S artifacts, not @H */

/* A HEADER THE REQUEST CARRIES — half of what makes an endpoint usable, and the half this surface did not have.
   An endpoint reachable only with `Authorization` and `X-Api-Version` is not reproducible without them, and the
   popup has read a `requiredHeaders` record per call site since before the engine could emit one. The value is a
   plain string for the same reason a param's example is: a concrete one is the literal the code computed, and an
   unknown one is its SHAPE (`{state}.token`), which is what marks it as a runtime value the reviewer must
   supply. Borrowed for the length of the call — the surface copies what it keeps. */
typedef struct { const char *name, *value; } EndpointHeader;

/* THE REQUEST BODY THE PAGE COMPOSED. `mime` is the content-type the request will actually send (the header
   list's, else the one Fetch §5.4 "Request class" step 37.4 extracts) and it is what decides how the bytes are
   READ — the body's own format, never a guess from the shape of the bytes. A JSON MIME type (MIME Sniffing
   §4.6 "MIME type groups") is read as a name -> value document and `application/x-www-form-urlencoded` by the
   same grammar as a query string; any other type records no fields rather than a guess at some.
   Borrowed for the length of the call like the headers.
   It is a separate struct and not three arguments because a body is one fact: bytes with no type are bytes
   nothing can name the fields of, and a type with no bytes is not a body. */
typedef struct { const char *mime, *bytes; size_t len; } EndpointBody;

/* Record one learned endpoint (deduped by method+url). `url` may be concolic (shape) or concrete. Headers are
   MERGED into a same-identity endpoint: a header seen with a concrete value supersedes the same header seen
   only as a shape, which is the rule the param values already follow. `body` is NULL where the request has
   none — which is a STATEMENT (this request sends no body), never an unknown. */
void    endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                        const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body);

/* The @H surface as a malloc'd JSON ARRAY (caller frees) — findings are C data, so the emit is C, never a
   JS-object round-trip.
   `[ {"method":..,"url":..,"params":[{"name":..,"location":..,"validValues":[..],"excludes":[..],
      "bounds":{"minimum"|"exclusiveMinimum":N,"maximum"|"exclusiveMaximum":N}}]}, ... ]`.
   Every param states WHERE IT LANDED — "path", "query" or "body" — because that is what the reviewer replays
   it with, and because a consumer that has to default the field cannot tell an unknown from a query param.
   AND EVERY PARAM STATES BOTH OF THE TWO FACTS A SHAPE IS MADE OF. `validValues` is PROVENANCE-and-example —
   who must supply the value, and what the code computed for it where it computed one. `excludes` is DOMAIN —
   what this endpoint's own equality gates PROVED the value is not, on every observed path to the request.
   Carrying only the first is a WRONG report and not a partial one: a param proved to be neither "admin" nor
   "prod" and a param nothing ever tested render with identical bytes, so the silence about the gate is read
   as the positive statement "anything goes". Forced multi-path is what makes the second fact plentiful — it
   runs BOTH arms of every equality gate, so a pin and an exclusion are minted at the same rate, and the arm
   that is not the one the shipped bundle took is exactly the arm this tool exists to explore.
   `excludes` is OMITTED where no such constraint held on every observed path, and that absence IS the
   statement — never an empty array, which a consumer could not tell apart from an unconstrained param.
   `bounds` IS THE SAME FACT OVER AN ORDERED DOMAIN — what this endpoint's own ORDERING gates proved the value
   must be greater or less than, on every observed path — and it is emitted in JSON Schema Validation 2020-12
   §6.2 Validation Keywords for Numeric Instances (number and integer)'s own vocabulary: at most one of
   §6.2.4 "minimum" / §6.2.3's exclusive twin "exclusiveMinimum", and at most one of §6.2.2 "maximum" /
   §6.2.3 "exclusiveMaximum". Each value is a JSON NUMBER, spelled as the page's own literal.
   BOTH SIDES CAN BE PRESENT, because `if (x > 5 && x < 100)` is TWO observations of one parameter and a
   record holding only one of them is a wrong report by this rule's own terms.
   It carries NO member of the interval: §@H forbids inventing `6` for `x > 5`, so a value appears in
   `validValues` only where the code COMPUTED one. `bounds` is omitted entirely where no ordering gate's claim
   survived every observed path, and that absence is the statement, exactly as `excludes`' is.
   It is an array and not a document
   because the DOCUMENT is one thing the host reads once (result.h): a surface that wrapped itself could not
   be composed with the others without a host-side splice, which is the host owning structure again. */
char   *endpoint_json_array(void);

/* WHAT THE RESOURCE AT AN ADDRESS TURNED OUT TO BE, told to the surface that filed the request for it. §Attacker
   sources: "Static assets are NEVER endpoints (magic-byte + content-type, not URL suffix) but still drive the
   code path" — a rule whose test is over BYTES, so it is answerable only on the reply, and whose subject is
   THIS surface, so the answer has to arrive here or it is a computation nothing reads. solver/reply_decode.c
   asks it of `computedType` (the trusted zone's one type decision, CLAUDE.md §Architecture) and calls this with
   the (method, url) pair the reply register was keyed on. The record is kept and OMITTED from the emit rather
   than deleted — the same address may be recorded again by a later call site and the verdict is about the
   resource, not about the sighting.
   `endpoint_count` STOOD HERE AND HAD NO CALLER ANYWHERE IN THE TREE. It returned the raw record count, which
   after the flag above is a different number from the one the surface emits, so what was merely dead became a
   second answer waiting for its first reader to trust it. */
void    endpoint_mark_asset(const char *method, const char *url);

#endif
