/* What a reply body teaches — see reply_decode.h. Stateless: every endpoint it learns goes to solver/endpoint.c. */
#include "solver/reply_decode.h"
#include "solver/endpoint.h"
#include "core/mime/mime_type.h"     /* Fetch §4's extract a MIME type — the PARSE, which is the renderer's */
#include "core/encoding/encoding.h"   /* §6's UTF-8 decode: what a `text/x-component` stream's bytes are read as */
#include "core/fetch/fetch.h"
#include "core/url/url.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* §4.6's ASSET GROUPS, together: a body in one of them is BYTES a decoder turns into pixels or samples, and
   there is no request shape, no schema and no address inside it.
   ASKED OF THE TYPE THE HOST COMPUTED AND STAMPED ON THE RECORD, WHICH THIS PROCESS MAY NOT DECIDE FOR ITSELF.
   Sniffing is a NETWORK-side algorithm: a real browser runs it where the bytes arrive, CORB/ORB gates on its
   result, and the renderer is TOLD a computed type it never derives from response bytes. This file is the
   renderer. An engine that sniffs for itself can classify — and then MINE — a cross-origin body a real renderer
   would have been handed as an opaque, empty response, and the endpoints it takes out of one are surface the
   page could never have obtained, reported as a finding about the page. That is the same rule CLAUDE.md states
   for active discovery ("CORS-bounded both ways") arriving from the other direction.
   AND IT IS NOT THE RAW HEADER EITHER, WHICH IS THE CHANGE THIS PARAGRAPH USED TO ARGUE AGAINST ITSELF. What
   stood here read `Content-Type` off the record's header list and ran Fetch §4's extract-a-MIME-type on it,
   and defended that as "the server's own STATEMENT, which the renderer legitimately parses". The parse is
   legitimate; deciding FROM IT WHAT THE RESOURCE IS is the thing that was already decided, by the zone that
   held the bytes, one hop earlier — so two zones were answering one question about one response with nothing
   to make them agree, and the one that could see the body was not the one being believed. `computedType` is
   that zone's answer (`extension/lib/safe-fetch.js`, CLAUDE.md §Architecture: "TYPE SNIFFING STAYS IN
   JAVASCRIPT, in `safeFetch`"), and reading it is what makes the sniff single-sourced rather than absent.
   The paragraph that stood here ended "The order is: the plumbing, then the reader." The plumbing is
   `fetch_reply_new`'s `computed_type` parameter and safeFetch's stamp; this is the reader. */
static bool is_asset(const MimeType *m)
{
    return mime_type_is_image(m) || mime_type_is_audio_or_video(m) ||
           mime_type_is_font(m)  || mime_type_is_zip_based(m) || mime_type_is_archive(m);
}

/* ── React Flight ────────────────────────────────────────────────────────────────────────────────────────
 *
 * A Flight response (React Server Components; `text/x-component`, which is what React's own fetch responses
 * carry) is LINE-FRAMED: `<hex row id>:<payload>\n`, where a payload beginning `I[` is a CLIENT REFERENCE —
 * `[moduleId, chunks, exportName]` — and the chunks are the JavaScript the page will fetch to hydrate that
 * route. Those are addresses the server NAMED, which is why they are endpoints in the §H sense and why they
 * belong to the same surface forced execution's own call sites land in: a route the user never navigated to
 * still ships its chunk list in the payload of the route they did.
 *
 * A ROW IS NOT SELF-CONTAINED, WHICH IS THE WHOLE OF WHY THERE IS A ROW TABLE BELOW. `chunks` is not a list of
 * strings: the stream dedups by defining each value once under its own row id and writing `"$<id>"` wherever
 * it recurs, and it nests (`[entry, [dependencies…], [sizes…]]`), so a reader that took the strings directly
 * under it recovered a row id and refused it as an address. What resolves a reference is the stream's own
 * table, never a rule about which lead bytes are ids — see it for why that distinction is the load-bearing
 * one.
 *
 * THE OTHER ROW KINDS ARE NOT READ HERE, and that is a statement about ownership rather than about difficulty.
 * A `json` row's SHAPE is schema inference, which `extension/lib/schema.js` does; an `E[` row's message is
 * credential extraction, which `extension/lib/keys.js` does. Reading them here would put two components'
 * work in a third. */

