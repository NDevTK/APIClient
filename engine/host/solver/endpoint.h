/* @H endpoint surface: the deduped set of endpoints forced execution learned, emitted as the `@H` array.
 *
 * Every HTTP-shaped request edge (fetch, XHR, sendBeacon, form submission, script/link/img loads, the
 * program-load park, multipart batch parts, reply-named chunks, a WebSocket's handshake, an EventSource's
 * request) calls
 * endpoint_record. A concolic URL
 * contributes its shape (`/api/region/{state}.region`), a concrete one its literal. A value lands in one of
 * three places and each param says which: the path (a `{hole}` segment whose example is recovered by aligning
 * the concolic's concrete URL against its shape; see path_scan), the query string, or the body (read in the
 * body's own content type). A hole's name carries the page's bytes verbatim; extension/lib/callsite-url.js
 * masks it around the trusted zone's `new URL` parse so it comes back intact.
 */
#ifndef ENGINE_HOST_SOLVER_ENDPOINT_H
#define ENGINE_HOST_SOLVER_ENDPOINT_H

#include "quickjs.h"
#include "core/fetch/fetch.h"   /* FetchCredentialsMode, the `creds` a producer states */

void    endpoint_init(void);
void    endpoint_free(void);
void    endpoint_suppress(int on);   /* 1 during a candidate/verify re-run: its requests are @S artifacts, not @H */

/* A header the request carries; an endpoint reachable only with, say, `Authorization` is not reproducible
   without it. `value` is the literal the code computed, or the unknown's shape (`{state}.token`), which marks
   a runtime value the reviewer supplies. Borrowed for the call; the surface copies what it keeps, and on a
   merge a concrete value supersedes a shape for the same header. `root` is the delivery root of the unknown
   whose shape `value` is (solver/concolic.h's concolic_root_c), NULL where the value is bytes. */
typedef struct { const char *name, *value, *root; } EndpointHeader;

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
/* `root` is the span's unknown's delivery root, NULL for none. */
typedef struct { size_t off, len; const char *shape, *example, *root; } EndpointBodySpan;

/* `root` is the delivery root of an EPB_SHAPE body's unknown, NULL for every other kind. */
typedef struct { const char *mime, *bytes; size_t len; EndpointBodyKind kind;
                 const EndpointBodySpan *span; int nspan; const char *root; } EndpointBody;

/* Which mechanism composed an address, stated by every producer because no consumer can re-derive it. Its
   zero is EPD_UNSTATED and endpoint_record refuses it. It is neither `prov` (what the sighting is evidence
   of) nor `mintedAt` (when): a `<link>` below a `<head>` script is minted post-program and is still markup.
   It is the first sighting's mechanism and is never re-armed, and it is not part of `same_identity`: two
   mechanisms reaching one address are one endpoint, and splitting it would scatter one request's params.
   Columns: id and wire token. One list keeps the enum, the token table and the door census in step. The
   three program doors share one call site,
   solver/engine.c's park consumer, whose `program_load_door` reads the park kind for `module-import` and the
   parser-inserted mark for the two element doors, because a kind is a queue position and not an origin. */
