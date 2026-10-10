/* @H endpoint surface: the deduped set of endpoints forced execution learned, emitted as the `@H` array.
 *
 * Every HTTP-shaped request edge (fetch, XHR, sendBeacon, form submission, script/link/img loads, the
 * program-load park, multipart batch parts, reply-named chunks) calls endpoint_record. A concolic URL
 * contributes its shape (`/api/region/{state}.region`), a concrete one its literal. A value lands in one of
 * three places and each param says which: the path (a `{hole}` segment whose example is recovered by aligning
 * the concolic's concrete URL against its shape; see path_scan), the query string, or the body (read in the
 * body's own content type). A hole's name carries the page's bytes verbatim; extension/lib/callsite-url.js
 * masks it around the trusted zone's `new URL` parse so it comes back intact.
 */
/* Named residual: an address handed to a socket-shaped constructor.
 * Not covered: `new WebSocket(u)` (WebSockets §3 "The WebSocket interface") and `new EventSource(u)` (HTML
 * §9.2.2 "The EventSource interface") reach this surface through no path: every endpoint_record caller is
 * HTTP-shaped and neither interface is installed, so such an endpoint is absent here rather than
 * under-reported. extension/intercept.js's passive wrap of window.WebSocket is a diagnostic and is not merged.
 * Next diff: HTML §9.2.2 steps 8-15 with HTML §9.2.3 "Processing model" (the first caller of
 * core/eventsource/event_source_parser.h), recording at step 8's request as every other edge records at its
 * own call. WebSocket follows and needs a connection source first: only WebSockets §4 "Feedback from the
 * protocol" moves ready state past CONNECTING and send() throws while CONNECTING, so an interface object with
 * no transport moves bundles that feature-detect `WebSocket` off a working fallback onto an arm that throws.
 * Absence shows as: a document whose API is carried over a socket emits an @H array holding only its
 * subresource loads while solver/absent.c's census names the interface as owed.
 */
#ifndef ENGINE_HOST_SOLVER_ENDPOINT_H
#define ENGINE_HOST_SOLVER_ENDPOINT_H

#include "quickjs.h"

void    endpoint_init(void);
void    endpoint_free(void);
void    endpoint_suppress(int on);   /* 1 during a candidate/verify re-run: its requests are @S artifacts, not @H */

/* A header the request carries; an endpoint reachable only with, say, `Authorization` is not reproducible
   without it. `value` is the literal the code computed, or the unknown's shape (`{state}.token`), which marks
   a runtime value the reviewer supplies. Borrowed for the call; the surface copies what it keeps, and on a
   merge a concrete value supersedes a shape for the same header. */
typedef struct { const char *name, *value; } EndpointHeader;

/* The request body the page composed. `mime` is the content type the request sends (the header list's, else
   the one Fetch §5.4 "Request class" step 37.4 extracts) and decides how the bytes are read: a JSON MIME type
   (MIME Sniffing §4.6 "MIME type groups") or `text/plain` as a JSON object of name -> value, and
   `application/x-www-form-urlencoded` by the query grammar. A body none of whose fields were named is kept
   under the key its `kind` selects; nothing decodes bytes into fields by protocol, because the page's own
   serializer already ran with the real names and values. A body composed as a string names its fields
   anyway: concatenation carries each operand's display form into the shape, so `'{"id":"' + id + '"}'`
   arrives as `{"id":"{state}.id"}`. Bytes, type, kind and spans are one fact, hence one struct. Borrowed
   for the call. */

/* Whose bytes `EndpointBody.bytes` are, which decides whether they may be replayed. It has no safe default:
   its zero is EPB_UNSTATED and endpoint_record refuses it, so forgetting is not an exemption. Every producer
   already knows which arm its body took from core/fetch/body.h. Each kind is emitted under its own key so a
   consumer never composes the claim:
     EPB_SENT    -> `bodyBase64`: the bytes the request sends (encoded with the engine's own base64 codec).
     EPB_SHAPE   -> `bodyShape`: the engine's display spelling of an unknown body (body.h's BODY_SHAPE). Its
                    characters look like a payload; publishing them as sent bytes would hand a reviewer a
                    request nobody made.
     EPB_EXAMPLE -> `bodyExampleBase64`: an example of the payload, which is the page's bytes where it computed
                    them, an unknown's example inside an EndpointBodySpan, and an unwritten byte inside a span
                    with no example (ECMAScript §10.4.5.18 TypedArraySetElement skips that write). Never
                    replayable. A body with spans must be this kind, which endpoint_record asserts. */
typedef enum {
    EPB_UNSTATED = 0,   /* nobody said; endpoint_record refuses it */
    EPB_SENT,           /* bytes the page composed — what the request will actually send, replayable */
    EPB_SHAPE,          /* the engine's display spelling of an unknown body; never bytes the page sent */
    EPB_EXAMPLE         /* an example of the body, with the spans that say where it is unknown; never replayable */
} EndpointBodyKind;

/* A byte range of the body the page did not determine, and where it came from: ECMAScript §10.4.5.18
   TypedArraySetElement ( obj, index, value )'s record of an unknown written into a typed array, projected
   onto this surface without the solver learning what a BodyState is. It lets a reviewer edit a byte-built
   payload rather than replay it, with no protocol decoder. `shape` is never NULL; `example` is NULL when the
   unknown had none, which states that nobody knows those bytes and is never a hole for a default. Spans are
   sorted and disjoint (asserted), and each becomes a body param named `body[off:end]`. Borrowed for the
   call. */
