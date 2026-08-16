/* WHAT A RESOURCE IS FOR — does this response body carry API structure to learn from, or is it a static asset
 * whose bytes a decoder turns into pixels, samples, glyphs or a program?
 *
 * WHOSE QUESTION THIS IS. It is not WHATWG's: MIME Sniffing §7 answers what a resource IS and stops there, and
 * §4.6's groups are the vocabulary but not the judgement. It is this product's, and it is the one CLAUDE.md
 * §Attacker-sources states — "Static assets are NEVER endpoints (magic-byte + content-type, not URL suffix) but
 * still drive the code path". So the judgement is here, in its own component, and every rule below is written
 * over §7's COMPUTED type and §4.6's groups rather than over a string a caller cut at a semicolon.
 *
 * WHY IN THE BROWSER PROCESS, WHICH IS THE WHOLE POINT OF THE MOVE. The question needs the resource's ACTUAL
 * type, which means reading its bytes, which is sniffing — and sniffing is the network service's, for the
 * reason mime_sniff.h and solver/reply_decode.c both give at length: a renderer that classifies for itself can
 * classify, and then MINE, a cross-origin body a real renderer would have been handed empty. The ruling this
 * file implements is one sentence — type checking is safeFetch's job and safeFetch is the only source of
 * sniffing — and a second implementation anywhere else is what it forbids.
 *
 * WHAT IT REPLACED. `sniffBinaryMagic`, `_sniffTextAssetSignature` and `classifyResponseAsset` in
 * `extension/lib/discovery.js`, called from `lib/response-decode.js` to gate every learning call in the
 * passive-capture path. All three are deleted. Rule by rule, what happened to each:
 *   - the magic-byte table WAS §6.1/§6.2/§6.4, hand-rolled and incomplete (no §6.2.1 mp4 box walk, no §6.2.2
 *     EBML DocType, `application/gzip` where the standard's table says `application/x-gzip`). It is now those
 *     tables, run through mime_sniff.c.
 *   - `<!doctype html` / `<html` WAS §7.1's scriptable table, two rows of nineteen. Same table now, all rows.
 *   - `%PDF-`, `%!PS` are §7.1's rows too.
 *   - the WOFF/WOFF2/TTF/OTF rows were §6.3's FONT table, which mime_sniff.c does not implement and states why:
 *     nothing in §7 or §7.1 runs it, only §8's font context does, and this engine has no font context. A font
 *     is caught here by its DECLARED type through §4.6's font group, which is how a served font arrives; a
 *     font MISLABELLED as something else now computes as `application/octet-stream` and is classified as API
 *     data, which is over-learning rather than under-learning and is the direction CLAUDE.md errs in.
 *   - the SVG, CSS-@-rule, WebVTT, HLS (`#EXTM3U`), DASH (`<MPD`) and SMIL sniffs are DELETED and not ported.
 *     None is in any standard, no browser sniffs them, and §RUN, DON'T MATCH names the shape: a server that
 *     serves an SVG STATES `image/svg+xml`, which §4.6's image group already answers. CSS survives as a rule
 *     over the DECLARED type, below, because a stylesheet genuinely has no schema and `text/css` is what a
 *     server sends; what does not survive is guessing CSS from a leading `@font-face`.
 *   - `ctAssetMimes` — a five-entry JavaScript-MIME list plus `text/css`, trusted only when the body did not
 *     start with `{` or `[` — is §4.6's JavaScript group (all sixteen essences, in mime_type.c) plus Chromium's
 *     JSON sniff (json_sniff.c) in place of the first-character test.
 *   - `kind: "empty"` is GONE rather than moved. Its caller collapsed it into "not an asset" at both reads, so
 *     it was a third value nobody could observe; an empty body reaches the last rule and is API data, which is
 *     what that caller already did with it.
 *
 * WHAT IT DOES NOT ANSWER, AND WHY THAT IS NOT A GAP. Whether the body is EXECUTED. That is CORB's decision
 * (corb.c) on the load path, over the same §7 answer, and the two are deliberately separate: a JavaScript chunk
 * served as `application/octet-stream` is ALLOWED by CORB and executed, and is classified here as API data
 * because §7 never upgrades a resource INTO a scriptable type — §7.2's note is that the refusal is the whole
 * point. Neither rule can suppress the other's surface. */
#ifndef ENGINE_HOST_BROWSER_PROCESS_NETWORK_RESOURCE_KIND_H
#define ENGINE_HOST_BROWSER_PROCESS_NETWORK_RESOURCE_KIND_H

#include <stdbool.h>
#include <stddef.h>

/* Long enough for the longest reason below (`sniffed-application/x-rar-compressed`, 36) with room for a rule
   added later, and bounded because the record is a fixed struct the caller owns. */
#define RESOURCE_KIND_REASON_MAX 64

typedef struct {
    bool asset;                              /* the body has no request shape, no schema and no address in it */
    char reason[RESOURCE_KIND_REASON_MAX];   /* which rule decided; one of the fixed strings resource_kind.c
                                                spells. It is the record's SECOND field and not a third,
                                                because §7's computed essence has no reader on this path and a
                                                field written for nobody is the contract CLAUDE.md calls
                                                greppable. The rule that fired is what the popup tags a method
                                                with and what a probe row compares. */
} ResourceKind;

/* `content_type_value` is the response's `Content-Type` as "get a header" joined it, or NULL for absent —
   §5.1's "the supplied MIME type is undefined", a positive statement and never an empty string standing in.
   `no_sniff` is Fetch's determine-nosniff over the same response (nosniff.h computes it from the header value).
   `opaque` is Fetch §2.2.6: the response is an OPAQUE FILTERED RESPONSE, whose body is null and whose header
   list is empty by construction, so there is nothing in it to learn from and no type to be wrong about. It is a
   fact the trusted zone HOLDS — it has the Response object and this program has neither a URL nor a principal —
   which is the same shape `same_origin` has in corb.h.
   `header` is the resource header, the response's first bytes, up to MIME_SNIFF_HEADER_MAX of them. */
void resource_kind_classify(ResourceKind *out, const char *content_type_value, bool no_sniff, bool opaque,
                            const unsigned char *header, size_t header_n);

#endif
