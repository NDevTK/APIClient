/* The Fetch API — Blink core/fetch. One global, installed on the baseline before any flow runs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_H
#include "quickjs.h"
#include "core/url/url.h"
#include "core/fetch/headers.h"
#include "core/frame/policy_container.h"   /* §2.2.5's two metadata fields ride the request — see below */

/* Install `fetch` on `global`. Every request forced execution reaches funnels one endpoint into the @H
   surface; the network itself is the trusted bridge's, never this sandbox's. */
/* §5.1/§5.5/§5.4's agent-wide declarations, including the three components' per-realm prototype entries. */
void fetch_init(JSContext *ctx);
void fetch_install(JSContext *ctx, JSValueConst global);
/* §5's four interned field names — the agent's, so core/platform.h's release column gives them back. */
void fetch_free(JSRuntime *rt);

/* THE HOST'S NETWORK, as a seam the browser half takes rather than names.
 *
 * §5.6's `fetch()` is the browser's; WHO actually goes to the network is the host's, and SECURITY.md puts
 * every byte of it behind a trusted chokepoint the sandbox cannot reach. `owe` is the whole contract: the
 * component has built a Promise<Response> and a `deliver` closure, and it hands the host the URL it must
 * satisfy; the host calls `deliver` with the body when it has one, and the flow cannot finish until it does —
 * which is what keeps reply-gated code reachable.
 * It is a PARAMETER because the two hosts differ and neither is a special case of the other: the extension's
 * host parks the request on the flow's pending register and lets the trusted zone fetch it, while the wpt
 * runner serves the checked-out corpus off disk. Naming the solver's register here made the browser half
 * depend on the scheduler, and through it on the whole DOM, so nothing could take `fetch` without taking the
 * solver too. */
/* WHAT THE HOST IS OWED, as the REQUEST and not as a URL. It was a URL string, which is the half of a request
   that names WHERE — and a host that can actually answer needs the rest: a POST's method and body decide what
   comes back, and a request's headers are what a handler echoes. The wpt runner reached the point of asking
   (its corpus's `echo-content.py` and `inspect-headers.py` answer exactly those), and the extension's host has
   always needed them to satisfy a real request through the trusted zone. `headers` is the request's own list,
   borrowed; `body`/`body_len` are its bytes, NULL for a request that has none. */
/* …AND ITS DESTINATION, WHICH IS WHAT THE BYTES ARE FOR. Fetch §2.2.5 "Requests": "A request has an associated
   destination, which is a destination type. Unless stated otherwise it is the empty string", the type being one
   of "", "audio", "audioworklet", "document", "embed", "font", "frame", "iframe", "image", "json", "manifest",
   "object", "paintworklet", "report", "script", "serviceworker", "sharedworker", "style", "text", "track",
   "video", "webidentity", "worker" or "xslt". It is as much a part of the request as the METHOD is, and it is
   carried for the same reason: the party that will ISSUE it cannot decide about a property it was never told.
   ITS READER IS THE CORB CLASS, and until that reader existed this field would have been the mirror of the
   defect core/html/html_image.c names at its own park — a producer writing what nothing reads. §2.2.5 makes
   a destination SCRIPT-LIKE if it is "audioworklet", "paintworklet", "script", "serviceworker", "sharedworker"
   or "worker", and script-like is exactly "these bytes will RUN as code", which is the one question the trusted
   zone's chokepoint asks of a reply before it hands it back (SECURITY.md §Network). A request that does not
   state one cannot be classified, and a body that is not classified as code and then compiled is the hole this
   field closes: a cross-origin HTML or JSON body ingested as data and handed to the compiler.
   BORROWED like `method` and `url` — the park copies it into the flow's register (solver/pending.h). */