typedef struct { size_t off, len; const char *shape, *example; } EndpointBodySpan;

typedef struct { const char *mime, *bytes; size_t len; EndpointBodyKind kind;
                 const EndpointBodySpan *span; int nspan; } EndpointBody;

/* Which mechanism composed an address, stated by every producer because no consumer can re-derive it. Its
   zero is EPD_UNSTATED and endpoint_record refuses it. It is neither `prov` (what the sighting is evidence
   of) nor `mintedAt` (when): a `<link>` below a `<head>` script is minted post-program and is still markup.
   It is the first sighting's mechanism and is never re-armed, and it is not part of `same_identity`: two
   mechanisms reaching one address are one endpoint, and splitting it would scatter one request's params.
   Columns: id, wire token, what a markup parse reaches through the door (ENDPOINT_REACHES), and whose bytes
   the door hands this surface (ENDPOINT_DOOR_BYTES). One list keeps the enum, the token table and every
   census in step; a door missing a column does not compile. The three program doors share one call site,
   solver/engine.c's park consumer, whose `program_load_door` reads the park kind for `module-import` and the
   parser-inserted mark for the two element doors, because a kind is a queue position and not an origin. */
#define ENDPOINT_DOORS(X)                                                                                    \
    /* HTML §4.12.1.1 "Processing model": a program load whose `<script src>` a parser inserted, read off   \
       the park's `parser document` mark and never off the queue its reply is delivered at. Narrower than   \
       `markup` by one population, a named residual at `program_load_door`: a `<script src>` a             \
       `document.write` put in the tree is parser-inserted and is not in the served bytes. */               \
    X(EPD_DOCUMENT_SCRIPT, "document-script", EPR_MARKUP, EPB_DOCUMENT)                             \
    /* a `<script src>` no parser inserted: running code put the element in the tree */                     \
    X(EPD_INJECTED_SCRIPT, "injected-script", EPR_BEYOND, EPB_DOCUMENT)                             \
    /* a dynamic `import()`: no element, so no parse of the document can find it */                          \
    X(EPD_MODULE_IMPORT,   "module-import", EPR_BEYOND, EPB_DOCUMENT)                               \
    /* a `<script src>` whose address running code assigned as an unknown this engine cannot fetch. The     \
       taint shadow map holds an entry only where a script wrote the attribute, so this door is never       \
       parser-inserted (asserted in core/html/html_script.c). */                                             \
    X(EPD_SCRIPT_ELEMENT,  "script-element", EPR_BEYOND, EPB_DOCUMENT)                              \
    /* HTML §4.2.4.3 "Fetching and processing a resource from a link element", preloads included. A `<link>` \
       the markup declared and one a script created reach it alike, and the door cannot say which. */       \
    X(EPD_LINK_ELEMENT,    "link-element", EPR_EITHER, EPB_DOCUMENT)                                \
    /* HTML §4.8.4.3.5 "Updating the image data", its source set and its undecided arm. An `<img>` the      \
       parser built and one `new Image()` composed are one door. */                                          \
    X(EPD_IMAGE_ELEMENT,   "image-element", EPR_EITHER, EPB_DOCUMENT)                               \
    /* a `<form action>` from the markup and one whose action a script wrote reach core/html/html_form.c's  \
       one recording pair alike */                                                                           \
    X(EPD_FORM_SUBMIT,     "form-submit", EPR_EITHER, EPB_DOCUMENT)                                 \
    X(EPD_FETCH,           "fetch", EPR_BEYOND, EPB_DOCUMENT)                                       \
    X(EPD_XHR,             "xhr", EPR_BEYOND, EPB_DOCUMENT)                                         \
    X(EPD_BEACON,          "beacon", EPR_BEYOND, EPB_DOCUMENT)                                      \
    /* a sub-request written inside a multipart batch body the page composed */                              \
    X(EPD_BATCH_PART,      "batch-part", EPR_BEYOND, EPB_DOCUMENT)                                  \
    /* an address a reply named (solver/reply_decode.c) and no line of the page composed */                  \
    X(EPD_REPLY_CHUNK,     "reply-chunk", EPR_BEYOND, EPB_OFF_DOCUMENT)

typedef enum {
    EPD_UNSTATED = 0,   /* nobody said; endpoint_record refuses it */
#define ENDPOINT_DOOR_MEMBER(id, token, reach, bytes) id,
    ENDPOINT_DOORS(ENDPOINT_DOOR_MEMBER)
#undef ENDPOINT_DOOR_MEMBER
    EPD_COUNT           /* list end; the members take no explicit values, so the enum is dense and the mint's
                           range check is a membership test */
} EndpointDoor;

/* The one wire token of a door. A value outside the list is a CHECK, not a DCHECK: this runs once per emitted
   row in every build, and a release fallthrough would publish a plausible mechanism name nothing ran. */
const char *endpoint_door_token(int door);

