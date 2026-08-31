/* The Fetch API — Blink core/fetch. One global, installed on the baseline before any flow runs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_H
#include "quickjs.h"
#include "core/url/url.h"
#include "core/fetch/headers.h"

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
typedef struct {
    const char   *method;
    const char   *url;
    const char   *destination;
    const HeaderList *headers;
    const char   *body;
    size_t        body_len;
} FetchRequest;

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
 *   • the extension's trusted zone ran Fetch §5.4's `text()` — "run consume body with this and UTF-8 decode" —
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

#endif