/* THE CHUNK'S ADDRESS, RESOLVED — and ONLY when the chunk is root-relative. React's client manifest writes a
   chunk either as an absolute path (`/_next/static/chunks/x.js`) or as a bare path relative to a build-time
   ASSET PREFIX this engine has not been told. Resolving a bare one against the Flight document's own directory
   would produce an address that parses, looks plausible and was never served — §RUN, DON'T MATCH's "COMPUTE OR
   SHAPE, NEVER INVENT", where inventing is the worse half. So a bare chunk is skipped and nothing is recorded
   for it. */
static void record_chunk(JSContext *ctx, const UrlRecord *base, const char *chunk)
{
    UrlRecord u;
    char *abs;
    JSValue uv;

    if (chunk[0] != '/') return;
    if (!url_parse(&u, chunk, strlen(chunk), base)) { url_record_free(&u); return; }
    abs = url_serialize(&u, /*exclude_fragment*/ true);
    CHECK(abs, "reply_decode: OOM serializing a Flight chunk address");
    uv = JS_NewString(ctx, abs);
    /* GET, because a chunk is a script the page LOADS — the verb is a fact about what a Flight client
       reference IS, not a default. There is no method parameter anywhere in this file because no caller has a
       second answer to give it. */
    endpoint_record(ctx, "GET", uv, NULL, 0, NULL);
    JS_FreeValue(ctx, uv);
    free(abs);
    url_record_free(&u);
}