/* What a markup parse of the served document reaches through a door: the door list's third column. Three
   words, not two: `link-element`, `image-element` and `form-submit` are reached by parser-inserted and
   script-created elements alike and the door does not record which, so a two-way split would have to guess,
   and either guess mis-credits one population. `beyond` is named for what a markup parse reaches and never
   for who ran, which is why `reply-chunk` is in it. This is a diagnostic, not the product's bar:
   `endpoint_razor_class_of` composes the bar and does not read the reach. */
#define ENDPOINT_REACHES(X)                                                                                  \
    /* a parse of the served bytes reaches every address through this door */                                \
    X(EPR_MARKUP, "markup")                                                                                  \
    /* the door is reached by a parser-inserted element and by a script-created one alike, and it does not    \
       record which — so neither claim may be made about a row that came through it */                        \
    X(EPR_EITHER, "either")                                                                                  \
    /* no parse of the served document reaches any address through this door */                              \
    X(EPR_BEYOND, "beyond")

typedef enum {
#define ENDPOINT_REACH_MEMBER(id, token) id,
    ENDPOINT_REACHES(ENDPOINT_REACH_MEMBER)
#undef ENDPOINT_REACH_MEMBER
    EPR_COUNT           /* list end, dense. No unstated member: a reach is stated in the door list, so no
                           producer can forget one, and a door without it does not compile. */
} EndpointReach;

/* The one wire token of a reach; a CHECK on a value outside the list, for endpoint_door_token's reason. */
const char *endpoint_reach_token(int reach);

/* The reach of `door`, read off the door list's third column; a CHECK on a door outside the list. */
int         endpoint_door_reach(int door);

/* Whether the bytes a door hands this surface were in the document this engine was served at all: the door
   list's fourth column and the door-side operand of the product's bar (an address, key or value no parse of
   the served bytes can state because it exists only at run time). It differs from ENDPOINT_REACHES, which
   asks what a markup parse reaches: `module-import` is `beyond`, yet its literal chunk URL stands in the
   served scripts. The member that proves nothing is first and zero, and a door added without this column does
   not compile. */
#define ENDPOINT_DOOR_BYTES(X)                                                                      \
    /* the address was composed out of the served document, its markup or its own scripts. It      \
       claims nothing about whether a parse of those bytes states the address; that depends on the  \
       value, which is ENDPOINT_ADDRESS_CLASSES' question. */                                       \
    X(EPB_DOCUMENT,     "document")                                                                 \
    /* the bytes reached this engine at run time from outside that document — today a reply         \
       payload, such as the Flight client reference chunk list solver/reply_decode.c reads. No      \
       parse of the served document holds them, so such a row clears the bar whatever its address  \
       class says. */                                                                               \
    X(EPB_OFF_DOCUMENT, "off-document")

typedef enum {
#define ENDPOINT_DOOR_BYTES_MEMBER(id, token) id,
    ENDPOINT_DOOR_BYTES(ENDPOINT_DOOR_BYTES_MEMBER)
#undef ENDPOINT_DOOR_BYTES_MEMBER
    EPB_COUNT           /* list end, dense; no unstated member, for EPR_COUNT's reason */
} EndpointDoorBytes;

/* Whose bytes `door` hands this surface, read off the door list's fourth column; a CHECK on a door outside the
   list, since a release fallthrough would answer the product's bar for a mechanism nothing named. */
int         endpoint_door_bytes(int door);

/* What the address value's own concolic provenance proves about it, derived by `address_class_of` from
   endpoint_record's `url` alone; no producer states it, so there is no unstated member. It answers two
   questions from one stored member, so they cannot disagree about a row:
     Q1, did the run determine this address:   `cls <= EPA_SOURCE_DETERMINED`;
     Q2, does the address side clear the bar:  `cls == EPA_UNKNOWN` (read by `endpoint_razor_class_of`).
   The order is load-bearing and asserted at `endpoint_address_hist_json`: `EPA_CONCRETE` is zero (the
   memset's identity and the class that proves nothing), the determined members are a prefix (Q1 is a
   threshold), and `EPA_UNKNOWN` is the top. A record's merge is a MAX over the order, because what one
   sighting proved about the address is not taken back by a later sighting that proved less. No member says
   whether a parse could state the address; what a static reader reaches is a moving frontier. `concrete`
   also covers an address composed of literals and the document's own address; a reply-carried address is
   concrete too, and the door's bytes column catches it. */
