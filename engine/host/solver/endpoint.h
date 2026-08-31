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
   none — which is a STATEMENT (this request sends no body), never an unknown.

   `prov` IS WHAT THIS SIGHTING IS EVIDENCE OF — one of solver/pending.h's PROV_* — AND IT IS PART OF THE
   ENDPOINT'S IDENTITY. CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE: "a forced reply's values are learned and
   CARRIED AS FORCED, never merged into the observed pool", and the choice between two pools and one pool with
   a grade every reader must consult is settled by which one makes the wrong answer IMPOSSIBLE rather than
   discouraged. A folded grade does not: a FORCED sighting merging into a record whose grade folds to
   `observed` publishes, under the strongest claim this surface can make, a value that exists only because a
   gate was forced — the same fabrication as inventing `6` for `x > 5`, arriving through a merge rule instead
   of through a solver. Putting the grade in `same_identity` means the merge cannot happen: a forced sighting
   and a derived one are two records, each carrying only the values observed at its own grade, and no
   consumer has to remember anything. It is the rule `loc` already follows one field down — "two params of
   the same name in two places are two params".
   AND IT IS DELIBERATELY *NOT* THE PENDING LINE'S RULE, which folds a deduped set to its MOST OBSERVED
   member. That fold is right there and wrong here, and the difference is not a preference: the pending set is
   ONE REQUEST that ONE reply answers, so the line has to state the single fact that survives every member or
   the host cannot fire it at all. This surface is a set of SIGHTINGS, and nothing forces two of them into one
   row but a dedup convenience — so two rows for one address, one graded `derived` and one `forced`, are two
   TRUE statements, and the second is precisely the row a reviewer needs in order to distrust it.

   EVERY PRODUCER STATES IT AND NOTHING HERE DERIVES IT. A request this engine builds by RUNNING the page's
   code states `engine_prov_of_running_path()` (solver/engine.h), read at the act because that is when the
   path it is about is standing; a reply-learned address states the grade of the reply that named it
   (solver/reply_decode.h), which no flow can answer because that path runs outside every flow. A default
   here would be `observed` by the numbering, on a record nobody graded. */
void    endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                        const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body, int prov);

/* The @H surface as a malloc'd JSON ARRAY (caller frees) — findings are C data, so the emit is C, never a
   JS-object round-trip.
   `[ {"method":..,"url":..,"provenance":"observed"|"derived"|"forced",
      "params":[{"name":..,"location":..,"validValues":[..],"excludes":[..],
      "bounds":{"minimum"|"exclusiveMinimum":N,"maximum"|"exclusiveMaximum":N},
      "predicates":[{"method":..,"arguments":[..],"holds":true|false}]}]}, ... ]`.
   Every param states WHERE IT LANDED — "path", "query" or "body" — because that is what the reviewer replays
   it with, and because a consumer that has to default the field cannot tell an unknown from a query param.
   AND EVERY RECORD STATES WHAT IT IS EVIDENCE OF. `provenance` is one of solver/engine.h's three words and it
   is ALWAYS PRESENT — there is no absence to read as a statement here, because the three words are exhaustive
   over the ways this engine can come to know an address and a silent grade is read as the strongest of them.
   It is the record's, not a param's: a sighting is graded as a whole (the path it was built on, or the reply
   that named it), and every value on it was observed at that grade because the grade is part of the record's
   identity — see `endpoint_record`. WHAT THE READER DOES WITH IT: `derived` is the tool's headline claim (the
   app's own code computes this request and no session sent it); `observed` is a real load of the document;
   `forced` is an address that exists only because a gate was forced, whose reply CLAUDE.md §@H forbids ever
   being reported as the other two, and which extension/lib/popup-send.js renders as its own tag rather than
   letting it wear `[UNUSED]`.
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
   §6.2.4 "minimum" / §6.2.5 "exclusiveMinimum", and at most one of §6.2.2 "maximum" / §6.2.3
   "exclusiveMaximum". Each value is a JSON NUMBER, spelled as the page's own literal.
   THE INTERVAL IS ONLY AN ASSERTION IF THE RECORD ALSO SAYS THE VALUE IS A NUMBER. §6.2.5's text is "If the
   instance is a number, then the instance is valid only if it has a value strictly greater than (not equal
   to) exclusiveMinimum" — so all four keywords assert NOTHING against a non-number instance, and a consumer
   that carries this interval beside a `string` type has emitted the domain and erased it in one record.
   That the value IS compared as a number is stated by this very field: concolic_rel_hook records a bound
   only for a finite Number operand, and ECMAScript §7.2.12 IsLessThan step 3 takes the string comparison
   only when BOTH sides are Strings. lib/learn.js is where that is read.
   BOTH SIDES CAN BE PRESENT, because `if (x > 5 && x < 100)` is TWO observations of one parameter and a
   record holding only one of them is a wrong report by this rule's own terms.
   It carries NO member of the interval: §@H forbids inventing `6` for `x > 5`, so a value appears in
   `validValues` only where the code COMPUTED one. `bounds` is omitted entirely where no ordering gate's claim
   survived every observed path, and that absence is the statement, exactly as `excludes`' is.
   `predicates` IS THE THIRD OF THE THREE WAYS A GATE NARROWS A DOMAIN, and the one §@H names in its own
   headline example (`{startsWith:/api}`). An equality determines a VALUE on one arm, an ordering an INTERVAL
   on both, and a METHOD CALL neither — so `if (!path.startsWith("/api")) return;` recorded nothing at all
   through the first two, and a parameter a prefix check gated rendered with the same bytes as one nothing had
   ever tested. Each entry is `{"method":<string>,"arguments":[<string>...],"holds":<boolean>}`: the property
   NAME the page read off the unknown, every argument as the page's own §7.1.19 ToString of it, and WHICH ARM
   this run took. `holds:false` is a fact and not a modifier — forced multi-path runs both arms of every gate,
   so the proved negation arrives at the same rate as the proof, and it is the arm the shipped bundle did not
   take. `arguments` may legitimately be EMPTY (`x.trim()` tested as a condition) and is always present.
   IT IS THE ENGINE'S OWN VOCABULARY ON PURPOSE. JSON Schema Validation 2020-12 §6.3.3 "pattern" is the only
   keyword that could carry one of these and it can carry only the true arm, only for a method whose meaning
   something decided, and only through a regex translation of the page's literal — three ways to be silently
   wrong where this record is merely a transcript. endpoint.c's emit states the same at the line that writes
   it. Nothing downstream re-implements a method either: lib/learn.js merges these by INTERSECTION (the rule
   `excludes` follows, because a predicate is a claim about the ENDPOINT and only one every observed path
   obeyed belongs on the record) and lib/popup-form.js renders them as a constraint badge.
   IT INVENTS NOTHING AND STAYS A SHAPE: no string satisfying the predicate is ever emitted, exactly as no
   member of `bounds`' interval is. `predicates` is omitted where no call predicate survived every observed
   path, and that absence is the statement.
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
