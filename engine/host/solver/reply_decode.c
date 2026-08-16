/* What a reply body teaches — see reply_decode.h. Stateless: every endpoint it learns goes to solver/endpoint.c. */
#include "solver/reply_decode.h"
#include "solver/endpoint.h"
#include "core/mime/mime_sniff.h"
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/url/url.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* Fetch's "determine nosniff": the header's value, and specifically its FIRST token, ASCII case-insensitively
   equal to "nosniff". The first token, because `X-Content-Type-Options: nosniff, nosniff` is one header whose
   values `header_list_get` has already joined — the algorithm reads the first and stops. */
static bool nosniff_of(const char *v)
{
    size_t i = 0;
    static const char WANT[] = "nosniff";

    if (!v) return false;
    while (v[i] == 0x09 || v[i] == 0x20) i++;
    for (size_t k = 0; k < sizeof WANT - 1; k++) {
        char c = v[i + k];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != WANT[k]) return false;
    }
    i += sizeof WANT - 1;
    return v[i] == 0 || v[i] == 0x09 || v[i] == 0x20 || v[i] == ',';
}

/* §4.6's ASSET GROUPS, together: a body in one of them is BYTES a decoder turns into pixels or samples, and
   there is no request shape, no schema and no address inside it. Asked of the COMPUTED type, so a PNG served as
   `application/octet-stream` is still a PNG and a JSON body served by a CDN that labels everything
   `image/jpeg` is still JSON — which is exactly what "magic-byte + content-type, not URL suffix" means. */
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

static void learn_flight(JSContext *ctx, const UrlRecord *base, const char *body)
{
    const char *p = body;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t line_n = nl ? (size_t)(nl - p) : strlen(p);
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
    MimeType computed;
    UrlRecord base;
    char *ct, *xcto, *essence;
    JSValue bodyv;
    const char *body;
    size_t body_n;

    DCHECK(url != NULL && *url,
           "a reply was read for its content while naming no address — every relative address inside a body "
           "resolves against the URL that was fetched, and a reply with none would resolve them against "
           "nothing and file the result under an endpoint nobody requested");
    /* A NETWORK ERROR IS AN ANSWER, and it carries no record at all. This is a shape test on what the host
       delivered rather than an `if` past a broken invariant — the same reading discovery_reply takes. */
    if (!JS_IsObject(reply)) return;

    bodyv = JS_GetPropertyStr(ctx, reply, "body");
    if (JS_IsException(bodyv)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    body = JS_IsString(bodyv) ? JS_ToCString(ctx, bodyv) : NULL;
    JS_FreeValue(ctx, bodyv);
    if (!body) return;   /* a reply record with no text body: nothing was read off the wire to read here */
    body_n = strlen(body);

    fetch_reply_header_list(ctx, reply, &hl);
    ct = header_list_get(&hl, "content-type");         /* NULL is Fetch's "values is null" = the supplied type is undefined */
    xcto = header_list_get(&hl, "x-content-type-options");
    mime_sniff_compute(&computed, ct, nosniff_of(xcto),
                       (const unsigned char *)body,
                       body_n < MIME_SNIFF_HEADER_MAX ? body_n : MIME_SNIFF_HEADER_MAX);
    free(ct);
    free(xcto);
    header_list_free(&hl);

    if (is_asset(&computed)) { mime_type_free(&computed); JS_FreeCString(ctx, body); return; }

    essence = mime_type_essence(&computed);
    CHECK(essence, "reply_decode: OOM reading a reply's computed essence");
    mime_type_free(&computed);

    if (!strcmp(essence, "text/x-component")) {
        if (url_parse(&base, url, strlen(url), NULL))
            learn_flight(ctx, &base, body);
        else
            DFAIL("a reply arrived naming an address the URL parser rejects — every address this engine parks "
                  "on was built by a component that parsed it first, so a failure here is a park recorded from "
                  "a string nothing in this engine produced");
        url_record_free(&base);
    }
    free(essence);
    JS_FreeCString(ctx, body);
}