/* …AND FETCH §2.2.5's THREE METADATA FIELDS, WHICH RIDE THE REQUEST FOR THE DESTINATION'S REASON EXACTLY.
   Their reader is Fetch §4.1 "Main fetch" step 7's CSP check, and the party that can state them is the
   algorithm that CREATES the request — a `<script>`'s [[CryptographicNonce]], its `integrity` attribute and
   whether the parser inserted it are facts about an element that is long off the stack by the time a park
   runs. Carried here, a component that builds a request states them once beside its method and its
   destination; carried as a seam parameter they would be stated by whichever seam its author remembered.
   THERE IS NO "I DO NOT KNOW" VALUE, and that is core/frame/policy_container.h's design rather than this
   field's: two spellings, each a claim about a named algorithm. A producer writes csp_request_metadata when
   the algorithm sets values and csp_request_metadata_unstated when the algorithm sets neither, and it owes
   the reader the name of that algorithm at its own site.
   ZERO-INITIALISE THE RECORD (`FetchRequest req = {0};`) SO A FORGOTTEN FIELD IS AN ABORT AND NOT GARBAGE.
   Both pointers are non-NULL in every legal value and the parser metadata's zero is its own not-a-value
   member, so a zero-filled struct is DISTINGUISHABLE from every legal one and policy_should_block_request's
   own asserts name it — while an uninitialised automatic would hand the CSP walk a stack address to compare
   bytes at. */

/* FETCH §2.2.5 "Requests"' CREDENTIALS MODE, AS ITS OWN CLOSED DOMAIN — "A request has an associated
 * credentials mode, which is `omit`, `same-origin`, or `include`. Unless stated otherwise, it is
 * `same-origin`."
 *
 * IT IS A STATEMENT ABOUT A REQUEST AND NEVER A PERMISSION, which is the whole reason it may live in this
 * engine at all. SECURITY.md puts the credential DECISION behind the trusted zone's chokepoint, and this
 * field does not move it: the algorithm that CREATES a request is the only party that knows what the standard
 * says its credentials mode is, and a zone that was never told cannot decide about it. That is the same
 * sentence `destination` and the §2.2.5 metadata are carried by, arriving at the field CLAUDE.md's
 * §A-REQUEST-CARRIES-THE-PROVENANCE names beside the method: "`safeFetch` decides, from the provenance the
 * request declares beside its method and credential state".
 *
 * IT IS AN ENUM AND NOT A `const char *`, which is where it parts company with `destination` one field up.
 * That one is text because §2.2.5's destination enumeration MOVES and is shared across the ABI with the
 * trusted zone, so two copies of a growing list would disagree. This domain is CLOSED at three members and
 * has not moved, and the one thing anything does with it is compare identity — so a string would put the
 * literal at every producer, where a misspelling is not a compile error and lands on whichever arm a
 * consumer's `else` happens to be. That is core/frame/policy_container.h's argument for making the parser
 * metadata an enum while its two neighbours stay strings, and it is the same argument here. The WIRE spelling
 * is `fetch_credentials_token`'s and nobody else's, exactly as solver/engine.h's provenance has one mapping
 * and two spellings so a caller cannot reach a fourth answer.
 *
 * `_UNPLACED` IS NOT A REQUEST STATE AND IS NOT "I DO NOT KNOW". It is what a zero-fill leaves, and it exists
 * for the reason the record's own paragraph above rests on: it is what makes a struct nobody assigned
 * DISTINGUISHABLE from every legal value. §2.2.5's "unless stated otherwise" default is `same-origin`, which
 * is a POSITIVE answer a producer may state and not a hole — so a producer that has not been told writes
 * nothing and `fetch_owe` aborts, rather than writing the default and making "the algorithm says
 * same-origin" indistinguishable from "nobody plumbed this". There is deliberately NO constructor meaning
 * unknown, for policy_container.h's reason word for word. */
typedef enum {
    FETCH_CREDENTIALS_UNPLACED = 0,
    FETCH_CREDENTIALS_OMIT,
    FETCH_CREDENTIALS_SAME_ORIGIN,
    FETCH_CREDENTIALS_INCLUDE,
} FetchCredentialsMode;

/* THE ONE WIRE SPELLING OF THAT DOMAIN, and the only place the three words are written. Fatal — never a
   DCHECK — on `_UNPLACED` and on anything outside the enum, for the reason solver/engine.h's
   `engine_provenance_token` gives about its own three: a release build falling through would hand the trusted
   zone whatever the compiler left in the register, and the zone would decide a credential question from it. */
const char *fetch_credentials_token(FetchCredentialsMode m);

/* …AND ITS INVERSE, for the one producer that holds the mode as TEXT because Web IDL handed it one: §5.4's
   `RequestInit` member is an `enum RequestCredentials`, so core/fetch/request.h keeps the record's field as
   the string §2.6.1-style reflection hands back to `request.credentials`. Mapping it here rather than at that
   producer is what keeps ONE vocabulary: a second switch beside the record would be free to answer a word
   this one does not. Fatal on a word §2.2.5 does not define, and that is an assertion about THIS engine and
   not about a page — Web IDL §3.2.19 Enumeration types has already refused every other string before the
   record was filled, so an arrival here is our own conversion having stopped. */