static bool hex_row_id(const char *s, size_t n)
{
    size_t i;
    if (n == 0) return false;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

/* THE STREAM'S OWN ROW TABLE, AND WHY A CLIENT REFERENCE CANNOT BE READ WITHOUT ONE.
 *
 * A row's payload is not self-contained: a Flight stream DEDUPS by emitting a value once under its own row id
 * and writing `"$<id>"` everywhere it recurs. A client reference's chunk list is where that bites, because the
 * chunk paths are the most repeated strings in the stream — every reference in a route shares an entry chunk
 * and a framework chunk — so what an `I[…]` row actually carries is references and not addresses:
 *
 *     2:"/_next/static/immutable/chunks/3ly0f72psnl18.js"
 *     a:I[477183,[["$2",["$3","$4"],[14703,910]],"$5",["$6",["$7","$8"],[7598,13470]]],"$9"]
 *
 * A reader that took only the STRINGS directly under the chunk list therefore recovered `"$5"`, which is not an
 * address and is refused by record_chunk's root-relative rule, and nothing else at all — the other entries are
 * arrays. It read ZERO chunks from a stream carrying twenty-three of them, and reported that as a reply that
 * named none: the read-with-no-writer defect wearing a parse, where the number is real, the code runs, and the
 * answer is a property of the reader rather than of the response.
 *
 * THE TABLE IS BUILT OVER THE WHOLE BODY BEFORE ANY ROW IS READ, because a reference may name a row that
 * appears later; it holds SPANS INTO THE BODY and copies nothing. A duplicate id resolves to the FIRST row that
 * defines it — a reference names the row that defined the id, and a stream defining one twice is one this
 * reader cannot disambiguate, so the definition every earlier reference already meant is the one kept.
 *
 * WHAT IS A REFERENCE IS DECIDED BY THE TABLE AND NEVER BY THE SHAPE OF THE `$`. React writes several TYPED
 * markers with the same lead byte — `"$Sreact.fragment"` is a Symbol and `"$D…"` a Date — and a rule that
 * guessed which letters are ids from a sample of one stream is §RUN, DON'T MATCH's pattern recognition. A
 * string is a reference exactly when the rest of it NAMES A ROW THIS STREAM DEFINES, which the stream itself
 * answers: `Sreact.fragment` is not a row id in any stream, so nothing has to know what `$S` means. */
typedef struct { const char *id, *payload; size_t id_n, payload_n; uint8_t walking; } FlightRow;
typedef struct { FlightRow *v; size_t n, cap; } FlightRows;

static void flight_rows_add(FlightRows *rs, const char *id, size_t id_n, const char *payload, size_t payload_n)
{
    if (rs->n == rs->cap) {
        size_t ncap = rs->cap ? rs->cap * 2 : 32;
        FlightRow *v = (FlightRow *)realloc(rs->v, ncap * sizeof *v);
        CHECK(v != NULL, "reply_decode: OOM indexing a Flight stream's rows");
        rs->v = v;
        rs->cap = ncap;
    }
    rs->v[rs->n].id = id;
    rs->v[rs->n].id_n = id_n;
    rs->v[rs->n].payload = payload;
    rs->v[rs->n].payload_n = payload_n;
    rs->v[rs->n].walking = 0;
    rs->n++;
}

static FlightRow *flight_row_of(FlightRows *rs, const char *id, size_t id_n)
{
    size_t i;
    if (id_n == 0) return NULL;
    for (i = 0; i < rs->n; i++)
        if (rs->v[i].id_n == id_n && !memcmp(rs->v[i].id, id, id_n)) return &rs->v[i];
    return NULL;
}

/* THE WALK OVER ONE CLIENT REFERENCE'S CHUNK LIST — ITERATIVE, over a worklist of owned values.
 *
 * IT IS A WORKLIST AND NOT RECURSION because the nesting is the SERVER'S: a chunk entry is
 * `[entry, [dependencies…], [sizes…]]` in the streams these sites serve, and a stream is free to nest deeper.
 * Recursing on it would put a remote party in charge of this process's C stack, which is the one thing
 * §C-stack says a depth may never be.
 *
 * A ROW IS ENTERED ONCE PER REFERENCE, marked while it is on the worklist, so a stream whose rows refer to
 * each other in a cycle terminates. The marks are cleared before each reference rather than left set, because
 * a row shared by twenty references is legitimately walked by all twenty — a mark that survived would be a
 * seen-set deciding what the NEXT reference is allowed to learn.
 *
 * EVERY STRING UNDER THE CHUNK LIST IS A CHUNK NAME OR A REFERENCE TO ONE, which is what the field IS; the
 * numbers beside them are sizes and are not walked because a number is not an address. record_chunk decides
 * what is addressable, and a bare (asset-prefix-relative) name is refused there for the reason stated at it. */
static void walk_chunks(JSContext *ctx, const UrlRecord *base, FlightRows *rs, JSValue root)
{
    JSValue *work = NULL;
    size_t n = 0, cap = 0, i;

    for (i = 0; i < rs->n; i++) rs->v[i].walking = 0;

#define FLIGHT_PUSH(v) do {                                                                      \
        if (n == cap) {                                                                          \
            size_t nc = cap ? cap * 2 : 8;                                                       \
            JSValue *w = (JSValue *)realloc(work, nc * sizeof *w);                               \
            CHECK(w != NULL, "reply_decode: OOM walking a Flight client reference's chunk list"); \
            work = w; cap = nc;                                                                  \
        }                                                                                        \
        work[n++] = (v);                                                                         \
    } while (0)

    FLIGHT_PUSH(root);
    while (n) {
        JSValue v = work[--n];

        if (JS_IsString(v)) {
            const char *s = JS_ToCString(ctx, v);
            CHECK(s != NULL, "reply_decode: OOM reading a Flight chunk entry");
            if (s[0] == '$') {
                FlightRow *r = flight_row_of(rs, s + 1, strlen(s + 1));
                if (r && !r->walking) {
                    /* THE REFERENCED ROW'S PAYLOAD IS JSON, run through the REAL codec like every other value
                       here. A row whose payload is a tagged one (`I[`, `HL[`) is not JSON and parses to an
                       exception, which is an ordinary fact about a stream whose rows are not all values. */
                    JSValue rv = JS_ParseJSON(ctx, r->payload, r->payload_n, "<flight>");
                    if (JS_IsException(rv)) JS_FreeValue(ctx, JS_GetException(ctx));
                    else { r->walking = 1; FLIGHT_PUSH(rv); }
                }
            } else {
                record_chunk(ctx, base, s);
            }
            JS_FreeCString(ctx, s);
        } else if (JS_IsArray(v)) {
            int64_t len = 0, k;
            if (JS_GetLength(ctx, v, &len) < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); len = 0; }
            for (k = 0; k < len; k++) {
                JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)k);
                if (JS_IsException(e)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
                FLIGHT_PUSH(e);
            }
        }
        JS_FreeValue(ctx, v);
    }
