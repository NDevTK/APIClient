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
   same grammar as a query string; any other type records no FIELDS rather than a guess at some — and
   FORWARDS ITS BYTES, which is the half that was missing. A protobuf or gRPC-Web payload has no field this
   engine can name, and for as long as that meant silence the record said `POST /pkg.Service/Method` and
   nothing whatever about what it posts, which is most of what §What-the-tool-produces asks. The bytes ride
   the record as `bodyBase64` beside `bodyMime` (the engine's own `btoa` codec, never a second base64).
   NOTHING DECODES THEM INTO FIELDS, AND THE SENTENCE THAT STOOD HERE SAID THE ZONE DID. It named a wire
   decoder in the trusted zone as the reader that turns these bytes into a field model, and that reader is
   gone: reconstructing a field number out of a byte stream is INFERRING an answer the run already had, since
   the page's own serializer executed with the real names and the real values in its hands, and the pattern
   over a MIME string that selected it was the protocol-specific recognizer §Architecture bans. The sentence
   is rewritten rather than deleted because a reader who re-derives the bytes-are-here-so-something-should-
   read-them argument will write that decoder again.
   SO THESE BYTES ARE AN OBSERVATION AND NEVER A FIELD MODEL — this request sent exactly these bytes under
   exactly this content-type, which is what a reviewer REPLAYS and is not an answer to "what values can be
   sent". What answers that is `params`, and for every body the page composed AS A STRING it already does:
   a concatenation carries its operands' display forms into the result's shape, so `'{"id":"' + id + '"}'`
   reaches this surface spelling `{"id":"{state}.id"}` and the JSON arm below names `id` with the hole as its
   value. Provenance through the page's own serializer is therefore not a thing to build for those bodies; it
   is what the shape IS.
   Borrowed for the length of the call like the headers.
   It is a separate struct and not three arguments because a body is one fact: bytes with no type are bytes
   nothing can name the fields of, and a type with no bytes is not a body — and `kind` is part of that one
   fact rather than a fourth argument for the same reason. */

/* WHOSE BYTES THESE ARE, WHICH THIS STRUCT COULD NOT SAY AND WHICH DECIDES WHETHER THEY MAY BE REPLAYED.
   A producer holding a body built out of UNKNOWN EXTERNAL INPUT has no bytes to hand over — there are none —
   so what it passes is the engine's own DISPLAY SPELLING of that unknown (core/fetch/body.h's BODY_SHAPE).
   Those characters look exactly like a payload here, and the surface base64'd them into `bodyBase64` under
   this header's promise that the request sent exactly these bytes.
   THAT IS FABRICATED EVIDENCE AND NOT A MISSING VALUE, which is the distinction that makes it worth a field.
   §@H's rule is that a value known only to satisfy a gate is INVENTED rather than computed; a SHAPE rendered
   as BYTES is that defect one layer out, and it is worse than a `||` default because a default merely reads
   as a measurement while this is handed to a reviewer to REPLAY — so a request nobody ever made gets sent,
   carrying the literal characters of a hole. An ABSENT `bodyBase64` beside a shape body is the honest state,
   and this field is what makes that difference expressible rather than guessable from the bytes.
   IT HAS NO SAFE DEFAULT, so its zero is UNSTATED and endpoint_record aborts on it. Every producer computes
   this already — each one asks core/fetch/body.h which arm its body took — and each was DROPPING the answer
   between that question and this call, so the fix is to carry a fact that exists rather than to derive a new
   one. A producer that forgets takes the same arm as one that has nothing to say, which is what stops
   forgetting from being a way to be exempted. */
/* AND A THIRD ANSWER, WHICH IS NEITHER OF THOSE TWO AND IS THE ONE A REVIEWER EDITS. The pair above is a
   complete partition of a body whose bytes are all of ONE provenance — the page computed every one of them,
   or the page computed none of them — and a body a serializer wrote BYTE BY BYTE is neither. `EndpointBodySpan`
   below names which ranges of it the run did not determine; the rest IS what the page computed, and inside a
   span stands the unknown's own EXAMPLE where it had one and a byte NOBODY WROTE where it did not, because
   §10.4.5.18 skips the block write rather than inventing one.
   SO THEY ARE AN EXAMPLE OF THE BODY AND NEVER WHAT THE REQUEST SENDS, and that difference is the whole of what
   makes them safe to publish. A reviewer handed them under EPB_SENT's claim would replay a byte no run ever
   computed, which is §@H's invented value one layer out and is exactly the fabrication the EPB_SHAPE split was
   made to end — so the constraint is not a preference to be re-derived: a body carrying spans has no bytes the
   request can be said to send, and endpoint_record asserts the pairing rather than trusting a producer to.
   WITHOUT IT THE HONEST STATE IS A SILENCE, which is the same pairing EPB_SHAPE's own arm records: the spans
   name WHERE a reviewer edits and say nothing about what currently stands there, so a panel holding them alone
   can change WHICH value a field carries and cannot show what it carries. An ABSENT example and an example of
   all-zero bytes are different facts, and this kind is what makes the first expressible.
   IT IS A THIRD KIND AND NOT A FLAG ON EPB_SENT for the reason the two fields it selects between are two
   fields: a consumer combining `sent` with `some of it is an example` decides the claim at the point of use,
   and the claim is precisely what the key is for. THE TWO RUNS ABOVE ARE IN BACKTICKS RATHER THAN QUOTED
   because they are SPELLINGS BEING SHOWN and not a spec sentence — a quoted run this near a section number is
   compared against that section by engine/citegen.mjs and reported as a fabricated quotation, which is that
   tool working and this file having written a claim it was not making. */
typedef enum {
    EPB_UNSTATED = 0,   /* nobody said; endpoint_record refuses it */
    EPB_SENT,           /* bytes the page composed — what the request will actually send, replayable */
    EPB_SHAPE,          /* the engine's display spelling of an unknown body; NEVER bytes the page sent */
    EPB_EXAMPLE         /* bytes that are an EXAMPLE of the body — the page's own where it computed them and an
                           unknown's where it had one, with a byte nobody wrote inside an exampleless span.
                           NEVER replayable: it is what the payload LOOKS LIKE, for a consumer holding the
                           spans to splice a replacement into. */
} EndpointBodyKind;

/* WHICH BYTE RANGES OF THE BODY THE PAGE DID NOT DETERMINE, AND WHERE EACH CAME FROM — the one fact that
   turns "replay these bytes verbatim" into a request a reviewer can EDIT, and the only one a sniffer can never
   produce. A page that composes its payload AS A STRING carries its operands' display forms into the result,
   so the JSON arm below already names its fields; a page that writes BYTE BY BYTE into a typed array carries
   nothing at all, because a data block holds uint8_t and an unknown cannot live in one. ECMAScript §10.4.5.18
   TypedArraySetElement ( obj, index, value ) records the fact beside the block instead, and this is that
   record projected onto this surface.
   NOTHING DECODES AND NOTHING BRANCHES ON A PROTOCOL, which is what makes it answer for an encoding this
   engine has never heard of. The page's own serializer ran with the real field names and the real values in
   its hands and wrote these bytes itself, so a span saying that bytes 17 to 23 of the payload are the value
   which entered at the page's own query string is an OBSERVATION of that run rather than a reconstruction
   from a byte stream — and a wire decoder selected
   by a MIME pattern is the protocol-specific recognizer §Architecture bans, whose next member is Connect, or
   grpc-web-text, or a framing invented next year.
   `shape` IS NEVER NULL AND `example` MAY BE. §10.4.5.18 skips the block write entirely for an unknown
   carrying no example, because a byte in a buffer is indistinguishable from a byte the page computed and
   inventing one is §@H's value known only to satisfy a gate. So the absence is a POSITIVE statement — nobody
   knows what this byte is — and never a hole for a default to fill.
   IT IS A PROJECTION AND NOT A SECOND REPRESENTATION, for the reason EndpointHeader is one: four fields
   either way, and the solver must not learn what a BodyState is. Borrowed for the length of the call. */
typedef struct { size_t off, len; const char *shape, *example; } EndpointBodySpan;

typedef struct { const char *mime, *bytes; size_t len; EndpointBodyKind kind;
                 const EndpointBodySpan *span; int nspan; } EndpointBody;

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
      "predicates":[{"method":..,"arguments":[..],"holds":true|false}],
      "looselyEquals":[{"value":..,"type":..}]}]}, ... ]`.
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
   `looselyEquals` IS THE FOURTH, AND IT IS THE ONE ARM OF AN EQUALITY THAT USED TO REACH THIS SURFACE AS
   SILENCE. ECMAScript §7.2.14 IsStrictlyEqual ( x, y ) step 1 is "If SameType(x, y) is false, return false",
   so a `===` that HELD determined the value and it is in `validValues`; §7.2.13 IsLooselyEqual ( x, y )
   coerces instead, so its holding arm determines none and the pin refuses it — correctly, and until this key
   existed that refusal was the whole of the record. A param whose only gate was `x == 0` therefore rendered
   with the same bytes as one nothing ever tested, while the SIBLING flow that took the same gate's other arm
   carried an `excludes` — two arms of one observation disagreeing about whether a gate was seen at all, which
   is this file's own wrong-report-not-a-partial-one rule read against itself.
   Each entry is `{"value":<string>,"type":"string"|"number"|"boolean"|"null"|"undefined"|"bigint"}`: the
   operand the page wrote, as its own §7.1.19 ToString ( arg ), and WHAT THAT OPERAND SPELLS. Both halves are
   load-bearing and the second is not decoration on the first — ToString flattens `undefined`, `null`, `0` and
   `false` onto text that is also a legal String operand, and `x == undefined` is a demand that the value be
   null or undefined (for a query parameter, that it be ABSENT) while `x == "undefined"` is a demand for nine
   characters. A consumer that carried the value alone would state one of those and mean the other.
   IT STATES THE PREDICATE AND NEVER THE SET IT ADMITS. §7.2.13's holding set differs per token kind and its
   step 12 arm runs the PAGE's own ToPrimitive, so rendering the set would mean re-implementing fourteen spec
   steps in a consumer, over code that is not running by then — CLAUDE.md §RUN-DON'T-MATCH, performed in a
   report. What is carried is the transcript, and a reader reads `== 0` as JavaScript.
   IT INVENTS NOTHING: no member of the holding set is emitted, exactly as no member of `bounds`' interval is,
   and `validValues` still carries only what the code COMPUTED. `looselyEquals` is omitted where no loose
   equality held on every observed path, and that absence is the statement.
   THERE IS NO `holds:false` HERE, AND THAT IS A PROPERTY OF THE RECORD RATHER THAN A GAP IN IT. The arm this
   key does not cover is the arm `excludes` already covers — a loose equality that FAILED proves the operand is
   not the token as strictly as a strict one that failed, because §7.2.14 step 1 makes `x === tok` imply
   SameType and §7.2.13 step 1 then hands a SameType pair straight to §7.2.14. So one gate files one fact per
   arm, into two keys, and a consumer that branched on a false arm here would be reading for a value no
   producer can emit.
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