FetchCredentialsMode fetch_credentials_of_token(const char *token);

typedef struct {
    const char   *method;
    const char   *url;
    const char   *destination;
    const HeaderList *headers;
    const char   *body;
    size_t        body_len;
    CspRequestMetadata metadata;
    /* Fetch §2.2.5 "Requests"' credentials mode — see the domain above. STATED by the algorithm that creates
       the request, because that algorithm is the only thing that knows: HTML §2.5.1 "Terminology"'s create a
       potential-CORS request answers it for an `<img>`, a `<link rel=preload>` and an `EventSource`, HTML
       §2.5.4 "CORS settings attributes"' CORS settings attribute credentials mode answers it for a
       `modulepreload`, XHR §3.5.6 "The send() method" answers it from `withCredentials`, and Fetch §5.4
       "Request class" answers it from `RequestInit`. Those five do not agree — §2.5.1 and §2.5.4 give the No
       CORS state OPPOSITE answers, which is §2.5.4's own "repurposed to have a slightly different meaning" —
       so there is no value this record could carry by default that is right for more than one of them.
       RESIDUAL — CORRECT AND NARROWER, NAMED RATHER THAN CRASHED ON, because every producer that reaches
       `fetch_owe` states the mode its own algorithm names and there is no case at that seam to abort on.
       WHAT IS NOT COVERED: the field is stated on the request and does not yet reach the party that decides
       from it. A park carries `method`, `url`, `destination`, `headers` and `body` onto the pending line and
       the trusted zone reads them there; the credentials mode has no slot on that line, so the ONLY seam
       whose credentials mode reaches the zone today is XMLHttpRequest's synchronous host request, whose
       record is JSON and states it as a member. WHAT THE NEXT DIFF BUILDS: the field on the pending line
       beside the destination, carried through the split every host performs on that line and read by
       `extension/lib/safe-fetch.js`'s caller in `bridge.js` — which already speaks these three words on the
       XHR route and DCHECKs them there. HOW ITS ABSENCE WOULD SHOW: a `<link rel=preload>` and an `<img>`
       state `include` here and reach the zone with nothing, so a same-origin resource a browser fetches with
       cookies is fetched without them and a personalised body is learned as the logged-out one.
       AND TWO PARKS DO NOT PASS THIS SEAM AT ALL, which is the other half of the same residual: a dynamic
       `import()` and an INJECTED `<script src>` build their own record and reach the park directly rather
       than through `fetch_owe`, so they carry the unplaced zero and no assert here can see them. Their mode
       is HTML §4.12.1 "The script element"'s — the CORS settings attribute credentials mode of the element's
       `crossorigin` content attribute for the injected script, and the referencing script's fetch options for
       the `import()`. HOW THAT HALF WOULD SHOW: a `<script crossorigin=use-credentials src>` chunk loads
       uncredentialed the day the field reaches the wire, which is the population @S reaches most. */
    FetchCredentialsMode credentials;
} FetchRequest;

