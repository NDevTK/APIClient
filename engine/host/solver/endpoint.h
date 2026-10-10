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

/* WHETHER THE RUN COMPOSED THIS ADDRESS OUT OF A VALUE IT HAD NOT DETERMINED, AND WHOSE UNKNOWN THAT VALUE
   RESTS ON — the fourth fact about a sighting, and the one CLAUDE.md §What-the-tool-produces' HARD BAR is a
   claim about. The second clause is not a second fact: it is what makes the first one a claim the bar may
   read, and both are answered by ONE stored member (see the two questions below). That bar is "an
   address, a key or a value that NO PARSE of the served bytes can state, because it exists only at run
   time", and the three facts already on a record cannot answer it at any grain: `door` is WHICH MECHANISM,
   `mintedAt` is WHEN, and `EPR_BEYOND` is a property of the DOOR — so a literal chunk URL delivered through
   `module-import` is `beyond` and clears NOTHING at this bar, while `/api/{location.hash}` through `fetch`
   clears it outright. Two addresses through ONE door differ on exactly this, and no count of doors can ever
   say which.
   IT IS READ OFF THE ADDRESS VALUE'S OWN CONCOLIC PROVENANCE AND NOTHING ELSE. §Every-value-is-CONCOLIC: the
   triple RIDES the value the interpreter computed, so the fact is standing in `endpoint_record`'s own `url`
   argument and needs no producer to carry it, no parser to be consulted, and no second implementation of
   `what a static reader could reach` — which is the auditor CLAUDE.md §AN-AUDITOR-DERIVES-THE-RULE forbids
   and which would additionally be a JS-layer dependency §Architecture forbids the engine having.
   SO IT IS THE ONLY ONE OF THE FOUR WITH NO `UNSTATED` MEMBER, for `ENDPOINT_REACHES`' reason exactly: a
   door and a grade are stated BY A PRODUCER and a producer can forget, while this is DERIVED at the one door
   every HTTP-shaped edge passes through, so there is nobody to forget and nothing for a zero to mean.
   IT IS A FLOOR AND IT IS NOT A BOOLEAN ABOUT PARSES, which is the whole of its honesty and the half a
   reader must not drop. `unknown` is a POSITIVE statement — the run reached this address holding a value it
   had not determined AND SOME ROOT OF THAT VALUE IS ONE A DOCUMENT, A SERVER, AN ATTACKER OR THE ENVIRONMENT
   SUPPLIED (core/frame/location.c's fragment, an absent server-injected global, a reply field, a device
   reading) — so the bytes are in no served byte and such a row DEFINITELY clears the hard bar. `concrete`
   claims nothing whatever about a parse: the run had determined this address by the time it recorded it.
   THE CONJUNCT IS NEW AND THE SENTENCE WITHOUT IT IS RETIRED RATHER THAN DELETED, BECAUSE IT IS WHAT A
   READER RE-DERIVES FROM THE WORD `unknown`. It read: the value "entered the program at a source
   (core/frame/location.c's fragment, an absent server-injected global, a reply field, a device reading, A
   DRIVEN ORPHAN'S OWN ARGUMENT) and which is therefore in no served byte". EVERY CLAUSE IS TRUE AND THE LAST
   STEP DOES NOT FOLLOW, and the enumeration names its own counterexample: a driven orphan's argument is a
   hole THIS ENGINE minted, so the row is in no served byte and nothing was LEARNED from it — the bar is true
   of the address and the product's claim is false of it. The population that entered at a source whose bytes
   the SERVED DOCUMENT spells fails the bar's sentence outright. Both are `unknown-unproven` now. WHETHER A PARSE COULD HAVE STATED IT IS NOT DECIDABLE BY ANYBODY — a bundler's chunk
   manifest needs a scope pass to resolve, so what a static reader reaches is a moving frontier rather than a
   property — and this list deliberately does not pretend otherwise. Neither word may be read as its
   complement over parses.
   IT IS ONE FACT AND TWO PREDICATES OVER IT, WHICH IS WHY IT IS A LIST AND NOT A BIT. The field was answering
   TWO questions with one bit and the two differ for one population, which is
   §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS in a declared list. The questions:
     Q1 — DID THE RUN DETERMINE THIS ADDRESS?  `cls <= EPA_SOURCE_DETERMINED`.
     Q2 — DOES THIS ROW CLEAR THE HARD BAR?    `cls == EPA_UNKNOWN`, which is what `endpoint_razor_class_of`
       already keys on, BYTE-IDENTICALLY, and is why that line has not changed at either widening.
   They are two QUESTIONS asked of ONE stored member rather than two members, so they cannot disagree about a
   row and there is no second field for a merge to get wrong. Where Q1 and Q2 both answer "proves nothing" the
   member is `EPA_CONCRETE`; where Q1 answers DETERMINED and Q2 PROVES NOTHING it is `EPA_SOURCE_DETERMINED`;
   where Q1 answers NOT DETERMINED and Q2 STILL PROVES NOTHING it is `EPA_UNKNOWN_UNPROVEN`; where Q1 answers
   NOT DETERMINED it is `EPA_UNKNOWN` and Q2 clears.
   Q1 WAS SPELLED `cls != EPA_UNKNOWN` AND THAT IS RETIRED RATHER THAN DELETED, because it is what a reader
   re-derives from "the two tests are one test negated" — which they were while every undetermined address
   was one member. They are not each other's complement now and were never obliged to be: what makes this one
   member rather than two FIELDS is that both are functions of the SAME stored value, not that one is the
   other's negation. Q1 IS A THRESHOLD AND THE DETERMINED MEMBERS ARE A PREFIX OF THE ORDER, which is the one
   thing that spelling needs and which `EPA_COUNT` asserts: a determined member added ABOVE an undetermined
   one would make `cls <= EPA_SOURCE_DETERMINED` silently wrong while every count still summed.
   THE POPULATION THAT FORCED THE SPLIT IS THE ONE Q1 WAS WRONG ABOUT, AND THE RAZOR WAS OVER-CLAIMING ON IT.
   `concolic_is` is TRUE for a source value a flow has pinned and is STILL HOLDING — concolic.h's pin arm
   re-mints a source on a RE-READ and hands back a bare primitive, but a value the page materialised into a
   variable before its own gate stays this engine's own concolic, and `concolic_example_state` answers
   DETERMINED for it. `var u = cfg.url; if (u === '/api/v2/items') fetch(u);` is the shape. Before this member
   that row read `unknown` and the bar read `runtime-only` for an address whose bytes are a STRING LITERAL
   STANDING IN THE BUNDLE, which is an over-claim on the one number CLAUDE.md §What-the-tool-produces judges
   this product by. It now reads `source-determined` and the bar reads `unproven`.
   AND THIS FILE USED TO ASSERT THE OPPOSITE AT THREE SITES — that a PINNED AND RE-READ source "really is past
   every parse" — WHICH IS THE SAME OVER-CLAIM IN THE OTHER DIRECTION AND IS RETIRED RATHER THAN DELETED,
   because a reader who re-derives it from "the taint is gone" will write it again. It read: the third
   population "really did derive from an unknown and this cannot say so, which is why the field is a floor",
   over the worked example `if (s.tier === 'silver') fetch('/api/' + s.tier)`. THE DERIVATION IS TRUE AND THE
   CONCLUSION DOES NOT FOLLOW. A pin's bytes are `literal_tok` of the equality's OTHER operand, and
   solver/concolic.c spells that operand ONLY where it is not itself concolic — so the bytes are a value the
   page's own text determined, which in the dominant case is a literal the bundle spells. BOTH of that
   example's segments (`'/api/'` and `'silver'`) stand in the served bytes, so `/api/silver` does NOT "exist
   only at run time", which is the clause the bar's own sentence turns on. CLAUDE.md
   §the-static-instrument-is-the-competitor forbids banking the difference: a parse that refines an unknown
   under its own guard is one anybody can write, `testing/static_surface.mjs`' own banner says a real
   extractor "would fold MORE", and a diff that teaches the parse to fold RAISES the bar rather than
   regressing this engine's score. What a pin proves is WHICH WORLD the run explored — a reachability finding,
   and a real one — and never that the bytes are absent from the document.
   SO WHAT `concrete` STILL HIDES IS TWO POPULATIONS AND NOT THREE, and `url_display`'s banner enumerates
   them: an address composed OF LITERALS (a bundler's module graph, the whole surface of a run that reached no
   API code), and one composed of THE DOCUMENT'S OWN ADDRESS, which is concrete for the document this engine
   loaded. Neither clears the bar and neither ever did. The third has its own member, and the half of it this
   list still cannot reach is a NAMED RESIDUAL at `address_class_of`.
   AND THE SECOND THING IT CANNOT ANSWER IS AN ADDRESS WHOSE RUNTIME-NESS THE *DOOR* CARRIES. `reply-chunk`
   is the sharpest: solver/reply_decode.c hands this surface a plain string it read out of a REPLY, so the
   value is `concrete` while no parse of the DOCUMENT reaches that address at all. THAT SET IS NOW THE FOURTH
   COLUMN OF `ENDPOINT_DOORS` (`ENDPOINT_DOOR_BYTES`) AND IS NO LONGER NAMED HERE, and the enumeration that
   stood in this paragraph is recorded rather than deleted because it is the one a reader re-derives from "a
   park's URL is what the reply seam is keyed on". It read: "`batch-part` and the three program doors are the
   same shape — each records an address already resolved to a `char *`". EVERY CLAUSE OF THAT IS TRUE AND THE
   CONCLUSION DOES NOT FOLLOW, because an address being a resolved `char *` is a fact about the ENGINE'S OWN
   PLUMBING and the bar is a question about the SERVED BYTES. The three program doors carry addresses the
   document's markup or its own scripts SPELL — `document-script` is `EPR_MARKUP` by this file's own third
   column, so one header was giving two answers about one door, and `module-import` is the address CLAUDE.md
   §What-the-tool-produces names by hand as "a literal chunk URL delivered through `module-import` is `beyond`
   and scores ZERO here". `batch-part`'s sub-request address is written into a body the page composed, whose
   literal segments stand in the bundle. So the floor is the UNION of this column's `unknown` rows and the rows
   whose door is `EPB_OFF_DOCUMENT`, and `endpoint_razor_class_of` composes exactly that, per row, at the one
   line where both facts are in hand.
   ITS MERGE IS A MAX OVER THE ORDER, which is the two-member UNION it always was spelled for a longer list,
   and it holds for that union's reason: this is a fact about what SOME observed path proved about the
   address, which a later sighting that proved LESS cannot take back. Every step is that one rule:
   `concrete` ∨ `source-determined` is `source-determined`, because a sighting that saw the address enter at a
   source is not taken back by a later one composed of literals; `source-determined` ∨ `unknown-unproven` is
   `unknown-unproven`, because a path that did NOT determine it is not taken back by one that did — which is
   the union's own sentence verbatim; and `unknown-unproven` ∨ `unknown` is `unknown`, because a sighting that
   composed the address out of an unknown somebody OUTSIDE this engine supplied is not taken back by one that
   composed it out of a hole this engine minted. That last step is concolic.h's `_any` at the grain of a
   RECORD rather than of a value, and it is why the record needs no second field and no second merge rule for
   the whose fact: the ORDER carries the disjunction. Intersecting at any step would answer a lower member for
   an address one path already proved more about, and at the last step would make the bar's floor read LOWER
   than what the run established. It is deliberately NOT the door's never-re-armed rule: the door asks which mechanism composed
   the record's FIRST sighting, and this asks what any sighting proved about the address.
   THE ORDER IS THEREFORE LOAD-BEARING AT BOTH ENDS and is asserted at the census walk rather than argued
   here — see `EPA_COUNT`. `valueClass` is still the same rule one grain out and is still spelled as a
   two-member union, because a param's value has no pinned-and-still-held population to carve out of it.
   A LIST AND NOT TWO `#define`s, for `ENDPOINT_DOORS`' reason: the enum, the token table and the census over
   it are ONE list, so a class added to the enum and not to the table cannot come off the end of a name
   array. THE FIRST MEMBER IS THE CONSERVATIVE ONE AND ITS VALUE IS ZERO BY CONSTRUCTION — the mint writes
   this field unconditionally one line after `door`, so nothing depends on the memset, and the ordering is
   what makes a field some later diff forgot read as the claim that PROVES NOTHING rather than as the claim
   that clears the product's own bar. */