#undef FLIGHT_PUSH
    free(work);
}

/* ONE `I[…]` PAYLOAD: the real JSON codec runs on it (§A JS-engine encoding builtin is modeled FAITHFULLY — the
   engine never hand-rolls a parser beside `JSON.parse`), and a payload that is not JSON is an ordinary fact
   about a stream half of whose rows are not. Element 1 is the chunk list and is handed to the walk WHOLE,
   because it may itself be a reference rather than an array. */
static void learn_client_reference(JSContext *ctx, const UrlRecord *base, FlightRows *rs,
                                   const char *payload, size_t n)
{
    JSValue arr, chunks;

    DCHECK(n >= 2 && payload[0] == 'I' && payload[1] == '[',
           "a Flight row was read as a client reference without carrying one — the caller tests the `I[` tag "
           "and this parses from the bracket, so a payload that does not start there is being parsed one byte "
           "off and would yield a JSON value belonging to no row");
    arr = JS_ParseJSON(ctx, payload + 1, n - 1, "<flight>");
    if (JS_IsException(arr)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return; }

    chunks = JS_GetPropertyUint32(ctx, arr, 1);
    if (JS_IsException(chunks)) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, arr); return; }
    walk_chunks(ctx, base, rs, chunks);   /* consumes `chunks` */
    JS_FreeValue(ctx, arr);
}

/* OVER A LENGTH AND NOT A NUL, because what arrives is a BYTE SEQUENCE decoded to text: §6's UTF-8 decode
   preserves a U+0000, and a `strchr` walk would have read the rest of a stream that contains one as absent.
   TWO PASSES, because a reference may name a row that has not been seen yet — see the row table above. */
static void learn_flight(JSContext *ctx, const UrlRecord *base, const char *body, size_t body_n)
{
    FlightRows rows = { 0 };
    const char *p, *body_end = body + body_n;
    size_t i;

    for (p = body; p < body_end; ) {
        const char *nl = memchr(p, '\n', (size_t)(body_end - p));
        size_t line_n = nl ? (size_t)(nl - p) : (size_t)(body_end - p);
        const char *colon;
        size_t end = line_n;

        if (end && p[end - 1] == '\r') end--;   /* a CRLF-framed stream is the same stream */
        colon = memchr(p, ':', end);
        if (colon && colon > p && hex_row_id(p, (size_t)(colon - p)))
            flight_rows_add(&rows, p, (size_t)(colon - p), colon + 1, end - (size_t)(colon + 1 - p));
        if (!nl) break;
        p = nl + 1;
    }

    for (i = 0; i < rows.n; i++)
        if (rows.v[i].payload_n >= 2 && rows.v[i].payload[0] == 'I' && rows.v[i].payload[1] == '[')
            learn_client_reference(ctx, base, &rows, rows.v[i].payload, rows.v[i].payload_n);

    free(rows.v);
}