/* FETCH §4.1 "Main fetch" STEP 7, AS THE ONE COMPONENT IT IS A STEP OF — "If should request be blocked due to
 * a bad port, should fetching request be blocked as mixed content, should request be blocked by Content
 * Security Policy, or should request be blocked by Integrity Policy Policy returns blocked, then set response
 * to a network error".
 *
 * WHAT IS NOT COVERED: the step is FOUR checks and this component runs TWO of them. Mixed content is not
 * implemented anywhere in this engine, and INTEGRITY POLICY is the disjunct §4.1 gained after the four copies
 * this replaced were written — every one of them quoted a three-check version of this sentence, and moving the
 * quotation to a fresh site is what made the auditor say so. WHAT THE NEXT DIFF BUILDS: Integrity Policy's own
 * "should request be blocked by Integrity Policy Policy", over a policy parsed from the `Integrity-Policy`
 * response header into the policy container beside the CSP list, called as a third disjunct here. HOW ITS
 * ABSENCE SHOWS: a document served `Integrity-Policy: blocked-destinations=(script)` loads a `<script src>`
 * carrying no `integrity` attribute here and is refused it by a browser — so an @S breakout measured against
 * that document's policy reports a sink the real page cannot reach.
 *
 * IT WAS FOUR HAND-WRITTEN COPIES, ONE PER ENTRY, and the fifth entry is what proved that shape wrong: a
 * `<script src>` ran NO CSP check at all, because §4.12.1.1's fetch is the one nobody remembered to add a copy
 * to, and nothing anywhere reported it. Each copy also hand-parsed the URL and hand-wrote the disjunction, so
 * they could drift in three ways rather than one. A question some entries ask and others do not is one
 * missing capability wearing two names.
 *
 * THE CALLER STATES WHAT ONLY THE CALLER KNOWS AND NOTHING ELSE. The DESTINATION is the creating algorithm's
 * (CSP §6.8.1 "Get the effective directive for request" switches on it, which is what makes `img-src` govern
 * an image and `script-src` a script) and the METADATA is the creating element's; the URL PARSE, the bad-port
 * check, the policy container and the redirect count are this component's, because every caller answered them
 * the same way and one of them answering differently would be a bug rather than a variation.
 *
 * A REQUEST WHOSE ADDRESS DOES NOT PARSE IS NOT BLOCKED BY THIS STEP, which is the behaviour all four copies
 * had and is kept deliberately: every copy guarded its disjunction with `url_parse(...) && (...)`, so a
 * failure answered ALLOWED and the caller's own algorithm dealt with the unparseable address. §4.1 step 7 is
 * a question about a request's URL and there is no request to ask it of.
 *
 * Answers non-zero for BLOCKED. `url` is the request's serialized current URL. */
int fetch_main_blocked(JSContext *ctx, const char *url, const char *destination, CspRequestMetadata metadata);

/* IS THIS ONE OF FETCH §2.2.5 "Requests"' DESTINATION TYPES — the enumeration quoted in the paragraph above,
 * as a predicate, in the component whose record carries the field.
 * IT IS AN EXPORT BECAUSE THE FIELD HAS THREE CONSUMERS AND ALL THREE ASSERT AGAINST IT, and while it was a
 * `static` in ONE of them the other two either restated the table or trusted a producer. `solver/engine.c`
 * kept its own copy for the pending join and the pending splitter; `core/html/html_link.c` needs it for Fetch
 * §2.2.7 "Miscellaneous"' translate-a-potential-destination assert; and `extension/lib/safe-fetch.js` holds the
 * one on the other side of the ABI, where the answer decides whether a reply may be ingested as CODE. Two
 * copies inside one program is what §2.2.5 being a moving enumeration makes expensive — a destination type
 * added to one copy and not the other is a request one half of the engine refuses and the other half fetches. */
bool fetch_is_destination_type(const char *destination);

/* IS THAT DESTINATION SCRIPT-LIKE — Fetch §2.2.5 "Requests": "A request's destination is script-like if it is
 * `audioworklet`, `paintworklet`, `script`, `serviceworker`, `sharedworker`, or `worker`."
 * IT IS AN EXPORT FOR THE REASON THE PREDICATE ABOVE IS, and the paragraph above already made the argument:
 * this is a MOVING ENUMERATION and a second copy of it is a question one half of the program answers `yes` to
 * and the other `no`. The consumers do not ask it for one purpose either, which is what makes a shared reading
 * matter rather than merely tidy — `extension/lib/safe-fetch.js` asks it to decide whether a reply may be
 * ingested as CODE, and CSP §6.7.1.1 "Script directives pre-request check" asks it as the GATE on its whole
 * step 1, so a destination one of them calls script-like and the other does not is a body compiled under a
 * check that was never run over it.
 * `xslt` IS NOT IN THE SET AND THAT IS DELIBERATE, in the standard rather than here: Fetch's own note says
 * "Algorithms that use script-like should also consider `xslt` as that too can cause script execution. It is
 * not included in the list as it is not always relevant and might require different behavior." A caller that
 * wants it says so at its own site; this predicate answers §2.2.5's question and no other. */
bool fetch_is_script_like(const char *destination);

typedef struct {
    void (*owe)(JSContext *ctx, JSValueConst deliver, JSValueConst value, const FetchRequest *req);
} FetchProvider;
void fetch_set_provider(const FetchProvider *p);