#define ENDPOINT_ADDRESS_CLASSES(X)                                                                          \
    /* the run had DETERMINED this address and this surface holds no evidence that any segment of it         \
       entered the program at a source. It claims nothing about a parse, for the reason the banner gives,    \
       and it is the member a later diff that forgot to write this field reads as. */                        \
    X(EPA_CONCRETE, "concrete")                                                                              \
    /* …and the run DETERMINED it OUT OF a value that entered at a source: the address is this engine's own  \
       concolic, its own `src` names it, and THIS FLOW'S equality pinned that path — concolic.h's            \
       CONCOLIC_EX_DETERMINED, which is the one answer of that accessor's four that is a PROOF.              \
       IT IS THE MEMBER AT WHICH THE TWO QUESTIONS PART, and it answers them differently: Q1 DETERMINED, Q2  \
       PROVES NOTHING. A pin's bytes are the equality's OTHER operand, which solver/concolic.c spells only   \
       where that operand is NOT concolic, so they are bytes the page's own text determined — in the         \
       dominant case a string literal standing in the bundle. The engine proved WHICH WORLD it explored; it  \
       did not prove the address's bytes are absent from the served document, and the bar is keyed on the    \
       bytes. So this row does NOT clear it. */                                                              \
    X(EPA_SOURCE_DETERMINED, "source-determined")                                                            \
    /* …and the run did NOT determine it, and NO root of the value is one anybody outside this engine        \
       supplied — `concolic_root_whose_any(url, CONCOLIC_WHOSE_WORLD)` answers NO. Three populations reach   \
       here and ONE predicate separates all three from the member below, which is why they are one member    \
       and not three: a hole THIS ENGINE minted so that a drive could happen at all (a driven orphan's own   \
       argument, which stood in no byte anywhere because it was never in any byte); a value whose bytes      \
       STAND IN THE SERVED DOCUMENT and which is minted unknown anyway so that a gate over it FORKS rather   \
       than being decided for the whole run (§4.12.1's data block, an injected global an inline script       \
       wrote); and one no mint could speak for at all (concolic.h's `UNSTATED`, and the value with no root   \
       at all, whose mask is empty so `_any` answers NO for every member).                                   \
       SO THIS ROW DOES NOT CLEAR THE BAR, AND THAT IS A REFUSAL RATHER THAN A SMALLER CLAIM. The bar is     \
       "no parse of the served bytes can state this address, because it exists only at run time": the first  \
       population makes that sentence TRUE of the row while the PRODUCT'S claim is false of it (nothing was  \
       LEARNED, because the hole is ours), the second makes the sentence itself false, and the third proves  \
       nothing either way. All three answer `unproven`, which is not a weaker version of clearing the bar —  \
       an unproven claim is not a true one.                                                                  \
       IT IS WHY Q1 AND Q2 STOPPED BEING ONE TEST NEGATED, and it is the member every row of this engine's   \
       own instrument holes moved INTO — see `address_class_of`, whose two residuals it retires. */          \
    X(EPA_UNKNOWN_UNPROVEN, "unknown-unproven")                                                              \
    /* …and the run reached this address holding a value it had NOT determined AND SOME ROOT OF IT IS ONE    \
       A DOCUMENT, A SERVER, AN ATTACKER OR THE ENVIRONMENT SUPPLIED (concolic.h's `WORLD`), so the bytes    \
       stand in no served byte and this row clears the hard bar.                                             \
       ONE WORLD ROOT IS ENOUGH, WHICH IS A DISJUNCTION AND NOT A LOOSE READING: an address whose root set   \
       MIXES a page unknown with holes this engine minted genuinely rests on something no parse can state,   \
       so `_any` and never `_all` is the question — concolic.h's own sentence, and the same rule the         \
       record's merge below is a MAX for, one grain out.                                                     \
       IT USED TO INCLUDE THE VALUE THIS ENGINE COULD NOT ATTRIBUTE TO A SOURCE AT ALL, AND THAT CLAUSE IS   \
       RETIRED RATHER THAN DELETED because a reader who re-derives it from "an unknown is an unknown         \
       wherever it came from" will write it again. It read: "It includes the value this engine could not     \
       attribute to a source at all (concolic.h's NULL root), which is a weaker claim about WHERE the bytes  \
       came from and the same claim about the run not having determined them." BOTH CLAUSES ARE TRUE AND     \
       THE CONCLUSION DOES NOT FOLLOW: this bar is a claim about WHERE THE BYTES CAME FROM, so a weaker      \
       claim about exactly that is not one it may read. That population is `unknown-unproven` above.         \
       IT IS THE TOP MEMBER AND THE MERGE IS A MAX, so this is the one member a reordering could silently    \
       lower a record OUT of; `endpoint_address_hist_json` asserts both ends of the order. */                \
    X(EPA_UNKNOWN,  "unknown")

typedef enum {
#define ENDPOINT_ADDRESS_CLASS_MEMBER(id, token) id,
    ENDPOINT_ADDRESS_CLASSES(ENDPOINT_ADDRESS_CLASS_MEMBER)
#undef ENDPOINT_ADDRESS_CLASS_MEMBER
    EPA_COUNT           /* the list's own end — dense by construction, for `EPR_COUNT`'s reason, and ORDERED,
                           which this list is not free to be arbitrary about for `EPW_COUNT`'s reason exactly:
                           the record's merge is a MAX over it. BOTH ENDS ARE LOAD-BEARING AND BOTH ARE
                           ASSERTED at `endpoint_address_hist_json` — `EPA_CONCRETE` is the identity element
                           the `memset` leaves behind, and `EPA_UNKNOWN` is the member the bar reads, so a
                           reordering that moved it off the top would let a MAX carry a record out of the one
                           class `endpoint_razor_class_of` keys on and the bar would stop claiming an address
                           a path had proved.
                           AND THE DETERMINED MEMBERS ARE A PREFIX OF IT, WHICH IS A THIRD LOAD-BEARING FACT
                           ABOUT THE ORDER AND IS ASSERTED AT THE SAME WALK. Q1 is spelled as a THRESHOLD
                           (`cls <= EPA_SOURCE_DETERMINED`), which it has to be once the undetermined
                           addresses are more than one member — so a determined member added ABOVE an
                           undetermined one makes that reading silently wrong while every count still sums
                           and both ends above still hold */
} EndpointAddressClass;

/* THE ONE WIRE SPELLING OF AN ADDRESS CLASS, and `endpoint_reach_token`'s severity for its reason: it runs
   once per emitted ROW and once per census row in EVERY build, so a release build falling through would put
   whatever the register held into a JSON string and publish a class name nothing decided. */
const char *endpoint_address_class_token(int cls);

/* WHETHER THIS RUN PROVED THE ADDRESS EXISTS ONLY AT RUN TIME — CLAUDE.md §What-the-tool-produces' HARD BAR
   AS ONE WORD PER ROW, and the thing that bar's own retirement condition asks for: "the emitter that states a
   razor figure composes that union ITSELF, so a door count cannot be assembled into one at all". Until this
   list there was no such word anywhere: `door`, `mintedAt` and `addressClass` are three facts a consumer had
   to UNION by hand, the union's second operand was named in prose at three sites with three different sets
   (see `ENDPOINT_DOOR_BYTES`), and a reader who assembled it out of `EPR_BEYOND` got the figure CLAUDE.md
   demoted rather than the one it chose.
   IT IS THE UNION OF TWO POSITIVE STATEMENTS AND IT IS A FLOOR IN BOTH DIRECTIONS. A row clears the bar if
   the run reached the address holding a value it had NOT DETERMINED AND SOME ROOT OF THAT VALUE IS ONE
   SOMEBODY OUTSIDE THIS ENGINE SUPPLIED (`EPA_UNKNOWN`, which is exactly that conjunction — see that
   member), or if the door handed this surface bytes that were never in the served document at all
   (`EPB_OFF_DOCUMENT`). Neither implies the other and each alone is sound, which is what makes the union a
   floor rather than a guess: `reply-chunk` is `concrete` every time and clears the bar every time, and
   `/api/{location.hash}` through `fetch` is `EPB_DOCUMENT` and clears it every time.
   THE ADDRESS-SIDE OPERAND USED TO BE `NOT DETERMINED` ALONE, AND THAT IS RETIRED RATHER THAN DELETED
   because it is what a reader re-derives from the word `unknown`. It read: "the run reached the address
   holding a value it had NOT DETERMINED (`EPA_UNKNOWN` — some segment entered at a source, so it stands in
   no served byte)". THE PARENTHETICAL IS THE STEP THAT DOES NOT FOLLOW: a segment entering "at a source"
   says a MINT happened and says nothing about WHOSE unknown it was, and the two populations that part there
   are the ones the bar is about — a hole THIS ENGINE minted for a drive, and a value whose bytes stand in
   the served document and which is minted unknown anyway so a gate over it forks. Measured over the
   committed corpus at the time, SIX of seven real sites cleared the bar ZERO times and the seventh's whole
   figure was this engine's own orphan arguments, so the retired operand's claim was paid for entirely by
   the engine talking to itself. The DERIVATION rather than the figures, because the censuses are untracked
   by design: `testing/corpus/holewhose.mjs`, run BARE.
   ITS TWO OPERANDS HAVE DIFFERENT MERGE RULES AND THE UNION IS SOUND UNDER BOTH, which a reader must be told
   because it looks like a defect. `addr_class` is UNIONED on a merge and `door` is the FIRST sighting's and is
   never re-armed. Both directions hold: if ANY sighting composed the address out of an undetermined value
   resting on an unknown somebody outside this engine supplied then the address exists only at run time and a
   later determined sighting cannot take that back, and if the FIRST
   sighting read the address out of a reply's payload then those bytes were in no served document and a later
   sighting of the same address through `fetch` does not put them there. So the verdict is computed at the
   EMITTER from the two fields rather than stored, and there is no third merge rule to get wrong — which the
   whose fact did not change, because it rides the ADDRESS CLASS's own member and its own MAX rather than
   arriving as a third field or a third operand here.
   WHAT `unproven` DOES NOT SAY IS THE WHOLE OF ITS HONESTY, and it is `EPA_CONCRETE`'s rule one grain out: it
   is NOT the claim that a parse could have stated the address. Whether a static reader could have stated one
   is a MOVING FRONTIER rather than a property — a bundler's chunk manifest needs a scope pass to resolve —
   and this list deliberately does not pretend otherwise in either direction.
   THIS PARAGRAPH USED TO NAME A POPULATION OF `unproven` THAT "really is past every parse" — a source this
   flow PINNED and re-read — AND THAT WAS AN OVER-CLAIM, retired rather than deleted because it is what a
   reader re-derives from "the taint is gone by the time the address is composed". It went on: "That half is
   bounded by solver/flow.h's `path_pinned` and is NOT folded in here, deliberately". The BOUNDING half
   stands and `EPW_MAY_REST_ON` is still never unioned in; the claim that the population is past every parse
   does not. A pin's bytes are the equality's OTHER operand, which solver/concolic.c spells only where that
   operand is NOT concolic, so they are bytes the page's own text determined — a literal the bundle spells, in
   the dominant case. `ENDPOINT_ADDRESS_CLASSES`' banner carries the derivation and the measurement; the
   consequence HERE is that the pinned population is a population `unproven` holds CORRECTLY, and the honest
   reading of this word is one floor rather than one floor plus an unquantified excuse.
   SO THE ONE POPULATION STILL UNACCOUNTED FOR IS NARROWER THAN THE RETIRED SENTENCE CLAIMED, and it is not
   about pins at all: a pin whose other operand was a non-concolic value that reached this engine from
   OFF-DOCUMENT bytes. Those bytes stand in no served document, so such an address does clear the bar and is
   not claimed here — and this engine cannot see it, because a non-concolic operand carries no provenance BY
   CONSTRUCTION. `address_class_of` records that as a named residual; `EPB_OFF_DOCUMENT` catches the reply
   DOOR independently, which is why the unseen half is a thin residue rather than the floor's main slack.
   A LIST AND NOT TWO `#define`s, and THE CONSERVATIVE MEMBER IS FIRST AND ZERO BY CONSTRUCTION, for
   `ENDPOINT_ADDRESS_CLASSES`' reasons exactly. */
#define ENDPOINT_RAZOR_CLASSES(X)                                                                   \
    /* this run proved nothing about whether a parse of the served bytes could state this          \
       address. It is not the complement of the member below over parses — what a static           \
       reader reaches is a moving frontier — and it is NOT a hole with a population inside it      \
       either: see the banner for why the pinned population it holds is held CORRECTLY, and        \
       `address_class_of` for the one thin residue that is still unaccounted for. IT NOW ALSO      \
       HOLDS EVERY `unknown-unproven` ROW, which is the population this word exists to be honest   \
       about rather than a hole it grew: an undetermined address resting on no unknown anybody      \
       outside this engine supplied is one this run proved nothing about. */                       \
    X(EPZ_UNPROVEN,     "unproven")                                                                 \
    /* …and this run PROVED the address exists only at run time: it reached it holding a value it   \
       had not determined WHICH RESTS ON AN UNKNOWN SOMEBODY OUTSIDE THIS ENGINE SUPPLIED, or the   \
       door handed it bytes that were never in the served document. The conjunct is not a           \
       narrowing of this word, it is the whole of why it is a CLAIM: an undetermined address        \
       standing on a hole this engine minted for its own drive exists only at run time and          \
       nothing was learned from it. */                                                              \
    X(EPZ_RUNTIME_ONLY, "runtime-only")