#define ENDPOINT_ADDRESS_CLASSES(X)                                                                          \
    /* the run determined this address and holds no evidence that any segment of it entered the program at   \
       a source (`concolic_is` is false). It claims nothing about a parse. */                                \
    X(EPA_CONCRETE, "concrete")                                                                              \
    /* the address is this engine's own concolic and this flow's equality pinned it: concolic.h's           \
       CONCOLIC_EX_DETERMINED, as in `var u = cfg.url; if (u === '/api/v2/items') fetch(u);`. Q1 answers   \
       determined and Q2 no: a pin's bytes are the equality's other operand, which solver/concolic.c spells \
       only where that operand is not concolic, so they are bytes the page's own text holds (usually a      \
       literal in the bundle). The run proved which world it explored, not that the bytes are absent from  \
       the served document. */                                                                               \
    X(EPA_SOURCE_DETERMINED, "source-determined")                                                            \
    /* the run did not determine it, and no root of the value is one anybody outside this engine supplied   \
       (`concolic_root_whose_any(url, CONCOLIC_WHOSE_WORLD)` answers no). It holds a hole this engine minted \
       to drive code (a driven orphan's own argument), a value whose bytes stand in the served document but  \
       is minted unknown so a gate over it forks (a data block, a global an inline script wrote), and a      \
       value no mint could attribute (concolic.h's `UNSTATED`, or no root at all). None clears the bar. */   \
    X(EPA_UNKNOWN_UNPROVEN, "unknown-unproven")                                                              \
    /* the run did not determine it, and some root is one a document, a server, an attacker or the            \
       environment supplied (concolic.h's `WORLD`: the location fragment, an absent server-injected global,  \
       a reply field, a device reading). One such root suffices (`_any`, not `_all`). This row clears the    \
       bar; it is the top member, so a reordering is what could lower a record out of it. */               \
    X(EPA_UNKNOWN,  "unknown")

typedef enum {
#define ENDPOINT_ADDRESS_CLASS_MEMBER(id, token) id,
    ENDPOINT_ADDRESS_CLASSES(ENDPOINT_ADDRESS_CLASS_MEMBER)
#undef ENDPOINT_ADDRESS_CLASS_MEMBER
    EPA_COUNT           /* list end, dense and ordered; the banner states the order the walk asserts */
} EndpointAddressClass;

/* The one wire token of an address class, emitted once per row and once per census row; a CHECK on a value
   outside the list, for endpoint_door_token's reason. */
const char *endpoint_address_class_token(int cls);

/* The product's bar per row: whether this run proved the address exists only at run time. It is the union of
   two positive statements, each sound alone, so it is a floor: the address side is `EPA_UNKNOWN`
   (undetermined, resting on an unknown supplied from outside this engine) and the door side is
   `EPB_OFF_DOCUMENT` (bytes never in the served document). `reply-chunk` is `concrete` and clears it;
   `/api/{location.hash}` through `fetch` is `EPB_DOCUMENT` and clears it. It is computed at the emitter from
   `door` and `addr_class` and never stored; it is sound under both fields' merges (`addr_class` is a MAX over
   sightings and `door` is the first sighting's) because no later sighting retracts either statement.
   `unproven` is not the claim that a parse could state the address, and `EPW_MAY_REST_ON` is never unioned
   in: it is a may, and folding it would turn the floor into an over-claim.
   Named residual. Not covered: an address determined by a pin whose other operand was a non-concolic value
   taken from off-document bytes exists only at run time but reads `unproven`, because a non-concolic operand
   carries no provenance. Next diff: the pin records whose bytes its other operand came from, beside the bytes
   it already copies, and `address_class_of` reads that. Absence shows as: a `source-determined`, `unproven`
   row whose pinning literal stands in no served byte. */
#define ENDPOINT_RAZOR_CLASSES(X)                                                                   \
    /* this run proved nothing about whether a parse of the served bytes could state the address:  \
       every `concrete`, `source-determined` or `unknown-unproven` row through a door whose bytes  \
       were in the served document. It is not the complement of the member below over parses. */   \
    X(EPZ_UNPROVEN,     "unproven")                                                                 \
    /* this run proved the address exists only at run time: it reached it holding an undetermined  \
       value resting on an unknown supplied from outside this engine, or the door handed it bytes  \
       that were never in the served document. */                                                  \
    X(EPZ_RUNTIME_ONLY, "runtime-only")

typedef enum {
#define ENDPOINT_RAZOR_CLASS_MEMBER(id, token) id,
    ENDPOINT_RAZOR_CLASSES(ENDPOINT_RAZOR_CLASS_MEMBER)
#undef ENDPOINT_RAZOR_CLASS_MEMBER
    EPZ_COUNT           /* list end, dense; derived from two fields written unconditionally, so there is no
                           producer and no unstated member */
} EndpointRazorClass;

/* The one wire token of a razor class; a CHECK on a value outside the list, for endpoint_door_token's
   reason. */
const char *endpoint_razor_class_token(int cls);

/* Whether the address may rest on a witness this engine chose (bytes the engine picked rather than a server
   sent), published per row as `witnessClass`. It bounds reproducibility, not the bar: a row resting on chosen
   bytes is one a real session may never see, and it is never unioned into the razor. It separates two
   opposite remedies for a surface of `concrete` rows: the solver keeps too few values unknown, or a pin
   erased a taint (concolic.c's `pin_mint` hands a pinned re-read back as a bare primitive). The order encodes
   how much is claimed and the record's merge is a MAX: a sighting that asked outranks one that could not, and
   one whose path chose a witness is not taken back by one whose path did not. `EPW_UNASKED` is zero (asserted
   at `endpoint_witness_hist_json`), so a mint that forgets to ask reads as having asked nothing. The question
   is asked only while a flow stands, since `engine_pinned_of_running_path` requires one. */