/* OWE THE HOST A REQUEST THAT IS NOT `fetch()`'S — the seam above, reached by the OTHER browser components
   whose own standards say "fetch request". HTML §4.8.4.3.5 "Updating the image data" is the first: an `img`
   element's request is a request in every sense this file means one — it goes to the same host, it is subject
   to the same SOP/CORS decision in the trusted zone, and its address belongs on the same @H surface — and it
   is not a `fetch()`, so it has no promise and no Response. `deliver` is called with the host's reply record
   (or JS_NULL for a network error) exactly as the one above is, and what the caller does with it is that
   caller's standard's processResponse steps.
   IT IS THIS ENTRY AND NOT THE PROVIDER STRUCT DIRECTLY, because the provider is a STATIC of this component
   and the assertion that a host installed one belongs with it: a component that reached for `g_provider`
   itself would each need its own copy of that check, and a request owed to nobody is a flow parked for the
   rest of the session. */
void fetch_owe(JSContext *ctx, JSValueConst deliver, const FetchRequest *req);

/* PARSE A URL A PAGE WROTE, against HTML's API base URL — the one operation every Fetch entry point performs
   on a URL string, so `new Request("/api/users")` and `Response.redirect("/there")` resolve the same address by
   the same rule rather than each reaching for url_parse with whatever base it remembered. Fills `*rec` and
   returns true; on failure `*rec` is already freed and the caller throws whichever error its spec names — a
   TypeError for both of today's two, but the spec says so at each site rather than here. */
bool fetch_parse_url(JSContext *ctx, UrlRecord *rec, const char *url, size_t len);

/* THE REPLY, as the value a host DELIVERS. It was the body's bytes and nothing else, so every reply built from
   it had no status but 200 and NO HEADERS AT ALL — `response.headers.get(...)` was null for everything a page
   fetched, and with it went the Content-Type that decides whether `.formData()` parses a body and the
   `Location` an endpoint's redirect is made of. A host builds one of these with whatever it knows; `headers`
   may be NULL for a host that knows none, which is a different statement from a reply that HAD none.
   `url_list`/`url_list_n` are §2.2.6's URL LIST, and they are the host's to report because the host is what
   FETCHED: only it saw the redirect chain, and `response.url` (the list's last item) and `response.redirected`
   (its size > 1) are read off nothing else. A host that followed no redirect reports the one URL it requested
   — §4.1's "If internalResponse's URL list is empty, then set it to a clone of request's URL list" — so the
   list is never empty and the DCHECKs at both ends say so. Only the FIRST and LAST items are ever exposed to
   script, which is why a host that cannot enumerate the middle of a chain still reports a faithful list.
   `body`/`body_len` are §2.2.5's BODY, which is a BYTE SEQUENCE and reaches the record as one.
   `computed_type` is §4.2's ESSENCE of what the HOST decided this resource IS — the sniff's answer, taken
   where the bytes were read. It is a PARAMETER for the same reason the URL list is: only the host performed
   the fetch, and only the host may sniff. A renderer that derives a type from response bytes for itself can
   classify — and then MINE — a cross-origin body a real renderer would have been handed as an opaque, empty
   response, which is why §7 runs in the network service and why this crosses as an answer rather than as
   evidence. In the extension the host is `extension/lib/safe-fetch.js`, whose `computedType` this is; a C host
   states what it served. The EMPTY string is §5.1's "the supplied MIME type is undefined" surviving the sniff
   — the server named nothing and the bytes named nothing either, a positive answer — and NULL is not allowed:
   a host that has not decided has not finished building the record. */
JSValue fetch_reply_new(JSContext *ctx, int status, const char *status_text, const HeaderList *headers,
                        const char *body, size_t body_len, const char *const *url_list, int url_list_n,
                        const char *computed_type);