#define ENDPOINT_DOORS(X)                                                                                    \
    /* HTML §4.12.1.1 "Processing model": a program load whose `<script src>` a parser inserted, read off   \
       the park's `parser document` mark and never off the queue its reply is delivered at; a `<script src>` \
       a `document.write` put in the tree is parser-inserted too. */                                         \
    X(EPD_DOCUMENT_SCRIPT, "document-script")                                                                \
    /* a `<script src>` no parser inserted: running code put the element in the tree */                     \
    X(EPD_INJECTED_SCRIPT, "injected-script")                                                                \
    /* a dynamic `import()`: a program load with no element */                                               \
    X(EPD_MODULE_IMPORT,   "module-import")                                                                  \
    /* a `<script src>` whose address running code assigned as an unknown this engine cannot fetch. The     \
       taint shadow map holds an entry only where a script wrote the attribute, so this door is never       \
       parser-inserted (asserted in core/html/html_script.c). */                                             \
    X(EPD_SCRIPT_ELEMENT,  "script-element")                                                                 \
    /* HTML §4.2.4.3 "Fetching and processing a resource from a link element", preloads included. A `<link>` \
       the markup declared and one a script created reach it alike, and the door cannot say which. */       \
    X(EPD_LINK_ELEMENT,    "link-element")                                                                   \
    /* HTML §4.8.4.3.5 "Updating the image data", its source set and its undecided arm. An `<img>` the      \
       parser built and one `new Image()` composed are one door. */                                          \
    X(EPD_IMAGE_ELEMENT,   "image-element")                                                                  \
    /* HTML §4.10.5.1.19 "Image Button state (type=image)": an `<input type=image>`'s own image fetch, a   \
       different algorithm from an `img`'s over the same destination. Markup and script reach it alike. */  \
    X(EPD_IMAGE_INPUT,     "image-input")                                                                    \
    /* a `<form action>` from the markup and one whose action a script wrote reach core/html/html_form.c's  \
       one recording pair alike */                                                                           \
    X(EPD_FORM_SUBMIT,     "form-submit")                                                                    \
    /* HTML §7.4.2.2 "Beginning navigation"'s destination, and §7.4's create when it folds one in: a page   \
       address, recorded where core/frame/navigable.c resolves it and before any policy decides whether it  \
       loads. A markup `<iframe src>` and a script's `open()` or `location` write reach it alike. */         \
    X(EPD_NAVIGATION,      "navigation")                                                                     \
    X(EPD_FETCH,           "fetch")                                                                          \
    X(EPD_XHR,             "xhr")                                                                            \
    X(EPD_BEACON,          "beacon")                                                                         \
    /* WebSockets §2.2 "Opening handshake": a socket's address, recorded where its constructor parks the \
       handshake */                                                                                         \
    X(EPD_WEBSOCKET,       "websocket")                                                                      \
    /* HTML §9.2.2 "The EventSource interface": an event stream's address, recorded where its constructor    \
       creates step 8's request */                                                                          \
    X(EPD_EVENT_SOURCE,    "event-source")                                                                   \
    /* a sub-request written inside a multipart batch body the page composed */                              \
    X(EPD_BATCH_PART,      "batch-part")                                                                     \
    /* an address a reply named (solver/reply_decode.c) and no line of the page composed */                  \
    X(EPD_REPLY_CHUNK,     "reply-chunk")

typedef enum {
    EPD_UNSTATED = 0,   /* nobody said; endpoint_record refuses it */
#define ENDPOINT_DOOR_MEMBER(id, token) id,
    ENDPOINT_DOORS(ENDPOINT_DOOR_MEMBER)
#undef ENDPOINT_DOOR_MEMBER
    EPD_COUNT           /* list end; the members take no explicit values, so the enum is dense and the mint's
                           range check is a membership test */
} EndpointDoor;

/* The one wire token of a door. A value outside the list is a CHECK, not a DCHECK: this runs once per emitted
   row in every build, and a release fallthrough would publish a plausible mechanism name nothing ran. */
const char *endpoint_door_token(int door);

/* What the address value's own concolic provenance proves about it, derived by `address_class_of` from
   endpoint_record's `url` alone; no producer states it, so there is no unstated member. It answers two
   questions from one stored member, so they cannot disagree about a row:
     Q1, did the run determine this address:                          `cls <= EPA_SOURCE_DETERMINED`;
     Q2, does the undetermined address rest on a world-supplied unknown: `cls == EPA_UNKNOWN`.
   The order is load-bearing and asserted at `endpoint_address_hist_json`: `EPA_CONCRETE` is zero (the
   memset's identity and the class that proves nothing), the determined members are a prefix (Q1 is a
   threshold), and `EPA_UNKNOWN` is the top. A record's merge is a MAX over the order, because what one
   sighting proved about the address is not taken back by a later sighting that proved less. `concrete`
   also covers an address composed of literals, the document's own address, and an address a reply carried.
   Named residual. Not covered: a `<form action>` composed from an unknown records `concrete`.
   core/html/html_form.c reads the action's taint at HTML §4.10.22.3 "Form submission algorithm" step 12 and
   spends it on the @S sink alone, then `form_record_request` records the `url_serialize`d bytes, while the
   link and image doors record the taint itself. Recording the bare taint would drop the entries the
   submission writes into the action's query, and unioning `valueClass` into the address would over-claim,
   because `concolic_hole_key` is a bare brace test. Next diff: the recorded address composed as a concolic
   from the action's taint and the entries, which needs `url_parse`/`url_serialize` to carry concolics.
   Absence shows as: `form-submit` rows reading `concrete` beside their own `valueClass` `unknown` while the
   same document's `link-element` rows read `unknown`. */
