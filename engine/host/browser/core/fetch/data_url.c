/* THE data: URL PROCESSOR — WHATWG Fetch §6, which §4.3's scheme fetch runs for the "data" scheme.
 *
 * WHY IT EXISTS. `data:` was not a scheme this engine knew, so a `data:` URL went out as an HTTP REQUEST: the
 * WPT runner's `xhr/template-element.html` put
 *     GET text/xml,<template xmlns='…'><test/></template> HTTP/1.1
 * on the wire with an EMPTY `Host:`, and wptserve answered `400 Bad request syntax`. A URL whose bytes are
 * already in the URL cannot be requested from anybody, and the request that was fabricated for it went to a
 * server that had never heard of it. §4.3 is where that stops: the switch on the request's URL's scheme
 * answers "data" from §6 and never reaches HTTP fetch.
 *
 * THE TWO ORDERS THAT ARE THE ALGORITHM. Both are places a hand-rolled version — a split on the first comma —
 * gets a real page wrong:
 *
 *   1. PERCENT-DECODING COMES FIRST, BASE64 DECODING SECOND (steps 10 and 11). The body is percent-decoded
 *      unconditionally, and only then, if the MIME part ends with `;base64`, is the RESULT base64-decoded.
 *      `data:text/plain;base64,%41%41%41%41` is therefore "AAAA" decoded as base64 (four bytes in, three out),
 *      not the percent-decoding of a base64 decode. The MIME part is NEVER percent-decoded.
 *   2. THE BASE64 SUFFIX IS TESTED ON THE WHITESPACE-STRIPPED MIME PART, and it is `;`, then zero or more
 *      U+0020, then an ASCII case-insensitive "base64" — so `data:text/plain;BASE64,QQ==` is base64 and
 *      `data:text/plain;base64x,QQ==` is not. The six code points, the spaces and the `;` come off in that
 *      order (steps 11.3-11.5), which is what leaves `text/plain` behind rather than `text/plain;`.
 *
 * NEITHER CODEC IS WRITTEN HERE. CLAUDE.md's bind-before-build: the percent-decoding is the URL Standard
 * §1.3's, which core/url/url.c already exports as `url_percent_decode` (the one `URLSearchParams` parses
 * with), and the forgiving-base64 decode is Infra's, which the fork already runs for `atob` — exported as
 * `JS_Base64Decode`/`JS_Base64DecodedMax` for exactly this reason. A second base64 in this file would be a
 * second thing to be wrong about padding.
 *
 * WHAT IS A FAILURE AND WHAT IS NOT. Only two inputs fail: a URL with no U+002C at all (step 7), and a
 * `;base64` body the forgiving decode rejects (step 11.2). Everything else RECOVERS — an unparsable MIME type
 * becomes text/plain;charset=US-ASCII (step 13), a MIME part starting with `;` gets `text/plain` prepended
 * (step 12), and an empty MIME part is that same default. §4.3 turns a failure into a network error, which
 * `fetch` rejects with a TypeError and XHR reports as its request error steps; a recovery is a 200 response. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/fetch/data_url.h"

/* Infra's "ASCII whitespace" — TAB, LF, FF, CR, SP. It is NOT Fetch's "HTTP whitespace" (which has no FF),
   and step 6 is the one place in this algorithm that uses it. The standard's own note says only U+0020 can
   actually survive here, because the URL serializer has percent-encoded every C0 control in the path. */