/* ---- §2.2.5's BODY, WHICH IS A BYTE SEQUENCE AND CROSSES AS ONE ------------------------------------------
 *
 * "A response has an associated body (null or a body)", and a body's is "a byte sequence". It was a JS STRING
 * on this record, at every producer, and that is not a representation choice — it is a DECODE, run by whoever
 * built the record, before any standard's own decode could run:
 *
 *   • the extension's trusted zone ran Fetch §5.3 "Body mixin"'s `text()`, whose steps are "to return the
 *     result of running consume body with this and UTF-8 decode" (§5.4 stood here and is "Request class",
 *     which INCLUDES the mixin rather than defining it, so the number named a caller for a definition),
 *     so a script served `charset=windows-1252` arrived already mangled and HTML §8.1.4.2's classic decode
 *     (core/loader/script_fetch.h), whose whole job is to honour that label, was handed the wrong bytes;
 *   • `fetch_reply_new` ran `JS_NewStringLen`, which is quickjs's own UTF-8 decode, so EVERY C host destroyed
 *     the same evidence a step earlier than the extension did. cutils.h states its error mode outright —
 *     "encoding errors are converted as 0xFFFD and use a single byte" — so a lone 0x81 became U+FFFD, and
 *     `JS_ToCStringLen` on the other side re-encoded that as EF BF BD. The classic decode has therefore never
 *     once received a byte a server actually sent, on any host.
 *
 * A decode is a SEMANTIC and semantics are the engine's (CLAUDE.md §Architecture), so the record carries the
 * bytes and each consumer runs the algorithm ITS OWN standard names: §8.1.4.2's two script decodes, Fetch's
 * "parse JSON from bytes" (UTF-8 decode, then JSON.parse), XHR §3.6.6's final encoding, MIME Sniffing §7 over
 * the bytes themselves. An ArrayBuffer is what a byte sequence is in this heap — it is already what
 * `pending_set_bytes` stores a REQUEST body as — and it is the one JS value whose contents survive a round
 * trip unexamined. */

/* WRITE the body onto a record whose other fields arrived as JSON. The bytes cross beside that text rather
   than inside it, because JSON cannot say a byte sequence: `JSON.stringify` on a Uint8Array answers
   `{"0":72,…}`, a plausible record carrying a body that is not the body. Asserts the record did NOT already
   carry one — a producer still sending a decoded string is the defect this edge exists to make impossible. */
void fetch_reply_set_body(JSContext *ctx, JSValueConst reply, const uint8_t *bytes, size_t n);

/* READ it: the record's body VALUE, a reference the caller frees. A network error (the JSON `null`) has no
   record and answers the EMPTY byte sequence — which is what a script that did not load runs, and what a
   reader of a reply that never arrived measures. */
JSValue fetch_reply_body(JSContext *ctx, JSValueConst reply);

/* …and the BYTES of a body value. The pointer is into the value's own buffer, so it is valid exactly as long
   as the caller holds that value — there is nothing to free. Never NULL: an empty body answers a zero length
   and a pointer that may be read zero times, so no caller needs a null test that a body of length 0 would be
   the only thing to exercise. */
const uint8_t *fetch_body_bytes(JSContext *ctx, JSValueConst body, size_t *out_n);

/* `fetch_reply_parse_json` STOOD HERE AND ITS TWO CALLERS ARE GONE. It was Infra's "parse JSON from bytes"
   over a reply RECORD, and the two askers were the solver's own readers of an API's rejection envelope and of
   its published description — both of which are the trusted zone's again (extension/lib/req2proto.js,
   extension/lib/discovery-probe.js). `Response.json()` never went through it: body.c reaches
   `byte_reader_json` directly, which is the one implementation both were sharing. A wrapper with no caller
   reads as a capability this surface offers, so it is deleted rather than kept for the next reader. */

/* …AND THE READ OF IT, in the component that owns the WRITE. The record's `headers` field is an Array of
   [name, value] pairs — a LIST and not a map, because §5.1 never combines two entries and Fetch §2.2.2's "get"
   is what joins them — and turning it back into a `HeaderList` is the one operation every consumer of a reply
   needs before it can ask for a header. It had two readers written out by hand (the fetch() delivery, and the
   script decode that needs the `Content-Type` charset); a record shape known in more than one place is a record
   shape that drifts from its writer.
   `out` must be an EMPTY list — a response has ONE header list, and filling one twice would make `get` join two
   responses' values together. A reply that is the JSON `null` (a network error) carries no headers and leaves
   `out` empty, which is what a caller reads as "the response had none". */
void fetch_reply_header_list(JSContext *ctx, JSValueConst reply, HeaderList *out);

/* WHAT THE HOST DECIDED THIS RESOURCE IS — the record's `computedType`, as a malloc'd string the caller frees.
   It is READ instead of derived: the alternative is this process running its own sniff over the body, or
   re-parsing the raw `Content-Type` and calling that a type, and both are the renderer answering a question
   the network side already answered about the same bytes. Two answers to one question is the shape that has
   nothing to make them agree.
   THE FIELD IS ASSERTED AND NEVER DEFAULTED. Every producer of this record writes it — `fetch_reply_new` takes
   it as a parameter and the trusted zone stamps it on the JSON — so an absent one is a producer that stopped,
   not a resource whose type is unknown. That case has its own value and it is the EMPTY string.
   A NETWORK ERROR (the JSON `null`) has no record at all and answers NULL, which is the one thing a reader
   must distinguish from "" — "this address answered nothing" against "it answered, and named nothing". */