#define ENDPOINT_WITNESS_CLASSES(X)                                                                 \
    /* no flow stood when this address was recorded (a record minted before any program started is  \
       the ordinary case), so the question was not asked. It proves nothing either way. */          \
    X(EPW_UNASKED,     "unasked")                                                                   \
    /* a flow stood and its path had chosen no witness: nothing this run pinned can be under this    \
       address, so `unproven` here really is the run having proved nothing. */                       \
    X(EPW_NO_WITNESS,  "no-witness")                                                                \
    /* a flow stood whose path had determined a source's value on an arm its own concrete example    \
       contradicted (solver/flow.h's `path_pinned`). The address may rest on bytes this engine       \
       chose rather than bytes a server sent; a may, because the pin is a fact about the path and    \
       this row does not say the address read that source. */                                       \
    X(EPW_MAY_REST_ON, "may-rest-on")

typedef enum {
#define ENDPOINT_WITNESS_CLASS_MEMBER(id, token) id,
    ENDPOINT_WITNESS_CLASSES(ENDPOINT_WITNESS_CLASS_MEMBER)
#undef ENDPOINT_WITNESS_CLASS_MEMBER
    EPW_COUNT           /* list end, dense and ordered: the merge is a MAX over it */
} EndpointWitnessClass;

/* The one wire token of a witness class; a CHECK on a value outside the list, for endpoint_door_token's
   reason. */
const char *endpoint_witness_class_token(int cls);

/* The emitted surface partitioned by witness class, as a malloc'd JSON object (caller frees): one row per
   member, zeroes included, summing to endpoint_surface_census's `emitted` (asserted). */
char *endpoint_witness_hist_json(void);

/* The bar for one row: `EPZ_RUNTIME_ONLY` when `addr_class` is `EPA_UNKNOWN` or `door`'s bytes are
   `EPB_OFF_DOCUMENT`, else `EPZ_UNPROVEN`. It takes the two wire fields rather than a record, so a consumer
   can check a row's `razorClass` against that row's own `door` and `addressClass`. */
int         endpoint_razor_class_of(int door, int addr_class);

/* The emitted surface partitioned by address class, as a malloc'd JSON object (caller frees): one row per
   member of ENDPOINT_ADDRESS_CLASSES, zeroes included, summing to endpoint_surface_census's `emitted`
   (asserted here, with the class order). It is keyed on the address value, an observation independent of the
   door histograms. A diagnostic, read within one run against `epEmitted` on the same census line: `unknown`
   at 0 is a refusal to claim the bar on this document, not a smaller version of it. */
char   *endpoint_address_hist_json(void);

/* The emitted surface partitioned by door, as a malloc'd JSON object (caller frees): one row per member of
   ENDPOINT_DOORS, zeroes included, summing to endpoint_surface_census's `emitted` (asserted). */
char   *endpoint_door_hist_json(void);

/* The emitted surface partitioned by ENDPOINT_REACHES, as a malloc'd JSON object (caller frees), one row per
   member, summing to `emitted` (asserted). Each count is the door histogram's counts summed over that reach's
   doors, so the two are one observation at two grains; what this adds is the door-to-reach map, which only
   the door list states. It is a diagnostic and not the bar: `beyond` holds a literal chunk URL through
   `module-import`, and a share read off it is over `epEmitted`. `either` is a decision rather than a gap: the
   standard sets a parser-inserted mark only on scripts and on form-associated controls, so no flag exists that
   could split the link and image doors, and the bar does not read the reach. */
/* Named residual, on the axis the bar reads. Not covered: a `<form action>` composed from an unknown reaches
   the bar as `concrete`. core/html/html_form.c reads the action's taint at HTML §4.10.22.3 "Form submission
   algorithm" step 12 and spends it on the @S sink alone, then `form_record_request` records the
   `url_serialize`d bytes, while the link and image doors record the taint itself. Recording the bare taint
   would drop the entries the submission writes into the action's query, and unioning `valueClass` into the
   address would over-claim, because `concolic_hole_key` is a bare brace test. Next diff: the recorded address
   composed as a concolic from the action's taint and the entries, which needs `url_parse`/`url_serialize` to
   carry concolics. Absence shows as: `form-submit` rows reading `unproven` beside their own `valueClass`
   `unknown` while the same document's `link-element` rows read `runtime-only`. */
char   *endpoint_reach_hist_json(void);

/* The emitted surface partitioned by ENDPOINT_RAZOR_CLASSES, as a malloc'd JSON object (caller frees), summing
   to `emitted` (asserted): the one census row that states the bar. It is its own walk with the emit's asset
   skip, never a sum of other rows: those are marginals, and the bar's two operands can both hold for one row.
   It restates the per-row `razorClass` at the population grain; the two can be read at different instants,
   and `epEmitted` falls when an asset verdict lands between them. It is a floor and a diagnostic:
   `runtime-only` at 0 is a refusal to claim, and its denominator is `epEmitted` on the same census line. */
char   *endpoint_razor_hist_json(void);

/* Records one learned endpoint, or merges it into the record of the same identity: method, path shape,
   `prov`, and the ordered names and locations of its params. `url` may be concolic (its shape is recorded) or
   concrete. `body` NULL states that the request sends no body. Headers merge into a same-identity record, a
   concrete value superseding a shape. Asserts that `prov` is one of solver/pending.h's PROV_*, that `door` is
   a member of ENDPOINT_DOORS, and that `body`'s kind and spans agree. Every call is counted by the ask census
   before the suppression gate.
   `prov` is part of the identity so a forced sighting can never merge into an observed or derived record and
   publish a forced value under the stronger grade; two rows for one address at two grades are two true
   statements. (The pending line folds a deduped set to its most observed member instead, because one reply
   answers that set.) Every producer states it: running code states `engine_prov_of_running_path()` read at
   the act, and a reply-learned address states the grade of the reply that named it, which is a fact about the
   request that reply answers and never about whatever flow stands when its bytes land. A default would be
   `observed` by the numbering, on a record nobody graded. */
