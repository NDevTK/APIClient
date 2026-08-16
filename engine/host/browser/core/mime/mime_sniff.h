/* MIME SNIFFING — WHATWG MIME Sniffing §5–§7: what a resource ACTUALLY is, decided from the type its server
 * claimed and the first bytes it sent. §4's RECORD lives next door in mime_type.h; this is the half that
 * COMPUTES one.
 *
 * WHY IT IS HERE. It was `sniffBinaryMagic`, `_sniffTextAssetSignature` and `classifyResponseAsset` in
 * `extension/lib/discovery.js` — jsaudit step 1 — and it is not discovery semantics at all: deciding what a
 * response IS is a thing every browser does to every response, and the algorithm has a standard. The JS was a
 * hand-rolled list beside that standard, which is the redundant second implementation §A-JS-engine-encoding-
 * builtin-is-modeled-FAITHFULLY forbids in the codec case for exactly the same reason.
 *
 * WHAT DID NOT COME WITH IT, AND WHY THAT IS THE POINT. The JS also sniffed SVG, CSS, WebVTT, HLS playlists and
 * DASH manifests out of the leading characters of a body — `lower.startsWith("<svg")`, a `@font-face` regex,
 * the literal "#EXTM3U". None of those are in this standard, no browser sniffs them, and CLAUDE.md §RUN, DON'T
 * MATCH names the shape by name: "no regex/name/identifier matching, scoring, heuristics". A server that serves
 * an SVG STATES `image/svg+xml`, and §7 returns exactly that. So those rows are DELETED rather than ported, and
 * the same sentence covers the JS's `ctAssetMimes` table, which trusted a declared JavaScript/CSS type only
 * when the body did not start with `{` or `[` — a guess layered on top of a statement.
 *
 * WHAT THE BYTES CAN AND CANNOT SAY HERE. The trusted zone reads a reply body with `Response.text()`, so what
 * crosses to the engine is a UTF-8 DECODE of the resource and not the resource: a PNG's 0x89 is already
 * U+FFFD by the time any C in this process sees it. That is a property of the host's reply record, not of this
 * algorithm, and it is why the caller passes whatever bytes it actually holds and why §7's dominant path here
 * is the SUPPLIED type — which is a statement the server made and needs no bytes to read. The pattern tables
 * are still exact, because the day the record carries bytes they must already be right.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_MIME_MIME_SNIFF_H
#define ENGINE_HOST_BROWSER_CORE_MIME_MIME_SNIFF_H

#include "core/mime/mime_type.h"

/* §5.2's RESOURCE HEADER is the first 1445 bytes of the resource. A caller holding more passes more and this
   component reads no further; a caller holding fewer passes fewer, which is §5.2's "the end of the resource is
   reached" and not an error. */
#define MIME_SNIFF_HEADER_MAX 1445

/* §7's MIME TYPE SNIFFING ALGORITHM — the computed MIME type of a resource.
 *
 * `content_type_value` is §5.1's supplied-type input: the value of the LAST `Content-Type` header, joined as
 * "get a header" joins duplicates, or NULL for §5.1's "the supplied MIME type is undefined" (no such header, or
 * a value that is not a MIME type — `mime_type_extract` decides that here rather than at every call site).
 * NULL is therefore a positive statement and never a hole a caller filled in.
 *
 * `no_sniff` is §5's no-sniff flag. Its one source in a browser is `X-Content-Type-Options: nosniff`, so the
 * caller reads that header and states the answer; this component never reaches back into a header list for a
 * second fact about the same response.
 *
 * §5.1's CHECK-FOR-APACHE-BUG FLAG IS NOT AN ARGUMENT, because it is a property of the very string being
 * passed: it is set when the supplied type is EXACTLY one of four byte sequences, and asking the caller to
 * re-derive that from the value it already handed over is how two call sites come to disagree about one
 * response. It is computed below, from `content_type_value`, at the one place that holds it.
 *
 * `out` is initialised by this call and is the caller's to `mime_type_free`. It always ends up holding a
 * parsed record: §7 has no failure outcome — every path ends in a type, down to "application/octet-stream". */
void mime_sniff_compute(MimeType *out, const char *content_type_value, bool no_sniff,
                        const unsigned char *header, size_t header_n);

/* §6.1 / §6.2 / §6.4's PATTERN TABLES, each on its own — §7 and §7.1 both run them, over the same header, and
   §7.1 additionally runs the two tables that are its own. They are exposed because a CONTEXT-SPECIFIC sniff
   (§8.2's image context, §8.3's audio-or-video context) is the same table with a different surrounding
   algorithm, and a context that re-implements the table is the duplicate this file exists to remove.
   Each returns the row's MIME type as a §4.2 ESSENCE string owned by this component (a static literal, alive
   for the process), or NULL for the algorithms' "return undefined". */
const char *mime_sniff_image_pattern(const unsigned char *header, size_t header_n);
const char *mime_sniff_audio_video_pattern(const unsigned char *header, size_t header_n);
const char *mime_sniff_archive_pattern(const unsigned char *header, size_t header_n);

#endif