/* ── the entry ───────────────────────────────────────────────────────────────────────────────────────────── */

void reply_decode_learn(JSContext *ctx, const char *method, const char *url, JSValueConst reply)
{
    MimeType computed;
    UrlRecord base;
    char *ct, *essence;
    JSValue bodyv;
    const uint8_t *body;
    size_t body_n = 0;

    DCHECK(method != NULL && *method,
           "a reply was read for its content while naming no method — the request it answers was owed under a "
           "(method, url) pair, and the asset verdict below is filed under that same pair, so a reply that has "
           "lost half of its name can only retract the wrong record or none");
    DCHECK(url != NULL && *url,
           "a reply was read for its content while naming no address — every relative address inside a body "
           "resolves against the URL that was fetched, and a reply with none would resolve them against "
           "nothing and file the result under an endpoint nobody requested");
    /* A NETWORK ERROR IS AN ANSWER, and it carries no record at all. This is a shape test on what the host
       delivered rather than an `if` past a broken invariant. */
    if (!JS_IsObject(reply)) return;

    /* THE BYTES, AS BYTES. This read the record's `body` as a STRING, which §2.2.5 says a body is not; the
       record carries a byte sequence now (core/fetch/fetch.h) and the decode below is this file's own. */
    bodyv = fetch_reply_body(ctx, reply);
    if (JS_IsException(bodyv)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    body = fetch_body_bytes(ctx, bodyv, &body_n);

    /* WHAT THE HOST DECIDED THIS RESOURCE IS, READ OFF THE RECORD (see `is_asset` above). The PARSE back into
       a §4.1 record is this file's, because the group questions below are asked of a record and not of a
       string; the DECISION it parses is not. */
    ct = fetch_reply_computed_type(ctx, reply);
    if (!mime_type_extract(&computed, ct)) {
        /* FAILURE IS A VALUE, and it is that nothing named this resource — neither the server nor its bytes.
           A reply with no type is not a Flight stream, that protocol being recognised by its type, so there is
           nothing here to read and no sniff this process may run to find out otherwise. */
        free(ct);
        mime_type_free(&computed);
        JS_FreeValue(ctx, bodyv);
        return;
    }
    free(ct);

    /* AND THE SURFACE THE RULE IS ABOUT IS TOLD. The `return` alone is what stood here, and it is only half of
       "Static assets are NEVER endpoints … but still drive the code path": declining to LEARN FROM a body says
       nothing about the @H record the request already minted, so a document of nine `<img>` elements published
       nine endpoints that were files. The verdict goes to the surface that holds the record before this
       returns, keyed on the pair the request was owed under. */
    if (is_asset(&computed)) {
        endpoint_mark_asset(method, url);
        mime_type_free(&computed);
        JS_FreeValue(ctx, bodyv);
        return;
    }

    essence = mime_type_essence(&computed);
    CHECK(essence, "reply_decode: OOM reading a reply's computed essence");
    mime_type_free(&computed);

    if (!strcmp(essence, "text/x-component")) {
        /* AND THE TEXT OF IT, decoded HERE. A Flight stream is `text/x-component` — text, whose charset React
           does not label and whose default is therefore UTF-8 — so §6's UTF-8 decode is the algorithm this
           reader owes, run on the bytes rather than inherited from whoever built the record. */
        size_t text_n = 0;
        char *text = encoding_utf8_decode((const char *)body, body_n, &text_n);
        CHECK(text, "reply_decode: OOM decoding a Flight stream's bytes");
        if (url_parse(&base, url, strlen(url), NULL))
            learn_flight(ctx, &base, text, text_n);
        else
            DFAIL("a reply arrived naming an address the URL parser rejects — every address this engine parks "
                  "on was built by a component that parsed it first, so a failure here is a park recorded from "
                  "a string nothing in this engine produced");
        url_record_free(&base);
        free(text);
    }
    free(essence);
    JS_FreeValue(ctx, bodyv);
}