#define ENDPOINT_ADDRESS_CLASSES(X)                                                                          \
    /* the run determined this address and holds no evidence that any segment of it entered the program at   \
       a source (`concolic_is` is false). */                                                                 \
    X(EPA_CONCRETE, "concrete")                                                                              \
    /* the address is this engine's own concolic and this flow's equality pinned it: concolic.h's           \
       CONCOLIC_EX_DETERMINED, as in `var u = cfg.url; if (u === '/api/v2/items') fetch(u);`. Q1 answers   \
       determined and Q2 no: a pin's bytes are the equality's other operand, which solver/concolic.c spells \
       only where that operand is not concolic. */                                                           \
    X(EPA_SOURCE_DETERMINED, "source-determined")                                                            \
    /* the run did not determine it, and no root of the value is one anybody outside this engine supplied   \
       (`concolic_root_whose_any(url, CONCOLIC_WHOSE_WORLD)` answers no). It holds a hole this engine minted \
       to drive code (a driven orphan's own argument), a value whose bytes stand in the served document but  \
       is minted unknown so a gate over it forks (a data block, a global an inline script wrote), and a      \
       value no mint could attribute (concolic.h's `UNSTATED`, or no root at all). */                        \
    X(EPA_UNKNOWN_UNPROVEN, "unknown-unproven")                                                              \
    /* the run did not determine it, some root is the world's, and its origin is undetermined: no page byte \
       ahead of the first hole decides the scheme and host against the document base (URL §4.4 "URL         \
       parsing"), and the address holds no example to parse (concolic.h's NONE or CONTRADICTED), as in a    \
       host read out of an absent cookie. A request whose origin no run determined names no endpoint of the  \
       app. A host from a reply field the run holds an example of is `unknown`. */                           \
    X(EPA_UNKNOWN_ORIGIN_OPEN, "origin-undetermined")                                                        \
    /* the run did not determine it, and some root is one a document, a server, an attacker or the            \
       environment supplied (concolic.h's `WORLD`: the location fragment, an absent server-injected global,  \
       a reply field, a device reading). One such root suffices (`_any`, not `_all`). It is the top member, \
       so a reordering is what could lower a record out of it. */                                           \
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

/* Whether the address may rest on a witness this engine chose (bytes the engine picked rather than a server
   sent), published per row as `witnessClass`. It bounds reproducibility: a row resting on chosen bytes is one
   a real session may never see. It separates two
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
       address. */                                                                                   \
    X(EPW_NO_WITNESS,  "no-witness")                                                                \
    /* a flow stood whose path had determined a source's value on an arm no example confirmed       \
       (solver/flow.h's `path_pinned`). The address may rest on bytes this engine                    \
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

/* The emitted surface partitioned by address class, as a malloc'd JSON object (caller frees): one row per
   member of ENDPOINT_ADDRESS_CLASSES, zeroes included, summing to endpoint_surface_census's `emitted`
   (asserted here, with the class order). It is keyed on the address value, an observation independent of the
   door histograms. A diagnostic, read within one run against `epEmitted` on the same census line. */
char   *endpoint_address_hist_json(void);

/* The emitted surface partitioned by door, as a malloc'd JSON object (caller frees): one row per member of
   ENDPOINT_DOORS, zeroes included, summing to endpoint_surface_census's `emitted` (asserted). */
char   *endpoint_door_hist_json(void);

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
/* `driven` is whose act the sighting was (solver/engine.h's PENDING_DRIVEN_*), part of the identity beside
   `prov` and independent of it: running code states `engine_driven_of_running_path()` (asserted against the
   running flow), and a reply-learned or park address states the request's. */
