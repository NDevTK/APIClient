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
 * AND IT IS IN THE WRONG PROCESS. This paragraph used to say the trusted zone read a reply body with
 * `Response.text()`, so a PNG's 0x89 was already U+FFFD before any C here saw it — that is fixed, the reply
 * record carries §2.2.5's byte sequence now (core/fetch/fetch.h), and fixing it is what made the real problem
 * visible: THE BYTES ARRIVING IS NOT THE SAME AS THIS PROCESS BEING ENTITLED TO SNIFF THEM.
 *
 * §7 is a NETWORK-side algorithm. In a real browser it runs in the network service; CORB/ORB gates on its
 * result; and the renderer is TOLD a computed MIME type it never derives from response bytes. Everything in
 * `engine/host` is the RENDERER — one WASM instance per origin-keyed agent cluster, running the untrusted
 * bundle (SECURITY.md). A renderer that computes its own type can classify, and then MINE, a cross-origin body
 * that a real renderer would have been handed as an opaque, empty response, and the endpoints taken out of one
 * are surface the page could never have obtained, reported as a finding about the page. It is also a DUPLICATE:
 * `extension/lib/safe-fetch.js` already classifies for CORB (`_jsMime`, `_corbProtectedMime`,
 * `_corbAllowsScript(mime, nosniff, body, …)`, which takes the body precisely because that decision needs the
 * bytes), so two answers to "what is this body" sat on opposite sides of the trust boundary and could disagree.
 *
 * SO NOTHING IN THIS PROCESS MAY CALL §7, AND `mime_sniff_compute` DFAILs SAYING SO. The implementation stays
 * because it is not wrong — it is the standard's own byte tables, written against the spec — it is HOUSED
 * wrongly. Its home is a BROWSER-PROCESS instance: the trusted-zone counterpart to the per-document renderer
 * instances, which does not exist yet and which `safe-fetch.js`'s hand-written SOP/CORS check belongs in beside
 * this. Until it does, the computed type is a fact NO zone can state, so it is not a field on the reply record
 * either — a reader with no writer is the contract CLAUDE.md calls greppable, and adding one here would be that
 * defect with a DCHECK attached.
 *
 * AND A SECOND LINK IS NOT THAT INSTANCE — this file was moved into an `engine/host/browser_process/` linked as
 * its own artifact, and that is deleted rather than kept, so the reason is recorded HERE where the next attempt
 * starts. WASM MODULES MUST NOT BE LINKED into a boundary: a second wasm-ld invocation over the same shared
 * object set emits a second artifact out of the same objects, so the trusted program was in fact built from the
 * whole engine and was the LARGER of the two; the difference in size was dead-stripping, not isolation. Both
 * Modules instantiate in the offscreen's own realm — no Worker anywhere, `extension/bridge.js` importing the
 * glue into that realm, the host holding an exported HEAPU8 over each — so what a link boundary keeps out of a
 * program is symbols, and what a trust boundary must keep out is a reader of another program's memory. The
 * boundary that is real is `extension/renderer.html` + `extension/renderer-host.js`: a frame with a unique
 * OPAQUE origin, cross-origin to the extension origin, which is what lets Site Isolation put it in its own
 * OS-sandboxed process, and across which everything the instance runs is HANDED to it. A trusted counterpart to
 * that is provisioned the same way or it is not provisioned at all.
 *
 * §4's RECORD next door is a different question and stays: `mime_type_extract` PARSES what a server STATED, and
 * that record is page-observable through `Blob.type`, `File.type`, `DataTransferItem.type` and `accept`
 * matching, so the renderer owes it. Parse is the renderer's; sniff is the network's.
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
