/* What a reply body teaches — see reply_decode.h. Stateless: every endpoint it learns goes to solver/endpoint.c. */
#include "solver/reply_decode.h"
#include "solver/endpoint.h"
#include "core/mime/mime_type.h"     /* Fetch §4's extract a MIME type — the PARSE, which is the renderer's */
#include "core/encoding/encoding.h"   /* §6's UTF-8 decode: what a `text/x-component` stream's bytes are read as */
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/url/url.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* §4.6's ASSET GROUPS, together: a body in one of them is BYTES a decoder turns into pixels or samples, and
   there is no request shape, no schema and no address inside it.
   ASKED OF THE SUPPLIED TYPE — the server's own statement — AND NOT OF §7's COMPUTED ONE, WHICH THIS PROCESS
   MAY NOT DECIDE. Sniffing is a NETWORK-side algorithm: a real browser runs §7 in the network service, CORB/ORB
   gates on its result, and the renderer is TOLD a computed type it never derives from response bytes. This file
   is the renderer. An engine that sniffs for itself can classify — and then MINE — a cross-origin body a real
   renderer would have been handed as an opaque, empty response, and the endpoints it takes out of one are
   surface the page could never have obtained, reported as a finding about the page. That is the same rule
   CLAUDE.md states for active discovery ("CORS-bounded both ways") arriving from the other direction.
   WHAT IT COSTS TODAY, MEASURED FROM §7 RATHER THAN ASSUMED: nothing. §7's computed type differs from the
   supplied one only where a rule fires — steps 1-2 (no type at all, `unknown/unknown`, `application/unknown`,
   the any-type `*` over `*`, or the Apache bug), step 5 (`text/html` feed-or-HTML), and steps 6-7 (an
   image or audio-or-video supplied type re-matched against §6.1/§6.2's patterns). The ONE consumer below
   keys on a supplied `text/x-component`, which no rule in §7 touches, so §7 reaches step 8 and answers the supplied type
   unchanged for every body this file has ever learned from. The asset early-out is the only other reader, and a
   body it would have skipped only on magic bytes is one whose essence is not `text/x-component` either way. So
   the sniff decided ZERO of the endpoints in this engine's @H surface, and removing it from this process
   removes no measurement — it removes a vote the renderer is not entitled to.
   WHERE THE COMPUTED TYPE COMES FROM WHEN IT COMES: the trusted zone, on the record, stamped like the sender's
   origin on a cross-document message — and it is NOT a field yet, because no zone can write one. §7 cannot live
   in `safe-fetch.js` (CLAUDE.md: the only irreducible JS is a bridge, never logic) and it cannot live here, so
   it belongs to a BROWSER-PROCESS instance that does not exist. A second wasm-ld link out of this same object
   set was tried as one and is deleted: it produced a second artifact, not a second process — same objects, one
   realm, the host holding an exported HEAPU8 over each — so what remains to be built is an instance across a
   real module boundary (`core/mime/mime_sniff.h` names the shape). A `computedType` field added now would be
   read here and written nowhere, which is the exact contract CLAUDE.md calls greppable and mechanical, with a
   DCHECK that could only fire on every reply for a value that decides nothing. */
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
 * A `json` row's SHAPE is schema inference (jsaudit step 3's moat_schema.c); an `E[` row's message is credential
 * extraction (step 3's keys.js → moat.c). Reading them here would put two components' work in a third. */

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
    HeaderList hl = { 0 };
    MimeType supplied;
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

    /* Fetch §4's EXTRACT A MIME TYPE over the record's own header list — a PARSE of what the server STATED,
       which is legitimately the renderer's because the same record is what `Blob.type`, `File.type` and
       `accept` matching are read off. What is NOT the renderer's is §7's sniff over the body's bytes; see
       `is_asset` above for why, and for the measurement that says this file never depended on it. */
    fetch_reply_header_list(ctx, reply, &hl);
    ct = header_list_get(&hl, "content-type");   /* NULL is Fetch's "values is null" = the supplied type is undefined */
    if (!mime_type_extract(&supplied, ct)) {
        /* FAILURE IS A VALUE, and it is the server having named nothing. A reply with no parseable type is not
           a Flight stream — that protocol is recognised by the type its server states — so there is nothing
           here to read and no sniff this process may run to find out otherwise. */
        free(ct);
        header_list_free(&hl);
        mime_type_free(&supplied);
        JS_FreeValue(ctx, bodyv);
        return;
    }
    free(ct);
    header_list_free(&hl);

    if (is_asset(&supplied)) { mime_type_free(&supplied); JS_FreeValue(ctx, bodyv); return; }

    essence = mime_type_essence(&supplied);
    CHECK(essence, "reply_decode: OOM reading a reply's supplied essence");
    mime_type_free(&supplied);

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