typedef enum {
#define ENDPOINT_RAZOR_CLASS_MEMBER(id, token) id,
    ENDPOINT_RAZOR_CLASSES(ENDPOINT_RAZOR_CLASS_MEMBER)
#undef ENDPOINT_RAZOR_CLASS_MEMBER
    EPZ_COUNT           /* the list's own end — dense by construction, and with NO unstated member for
                           `ENDPOINT_ADDRESS_CLASSES`' reason twice over: this is DERIVED from two fields that
                           are themselves written unconditionally, so there is no producer at all, let alone
                           one who could forget. */
} EndpointRazorClass;

/* THE ONE WIRE SPELLING OF A RAZOR CLASS, and `endpoint_address_class_token`'s severity for its reason: it
   runs once per emitted ROW in EVERY build, so a release build falling through would publish a word nothing
   decided as this engine's answer to the product's own bar. */
const char *endpoint_razor_class_token(int cls);

/* WHETHER THIS ADDRESS MAY REST ON A WITNESS THIS ENGINE CHOSE — the NECESSARY CONDITION under the one
   population `EPZ_UNPROVEN` holds whose BYTES THIS ENGINE CHOSE RATHER THAN A SERVER SENT, published as its
   own word per row because nothing anywhere published it.
   THAT PURPOSE CLAUSE USED TO READ "the one population `EPZ_UNPROVEN` holds that really is past every parse",
   and it is re-aimed rather than deleted for the reason `ENDPOINT_ADDRESS_CLASSES`' banner gives at length: a
   pin's bytes are a literal the page's own predicate spelled, so that population is NOT past a parse and this
   class was never a bound on the bar's slack. WHAT IT BOUNDS IS REPRODUCIBILITY, which is what its own member
   comment below already says and is a first-class §@H fact: an address resting on bytes THIS ENGINE chose is
   one a real session may never see, so a reader who takes such a row for an observation is taking the
   invention §@H forbids. The class, its order, its MAX merge and its refusal to be unioned into the bar are
   all unchanged — only the sentence saying what the population IS.
   IT IS NOT THE BAR AND IS NEVER UNIONED INTO IT, which is the whole reason it is a third list rather than a
   third operand of `endpoint_razor_class_of`. The razor's own banner states the hazard and the reason, and
   this is that reason obeyed rather than repeated: `may-rest-on` is a MAY, so folding it in would turn a FLOOR
   into an OVER-claim — the same defect as unioning over `EPR_BEYOND` one operand back, which CLAUDE.md
   §a-widening-in-the-coverage-gaining-direction prices in both directions.
   WHAT IT IS FOR, AND IT IS A DECISION BETWEEN TWO OPPOSITE WORK PROGRAMMES RATHER THAN A COLUMN. The razor
   reads `unproven` for every address a run DETERMINED, and `address_class_of` asks `concolic_is`, which
   CONCRETIZE-ON-PIN makes answer FALSE for a source this flow pinned and re-read — `pin_mint` hands a pinned
   read back as a BARE primitive, so the taint is gone by the time the address is composed. A surface of
   `concrete` rows is therefore consistent with an engine that genuinely computed every address (in which case
   the SOLVER must keep more values unknown) AND with one whose pin erased a taint the bar should have seen (in
   which case the pin owes the address a provenance it can still carry). Those take opposite diffs, and no
   figure published before this list could tell them apart.
   THE CONSERVATIVE MEMBER IS FIRST AND IS ZERO BY CONSTRUCTION, for `ENDPOINT_DOOR_BYTES`' reason exactly —
   and here the member that proves nothing is the one for THE QUESTION NOT HAVING BEEN ASKED, because the
   accessor that answers it is only answerable while a flow stands. A mint that forgets to ask reads as having
   asked nothing, which is the direction CLAUDE.md §AN-UNDER-CLAIM-IS-NOT-FOUND-BY-ACTING-ON-IT rates as the
   invisible one.
   THE ORDER ENCODES HOW MUCH IS CLAIMED, AND THAT IS WHAT MAKES THE MERGE A `MAX`. A record merges sightings,
   and the two directions are not alike: a sighting that ASKED outranks one that could not (a real reading
   beats a hole), and a sighting whose path HAD chosen a witness cannot be taken back by a later one that had
   not — which is `addr_class`' own union rule stated over a list of its own length. So the merge is
   `if (new > old) old = new` and the list's order is load-bearing; it is asserted at the walk. */
#define ENDPOINT_WITNESS_CLASSES(X)                                                                 \
    /* no flow stood when this address was recorded, so the question was not asked. It proves        \
       NOTHING either way and is zero on any run whose every record is minted inside a flow — a      \
       nonzero row here is a door composing a request outside the scheduler, which is a finding      \
       about that door and not about the address. */                                                 \
    X(EPW_UNASKED,     "unasked")                                                                   \
    /* a flow stood and its path had chosen no witness: nothing this run pinned can be under this    \
       address, so `unproven` here really is the run having proved nothing. */                       \
    X(EPW_NO_WITNESS,  "no-witness")                                                                \
    /* a flow stood whose path HAD determined a source's value on an arm its own concrete example    \
       contradicted (solver/flow.h's `path_pinned`). The address MAY rest on bytes this engine        \
       chose rather than on bytes a server sent — a MAY and never a bar, because the pin is a fact   \
       about the PATH and this row does not say the address read that source at all. */              \
    X(EPW_MAY_REST_ON, "may-rest-on")

typedef enum {
#define ENDPOINT_WITNESS_CLASS_MEMBER(id, token) id,
    ENDPOINT_WITNESS_CLASSES(ENDPOINT_WITNESS_CLASS_MEMBER)
#undef ENDPOINT_WITNESS_CLASS_MEMBER
    EPW_COUNT           /* the list's own end — dense by construction, for `EPR_COUNT`'s reason, and ORDERED,
                           which this one is not free to be arbitrary about: the merge is a MAX over it. */
} EndpointWitnessClass;

/* THE ONE WIRE SPELLING OF A WITNESS CLASS, and `endpoint_address_class_token`'s severity for its reason: it
   runs once per emitted ROW and once per census row in EVERY build, so a release build falling through would
   put whatever the register held into a JSON string. */
const char *endpoint_witness_class_token(int cls);

/* THE EMITTED SURFACE PARTITIONED BY IT, as a malloc'd JSON OBJECT (caller frees) — one row per member,
   zeroes included, summing to the same `emitted` figure the other three walks sum to, for the reason stated
   at `endpoint_address_hist_json`: an `unasked` that is ABSENT and one that read 0 are the two facts a reader
   most needs kept apart, the first being an instrument that stopped writing the field. */
char *endpoint_witness_hist_json(void);

/* …AND THE UNION ITSELF, COMPOSED WHERE BOTH FACTS ARE IN ONE HAND. It takes the two fields rather than a
   record because that is what makes it a function of exactly what the wire carries: a consumer holding a row
   can check this answer against the row's own `door` and `addressClass`, which is the one property of a
   derived verdict a reader can verify without re-deriving the mechanism. */
int         endpoint_razor_class_of(int door, int addr_class);

/* THE EMITTED SURFACE PARTITIONED BY THAT CLASS, as a malloc'd JSON OBJECT (caller frees) — one row per
   member of `ENDPOINT_ADDRESS_CLASSES`, zeroes included, summing to the same `emitted` figure
   endpoint_surface_census reports, with the identity asserted at the composer where both sides are in one
   hand.
   THIS IS THE ONE ROW THE HARD BAR CAN BE SCORED OFF, and it is a SECOND OBSERVATION rather than a
   coarsening of either histogram beside it — which `endpoint_reach_hist_json` is not, and says so. The
   door and the reach classes are one fact at two grains; this is keyed on a property of the ADDRESS VALUE
   that no door implies in either direction, so a reader holding all three holds two observations and not
   three (CLAUDE.md §EVIDENCE-INFLATION, and the reason the derivation is named here rather than left to be
   noticed).
   IT IS A DIAGNOSTIC AND NEVER A TARGET, on §netdiff's terms and in the words `endpoint_reach_hist_json`
   already carries: `unknown` 0 against a nonzero `emitted` is a REFUSAL TO CLAIM the hard bar on this
   document, not a smaller version of it, and it is an identity read WITHIN one run rather than across two of
   a wall-denominated quantum. Its DENOMINATOR is `epEmitted` on the same census line and is not this row's
   to omit. */
char   *endpoint_address_hist_json(void);

/* THE EMITTED SURFACE PARTITIONED BY DOOR, as a malloc'd JSON OBJECT (caller frees) — one row per member of
   `ENDPOINT_DOORS`, zeroes included, summing to the `emitted` figure endpoint_surface_census reports. It is
   what makes CLAUDE.md §What-the-tool-produces' razor a PARTITION rather than a subtraction of two totals;
   the identity that says so is asserted at the composer, where both sides are in one hand. */
char   *endpoint_door_hist_json(void);