/* `door` is which mechanism composed the address, stated by the producer; see ENDPOINT_DOORS. */
void    endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                        const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body, int prov, int door);

/* The @H surface as a malloc'd JSON array (caller frees), one object per record not marked an asset; it is an
   array and not a document because result.h composes the document. Record keys:
     `method`; `url` (the path shape); `bodyMime` with one of `bodyBase64`, `bodyShape` or
       `bodyExampleBase64` (see EndpointBodyKind); `provenance` (`observed`|`derived`|`forced`); `door`;
       `mintedAt` (`pre-program`|`post-program`); `addressClass`; `razorClass`; `witnessClass`; `addressRoot`
       (null for a `concrete` address, false when the value has no root, else the root's name); `params`;
       and `headers` (an object, present only when some header was seen).
   Every class key is always present, since its words are exhaustive. `provenance` is the record's, its grade
   being part of the identity: `derived` is the headline claim (the app's code computes a request no session
   sent), `observed` a real load of the document, and `forced` an address that exists only because a gate was
   forced, never to be reported as either other word. `addressClass` cannot be re-derived from braces in
   `url`: extension/lib/learn.js mints `{path_*}` holes from live traffic into addresses it reads `concrete`
   for. Q1 (determined) is `concrete` or `source-determined`; Q2 is `unknown`, which `razorClass` already
   composes, so a consumer reading the bar as `!= "concrete"` over-claims. */
/* Each param: `name`; `location` (`path`|`query`|`body`, which is what the reviewer replays it with);
   `valueClass`; `validValues`; and the optional domain keys below. `valueClass` is `unknown` when some
   observed path minted the param from a value the code did not compute, else `concrete` (the address's two
   older words; a param value has no pinned-and-held population for a third). It is always present and merges
   as a union, since a later concrete sighting cannot take back a hole one path saw. It is what makes the
   domain keys' absence readable: they are read through the param's hole key, so on a `concrete` param their
   absence means there was nothing to look up, not that no gate narrowed a hole. It cannot be re-derived from
   `validValues`, which carries the aligned example where the shape held a hole. A `location:"path"` param is
   always `unknown` (asserted in `kv_add`); a body span's param is named `body[off:end]`. `validValues`
   carries only values the code computed: nothing here emits a member of a domain (no `6` for `x > 5`, no
   string satisfying a predicate). */
/* Domain keys, each omitted unless a claim of its kind held on every observed path (that absence is the
   statement, never an empty value); on a merge only claims every sighting made survive (`bounds` widens).
   Forced multi-path runs both arms of every gate, which is what makes them plentiful.
     `excludes`: values this endpoint's equality gates proved the param is not.
     `bounds`: what ordering gates proved, in JSON Schema Validation 2020-12 §6.2 Validation Keywords for
       Numeric Instances (number and integer)'s vocabulary — at most one of `minimum`/`exclusiveMinimum` and one
       of `maximum`/`exclusiveMaximum`, each a JSON number spelled as the page's literal. Those keywords assert
       nothing about a non-number, so the record also states that the value is compared as a number:
       concolic_rel_hook records a bound only for a finite Number operand (ECMAScript §7.2.12 IsLessThan step 3).
     `predicates`: `{method, arguments, holds}` per method-call gate (`path.startsWith("/api")`), with the
       property name, each argument as the page's ToString of it (possibly none), and which arm this run took.
     `looselyEquals`: `{value, type}` per `==` that held (ECMAScript §7.2.13 IsLooselyEqual ( x, y )), with the
       operand's ToString and its type, since `x == undefined` and `x == "undefined"` differ. A failed loose
       equality proves what a failed `===` does and is filed in `excludes`, so there is no `holds:false` here. */
char   *endpoint_json_array(void);

/* Marks every record of (`method`, the path shape of `url`) as a static asset. solver/reply_decode.c calls it
   with the pair the reply register was keyed on when the trusted zone's `computedType` says the reply is a
   file: whether a resource is a static asset is a test over the reply's bytes, not the URL. The record is kept
   and omitted from the emit and from the histograms, so a later sighting of the address stays an asset; the
   verdict is about the resource, not the sighting. */
void    endpoint_mark_asset(const char *method, const char *url);

/* Partitions the record array in one walk: `minted` records, of which `assets` were marked files and `emitted`
   were not, and `pre_program`, the emitted records minted before this instance had started any program. The
   identities are asserted here. `emitted` uses the emit's own skip, so the array's length reads as a fraction.
   `emitted - pre_program` is a ceiling on what forced execution contributed, never a measure of it: a markup
   address can be minted after the first program starts. It is not a count of `prov`, because a browser
   algorithm's subresources (links, images) are graded `derived` by `engine_prov_of_running_path`, which never
   answers `observed`. Four outputs of one call, so they are one instant.
   Named residual. Not covered: markup of a child navigable's document, minted after the root has started
   programs, so it widens the complement without any code having composed it. Next diff: the boundary taken
   per Document, an any-program-started bit on the Document the record's realm belongs to, raised where that
   document's first program starts. Absence shows as: a complement that grows with the number of child
   navigables while the array beside it holds only files. */