static bool data_ascii_ws(unsigned char c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

static bool data_ci_equal(const char *s, const char *lower, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

void data_url_struct_free(DataUrlStruct *s)
{
    mime_type_free(&s->mime);
    free(s->body);
    s->body = NULL;
    s->body_len = 0;
}

bool data_url_process(const UrlRecord *data_url, DataUrlStruct *out)
{
    char *input;
    size_t len, p, ms, me;
    bool base64 = false;

    mime_type_init(&out->mime);
    out->body = NULL;
    out->body_len = 0;

    /* Step 1's ASSERT is the standard's own, which makes it a DCHECK and not a branch: the switch in §4.3 is
       what routes here, so a caller that arrives with another scheme has mis-read its own request. */
    DCHECK(data_url->scheme != NULL && !strcmp(data_url->scheme, "data"),
           "the data: URL processor was handed a URL whose scheme is not \"data\" — §6 step 1 asserts it, and "
           "§4.3's switch on the scheme is the only thing that may call this");
    /* Steps 2-3: the URL SERIALIZED with the fragment excluded, less the leading "data:". It is the
       serialization and not the caller's original string because the parser is what decided which code points
       are percent-encoded, and step 10 decodes exactly those. */
    input = url_serialize(data_url, /*exclude_fragment*/ true);
    CHECK(input != NULL, "data: URL: OOM serializing the URL to process");
    len = strlen(input);
    DCHECK(len >= 5 && !strncmp(input, "data:", 5),
           "a URL whose scheme is \"data\" serialized without a leading \"data:\" — the serializer writes the "
           "scheme and a U+003A first, and step 3 removes exactly those");
    p = 5;

    /* Step 5: the MIME part is everything up to the first U+002C. */
    ms = p;
    while (p < len && input[p] != ',') p++;
    me = p;
    /* Step 6: strip leading and trailing ASCII whitespace. */
    while (ms < me && data_ascii_ws((unsigned char)input[ms])) ms++;
    while (me > ms && data_ascii_ws((unsigned char)input[me - 1])) me--;
    /* Step 7: no U+002C anywhere is the first of the algorithm's two failures. */
    if (p >= len) { free(input); return false; }
    p++;                                                        /* step 8 */

    /* Steps 9-10: the rest of the URL, PERCENT-DECODED. This runs whether or not the body is base64 — that is
       the order, and it is why a percent-encoded base64 body works. */
    out->body = url_percent_decode(input + p, len - p, &out->body_len);
    CHECK(out->body != NULL, "data: URL: OOM percent-decoding the body");

    /* Step 11: does the MIME part end with U+003B, zero or more U+0020, and "base64" (ASCII case-insensitive)? */
    {
        size_t e = me;
        if (e - ms >= 6 && data_ci_equal(input + e - 6, "base64", 6)) {
            size_t q = e - 6;
            while (q > ms && input[q - 1] == 0x20) q--;
            if (q > ms && input[q - 1] == ';') {
                base64 = true;
                me = q - 1;                                     /* steps 11.3-11.5, in that order */
            }
        }
    }
    if (base64) {
        /* Step 11.1-11.2: the isomorphic decode is a no-op on a byte sequence held as bytes, and the decode
           itself is the ENGINE'S OWN forgiving-base64 — the one `atob` runs. A rejection here is the
           algorithm's second and last failure. */
        size_t cap = JS_Base64DecodedMax(out->body_len);
        uint8_t *dst = malloc(cap ? cap : 1);
        size_t n;
        int err = 0;

        CHECK(dst != NULL, "data: URL: OOM decoding a base64 body");
        n = JS_Base64Decode(dst, cap, out->body, out->body_len, &err);
        if (err) {
            free(dst);
            free(input);
            free(out->body);
            out->body = NULL;
            out->body_len = 0;
            return false;
        }
        free(out->body);
        out->body = (char *)dst;
        out->body_len = n;
    }

    /* Steps 12-13: a MIME part that starts with U+003B is `text/plain` plus its parameters, and one that will
       not parse at all — including the empty one a bare `data:,x` leaves — is text/plain;charset=US-ASCII. */
    {
        char *mime;
        size_t mn = me - ms;
        bool ok;

        mime = malloc(mn + 11);
        CHECK(mime != NULL, "data: URL: OOM building the MIME type to parse");
        if (mn && input[ms] == ';') {
            memcpy(mime, "text/plain", 10);
            memcpy(mime + 10, input + ms, mn);
            mn += 10;
        } else {
            memcpy(mime, input + ms, mn);
        }
        mime[mn] = 0;
        ok = mime_type_parse(&out->mime, mime, mn);
        free(mime);
        if (!ok) {
            static const char DEFAULT_MIME[] = "text/plain;charset=US-ASCII";
            mime_type_free(&out->mime);
            ok = mime_type_parse(&out->mime, DEFAULT_MIME, sizeof(DEFAULT_MIME) - 1);
            DCHECK(ok, "the data: URL processor's own default MIME type did not parse — step 13 names a "
                       "literal the parser must accept, so this is the parser disagreeing with §4.4");
            (void)ok;
        }
    }
    free(input);
    return true;
}