/* …AND THE SAME SURFACE PARTITIONED BY WHAT A PARSE OF THE DOCUMENT WOULD HAVE REACHED, as a malloc'd JSON
   OBJECT (caller frees) — one row per member of `ENDPOINT_REACHES`, zeroes included, summing to the same
   `emitted` figure. IT IS THE PARTITION CLAUDE.md §What-the-tool-produces DEMOTED AND IT IS NOT THAT
   SECTION'S BAR — this line read "CLAUDE.md §What-the-tool-produces' razor STATED" until the bar became a
   column, and the retired sentence is kept because a reader holding a row named `beyond` re-derives it. The
   bar is "AN ADDRESS, A KEY OR A VALUE NO PARSE OF THE SERVED BYTES CAN STATE", and `endpoint_razor_class_of`
   composes it PER ROW out of `addressClass` and the door list's FOURTH column; this row is keyed on the THIRD,
   which is what a MARKUP parse reaches — so `beyond` holds a literal chunk URL through `module-import`, which
   that same section names by hand as scoring ZERO on the bar. ONE HEADER WAS GIVING TWO ANSWERS ABOUT WHICH
   ROW IS THE RAZOR, which is the defect it already records one door over, and the cost was not a reading: the
   residual below inherited the wrong answer and priced its own gap against the bar rather than against this
   diagnostic. `endpoint_door_hist_json` is the raw material this row is computed from, and NEITHER OF THEM IS
   THE BAR.
   IT IS ONE FACT AT TWO GRAINS AND NOT TWO FACTS, WHICH A READER COUNTING ZEROES MUST BE TOLD. Every count
   here is the sum of `endpoint_door_hist_json`'s counts for the doors of that class, so the two histograms do
   not corroborate each other and a reader holding both holds ONE observation — CLAUDE.md
   §EVIDENCE-INFLATION, and the reason the derivation is named here rather than left to be noticed. What it CARRIES that the door row does not is the map: which
   doors a markup parse reaches, which is stated in exactly one place (the third column of `ENDPOINT_DOORS`)
   and which no consumer of the emitted surface can re-derive.
   IT IS A DIAGNOSTIC AND NEVER A TARGET, on §netdiff's own terms and in CLAUDE.md's own words: a zero in
   `beyond` is a REFUSAL TO CLAIM the capability on this document, not a smaller version of it. Optimising
   toward it optimises the instrument. It is an IDENTITY read WITHIN one run — `beyond` 0 against a nonzero
   `emitted` is the whole finding — and it is not comparable across two runs of a wall-denominated quantum.
   THE DENOMINATOR TRAVELS WITH IT AND IS NOT THIS ROW'S TO OMIT: `epEmitted` is on the same census line and
   the sum identity is asserted at the composer, so a share read off this row is over the population the row
   is a partition of. A bare `beyond` count is the defect
   CLAUDE.md §a-coverage-figure-states-what-it-is-a-fraction-of is about.
   NOT A RESIDUAL BUT A DECISION, AND THE RESIDUAL THAT STOOD HERE IS RECORDED RATHER THAN DELETED BECAUSE
   ITS REASONING IS WHAT A READER RE-DERIVES. `EPR_EITHER` says the three doors it holds are reached by a
   parser-inserted element AND by a script-created one and that the door records neither, so every row through
   them lands in `either`. THAT MUCH IS TRUE AND IS THE RIGHT ANSWER. What was wrong was the remedy the record
   went on to name: "WHAT THE NEXT DIFF BUILDS: the element's own insertion recorded ON THE RECORD at the mint,
   the way `pre_program` already is, so `endpoint_door_reach` becomes a function of the door AND that flag",
   on the ground that "It is a fact the three markup recorders have in hand and do not carry".
   THE MECHANISM HALF WAS FALSE AND ITS OWN TWO CITATIONS REFUTE IT IN THE SAME SENTENCE — it cited
   core/html/html_form.c as saying "this engine has no parser-inserted association to" unset and
   core/html/html_link.c as carrying no record of how the element arrived, which is a pair of recorders NOT
   having the fact, offered as evidence that they have it. The SPEC reasoning above it was sound, which is the
   split CLAUDE.md rates worst, so what a later reader must not inherit is the METHOD: the clause was reasoned
   from WHAT THE RECORDERS PARSE rather than from what any algorithm WRITES, and reasoning that way produces
   the same clause again.
   AND THE FACT IT NAMED DOES NOT EXIST FOR TWO OF THE THREE DOORS, WHICH IS A QUESTION ABOUT THE STANDARD
   AND NOT ABOUT THIS TREE. HTML §13.2.6.1 "Creating and inserting nodes"' create an element for a token
   sets exactly two marks of this kind and neither is an element-insertion record: the FORM one, only where it
   "associate[s] element with the form element pointed to by the form element pointer and set[s] element's
   parser inserted flag", which §4.10.18.3 "Association of controls and forms" declares as "A
   form-associated element has an associated parser inserted flag" — a flag on form-associated CONTROLS about
   FORM OWNERSHIP, which `EPD_FORM_SUBMIT` (the `<form action>`) is not keyed on at all; and the SCRIPT one,
   §4.12.1.1 "Processing model"'s "A script element has a parser document, which is either null or a
   Document, initially null. It is set by the HTML parser and the XML parser on script elements they insert",
   whose non-null case "script elements with non-null parser documents are known as parser-inserted" is the
   mark core/html/html_script.h already carries. §4.2.4.3 "Fetching and processing a resource from a link
   element"'s page and §4.8.4.3.5 "Updating the image data"'s page carry the phrase ZERO times each
   (positive controls 37 and 30 on the same fetched bytes). So for `link-element` and `image-element` the clause
   asked for a flag no standard defines, and building it would put an INVENTED datum under the one row that
   states this product's own bar.
   AND IT WOULD HAVE BEEN THE WRONG OPERAND EVEN IF IT EXISTED, WHICH IS THE SAME MISTAKE endpoint.c'S RETIRED
   RESIDUAL ALREADY RECORDS ONE STEP LATER. `endpoint_razor_class_of` is the bar and reads `addressClass` and
   `endpoint_door_bytes`; it does not read `endpoint_door_reach` at ALL, so narrowing `either` narrows the
   DIAGNOSTIC this banner is about and moves the bar by nothing. The record priced its gap as "the razor is
   published as a FLOOR with a ceiling" because the banner above it called this row the razor, which it no
   longer does.
   AND PARSER-INSERTED IS NOT WHAT THE BAR ASKS ANYWAY, WHICH THIS HEADER ALREADY SAYS ONE DOOR OVER:
   `EPD_DOCUMENT_SCRIPT` records that "a `<script src>` a `document.write` put in the tree is parser-inserted
   and is NOT in the served bytes", and §13.2.6.1 confirms the shape from the standard's side — the mark is
   set on elements the parser INSERTS, which is why §4.12.1.1's flag needed a Fragment carve-out and why a
   fragment-parsed element would read parser-inserted too. The mark is therefore decisive in ONE direction only
   (stated NOT-parser-inserted proves beyond a parse) and undecidable in the other, so it could never have made
   `either` a value.
   AND THE LOSS THE RECORD CLAIMED IS ALREADY ABSENT AT TWO OF THE THREE DOORS. Its worked example was that "a
   router-built `<link>` really is beyond a parse and this cannot say so", and core/html/html_link.c records
   the `href`'s TAINT as the address where one exists — its own comment says "Lexbor would have ToString'd the
   taint away" — so `address_class_of` answers `EPA_UNKNOWN` and the bar answers `runtime-only` for that row
   today, with core/html/html_image.c doing the same for its undecided source set. The ambiguity `either` names
   is real and is bounded to a diagnostic CLAUDE.md §netdiff says is never a target.
   NAMED RESIDUAL, ON THE AXIS THE BAR ACTUALLY READS AND AT THE ONE DOOR WHERE IT IS OPEN. NOT COVERED: a
   `<form action>` this run proved was composed out of an unknown reaches the bar as `concrete`.
   core/html/html_form.c reads that attribute's taint at §4.10.22.3 "Form submission algorithm" step 12 and
   spends it on the @S sink alone (`solve_url_sink`), then `form_record_request` records `url_serialize`'d
   bytes — so the two sibling doors carry an unknown into `addressClass` and this one does not.
   WHERE THE UNKNOWN GOES, WHICH IS A DATA-FLOW FACT AND WAS READ RATHER THAN INFERRED: core/dom/element.c's
   `el_attr_value` writes a concolic attribute value's SHAPE into Lexbor and the concolic into the taint
   shadow ("NEVER ToString a concolic to get bytes"), so the unknown survives into the tree AS BRACE TEXT; a
   braced path or query is ordinary URL text, so step 14's `url_parse` SUCCEEDS and `parsed` carries the
   braces; and the taint itself is read into a local at step 12 and stored on no step state, so it is gone by
   the recorder. NEITHER HALF DIES AT THE PARSE — the shape survives it and the taint never reaches it.
   AND THE REMEDY THIS RECORD FIRST NAMED WAS A REGRESSION, KEPT HERE BECAUSE IT IS WHAT A READER RE-DERIVES
   FROM THE SIBLING DOOR. It said: "WHAT THE NEXT DIFF BUILDS: the taint recorded as the address at
   core/html/html_form.c's submission recorder where the action carries one, which is what
   core/html/html_link.c already does at its own door and needs no new field here." BOTH HALVES ARE FALSE.
   §4.10.22.3's cells MUTATE the parsed action — `CELL_MUTATE_ACTION_URL` and `CELL_ENTITY_BODY` REPLACE its
   query with the serialized entry list — so recording the bare taint would discard the PARAMETERS, which is
   what an @H record is for and is strictly worse than the class it would buy. For a `<link>` the `href` IS
   the address, and a form's address is the action PLUS its entries; the analogy was the whole error, and it
   is the shape CLAUDE.md names as reasoning from where the code SITS rather than from what it COSTS. The
   second half is false because `endpoint_record` has no parameter by which a caller can state the class:
   `addr_class` is derived once, from the url alone, by `address_class_of`.
   AND THE NEXT REMEDY A READER REACHES FOR IS UNSOUND, WHICH IS WHY IT IS NAMED HERE RATHER THAN LEFT TO BE
   TRIED. This record already publishes, PER PARAM, the fact the address row lacks: `valueClass` is
   `has_hole`, merged as a union, and the razor banner above calls that merge "`valueClass`'s rule one grain
   out" — so one row can carry `valueClass` `unknown` beside `addressClass` `concrete` about the same
   address, which is this header's two-answers defect arriving in DATA instead of in prose. Unioning them
   would OVER-CLAIM the floor: `concolic_hole_key` is `if (!shape || !strchr(shape, '{')) return NULL`, a bare
   brace test that answers for a literal `{"a":1}` a page composed out of constants, and a FLOOR may not be
   wrong in the direction that credits the bar. The same argument rules out reading braces off the address.
   SO WHAT IS OWED IS A DECOMPOSITION AND NOT A LINE, AND IT IS NAMED RATHER THAN ATTEMPTED: the recorded
   address would have to BE a concolic composed from the action's taint and the entries, and the composition
   runs through `url_parse` and `url_serialize` over `const char *`, outside the concolic machinery entirely.
   HOW ITS ABSENCE SHOWS: a document whose forms submit to script-composed actions publishes `form-submit`
   rows whose `razorClass` is `unproven` beside their own `valueClass` `unknown`, while the same document's
   `link-element` rows prove `runtime-only` — an under-claim on the bar, which is the direction nobody
   discovers by acting on it. RETIREMENT: this record goes when an address composed out of a value this run
   did not determine cannot be recorded as a plain string, because the door has then no arm left in which to
   lose the class. */
char   *endpoint_reach_hist_json(void);

/* …AND THE SAME SURFACE PARTITIONED BY THE BAR ITSELF, as a malloc'd JSON OBJECT (caller frees) — one row per
   member of `ENDPOINT_RAZOR_CLASSES`, zeroes included, summing to the same `emitted` figure
   endpoint_surface_census reports, with the identity asserted at the composer where both sides are in one
   hand. Every value comes off `endpoint_razor_class_of`, which is the UNION CLAUDE.md
   §What-the-tool-produces' bar names, so this is the one census row that states the bar rather than one of
   its operands.
   IT EXISTS BECAUSE THE TWO ROWS ABOVE ARE MARGINALS AND A UNION IS A STATEMENT ABOUT PER-ROW MEMBERSHIP.
   `endpoint_address_hist_json` counts addresses by whether the run had determined them and whose unknown
   they rest on, and `endpoint_reach_hist_json` counts what a MARKUP parse reaches, and NEITHER carries the other's fact for any row — so a census reader
   holding both had no way to reach the bar and the nearest thing to one was keyed on the DOOR. That is not a
   coarsening anybody could perform afterwards: a row is `runtime-only` if EITHER operand says so, and a
   marginal tells you how many rows each operand accounts for and nothing about their overlap.
   IT IS A FOURTH WALK OVER `g_eps` AND NOT A SUM OF THE TWO TABLES, for the reason endpoint.c gives for the
   third — the walks carry the SAME `is_asset` skip precisely so that one of them drifting is a LOUD identity
   failure, and a table summed from another is true by construction of it and blind to the walk it is
   supposed to be checking. A sum would additionally be ARITHMETICALLY WRONG here rather than merely weak,
   because the two operands OVERLAP: a `reply-chunk` row whose address the run had not determined is counted
   by both marginals and is ONE row of this partition.
   IT IS A CROSS-CHECK AGAINST THE PER-ROW COLUMN AND NEVER A SUM WITH IT. The emitted @H array carries
   `razorClass` on every row from the same `endpoint_razor_class_of`, and this census is the same union at the
   population grain — so a reader holding both holds ONE observation twice (CLAUDE.md §EVIDENCE-INFLATION),
   and the two are worth having because they are taken at two INSTANTS over a GAUGE: `epEmitted` falls when an
   asset verdict lands between two censuses, so a disagreement is a real finding about which records each
   document was describing rather than an error in either.
   IT IS A FLOOR AND A DIAGNOSTIC, in the words both rows above already carry: `runtime-only` is 0 by
   construction on a document that composed every address out of constants, and that 0 is a REFUSAL TO CLAIM
   the bar rather than a smaller version of it; `unproven` claims nothing whatever about a parse, for the
   reason `endpoint_address_hist_json`'s `concrete` does not. Its DENOMINATOR is `epEmitted` on the same
   census line and is not this row's to omit. */
char   *endpoint_razor_hist_json(void);

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
   path it is about is standing; a reply-learned address states the grade of the REPLY that named it
   (solver/reply_decode.h), which is a fact about the REQUEST that reply answers and never about whoever is
   standing when the bytes land. THAT REASON USED TO READ "which no flow can answer because that path runs
   outside every flow", and it is rewritten rather than deleted because a reader who re-derives it from the
   `engine_provide` door will re-derive the word `every`: that door does run outside every flow, and
   core/xhr/xml_http_request.c's does not — it runs INSIDE the flow that sent, on a later turn than the one
   §3.5.6 "The send() method" step 6 composed the request on, and carries the grade it took at that step
   rather than asking the path it is standing on. The rule is the same at both and only the sharper half of
   the reason was transport-specific. A default here would be `observed` by the numbering, on a record nobody
   graded. */
/* `door` IS WHICH MECHANISM COMPOSED THE ADDRESS — one of the EPD_* above, stated by the producer because
   it is the one fact about a sighting no consumer of this surface can re-derive. See the enum for why it is
   neither `prov` nor the program-state flag, and why it is not part of the endpoint's identity. */
void    endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                        const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body, int prov, int door);