void    endpoint_surface_census(long *minted, long *assets, long *emitted, long *pre_program);

/* Counts taken at endpoint_record's entry, before the suppression gate (the ask, not the outcome): `asks` is
   every call and `pre_program` those made before any program started; `suppressed` (a candidate/verify
   re-run), `merged` into an existing record and `minted` partition `asks`. `merged_pre_program` is a cut
   inside `merged`, not a fourth arm: asks made with a program running that merged into a record minted before
   any program, i.e. running code re-composing a markup address; it is contained in `merged` and in
   `asks - pre_program`. All of these identities are asserted here.
   It separates two readings of a zero `emitted - preProgram` that the surface census cannot: no post-program
   request was composed (`asks == preProgram`), or each one merged into an address already held. A zero
   `asks - preProgram` means no request reached this door post-program, not that no call site was reached: a
   host edge's own algorithm can throw or park before it records (see the edge censuses below).
   Every row is a lifetime count, never a gauge, scoped to the surface: reset with the record array in
   endpoint_init and endpoint_free. They are reports and never bounds. */
void    endpoint_ask_census(long *asks, long *pre_program, long *suppressed, long *merged, long *minted,
                            long *merged_pre_program);

/* The fetch edge's own entry, the population the ask census cannot see, counted over the states of
   core/fetch/fetch.c's §5.4/§5.6 machine. Each row names its kind (`Ask` or `Out`; `Life` is a lifetime count):
     `epFetchAskNamedLife`, `…NamedTypeofLife`, `…NamedPropLife`: compile-time resolutions of the edge's entry
       name (see endpoint_compile_global_named and endpoint_compile_global_member).
     `epFetchAskCalledLife`: calls entering the hosting machine's prologue (core/idl_args.c's stage 0), ahead
       of the argument conversions, which can throw or park.
     `epFetchAskBeganLife`: constructions that reached the member body, raised at the one-time capture.
     `epFetchAskOfferedLife`: constructions that reached §5.6 step 12 and offered an address, raised on the
       line before the edge's endpoint_record call.
     `epFetchOutFreedLife`: states torn down after §5.4 began, deep-fork copies included.
     `epFetchOutFreedOfferedLife`: of those, the ones that had offered an address.
     `epFetchOutDiedAtLife`: of the freed states that offered none, one arm per stage they stood at, keyed by
       the machine's own `js_fetch_steps[]` labels handed over at the declaration. */
/* Identities, asserted in endpoint_fetch_edge_rows: the died-at arms plus freed-and-offered equal freed (two
   arms of one teardown); freed-and-offered <= offered (the slack is offered states still parked on their
   reply; the offering stage returns rather than parks and no page code runs before teardown, so no flagged
   state is cloned); offered <= `epAsks` (the slack is every other door); began <= called.
   Not assertable: `freed <= began` and `offered <= began`. A deep fork byte-copies a step state with its
   capture flag, so one `fetch()` whose input forks composes two requests against one capture; fetch.c's
   banner states which stages permit a fork. For the same reason `began - freed` is not a live count. The
   stage rows are a partition, not a ladder: a zero at one stage says nothing about its neighbours.
   `called == 0` cannot separate a page that calls no `fetch()` from a flow that never reached its call; the
   `Named` rows answer that from the compiler. A concolic URL crosses the USVString conversion without parking
   (`idl_concolic_rule` answers IDL_CONCOLIC_CROSSES), so the conversions park only on a page getter such as
   a `Request` input's `url` or a `RequestInit` member. Scope in time is the surface's (reset in endpoint_init
   and endpoint_free). Reports, never bounds. */
/* The edge declares its global entry identifier, its stage table and its first stage at init. Both pointers
   are borrowed, never copied, and let go at endpoint_free; the edge owns the name it installs under, so this
   file keeps no table of names. A second, different declaration is a DCHECK. */
void    endpoint_fetch_edge_declare(const char *entry, const char *const *steps, int first_stage);
/* core/idl_args.c's stage 0 calls this once per call of every declared member with the member's stage table
   (NULL for a member that declares none, which is dropped). It raises `…AskCalledLife` for the edge that
   declared that same table pointer: the match is by identity, so the caller keeps no list of network members
   and cannot need to know which edge it is calling. */
void    endpoint_edge_member_asked(const char *const *steps);

/* The compiler resolved a free identifier against the global object; reached through solver/concolic.c's
   `.global_named` dispatch. `name` is valid for the call only and `typeof_only` marks a `typeof` read. It
   raises `…AskNamedLife` (or `…AskNamedTypeofLife`) for the edge whose entry is `name`, and composes no
   address, mints no ask and decides nothing. It is the one entry here not about a flow: it records that the
   program spells the call even when no flow reaches it, which a runtime edge cannot see. It counts compiler
   resolutions, and every replaying flow recompiles, so it is read as a bit and never as a magnitude, and no
   containment with the call row is asserted. It is a floor: `self[n]` and a parameter the bundle shadows the
   name with reach no global resolution. */