/* `door` is which mechanism composed the address, stated by the producer; see ENDPOINT_DOORS. */
/* `creds` is the request's Fetch §2.2.5 "Requests" credentials mode (core/fetch/fetch.h's FetchCredentialsMode),
   stated by the producer that created the request: fetch, XHR and a program park.
   Named residual. Not covered: every other door passes FETCH_CREDENTIALS_UNPLACED, so its record states no
   credentials mode. Next diff builds: each element door's mode from its CORS settings attribute
   (core/html/cors_settings_attribute.h) and each navigation's and beacon's from its request. Absence shows as:
   `credentials: null` on a link, image, form, navigation or beacon record. */
void    endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                        const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body, int prov, int driven,
                        int door, int creds);

/* The @H surface as a malloc'd JSON array (caller frees), one object per record not marked an asset; it is an
   array and not a document because result.h composes the document. Record keys:
     `method`; `url` (the path shape); `bodyMime` with one of `bodyBase64`, `bodyShape` or
       `bodyExampleBase64` (see EndpointBodyKind); `provenance` (`observed`|`derived`|`forced`); `driven`
       (a boolean: the sighting was minted inside a drive of a function the page never called); `door`;
       `mintedAt` (`pre-program`|`post-program`); `addressClass`; `witnessClass`; `witness`
       (below); `addressRoot` (null for a `concrete` address, false when the value has no root, else the
       root's name); `attackerRooted` (below); `params`; `headers` (an object, only when a header was seen).
   Every class key is always present, since its words are exhaustive. `provenance` is the record's, its grade
   being part of the identity: `derived` is the headline claim (the app's code computes a request no session
   sent), `observed` a real load of the document, and `forced` an address that exists only because a gate was
   forced, never to be reported as either other word. `addressClass` cannot be re-derived from braces in
   `url`: extension/lib/learn.js mints `{path_*}` holes from live traffic into addresses it reads `concrete`
   for. Q1 (determined) is `concrete` or `source-determined`; Q2 is `unknown`, so a consumer reading Q2 as
   `!= "concrete"` over-claims. */
/* `attackerRooted`: false, or `{address?, headers?, body?, victimAct}`, which marks the record
   security-relevant: each piece an attacker source rooted on a path an attacker reaches (every act on it one the
   attacker performs or a victim performs on the page) names its first such sighting's root (`headers` as an
   object of header name to root; `body` as an array of every distinct root its unknown and its spans carry),
   and `victimAct` is whether every marking sighting's path needed a victim act. At least one piece is present. */
/* `credentials`: each Fetch §2.2.5 credentials mode a sighting's producer stated, in that enum's order, with null
   for a sighting whose producer stated none (the `creds` residual above). Whether cookies are sent is derived
   where the principal is (solver/pending.h's `credentials` field states why the engine sends three words). */
/* `destinations`: each of `same-origin`, `cross-origin` (the address's origin against its document's, from the
   bytes the run determined it from) and `unknown` (it determined none) some sighting read. */
/* `attackerNeeds`: false for an unmarked record, else `{common, deliveries}`. `deliveries` holds each distinct
   marking sighting's `{victimAct, sources}`: `victimAct` whether a victim act stands on its path
   (flow_path_victim_act, which counts no message act), and `sources` its
   declared sources as `{source, delivery, deliveryPrefix}`, a message's with the `messageRoutes`
   document_message_routes leaves the attacker page (empty where COOP and framing refuse both). `common` is
   whether some marking sighting was rooted in address components alone. */
/* `assumes`: null where no flow composed a sighting, else the minimal sets of priced decisions its sightings'
   paths rested on (solver/flow.h's flow_priced_each), none a subset of another: `[[]]` is a record some path
   reached assuming nothing. An entry is `{kind, name}`: `absent-read` an absent global or member read past its
   throw, `absent-arm` an arm taking absent application state (a global, a member, a stored key, a cookie) as
   present, `environment-arm` an arm against an environment unknown's example; `name` the source or root. */