/* The @H surface as a malloc'd JSON ARRAY (caller frees) — findings are C data, so the emit is C, never a
   JS-object round-trip.
   `[ {"method":..,"url":..,"provenance":"observed"|"derived"|"forced",
      "door":"document-script"|…|"reply-chunk","mintedAt":"pre-program"|"post-program",
      "addressClass":"concrete"|"source-determined"|"unknown",
      "params":[{"name":..,"location":..,"valueClass":"unknown"|"concrete","validValues":[..],"excludes":[..],
      "bounds":{"minimum"|"exclusiveMinimum":N,"maximum"|"exclusiveMaximum":N},
      "predicates":[{"method":..,"arguments":[..],"holds":true|false}],
      "looselyEquals":[{"value":..,"type":..}]}]}, ... ]`.
   `addressClass` IS WHETHER THE RUN HAD DETERMINED THIS ADDRESS WHEN IT RECORDED IT, and it is the one field
   on this record that answers CLAUDE.md §What-the-tool-produces' HARD BAR at the grain the bar is stated at.
   It is ALWAYS PRESENT for `provenance`'s reason and `valueClass`'s: the words are exhaustive over the
   states this engine can be in about an address value, so there is no absence to read as a statement. See
   `ENDPOINT_ADDRESS_CLASSES` for what each word claims, for the two populations `concrete` still hides, and
   for why the honest field is a FLOOR under the bar rather than a boolean about what a parse could reach — a
   consumer that read `concrete` as `a static reader gets this for free` would be making a claim no producer
   here makes.
   THREE WORDS AND NOT TWO, AND THE BAR IS KEYED ON ONE OF THEM RATHER THAN ON A COMPLEMENT. A consumer asks
   Q1 (did the run determine this address) as `!= "unknown"` and Q2 (does this row clear the hard bar) as
   `== "unknown"`, which is what `razorClass` already composes; `"source-determined"` answers Q1 DETERMINED
   and Q2 PROVES NOTHING, so a consumer reading the bar as `!= "concrete"` over-claims on exactly it. A
   consumer that knows only the two older words still reads the BAR correctly — `unknown` is unchanged and is
   still the bar's address-side operand — and loses only the split inside the determined side, which is why
   this widening needed no consumer to land with it.
   IT IS STILL THE SAME VOCABULARY AS `valueClass` ONE GRAIN OUT for the two words they share, and `valueClass`
   deliberately does NOT gain the third: a param's value has no pinned-and-still-held population to carve out,
   so the pair means one thing at two grains wherever both spell it.
   A CONSUMER CANNOT RE-DERIVE IT FROM `url`, which is why it is carried. A concolic address is printed as
   its SHAPE, so a brace in `url` looks like the evidence — and extension/lib/learn.js's live-traffic path
   walk mints a `{path_*}` hole into a URL from two observed addresses that differ at one segment, which is a
   brace no concolic put there and which arrives on rows this key reads `concrete` for.
   Every param states WHERE IT LANDED — "path", "query" or "body" — because that is what the reviewer replays
   it with, and because a consumer that has to default the field cannot tell an unknown from a query param.
   AND EVERY PARAM STATES WHETHER ITS VALUE EVER NAMED A HOLE. `valueClass` is "unknown" where some observed
   path minted this param from a value the code did NOT compute, and "concrete" where every one of them minted
   it from a literal. It is ALWAYS PRESENT for `provenance`'s reason one level in: the two words are
   exhaustive, so there is no absence to read as a statement, and this record already spells "unobserved" as a
   silence four fields down.
   IT IS WHAT MAKES THOSE FOUR SILENCES READABLE AT ALL, and that is the whole of why it is on the record.
   Each of `excludes`, `bounds`, `predicates` and `looselyEquals` is OMITTED where no such claim survived every
   observed path, and endpoint.c reads all four through the param's HOLE KEY — so where there was no hole, all
   four were skipped at the mint and their absence means something else entirely. "No equality gate over this
   hole took its false arm on every path" presupposes a hole; "this param is a literal and there was never
   anything to look up" is the other reading, and the two take OPPOSITE work — the first wants more gates
   observed and the second wants nothing, however good the solver gets. Without this key they render with
   identical bytes, so a consumer counting parameters whose domain could have been narrowed is counting a
   population it cannot name, and its zero is consistent with a page whose every forced-execution param was a
   concrete query pair.
   A CONSUMER CANNOT RE-DERIVE IT FROM `validValues`, which is why it is carried. endpoint.c's query scan
   emits the ALIGNED EXAMPLE where the shape held a hole, so a param minted from `{location.hash}` renders a
   computed-looking literal — the one spelling that would have betrayed the hole is exactly the one the
   example replaced.
   A `location:"path"` PARAM IS ALWAYS "unknown" and endpoint.c's `kv_add` asserts it: the path scan mints a
   param only for a braced segment and passes that segment's brace-stripped name AS the hole key. That
   invariant used to be re-derived by readers of this file — testing/corpus/site.mjs reasoned it out to build
   a denominator it could trust — and it is now a fact the record states and the engine crashes on.
   ITS MERGE IS A UNION AND NOT THE INTERSECTION THE FOUR DOMAINS TAKE. Those are claims about the VALUE, so a
   path that reached the request without obeying one disproves it; this is a fact about whether the
   OBSERVATION could ask anything, which a later concrete sighting cannot take back. The direction is also the
   only sound one: intersecting would answer "concrete" for a param one observed path did mint from a hole,
   dropping a row that genuinely could have carried a domain and making a coverage fraction read HIGHER than
   the truth.
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

/* WHAT THE SURFACE ABOVE IS A FRACTION OF, WHICH ITS LENGTH ALONE CANNOT SAY. `endpoint_json_array` SKIPS every
   record the verdict above marked, so the emitted array's length is three states wearing one number: N real
   endpoints, or N real ones out of a far larger mint whose rest were files, or N records nothing ever
   classified because no reply named a type. Those take different work — the first is a result, the second is a
   working classifier, the third is a reply door that answered without a type — and a reader of the array can
   tell them apart from nothing.
   IT IS NOT `endpoint_count` COMING BACK. That returned a bare mint total with NO CALLER, which is why it went:
   a second answer waiting for its first reader to trust it. This is a PARTITION with a reader in the diff that
   added it (solver/result.c's census composes `epMinted`/`epAssets`/`epEmitted`) and an identity asserted where
   all three are in one hand, so the total cannot move without one of its parts moving — which is the one
   property that makes a count readable at all.
   THE EMITTED FIGURE IS DERIVED BY THE SAME SKIP THE EMIT PERFORMS rather than by subtracting, so a record kind
   that stops being written cannot make the two disagree silently.
   RETIREMENT: this record goes when the emitted array carries its own denominator, because the partition is
   then a property of the document rather than a row beside it.

   …AND THE FOURTH NUMBER, WHICH IS ABOUT WHO COMPOSED THE ADDRESS RATHER THAN WHAT THE RESOURCE TURNED OUT TO
   BE. The three rows above partition the surface by the REPLY — a file or not a file — and say nothing
   whatever about whether the page's own code had run when a record was born. `pre_program` is how many of the
   EMITTED records were minted while this instance had started no program at all, so `emitted - preProgram` is
   the largest number of addresses forced execution could have contributed, and it is the product's headline
   claim stated as a figure a reader of one run can check.
   WHY IT IS A ROW AND NOT A COUNT OF `prov`. Every record already carries a provenance and a reader can count
   them; on a real page that count answers the wrong question, and answers it in the flattering direction.
   `observed` requires HTML §4.12.1.1 "Processing model"'s `parser document`, which only the PARK register
   holds, so every subresource a browser algorithm records — core/html/html_link.c's stylesheets and preloads,
   core/html/html_image.c's candidates — is graded through `engine_prov_of_running_path`, whose own
   declaration states that it can never answer `observed`. Those records land on `derived`, whose definition is
   that running code COMPUTED the address. A document whose whole surface is its own markup therefore publishes
   a surface of `derived` rows, and a reader counting them counts addresses that were read out of the `<head>`.
   §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS is that shape exactly, and its cure is the one taken here: the
   provenance is left alone, because it is right for the firing policy that reads it, and the REPORTING
   question gets its own predicate over a fact the engine already computes.
   IT IS A CEILING ON THE CONTRIBUTION AND NOT A MEASURE OF IT, which is the one thing a reader must not
   over-read. A record minted after the first program started is not thereby code-composed — a `<link>` the
   parser inventoried is served by the first flow, and a document whose markup is walked late would mint markup
   addresses on the far side of the boundary — so the complement BOUNDS what forced execution can have
   contributed and never states it. The direction is the honest one: it cannot report a contribution that did
   not happen.
   NAMED RESIDUAL — CORRECT AND NARROWER. WHAT IS NOT COVERED: the markup of a document that is not the one
   this instance opened over. The boundary is per INSTANCE, and a CHILD navigable's Document is parsed and its
   `<link>` inventory served long after the root has started programs — so every subresource a child's own
   markup names is minted on the post-program side while being markup, exactly like the root's own would have
   been had it arrived later. Those records widen the complement and none of them is an address running code
   composed. WHAT THE NEXT DIFF BUILDS: the boundary taken per DOCUMENT rather than per instance — an
   any-program-started bit carried on the Document the record's realm belongs to, raised where that document's
   first program starts, so a child's markup grades against its own parse and not against the root's progress.
   HOW ITS ABSENCE WOULD SHOW: a page whose subresources are mostly its children's reports a complement that
   grows with the number of child navigables it creates rather than with anything its code computed, and it
   does so while the array beside the number holds nothing but files.
   FOUR NUMBERS AND NOT A FIFTH CALL: they are one walk over one array and a second accessor would be a second
   instant, which is the two-instants collapse §Testing names. */
void    endpoint_surface_census(long *minted, long *assets, long *emitted, long *pre_program);

/* WHAT THE CENSUS ABOVE COUNTS IS AN OUTCOME, AND THIS IS THE ASK. Every figure that function produces comes
   off a walk of the RECORD ARRAY, so each one says what LANDED — and CLAUDE.md §AN-INVARIANT-OVER-A-GATED-
   OPERATION names the cost of that exactly: a census of what landed cannot tell a component that never asked
   from one whose ask a gate correctly refused, and the repair is to record at the CALL rather than to relocate
   the outcome. endpoint_record is the ONE door every HTTP-shaped edge in this engine passes through (see this
   header's funnel note above and the derivation beside it), so a count taken at its entry, BEFORE the
   suppression gate, is the number of times this engine's execution COMPOSED a request — every field of it,
   through every step its standard states — and offered the address to this surface.
   THAT SENTENCE READ `the number of times this engine's execution REACHED a network call site`, AND THE
   CONCLUSION DOES NOT FOLLOW FROM THE PREMISE ABOVE IT. Being the ONLY door says every request that is
   RECORDED passes here; it says nothing whatever about a request that was STARTED and never recorded, and the
   two are different populations. What stands between a page's call and this door is the whole of the host
   edge's own algorithm: core/fetch/fetch.c's machine records in `FETCH_CALL`, its SIXTH stage, so Fetch
   §5.4's steps 10-27, steps 32-33 and steps 35-39 and §5.6 step 4's already-aborted signal all stand in front
   of it — each of them a TypeError the standard states, and two of them stages that run the page's own code
   and can therefore PARK and never be resumed. A `fetch()` the page called and the engine threw out of, or
   parked inside and never came back to, is a network call site REACHED and is in none of these five rows.
   THE COST OF THE WRONG READING IS THE ARM NAMED `NEVER REACHED` BELOW, which inherits it: `asks ==
   preProgram` says no post-program request was fully COMPOSED, and a reader who takes it for `no arm arrived
   at a network call site` looks for the defect upstream of every host edge when it may be inside one.
   ITS RESIDUAL IS RETIRED IN HALF AND THE ARGUMENT IS KEPT, WHICH IS WHAT A REPLACEMENT OWES. It named the
   host edge's OWN entry as not covered, and named the next diff as a count raised at each script-API edge's
   one-time capture — core/fetch/fetch.c's `!s->captured` arm and core/xhr/xml_http_request.c's `send()`. The
   FETCH half is built and is the `epFetch*` block at the foot of this header; the XHR half is not, and what
   is written below is the residual for that half alone.
   THE TRAP THE RETIRED CLAUSE NAMED WAS EXACT AND IS WHY THE BUILT ROWS HAVE THE SHAPE THEY DO, so it is
   restated rather than dropped: a step state is BYTE-COPIED at a deep fork and the copy inherits the capture
   flag, so one `fetch()` whose `input` ToString forks composes TWO requests against ONE capture and
   `composed <= called` is FALSE — an assert on it would fire on a legitimate state, which is the concession
   shape §Offensive-programming refuses. The sound pairing it named, per STATE rather than per call, is what
   landed: a raise at the capture, a raise at the teardown, and the teardowns that offered nothing partitioned
   by the stage each stood at.
   ITS XHR RESIDUAL IS RETIRED BY THE `epXhr*` BLOCK AT THE FOOT OF THIS HEADER, AND ITS NEXT-DIFF CLAUSE
   NAMED THE WRONG MACHINE — recorded HERE, where the clause was written, because a remedy clause is read once
   by somebody who has already decided to do the work and a wrong one is therefore not caught but EXECUTED.
   It read: `XMLHttpRequest`. Its own machine records at `XR_FETCH`, the FIRST stage it has, so the gap
   between a page's `send()` and this door is structurally smaller there than at `fetch` … WHAT THE NEXT DIFF
   BUILDS: the same three calls in core/xhr/xml_http_request.c, keyed on THAT MACHINE's own step labels.
   THE PREMISE IS TRUE AND THE CONCLUSION IS ABOUT A DIFFERENT MACHINE. `js_xhr_run_steps[0]` IS `XR_FETCH` —
   that X-list takes no IDL_STEP_STAGE_BASE, so the lifecycle machine owns all of its stages and numbers them
   from zero — and the lifecycle machine is not the one a page's `send()` enters. `send()` is its OWN declared
   member (XHR_SEND_DECL over SEND_STAGES, based at IDL_STEP_FIRST) with SEVEN stages, and it MINTS the
   lifecycle machine at the last of them. Between the page's call and this door stand SEND_CHECKS (a TypeError
   and two InvalidStateErrors, and a DECLARED FORK on a concolic method, which PARKS), SEND_BODY_STR (the
   page's own `toString`, which PARKS), SEND_BODY (an extraction that can fail), SEND_FLAGS, SEND_LOADSTART
   and SEND_UPLOAD_LOADSTART (the page's own listeners, both of which PARK, each carrying §3.5.6 step 12.6's
   early return for a listener that aborted or reopened), and then SEND_RUN's task hop. SEVEN stages and FOUR
   page-code park points against core/fetch's SIX and its record at the sixth — so the gap is LARGER, not
   smaller, and larger again if `open()` is counted, which is where the method and the URL are parsed at all.
   THE ERROR IS WORTH MORE THAN THE CLAUSE: the author reasoned from WHERE THE `endpoint_record` CALL SITS
   rather than from WHAT THE CONSTRUCTION COSTS, and the two machines share a FILE — which is the strongest
   thing there is for making one look like the other.
   WHAT BUILDING THE CLAUSE WOULD HAVE COST, which is why this is a refutation and not a wording repair.
   `XR_FETCH` runs `xhr_record_endpoint` UNCONDITIONALLY at its top and does not park before it, so on the
   lifecycle machine a `began` row and an `offered` row could not differ except for a closure enqueued and
   never stepped and for the XHR_MODE_ERROR machines abort() and the request error steps mint — two rows that
   cannot disagree, which is a non-check wearing a census's shape — and the stage histogram would be over
   twenty stages that are all DOWNSTREAM of the record, answering where the RESPONSE LIFECYCLE died and never
   where the CONSTRUCTION did. It would have been structurally blind to the whole population the paragraph
   above names as this census's reason for existing: a call the page MADE that the engine threw out of, or
   parked inside and never resumed.
   WHAT LANDED INSTEAD is that census over `send()`'s OWN machine — which is where an XHR request is
   constructed and where it can die — plus ONE row raised at the lifecycle machine's door, so the edge's share
   of `epAsks` is still readable. The clause's other two demands were right and are obeyed: the rows are NOT
   summed with the `epFetch*` ones and they do NOT go through the same per-stage array.
   WHAT IT SEPARATES, WHICH IS THE PRODUCT'S OWN QUESTION AND WAS UNMEASURABLE. `emitted - preProgram` is
   documented above as a CEILING on what forced execution contributed, and a ZERO there has at least two
   readings that take opposite work:
     NEVER REACHED — no arm ever arrived at a network call site, so there was nothing for the surface to learn
       and the work is upstream of this file entirely;
     REACHED AND ALREADY KNOWN — arms arrived and every address they composed was one the surface already held,
       so each ask MERGED: the record exists, its `pre_program` was decided at ITS mint and is deliberately not
       re-armed (see the struct), `g_eps_n` does not move, and all four rows above are byte-identical to the
       first case.
   `asks - preProgram` is nonzero in the second and zero in the first. That is the separation, and nothing on
   the surface could state it, because a merge is an event with no record of its own.
   IT IS NOT A THIRD READING OF THE SAME FACT. A reader who has both lines can also see the case neither has
   alone: in-program asks that MINTED, which must show up as `emitted - preProgram` moving unless the reply
   classified them as files — so the two censuses constrain each other rather than repeating each other.
   EVERY ROW IS A LIFETIME COUNT AND NONE IS A GAUGE. They may be differenced across samples and accumulated,
   they cannot decrease, and a sample below its predecessor is this instrument and not the run — stated in the
   contract because CLAUDE.md §Testing records this tree being misled by that distinction twice. Their SCOPE is
   the SURFACE's: they are reset wherever `g_eps_n` is, so they answer for the session whose records the census
   above is walking and never for the process.
   A REPORT AND NEVER A BOUND (§NO BOUNDS): nothing branches on one, no ask is refused because of one, and no
   arm is narrowed by one.
   …AND THE SIXTH, WHICH IS THE ARM NAMED `REACHED AND ALREADY KNOWN` STATED RATHER THAN BOUNDED. The
   paragraph above offers `asks - preProgram` as the separation between the two readings, and that is a CEILING
   on the second exactly as `emitted - preProgram` is a ceiling on forced execution's contribution: the
   subtraction is also nonzero for a post-program ask that MINTED, and for one that merged into a record
   post-program code had itself minted, and neither of those is running code re-composing the MARKUP's
   addresses. `merged_pre_program` is that population and only that one — the ask was made with a program
   running and the record it reached was minted before any program had, which is the one shape that leaves
   every figure on both censuses byte-identical to a run in which nothing reached a network call site at all.
   IT IS CONTAINED IN `merged` AND IN `asks - preProgram`, asserted at the accessor where every term is in one
   hand, and it is a CUT rather than a fourth arm: the three arms partition the door's exits and this selects
   inside one of them on a fact about the RECORD, so it may not be summed with them.
   READ ITS ZERO THROUGH THE ROWS BESIDE IT AND NEVER ALONE — it is four states (no post-program ask;
   suppressed; minted, which `emitted - preProgram` then shows unless the reply classified them as files; or
   merged only into post-program records), and its NONZERO is one statement. */
