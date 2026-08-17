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
 * `[moduleId, [chunk, …], exportName]` — and the chunks are the JavaScript the page will fetch to hydrate that
 * route. Those are addresses the server NAMED, which is why they are endpoints in the §H sense and why they
 * belong to the same surface forced execution's own call sites land in: a route the user never navigated to
 * still ships its chunk list in the payload of the route they did.
 *
 * THE OTHER ROW KINDS ARE NOT READ HERE, and that is a statement about ownership rather than about difficulty.
 * A `json` row's SHAPE is schema inference (jsaudit's `lib/schema.js` row → moat_schema.c); an `E[` row's
 * message is credential extraction (jsaudit's `lib/keys.js` row → moat.c). Reading them here would put two
 * components' work in a third. */

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
    /* GET, because a chunk is a script the page LOADS. There is no method anywhere in this file for the same
       structural reason discovery.c has none: §Attacker sources' "a state-mutating request is NEVER fired to
       learn" is a property of the shape rather than a check someone remembers to write. */
    endpoint_record(ctx, "GET", uv, NULL, 0);
    JS_FreeValue(ctx, uv);
    free(abs);
    url_record_free(&u);
}

/* ONE `I[…]` PAYLOAD: the real JSON codec runs on it (§A JS-engine encoding builtin is modeled FAITHFULLY — the
   engine never hand-rolls a parser beside `JSON.parse`), and a payload that is not JSON is an ordinary fact
   about a stream half of whose rows are not. */
static void learn_client_reference(JSContext *ctx, const UrlRecord *base, const char *payload, size_t n)
{
    JSValue arr, chunks, lenv;
    uint32_t len = 0, i;

    DCHECK(n >= 2 && payload[0] == 'I' && payload[1] == '[',
           "a Flight row was read as a client reference without carrying one — the caller tests the `I[` tag "
           "and this parses from the bracket, so a payload that does not start there is being parsed one byte "
           "off and would yield a JSON value belonging to no row");
    arr = JS_ParseJSON(ctx, payload + 1, n - 1, "<flight>");
    if (JS_IsException(arr)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return; }

    chunks = JS_GetPropertyUint32(ctx, arr, 1);
    if (JS_IsException(chunks)) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, arr); return; }
    if (JS_IsArray(chunks)) {
        lenv = JS_GetPropertyStr(ctx, chunks, "length");
        if (JS_ToUint32(ctx, &len, lenv) < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); len = 0; }
        JS_FreeValue(ctx, lenv);
        for (i = 0; i < len; i++) {
            JSValue cv = JS_GetPropertyUint32(ctx, chunks, i);
            if (JS_IsException(cv)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
            if (JS_IsString(cv)) {
                const char *s = JS_ToCString(ctx, cv);
                if (s) { record_chunk(ctx, base, s); JS_FreeCString(ctx, s); }
            }
            JS_FreeValue(ctx, cv);
        }
    }
    JS_FreeValue(ctx, chunks);
    JS_FreeValue(ctx, arr);
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

/* OVER A LENGTH AND NOT A NUL, because what arrives is a BYTE SEQUENCE decoded to text: §6's UTF-8 decode
   preserves a U+0000, and a `strchr` walk would have read the rest of a stream that contains one as absent. */
static void learn_flight(JSContext *ctx, const UrlRecord *base, const char *body, size_t body_n)
{
    const char *p = body, *body_end = body + body_n;

    while (p < body_end) {
        const char *nl = memchr(p, '\n', (size_t)(body_end - p));
        size_t line_n = nl ? (size_t)(nl - p) : (size_t)(body_end - p);
        const char *colon;
        size_t end = line_n;

        if (end && p[end - 1] == '\r') end--;   /* a CRLF-framed stream is the same stream */
        colon = memchr(p, ':', end);
        if (colon && colon > p && hex_row_id(p, (size_t)(colon - p))) {
            const char *payload = colon + 1;
            size_t pn = end - (size_t)(payload - p);
            if (pn >= 2 && payload[0] == 'I' && payload[1] == '[')
                learn_client_reference(ctx, base, payload, pn);
        }
        if (!nl) break;
        p = nl + 1;
    }
}

/* ── the entry ───────────────────────────────────────────────────────────────────────────────────────────── */

void reply_decode_learn(JSContext *ctx, const char *url, JSValueConst reply)
{
    MimeType computed;
    UrlRecord base;
    char *ct, *essence;
    JSValue bodyv;
    const uint8_t *body;
    size_t body_n = 0;

    DCHECK(url != NULL && *url,
           "a reply was read for its content while naming no address — every relative address inside a body "
           "resolves against the URL that was fetched, and a reply with none would resolve them against "
           "nothing and file the result under an endpoint nobody requested");
    /* A NETWORK ERROR IS AN ANSWER, and it carries no record at all. This is a shape test on what the host
       delivered rather than an `if` past a broken invariant — the same reading discovery_reply takes. */
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

    if (is_asset(&computed)) { mime_type_free(&computed); JS_FreeValue(ctx, bodyv); return; }

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