/* `witness`: one element per distinct sighting witness, never a union of two (it would state a combination no
   flow assumed), never empty. An element is `null` where no flow stood (`witnessClass` `unasked`), else
   `{assumed, verification}`, `assumed` being the minting flow's assumptions at the act, sorted by kind then
   text: `{source, value, kind}` per pin (the source path, the ToString of the operand its `===` determined,
   that operand's type), whether or not an observation agreed; `{decision, taken}` per branch arm over an
   input another party sets (a root member declaring a precondition or a delivery) that pinned nothing and that
   no observation established (its example contradicts it, or there is none), as the condition's shape and its
   ToBoolean; `{decision, completion}` per native operation over such an input settling on a completion other
   than its machine's declared real one, as `op(operand)` and the completion index. A decision states the arm,
   never a value satisfying it; an environment unknown's arm is none. */
/* Named residual. Not covered: an assumed arm over an input whose predicate has no spellable
   identity, a declined request's failure arm, a modelled close request and a document's forced arrival
   (engine.h's engine_set_arrival) mark the path forced and file no decision. Next diff: a decision row for
   each, keyed by the act's own name (for an arrival, the creating instance's witness crossing beside the
   arrival grade). How its absence shows: a `forced` row whose every witness element has an empty `assumed`
   on a page whose code reads no environment unknown. */
/* `verification` is what firing the witness established (solver/witness_verify.h): `verified` when a run with
   its value delivered through the source's real transform reached this method and path assuming nothing of
   its own; `candidate` when such a run was minted and has not; `no-candidate` when firing cannot check it
   (endpoint.c's witness_payload names what that covers). Neither unverified word is a verdict that the
   witness is infeasible. */
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
/* `origin_finding` and `origin_driven` are the two further exits of an address whose scheme and host an
   attacker source decides: handed to the request class, or dropped under a drive with no victim act. With the
   three above they partition `asks`. */
void    endpoint_ask_census(long *asks, long *pre_program, long *suppressed, long *merged, long *minted,
                            long *merged_pre_program, long *origin_finding, long *origin_driven);

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
/* Identities, asserted in endpoint_fetch_edge_rows or, where solver/metrics/fetch_edge.def declares them, at
   the family's composition: the died-at arms plus freed-and-offered equal freed (two arms of one teardown);
   freed-and-offered <= offered (the slack is offered states still parked on their
   reply; the offering stage returns rather than parks and no page code runs before teardown, so no flagged
   state is cloned); offered <= `epAsks` (the slack is every other door); began > 0 implies called > 0.
   Not assertable: `freed <= began`, `offered <= began`, `began <= called`. A deep fork byte-copies a step
   state with its capture flag (fetch.c's banner names the stages that permit one), and a fork in the
   prologue's conversions leaves stage 0 behind, so siblings begin against one call; `began - freed` is not a
   live count either. The stage rows are a partition: a zero at one stage says nothing about its neighbours.
   `called == 0` cannot separate a page that calls no `fetch()` from a flow that never reached its call; the
   `Named` rows answer that from the compiler. A concolic URL crosses the USVString conversion without parking
   (`idl_concolic_rule` answers IDL_CONCOLIC_CROSSES), so the conversions park or fork only on a page getter
   such as a `Request` input's `url` or a `RequestInit` member, or on an enum or presence question over
   unknown input. Scope in time is the surface's (reset in endpoint_init and endpoint_free). Reports, never
   bounds. */
/* Named residual. Not covered: a call whose conversions forked and then threw or parked in some worlds is
   not counted as a death in those worlds; no row is raised per world at the prologue. Next diff: a row
   raised at the teardown of a hosting state still standing at stage 0 or 1, one per world as `freed` is.
   Absence shows as: `epFetchAskCalledLife - epFetchAskBeganLife` zero or negative on a page whose RequestInit
   conversion throws in some worlds. */
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
/* Identities, asserted in endpoint_xhr_edge_rows or, where solver/metrics/xhr_edge.def declares them, at the
   family's composition: the died-at arms plus freed-and-placed equal freed (a deep fork's copy gets its own
   teardown, so this survives forks); offered <= `epAsks`; began <= called.
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