char *fetch_reply_computed_type(JSContext *ctx, JSValueConst reply);

/* WHAT THE SERVER ANSWERED WITH — Fetch §2.2.6 "Responses"' status, whose vocabulary Fetch §2.2.3
   "Statuses" fixes: "A status is an integer in the range 0 to 999, inclusive."
   IT IS A READER BECAUSE THE FIELD HAD TWO HAND-WRITTEN ONES AND BOTH OF THEM DEFAULTED IT. This is
   `fetch_reply_header_list`'s argument about the same record — "a record shape known in more than one place is
   a record shape that drifts from its writer" — arriving at the one field whose absence is INDISTINGUISHABLE
   FROM A VALUE. Each did `int32_t status = <literal>; JS_ToInt32(ctx, &status, v);`, so a record that had lost
   the field reported that literal: 200 at the `fetch()` delivery (a refusal read as a success) and 0 at
   XMLHttpRequest (a real reply read as a network error). Neither could crash, because both numbers are
   statuses a reply legitimately carries. The `fetch()` one is converted; XMLHttpRequest's §3.5.6 reply read is
   the one still spelled out by hand.
   A NETWORK ERROR ANSWERS 0, AND THAT IS THE SPEC'S OWN VALUE RATHER THAN THIS READER'S SENTINEL: §2.2.6 says
   "A network error is a response whose type is `error`, status is 0, status message is the empty byte
   sequence, header list is « », …", and the JSON `null` a host sends for one IS that response. So a caller
   telling "nothing answered" apart from "the server refused" compares 0 against 401 and needs no second call.
   THE FIELD IS ASSERTED AND NEVER DEFAULTED, for `computedType`'s reason with a sharper failure: `JS_ToInt32`
   of `undefined` is 0, which is exactly §2.2.6's network-error status — so a producer that stopped writing
   `status` would make every reply this engine ever fetched read as a request that never reached a server, and
   every reader downstream would be correct about the value it was handed. */
int fetch_reply_status(JSContext *ctx, JSValueConst reply);

/* …AND THE STATUS MESSAGE BESIDE IT — Fetch §2.2.6 "Responses"' status message, as a malloc'd string the
   caller frees. It is the LAST field of this record that two consumers still read by hand, and it defaults in
   both: the fetch() delivery did `JS_ToCString(...); stx ? stx : ""` and XMLHttpRequest's §3.5.6 reply read did
   `JS_IsString(v) ? dup : JS_NewString(ctx, "")`. That is `fetch_reply_status`' argument arriving at the one
   field where the default is INDISTINGUISHABLE FROM THE COMMONEST REAL VALUE rather than merely from a legal
   one: §2.2.6 says "A response has an associated status message. Unless stated otherwise it is the empty byte
   sequence." and, for the protocol most of the web now speaks, "Responses over an HTTP/2 connection will
   always have the empty byte sequence as status message as HTTP/2 does not support them." So `""` is not an
   edge case a reader might notice — it is what a correct producer writes most of the time, and a producer that
   stopped writing the field at all lands on the same two bytes with nothing anywhere to say which happened.
   THE FIELD IS ASSERTED AND NEVER DEFAULTED, for `computedType`'s reason. `fetch_reply_new` defines it on
   every record and the trusted zone's `safeFetch` stamps it on both of its return paths — a real reply carries
   the server's reason phrase and a REFUSED one carries the refusal's own reason, which extension/lib/
   safe-fetch.js calls "the only account a page or a person ever gets of a request this zone did not make". So
   an absent one is a producer that stopped, and the account it was carrying is what gets deleted.
   ONLY THE TYPE IS ASSERTED, NEVER THE CONTENT. The bytes are a SERVER'S reason phrase, so what they say is
   not an invariant this codebase's logic can violate; that the field is a string is this project's producers'
   contract, and it is the half worth crashing on.
   A NETWORK ERROR (the JSON `null`) ANSWERS NULL, WHICH IS THE ONE THING A READER MUST NOT CONFUSE WITH "" —
   §2.2.6 gives a network error "status message is the empty byte sequence", so "nothing answered" and "it
   answered and named nothing" are two facts that the spec's own value cannot separate and this reader can. */
char *fetch_reply_status_text(JSContext *ctx, JSValueConst reply);

#endif
