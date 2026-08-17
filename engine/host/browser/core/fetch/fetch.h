/* The Fetch API — Blink core/fetch. One global, installed on the baseline before any flow runs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_H
#include "quickjs.h"
#include "core/url/url.h"
#include "core/fetch/headers.h"

/* Install `fetch` on `global`. Every request forced execution reaches funnels one endpoint into the @H
   surface; the network itself is the trusted bridge's, never this sandbox's. */
/* §5/§6/§5.3's agent-wide declarations, including the three components' per-realm prototype entries. */
void fetch_init(JSContext *ctx);
void fetch_install(JSContext *ctx, JSValueConst global);
/* §5's four interned field names — the agent's, so core/platform.h's release column gives them back. */
void fetch_free(JSRuntime *rt);

/* THE HOST'S NETWORK, as a seam the browser half takes rather than names.
 *
 * §5.5's `fetch()` is the browser's; WHO actually goes to the network is the host's, and SECURITY.md puts
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
typedef struct {
    const char   *method;
    const char   *url;
    const HeaderList *headers;
    const char   *body;
    size_t        body_len;
} FetchRequest;

typedef struct {
    void (*owe)(JSContext *ctx, JSValueConst deliver, JSValueConst value, const FetchRequest *req);
} FetchProvider;
void fetch_set_provider(const FetchProvider *p);

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
 *   • the extension's trusted zone ran Fetch §5.2's `text()` — "run consume body with this and UTF-8 decode" —
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

/* A REPLY'S BODY AS JSON, which is Infra's "parse JSON from bytes" and therefore TWO steps: "let string be the
   result of running UTF-8 decode on bytes", then parse JSON from string. It is here rather than at each asker
   because the solver has two of them — an API's own rejection envelope (req2proto.c) and its published
   description (discovery.c) — and both used to reach for the record's `body` as a STRING, which after this
   change is a read that answers nothing at all: a body is a byte sequence. A reply that is not JSON (an HTML
   error page, a script, an image) answers JS_EXCEPTION with the real SyntaxError live, which is an ordinary
   fact about the web that each caller takes and drops. */
JSValue fetch_reply_parse_json(JSContext *ctx, JSValueConst reply);

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

#endif