/* Returns nonzero when an edge declared `name` and the read was not a `typeof` guard; quickjs's orphan walk
   orders its candidates by that, from the same `entry` compare the rows use. */
int     endpoint_compile_global_named(const char *name, int typeof_only);
/* The property spelling of the same fact (`window.fetch`), reached through solver/concolic.c's member
   dispatch, which alone decides that the receiver is a realm's global. It raises `…AskNamedPropLife` and
   answers the orphan order like endpoint_compile_global_named. It has no `typeof` split, because
   `typeof window.fetch` emits the same field get as `window.fetch`. */
int     endpoint_compile_global_member(const char *member);
void    endpoint_fetch_edge_began(void);
void    endpoint_fetch_edge_offered(void);
void    endpoint_fetch_edge_freed(int stage, int offered);
/* The fetch edge's rows as a heap string (caller frees; NULL only on allocation failure, which the result
   seam treats as an absent census). Each row carries a leading comma so solver/result.c splices them into
   `_cold` beside the `ep*` rows they explain, where extension/bridge.js and extension/popup.js render them
   with no edit. The empty string is the absent form: a host that installs no fetch has no population, which
   is a different fact from five zeroes. */
char   *endpoint_fetch_edge_rows(void);

/* The XHR edge's own entry: the same census over an edge that splits construction from recording. `send()`
   (XHR_SEND_DECL over SEND_STAGES) constructs the request and, at XHR §3.5.6 "The send() method" step 12 or
   13, places it by minting the lifecycle machine (js_xhr_run_steps), whose `XR_FETCH` stage records. The send
   state is torn down before the asynchronous arm's task runs, so the partition is over placement, not offer:
     `epXhrAskNamedLife`, `…NamedTypeofLife`, `…NamedPropLife`: compile-time resolutions of the entry name.
     `epXhrAskCalledLife`: `send()` calls entering the prologue (core/idl_args.c's stage 0).
     `epXhrAskBeganLife`: `send()` calls reaching the member body, raised at SEND_CHECKS's one-time capture,
       which is gated on a flag because that stage parks on step 3's declared fork over a concolic method.
     `epXhrAskPlacedLife`: of those, the ones that reached step 12/13 and placed the fetch.
     `epXhrAskOfferedLife`: addresses offered to @H, raised in the lifecycle machine on the line before
       `xhr_record_endpoint`'s endpoint_record call; it belongs to no send state.
     `epXhrOutFreedLife`, `epXhrOutFreedPlacedLife`: send states torn down after a construction began (deep-fork
       copies included), and of those the ones that had placed.
     `epXhrOutDiedAtLife`: of the freed states that placed nothing, one arm per `SEND_STEPS` stage. */
/* Identities, asserted in endpoint_xhr_edge_rows: the died-at arms plus freed-and-placed equal freed (a deep
   fork's copy gets its own teardown, so this survives forks); offered <= `epAsks`; began <= called.
   Not assertable, unlike the fetch edge: freed-and-placed <= placed, `freed <= began` and `placed <= began`.
   A deep fork byte-copies the send state with its flags, and `send()` runs page code after placing: the
   synchronous arm parks inside the lifecycle machine's `readystatechange` and `progress` listeners, and
   step 3's fork and step 4's `toString` run page code before it. Nor does any relation hold between placed
   and offered: a placed asynchronous send offers only once its enqueued closure is stepped, and abort() and
   the request error steps mint lifecycle machines that record nothing. Scope in time is the surface's (reset
   in endpoint_init and endpoint_free). Reports, never bounds. */
/* Named residual. Not covered: a placed asynchronous send whose lifecycle closure, a `JS_EnqueueCallTask`
   entry that solver/engine.c's `run-a-task` arm steps, is never stepped offers nothing, and no row says so.
   Next diff: a row counting the lifecycle closures this edge enqueued against those that reached `XR_FETCH`,
   raised on opposite sides of the task hop so the loss is a subtraction. Absence shows as:
   `epXhrAskPlacedLife` nonzero with `epXhrAskOfferedLife` zero and every send state accounted for, a zero
   `asks - preProgram` that is neither "never reached" nor "already known".
   Named residual. Not covered: an `open()` that threw or parked and never resumed raises no row, since this
   census begins at `send()`, and `open()` is where the method and the URL are parsed. Next diff: `open()`'s
   stage table matched beside `send()`'s by endpoint_edge_member_asked. Absence shows as: a page whose bundle
   calls `open()` reading `epXhrAskCalledLife` zero, with nothing separating it from a page that makes no XHR. */
void    endpoint_xhr_edge_declare(const char *entry, const char *const *steps, int first_stage);
void    endpoint_xhr_edge_began(void);
void    endpoint_xhr_edge_placed(void);
void    endpoint_xhr_edge_offered(void);
void    endpoint_xhr_edge_freed(int stage, int placed);
/* The XHR edge's rows (caller frees; NULL only on allocation failure), spliced into `_cold` beside the fetch
   edge's with their own leading comma and never summed with them, since they count a different machine's
   states. The empty string is the absent form: a host that installs no XMLHttpRequest has no population. */
char   *endpoint_xhr_edge_rows(void);

#endif