void    endpoint_ask_census(long *asks, long *pre_program, long *suppressed, long *merged, long *minted,
                            long *merged_pre_program);

/* THE HOST EDGE'S OWN ENTRY, WHICH IS THE POPULATION THE CENSUS ABOVE CANNOT SEE. Its residual names this
   diff by name: the five rows above are counted at endpoint_record's door, so a `fetch()` the page called and
   the engine threw out of — or parked inside and never resumed — is a network call site REACHED and is in
   none of them. These rows are the door's UPSTREAM: they count the STATES of core/fetch/fetch.c's §5.4/§5.6
   machine, at its one-time capture and at its teardown, so a run reading `asks == preProgram` can say whether
   the page called a request-composing API at all and, when it did, WHERE the construction died.
   IT IS A PARTITION AND NOT A LADDER, AND THAT IS THE FIRST THING TO READ. The stage rows below are the arms
   of ONE outcome — the stage a torn-down state was standing at — and no arm implies another: a zero in one
   stage says NOTHING about its neighbours, so `the lowest 0 is the localisation` is not a reading this
   histogram supports and never will be. What the rows DO support is a partition (they sum to the states that
   were freed) and two containments, and those three are the whole of what a reader may do arithmetic with.
   THE ROWS, AND EACH SAYS ITS KIND IN ITS OWN NAME rather than in a comment no consumer reads — `Ask` or
   `Out` for which side of §AN-INVARIANT-OVER-A-GATED-OPERATION it counts, `Life` for a LIFETIME COUNT and
   never a gauge. They are terse and camelCase because they are rows of `_cold`, whose own rows are:
     `epFetchAskCalledLife` — the calls that entered the HOSTING machine's prologue. core/idl_args.h numbers a
       declared member's stages from IDL_STEP_FIRST because stages 0 and 1 are that machine's — the argument
       count check and the ES-to-IDL conversions — and BOTH are rest points, so a call that threw or PARKED in
       them never reaches Fetch §5.4 at all. This is raised at stage 0, which needs no one-time flag because
       that block's only exits are two abrupt throws and a park leaves the stage at 1. It is the row that turns
       the next one's zero from THREE states into one: the page called nothing, the page called and the
       conversion died, or the flow never reached the call — and the first and third are `called == 0` while
       the second is `called > 0` with the next row at zero.
     `epFetchAskBeganLife` — the constructions that BEGAN. Raised at the machine's one-time capture, which is
       Fetch §5.4's first stage, so it is one per page-level `fetch()` call that reached the member body at
       all. A call whose ARGUMENT CONVERSION threw or parked is upstream of it and is in no row here; that
       population is core/idl_args.c's and is the difference between this row and the one above.
     `epFetchAskOfferedLife` — the constructions that reached §5.6 step 12 and offered an address. Raised
       on the line before the edge's own call to endpoint_record.
     `epFetchOutFreedLife` — the states TORN DOWN after §5.4 began, DEEP-FORK COPIES INCLUDED.
     `epFetchOutFreedOfferedLife` — of those, the ones that had offered an address.
     `epFetchOutDiedAtLife` — ONE ARM PER STAGE: of the freed states that offered NONE, the stage each was
       standing at, keyed by the machine's OWN label for it. The labels are `js_fetch_steps[]`, handed over
       at the declaration rather than copied here, so a stage added to that X-list adds a row and a stage
       renamed renames one: there is no second list to drift (§AN-AUDITOR-DERIVES-THE-RULE).
   THE THREE IDENTITIES, EVERY ONE ASSERTED AT THE ACCESSOR WHERE ALL ITS TERMS ARE IN ONE HAND:
     PARTITION — the stage rows plus the freed-and-offered row equal the freed row. The two sides are raised
       in two arms of one teardown, so this fires on a third arm added without classifying it, which is
       precisely how a state that dies in a new way would go missing.
     CONTAINMENT — freed-and-offered <= offered. The two are raised at DIFFERENT events, one at the teardown
       and one at the edge's record call, so the slack is a real population and not a tolerance: it is the
       states that offered an address and are STILL LIVE at the read, parked on the reply they asked for,
       which on a page mid-run is most of them.
     CONTAINMENT — offered <= the ask total above. Different events again, in different files, and the slack
       is every OTHER door into this surface: core/xhr's, the markup inventory's, the reply decoder's. It is
       what makes the fetch edge's SHARE of the @H ask population readable, and it is the only relation that
       ties this census to the razor it was built to explain.
   WHAT MAY NOT BE ASSERTED, AND THE REASON IS THE TRAP THE RESIDUAL ABOVE NAMED. `freed <= began` is FALSE
   and `offered <= began` is FALSE, both for one mechanism: a step state is BYTE-COPIED at a deep fork and the
   copy inherits the capture flag, so ONE `fetch()` whose `input` ToString forks composes TWO requests against
   ONE capture. The fork is not exotic — core/fetch/fetch.c's own `unforkable` banner names which stages permit
   it, and the `input` ToString that composes the second request is one of them — and it is exactly the
   population this tool exists for, since a forked address is an address built out of unknown external input.
   THE PERMITTED SET GREW WHEN §2.2.5's REQUEST RECORD BECAME JSValues that machine's `visit` names, which is
   why this sentence no longer counts those stages: it used to read "the two stages that permit it and both run
   the page's code", and §5.4 steps 10-27 are now forkable as well. That strengthens this refusal to assert
   rather than weakening it, and the banner is the one place the current set is stated. `began - freed` is likewise not a live
   count: it is that difference MINUS the copies, and a census taken while states are parked is taken with
   most of them live. So the begun row and the freed row are read as two facts and never subtracted.
   SCOPE IS THE FETCH EDGE AND THE ROWS SAY SO IN THEIR NAMES, and a row that averaged the two edges would
   hide which one it was about (§a-coverage-figure-states-what-it-is-a-fraction-of). THIS SENTENCE USED TO
   CARRY A REASON THAT WAS FALSE AND IS REWRITTEN RATHER THAN DELETED, because it is the reason a reader
   re-derives: it said core/xhr's edge records at `XR_FETCH`, the FIRST stage of its own machine, SO the gap
   these rows measure is structurally small there. The premise is about the LIFECYCLE machine and the
   conclusion is about `send()`, which is a SEPARATE declared member with seven stages and four page-code
   park points in front of that mint — a LARGER gap than this machine's six. The refutation is recorded in
   full at the ask census above, where the clause it defeated was written; what the two edges are is stated
   at `endpoint_xhr_edge_declare` below. Their SCOPE IN TIME is the SURFACE's: they are reset in endpoint_init and endpoint_free
   beside every other counter in this file, which is what makes them comparable with the ask rows above at all
   — the scope defect `g_boundary_spent` exists to catch is the one this placement makes unreachable.
   A REPORT AND NEVER A BOUND (§NO BOUNDS): nothing branches on one, no construction is refused because of
   one, and no arm is narrowed by one.
   THE RESIDUAL THAT STOOD HERE IS DISCHARGED BY `epFetchAskCalledLife`, AND WHAT IS KEPT IS THE HALF OF IT
   THE ROW DOES NOT ANSWER. It read: NOT COVERED, a `fetch()` whose ARGUMENT CONVERSION threw or parked, which
   never reaches the member body and so raises nothing here; NEXT DIFF, the same pair one frame out raised by
   idl_args.c at the prologue's entry; ABSENCE SHOWS, a document whose page calls `fetch()` and whose begun row
   reads zero, with nothing in this census distinguishing that from a page that called none. Its retirement
   condition was that a construction which never reached the member body raise a row here, and the call row
   above is that row — raised at core/idl_args.c's stage 0, keyed on this edge's own stage table so no second
   list of network members exists anywhere, and tied to the begun row by a containment asserted at the emitter.
   ITS ABSENCE CLAUSE WAS EXACTLY WHAT WAS THEN OBSERVED, WHICH IS WHY THE CLAUSE AND NOT THE FIGURE IS WHAT
   THE NEXT READER NEEDS. Four real application pages were reported as having started no request-construction
   machine, with `epFetchAskBeganLife` at zero — and a zero there was consistent with all three states above,
   so the observation could not be acted on and the reading that was drawn from it (the page calls no `fetch`)
   was the one the census is structurally least able to support. Three of those pages' captured bundles hold
   real global `fetch(` call sites and `new XMLHttpRequest` constructions, so that reading was false.
   WHAT THE ROW STILL CANNOT DO, AND IT IS A PROPERTY OF THE MACHINE AND NOT AN OMISSION: `called == 0` does
   not separate a page that calls no `fetch()` from a flow that never reached a call the page does make, and no
   counter at this edge ever will — the prologue is not entered in either case. That question is the ORDER's,
   and `ready_picks_lifetime` is the instrument for it: solver/result.c declares its KIND beside the rest of the
   WFQ census and states what the row answers (the dispatch REACHES a job holder, with the ladder quartet beside
   it saying what the ladder then did with it), and solver/flow.c carries the three-reading enumeration that
   makes a value of it actionable.
   AND THIS SENTENCE SAID `solver/flow.h's legend`, WHICH IS A FILE THAT DOES NOT CONTAIN THE NAME — kept here
   because the pointer is the load-bearing half of this paragraph and a reader who re-derives it from "the order
   owns that question" will reach for `flow.h` again, the order's own header being the obvious place for it.
   A MIS-AIMED POINTER IN A LEGEND IS WORSE THAN ONE IN A CITATION AND THIS IS WHY: a citation sends a reader to
   a section that does not say what the code claims and they FIND OUT, while a legend naming the wrong file for
   an instrument is read as `no such instrument` the moment the grep answers zero — which is exactly the
   stale-absence direction, because the only reader of a named gap is somebody about to go and build one. It was
   relayed verbatim out of this paragraph into a lane brief as "the whole brief" and cost that lane a reading
   before it refuted the coordinate; it greped the named file, got zero, and said so rather than concluding the
   row did not exist, which is the behaviour the pointer should not have required.
   THE AUTHORING RULE THAT REMOVES THE NEED, AND IT IS FREE: a pointer to an instrument names the file that
   DECLARES it, which is derivable rather than recalled — `git grep -n <row> -- engine/host/solver/` answers in
   one command, and a pointer written without running it is a claim about this tree in the future tense.
   THE NARROWING THAT MAKES THIS ROW LIKELIER TO READ ZERO THAN ITS OWN ARGUMENT SUGGESTS, stated so that a
   zero here is not read as a broken hook: `idl_concolic_rule` answers IDL_CONCOLIC_CROSSES for IDL_USVSTRING,
   so a concolic URL — `fetch('/api/u?uid=' + state.id)`, the computed address this tool exists to report —
   CROSSES the conversion without parking or forking and reaches §5.4. The conversions park on a page GETTER,
   which is a `Request` input's `url` or a `RequestInit` member, and not on an unknown string.
   RETIREMENT — MET BY A DIFFERENT ROUTE THAN THIS CONDITION NAMED, AND THE RECORD IS REWRITTEN RATHER THAN
   DELETED BECAUSE THE SENTENCE ABOVE IT IS STILL TRUE AND A READER WILL RE-DERIVE THE WRONG CONSEQUENCE FROM IT.
   The condition asked for the separation to arrive as a row on the ORDER's own census. It arrived instead as
   `epFetchAskNamedLife` two rows down, raised where the COMPILER resolves the free identifier against the global
   object — so `no counter AT THIS EDGE ever will` is unchanged and correct, and what has changed is that the
   state it names is no longer unseparated. A reader who met the condition as it stood would go and build the
   ORDER row believing nothing yet answers the question, which is the stale-absence direction this file rates
   worst: the only reader of a named gap is somebody about to fill it.
   WHY THE COMPILER AND NOT THE ORDER, since the condition guessed the other one: the ask must be recorded
   upstream of every arm that may legitimately DECLINE, and REACH is such an arm — reach is what running is — so
   no row on the order's census is upstream of it either. The order answers WHY a call was not reached; the
   compiler answers WHETHER the program contains one. Those are two questions and the second is the one a zero
   here was being read as.
   WHAT IS STILL OWED IS NARROWER: the compile row sees ONE SPELLING, the free identifier, so `window.fetch(u)`
   and a parameter a bundle shadowed the name with reach no global resolution and raise nothing.
   AND THE CONDITION THAT STOOD HERE NAMED A MECHANISM THAT IS NOW KNOWN TO BE WRONG TWICE OVER, SO IT IS
   REWRITTEN RATHER THAN LEFT TO BE EXECUTED. It asked for `a member-name channel at the FIELD-GET EMITTER`
   reporting `into the same rows`. The channel is built, for solver/rung_entry.c's rungs, and it is NEITHER of
   those: it is raised at the SAME funnel as the row above, because a field get is where the RECEIVER exists and
   that is the interpreter — downstream of REACH, which is the one arm this whole row is upstream of, so a
   counter there answers `a flow got to a property-spelled read` and re-opens the three-state zero; and it has
   rows of ITS OWN, because the `…TypeofLife` split cannot be reproduced for a property — §13.5.3 step 2.a needs
   a non-throwing read only for an unresolvable REFERENCE, a property of an object is `undefined` when absent, so
   `typeof window.fetch` emits the same field get as `window.fetch` and reporting into `…AskNamedLife` would
   merge feature detection into the population read as uses.
   WHETHER THIS FILE OWES THE CHANNEL AT ALL IS A MEASUREMENT AND NOT AN ARGUMENT, which is why the new condition
   is a command. `node testing/static_surface.mjs` prints, per declared entry name over a mirrored corpus, the
   count of sites that spell it ONLY as a property of the global — the one population on which this row answers
   zero about a program that does spell the call. RETIREMENT: this record goes when that column is nonzero for an
   entry name declared HERE and the sibling channel routes to this file, or when the row publishes that column's
   own derivation beside itself; a relayed figure is not the condition, because it is a fact about which bundles
   were mirrored on the day it was taken. */
/* …AND THE FREE GLOBAL IDENTIFIER A PROGRAM MUST SPELL TO REACH IT — `entry`, stated by the edge beside its
   stage table for `first_stage`'s reason exactly: which name a component installs itself under is that
   component's own fact, and a table of them in this file would be the drifting second copy
   §AN-AUDITOR-DERIVES-THE-RULE forbids, drifting in the direction where a name that stopped matching reads as a
   corpus of programs none of which spells it. It is borrowed and never copied, and it is let go with the stage
   table at endpoint_free, which is what scopes the compile report below to this session. */
void    endpoint_fetch_edge_declare(const char *entry, const char *const *steps, int first_stage);
/* A DECLARED MEMBER'S CALL ENTERED THE HOSTING MACHINE'S PROLOGUE — core/idl_args.c's stage 0, called once per
   call for EVERY declared member and not only the two this file has edges for. The argument is the member's
   own stage table, so the caller states a FACT about the member and this file decides whether it is an edge:
   a list of network members spelled in core/idl_args.c would be the drifting second copy
   §AN-AUDITOR-DERIVES-THE-RULE forbids, and it would drift silently, since a member missing from it reads as a
   door nobody called. Both edges declared themselves with that same static pointer, so the match is by
   IDENTITY and never by a label two members could share; NULL is a member that declares no steps and is
   dropped here rather than at the caller. It is NOT per-edge for the same reason, and it is one entry rather
   than two because the caller cannot know which edge it is calling and must not have to. */
void    endpoint_edge_member_asked(const char *const *steps);

/* THE COMPILER RESOLVED A FREE IDENTIFIER AGAINST THE GLOBAL OBJECT — install as JSConcolicHooks.global_named.
   `name` is bytes valid for the call only and `typeof_only` says which of the two reads it was. It raises the
   `…AskNamed…` rows of whichever edge declared that identifier and NOTHING else: it composes no address, mints
   no ask and takes no decision, so it cannot move a single row it exists to make readable.
   IT IS THE ONE THING IN THIS FILE THAT IS NOT A FACT ABOUT A FLOW, AND THAT IS THE WHOLE REASON IT EXISTS.
   Every other entry here is called from a live frame, so every row this census publishes is a row about a site
   some flow REACHED — which left `called == 0` standing for three states that take opposite work, and left the
   one that matters (a program spells this call and no flow got to it) reading as the one that does not (a
   program spells no such call). A site nobody reached has no frame, no operand and no moment, so no runtime
   edge in this engine could have reported it — and, sharper, no ASSERT could have stood on it either: the only
   invariant that would have fired is one whose operand is a count derived from the PAGE'S OWN BYTES, which
   §WHOSE-BYTES-STATE-THE-VALUE forbids outright, since a page does not have to be hostile to hold an abort
   switch and only has to ship no `fetch`. That is why this lands as a ROW WITH A DENOMINATOR rather than as a
   crash, and it is the reason §Offensive-programming was structurally unable to name this gap.
   WHY THE COMPILER AND NOT SOME EARLIER RUNTIME EDGE. §AN-INVARIANT-OVER-A-GATED-OPERATION says to record the
   ASK rather than the OUTCOME, and to record it upstream of every arm that may legitimately decline. REACH is
   such an arm, and reach is what RUNNING IS — so the recording point has to be upstream of execution, and the
   only point upstream of execution at which this engine holds the page's own program is the moment it compiles
   it. The door census obeyed that rule one level too shallow: it censused the ask AT THE DOOR, which is itself
   downstream of reach.
   WHAT IT IS NOT. It is not a call-site count and its rows say `Named` rather than `Sites` so that nobody reads
   one against a static per-bundle figure: a program is recompiled by every flow that replays it, so the row
   counts COMPILER RESOLUTIONS and is read as a BIT — zero against nonzero — and never as a magnitude. It sees
   ONE SPELLING, the free identifier; `window.fetch`, `self[n]` and a parameter a bundle shadowed the name with
   are a property read or a local slot and reach no global resolution at all. So it is a FLOOR in the direction
   that withholds a finding rather than manufactures one, and no containment between these rows and the call row
   may be asserted in either direction — solver/endpoint.c states at the fields which spellings make each
   inequality ordinary, and that is why those are the only ask rows in this file with no identity over them. */
/* …AND IT ANSWERS, for the one consumer that is not a census: nonzero means an edge declared THIS name and the
   read was not a `typeof` guard, which is what quickjs's orphan walk orders its candidates by. The rows it raises
   are unchanged and are the whole of what this file keeps; the answer is derived from the SAME `entry` compare, so
   there is no second list and no way for the order and the census to disagree about which names are doors. */
int     endpoint_compile_global_named(const char *name, int typeof_only);
/* …AND THE PROPERTY SPELLING OF ONE, reached through solver/concolic.c's dispatch for the two reasons stated at
   the definition: it is that hook's SECOND consumer, and the GLOBAL-RECEIVER TEST IS NOT THIS FILE'S — the
   dispatch holds the one statement of which names denote a realm's own global, so no `base` arrives here. It
   raises `ep*AskNamedPropLife` and answers the orphan order; there is no `typeof` parameter and there cannot be,
   because §13.5.3 step 2.a has nothing to patch for a property of an object. */
int     endpoint_compile_global_member(const char *member);
void    endpoint_fetch_edge_began(void);
void    endpoint_fetch_edge_offered(void);
void    endpoint_fetch_edge_freed(int stage, int offered);
/* The rows on the heap (caller frees; NULL only on allocation failure, which every composer on the result seam
   treats as "this census is absent" rather than as a reason to fail a run).
   ROWS AND NOT A CENSUS OF ITS OWN, WHICH IS WHERE THEY ARE READ AND IS THE WHOLE ARGUMENT FOR THE SHAPE. They
   are spliced into `_cold`, between the `ep*` rows they explain, each with a LEADING comma, because a reader
   compares WITHIN a census — extension/bridge.js and extension/popup.js both say so in those words, and both
   render a row added to `_cold` with nothing edited in either. A sixth NESTED census beside `_absent` would
   have needed a name in bridge.js's relay list and a row in popup.js's, and those are TRUSTED-ZONE JavaScript
   which is live on WRITE while this half is live only after a build — so it would have aborted every document
   until an artifact carrying these rows was installed, which is the asymmetry §A-CROSS-BOUNDARY-DIFF names and
   which bridge.js's own census loop records having already paid once.
   THE EMPTY STRING IS THE ABSENT FORM AND IS NOT FIVE ZEROES. A host that installs no fetch runs no fetch
   machine and there is no population; §Testing's rule is that an absent count and a zero count are different
   facts and must never be averaged, so the rows and the comma in front of them go together. */
char   *endpoint_fetch_edge_rows(void);

/* THE XHR EDGE'S OWN ENTRY — THE SAME CENSUS OVER A DIFFERENT SHAPE OF EDGE, AND THE DIFFERENCE IS WHY IT IS
   SIX ROWS AND NOT FIVE. core/fetch's machine CONSTRUCTS the request and OFFERS it at the last of its own
   stages, so one state holds both facts and the teardown can say which of them it had reached.
   XMLHttpRequest splits that across TWO machines: `send()` (XHR_SEND_DECL over SEND_STAGES) constructs, and
   the LIFECYCLE machine it mints at §3.5.6 step 12 or 13 (js_xhr_run_steps, XR_FETCH) is what records. The
   send state is torn down BEFORE the asynchronous arm's task has run, so `did this state offer an address`
   is a question it cannot answer about itself, and a census built over the partition the fetch edge uses
   would have to invent it.
   IN BACKTICKS AND NOT IN QUOTATION MARKS, WHICH IS AN AUTHORING RULE AND NOT A TIDY-UP. That run is this
   file's own phrasing of a question, and quotation marks put it in the citation auditor's QUOTATION channel,
   where it is compared against whatever standard the nearest anchor names. It was never judged while this
   header named no standard near it — the shielded form — and the §3.5.6 citation added one paragraph up in
   the same diff that wrote this note UNSHIELDED it, so a sentence nobody had written as a spec quotation was
   reported as diverging from XHR §3.5.6 "The send() method" at word one. A spelling being SHOWN goes outside
   that channel by construction rather than by relying on no anchor being in range.
   SO THE PARTITION IS OVER THE PLACEMENT AND NOT OVER THE OFFER, and the `Placed` in the row names says so:
   a send state either reached §3.5.6 step 12/13 and handed the constructed request to the lifecycle machine,
   or it DIED, and where it died is the whole content of the census. That is the same question the fetch
   edge's stage histogram answers — where did a request the page asked for stop being built — asked of the
   machine where an XHR request is actually built.
   THE ROWS:
     `epXhrAskCalledLife`      — the `send()` calls that entered the HOSTING machine's prologue, which is
       core/idl_args.c's stage 0. SEND_STAGES is based at IDL_STEP_FIRST, so the argument-count check and the
       ES-to-IDL conversion of `send()`'s own argument run in front of the row below and BOTH are rest points.
       It is the row that turns that row's zero from three states into one; core/fetch's sibling block states
       the split and what it still cannot answer, and it is the same split here.
     `epXhrAskBeganLife`       — the `send()` calls that reached the member body. Raised at SEND_CHECKS's
       one-time capture, which is gated on a FLAG and not on a slot for core/fetch's reason exactly: that
       stage PARKS on §3.5.6 step 3's declared fork over a concolic method, and a parked stage is re-entered
       at its first line. A call whose ARGUMENT CONVERSION threw or parked is upstream of it and is in no row
       here — the same population core/fetch's own residual names, and it is named again below.
     `epXhrAskPlacedLife`      — …of those, the ones that reached §3.5.6 step 12/13 and PLACED the fetch.
     `epXhrAskOfferedLife`     — the addresses this edge OFFERED the @H surface, raised on the line before
       `xhr_record_endpoint`'s own endpoint_record call, which is INSIDE the lifecycle machine and therefore
       is NOT a fact about any send state. It is here because it is the only row that ties this census to
       `epAsks`, and the containment below is what makes the XHR edge's share of the ask population readable.
     `epXhrOutFreedLife`       — the send states TORN DOWN after a construction began, DEEP-FORK COPIES
       INCLUDED.
     `epXhrOutFreedPlacedLife` — of those, the ones that had placed the fetch.
     `epXhrOutDiedAtLife`      — ONE ARM PER STAGE: of the freed states that placed NOTHING, the stage each
       was standing at, keyed by `SEND_STEPS` itself, handed over at the declaration rather than copied here.
   THE IDENTITIES, EVERY ONE ASSERTED WHERE ALL ITS TERMS ARE IN ONE HAND:
     PARTITION — the stage rows plus the freed-and-placed row equal the freed row. Two arms of ONE teardown,
       and the one relation here that survives a deep fork: a copy gets its own teardown and files in one arm
       of it like any other state.
     CONTAINMENT — offered <= the ask total. Different events in different files, and the slack is every
       OTHER door into this surface, core/fetch's included. This is the only row that ties the edge to the
       razor, which is why it is carried even though it belongs to no partition here.
   AND THERE IS NO `freed-and-placed <= placed`, WHICH core/fetch's SIBLING BLOCK DOES HAVE — a difference
   between the two machines and not an omission. A step state is BYTE-COPIED at a deep fork and the copy
   inherits `placed`, so two copies file against one placement; core/fetch survives that because the span
   between its offer and its teardown runs no page code and its offering stage RETURNS rather than parking, so
   no state holding a raised flag can be cloned at all. THAT REASON USED TO READ "`js_fetch_unforkable` REFUSES
   the fork once the state holds §5.4's record", and it is rewritten rather than deleted because a reader will
   re-derive it from the word "survives": a sibling that cannot be forked needs no argument about where its
   offer sits, so a reader reaching for one will reach for a refusal first.
   THERE IS NO SUCH GUARD TO REACH FOR — `js_fetch_unforkable` is DELETED, along with the sibling refusals at
   the `Headers` and `Request` constructors, so it has no `visit`, no terms and no population. This clause
   used to say the record "is not one of its terms", which was a true statement about a guard that had terms
   and is now a statement about nothing; the retired wording is kept because it is the shape a reader
   re-derives, and the SYMBOL still greps nonzero in this tree for the reason this file's own rules give — a
   retired argument stays at its site, so every surviving hit is prose that TALKS ABOUT the deletion, and the
   lines were READ rather than tallied.
   WHAT IT CHANGES ABOUT THE CONTAINMENT IS NOTHING, AND THAT WAS ALREADY THE CLAUSE'S OWN CONCLUSION: a fork
   inside §5.4 steps 10-27 is allowed, it happens BEFORE the offer, and two arms each make their own offer. So
   core/fetch's survival rests on the offer's POSITION alone and never on a refusal, which is what the
   emitter's own paragraph at this census's containment states in its own words.
   IT IS NOT A CORNER: §3.5.6's SYNCHRONOUS arm sets the flag and then PARKS inside its own call
   to the lifecycle machine, which fires `readystatechange` and `progress` at the page's own listeners — page
   code, at a depth where the send frame is live and forkable. An assert would fire on a legitimate state,
   which is the concession shape §Offensive-programming refuses, so the two rows are read as two facts and
   never subtracted.
   WHAT IS DELIBERATELY NOT ASSERTED, AND THE REASON IS THE SPLIT ABOVE. There is no relation between
   `epXhrAskPlacedLife` and `epXhrAskOfferedLife`. They are raised in two machines whose states are not
   paired: a placed send mints a lifecycle machine the asynchronous arm ENQUEUES, and a task that is never
   run offers nothing, while abort() and the request error steps mint lifecycle machines of their own that
   record nothing. An inequality between them would be a claim about which sites mint that machine, which is
   not a fact this file can check, and it would fire on a legitimate state — which is the concession shape
   §Offensive-programming refuses.
   AND THAT UNASSERTABLE GAP IS THE ORDINARY STATE OF A REAL BUNDLE'S XHR DOOR, WHICH IS MEASURED HERE
   RATHER THAN LEFT AS THE HYPOTHETICAL THE PARAGRAPH ABOVE NAMES. `a task that is never run offers nothing`
   is that paragraph's own phrasing of the only way these two rows can differ on this machine, and it is what
   a real application page does at EVERY call. Measured on one attributed row of `app.slack.com` through an
   installed artifact stamped 5e2485ab95232e66bdb6851f175c833644fe7250, clean cone, dev asserts, 120 s:
   `epXhrAskCalledLife` 379, `epXhrAskBeganLife` 379, `epXhrAskPlacedLife` 379, `epXhrAskOfferedLife` ZERO.
   The PARTITION closes to the digit — every arm of `epXhrOutDiedAtLife` 0, `epXhrOutFreedPlacedLife` 379,
   `epXhrOutFreedLife` 379 — so not one send state died at any stage of `send()`. The page called `send()` 379
   times, the request was CONSTRUCTED 379 times, and the address reached the @H surface never.
   THE CONTROL IS THE SIBLING EDGE AND IT IS ARMED. The same revision's smoke fixture reads
   `epFetchAskCalledLife` 1954, `epFetchAskBeganLife` 1954, `epFetchAskOfferedLife` 1954 — one to one, because
   core/fetch OFFERS inside the machine that CONSTRUCTS, which is the difference this whole block is written
   around. That fixture also reads `epXhrAskCalledLife` 0, so NO GATE IN THIS TREE EXERCISES THIS EDGE: the
   loss sits in the door the smoke cannot see, while the door the smoke drives 1954 times is the one that real
   corpus did not call at all (`epFetchAskCalledLife` 0 on every attributed real-page row that produced a
   census). That is §A-FIXTURE-BUILT-TO-EXERCISE-EVERY-MECHANISM landing on a door rather than on a rate.
   WHAT IT LOCALISES, BECAUSE THE OFFER'S RAISE SITE ADMITS NO OTHER READING. `xhr_record_endpoint` holds no
   `return` above `endpoint_xhr_edge_offered`; `XR_FETCH` is the lifecycle machine's FIRST stage (xhr_run_closure
   mints at stage 0 and XR_FETCH is the first XR_ entry) and parks before nothing; and SEND_RUN's asynchronous
   arm mints its closure XHR_MODE_FETCH, so the one arm that skips the record is not the arm that placed these.
   A STEPPED CLOSURE THEREFORE RAISES THE ROW, and 0 against 379 says NONE OF THE 379 ENQUEUED CLOSURES WAS
   EVER STEPPED. They are `JS_EnqueueCallTask` entries, which puts the recording site at solver/engine.c's
   `run-a-task` arm — the `else` below the program sequence — and the same row set reads `jobsReady` 36169 with
   `jobsReadyMicro` 0 and `jobsFramed` 28533 against `run-a-task` 163 steps of 6673 and `finished` 0.
   AND IT IS A THIRD READING OF `asks - preProgram == 0`, WHICH THE FOURTH-NUMBER ENUMERATION ABOVE DOES NOT
   CONTAIN AND WHICH TAKES DIFFERENT WORK FROM EITHER OF ITS TWO. That enumeration offers NEVER REACHED and
   REACHED AND ALREADY KNOWN; this row is neither, because the door WAS reached 379 times and NOTHING merged —
   the ask lives on the far side of a task hop and was never made. A reader holding a razor of zero on a run of
   thousands of jobs may not spend it on either of those two without reading this edge's Called row first, and
   the same corpus carries a row that IS the second reading (146 post-program asks, every one merged into a
   record the markup already held) so the two are separable in practice and not only in principle.
   RETIREMENT: this record goes when a placed send whose closure is never stepped is visible WITHOUT it — a
   row counting the lifecycle closures this edge ENQUEUED against the ones that reached `XR_FETCH`, which makes
   the loss a subtraction. That is NOT the began/offered pair this block already refutes as a non-check: those
   two cannot disagree because both are raised inside the lifecycle machine, and these two are raised on
   opposite sides of the task hop, which is exactly where the 379 went.
   WHAT MAY NOT BE ASSERTED IS ALSO core/fetch's, and for the same mechanism as the paragraph above:
   `freed <= began` and `placed <= began` are both FALSE,
   because a step state is BYTE-COPIED at a deep fork and the copy inherits the capture flag. §3.5.6 step 3's
   fork over a concolic method and step 4's `toString` are two stages of `send()` that run the page's code, so
   this is not a corner — it is the population this tool exists for. The begun row and the freed row are read
   as two facts and never subtracted.
   SCOPE IN TIME IS THE SURFACE'S, exactly as core/fetch's is: reset in endpoint_init and endpoint_free beside
   every other counter in this file, which is what makes them comparable with the ask rows at all.
   A REPORT AND NEVER A BOUND (§NO BOUNDS): nothing branches on one, no construction is refused because of
   one, and no arm is narrowed by one.
   NAMED RESIDUAL — CORRECT AND NARROWER. WHAT IS NOT COVERED: an `xhr.send()` whose ARGUMENT CONVERSION threw
   or parked, and an `open()` that never completed — neither reaches SEND_CHECKS, so neither raises a row
   here, and `open()` is where §3.5.1 parses the method and the URL this record is made of. WHAT THE NEXT DIFF
   BUILDS: the began/freed pair one frame out, raised by core/idl_args.c for EVERY declared member at its
   prologue's entry and at its teardown, which answers it for every host edge at once instead of per
   component — the same diff core/fetch's own residual names, so the two retire together. HOW ITS ABSENCE
   WOULD SHOW: a document whose page calls `xhr.send()` and whose begun row reads zero, with nothing in this
   census distinguishing that from a page that called none.
   RETIREMENT: this record goes when a `send()` that never reached the member body raises a row here. */
void    endpoint_xhr_edge_declare(const char *entry, const char *const *steps, int first_stage);
void    endpoint_xhr_edge_began(void);
void    endpoint_xhr_edge_placed(void);
void    endpoint_xhr_edge_offered(void);
void    endpoint_xhr_edge_freed(int stage, int placed);
/* The rows on the heap (caller frees; NULL only on allocation failure), spliced into `_cold` beside the fetch
   edge's with their own LEADING comma, and NEVER summed with them: they count states of a DIFFERENT machine
   whose stages are its own, so one number over both would be the averaged population §a-coverage-figure-
   states-what-it-is-a-fraction-of names. THE EMPTY STRING IS THE ABSENT FORM — a host that installs no
   XMLHttpRequest runs no send machine and has no population, and an absent count and a zero count are
   different facts. */
char   *endpoint_xhr_edge_rows(void);

#endif
