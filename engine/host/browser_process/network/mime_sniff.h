/* MIME SNIFFING — WHATWG MIME Sniffing §5–§7: what a resource ACTUALLY is, decided from the type its server
 * claimed and the first bytes it sent. §4's RECORD lives in the renderer's core/mime/mime_type.h; this is the
 * half that COMPUTES one, and it is in the BROWSER PROCESS because §7 is the network service's algorithm.
 *
 * WHAT IT WAS WRITTEN AGAINST. `sniffBinaryMagic`, `_sniffTextAssetSignature` and `classifyResponseAsset` in
 * `extension/lib/discovery.js` — a hand-rolled magic-byte list beside a standard, which is the redundant second
 * implementation CLAUDE.md forbids in the codec case for exactly the same reason. That JS also sniffed SVG,
 * CSS, WebVTT, HLS playlists and DASH manifests out of the leading characters of a body —
 * `lower.startsWith("<svg")`, a `@font-face` regex, the literal "#EXTM3U". None of those are in this standard,
 * no browser sniffs them, and CLAUDE.md §RUN, DON'T MATCH names the shape: "no regex/name/identifier matching,
 * scoring, heuristics". A server that serves an SVG STATES `image/svg+xml`, and §7 returns exactly that. So
 * those rows are DELETED rather than ported, and the same sentence covers the JS's `ctAssetMimes` table, which
 * trusted a declared JavaScript/CSS type only when the body did not start with `{` or `[`.
 *
 * WHAT THIS FILE'S ARRIVAL ACTUALLY DELETED IS SOMEWHERE ELSE, and saying otherwise would be the stale-DFAIL
 * failure mode in the first paragraph of a new file. `discovery.js`'s classifier is still on disk and still
 * called: `_isAsset`/`_isBoringFetch` gate every learning call in `lib/response-decode.js`, and it answers a
 * DIFFERENT question — is this reply a static asset to skip — for which this program serves no entry. It leaves
 * with its caller at jsaudit step 2 and its row says so. What this arrival deleted is `safe-fetch.js`'s
 * `_jsMime`, `_corbProtectedMime`, `_sniffsProtected` and `_corbAllowsScript`, which asked THIS question about
 * the same bytes on the wrong side of the boundary — see network/corb.h.
 *
 * WHY THIS DIRECTORY EXISTS, AND WHAT MAKES IT REAL THIS TIME. §7 is a NETWORK-side algorithm: in a real
 * browser it runs in the network service, CORB gates on its result, and the renderer is TOLD a computed MIME
 * type it never derives from response bytes. Everything under `engine/host/browser` and `engine/host/solver` is
 * the RENDERER — one WASM instance per origin-keyed agent cluster, running the untrusted bundle (SECURITY.md).
 * A renderer that computes its own type can classify, and then MINE, a cross-origin body that a real renderer
 * would have been handed as an opaque, empty response, and the endpoints taken out of one are surface the page
 * could never have obtained, reported as a finding about the page.
 *
 * This file was once moved into a `browser_process/` directory linked as its own wasm-ld artifact and that was
 * DELETED, because A SEPARATE LINK IS NOT A PROCESS: a second invocation over the SAME shared object set emits
 * a second artifact out of the same objects (the trusted program was in fact built from the whole engine and
 * was the LARGER of the two; the size difference was dead-stripping), and both Modules were instantiated in the
 * offscreen's own realm with the host holding an exported `HEAPU8` over each — one address space, one trust
 * position, two file names.
 *
 * WHAT IS HERE NOW IS A PROCESS. `extension/browser-process.js` is a dedicated WORKER of the offscreen
 * document: its own realm, its own module instance, its own thread, reached ONLY by `postMessage`. No zone
 * holds a `HEAPU8` over it. It is a Worker and not the sandboxed opaque-origin frame the RENDERER gets
 * (`extension/renderer.html`) because the two boundaries face opposite ways — that frame exists to CONFINE
 * untrusted page execution, and this is the thing that confinement protects; an opaque origin would also cost
 * it `connect-src` and credentialed fetch, which is the opposite of what a network service needs. The trusted
 * side is `extension/browser-process-host.js`, and `engine/build.mjs` links THIS program out of its OWN source
 * list into its OWN objects: main.c, network/corb.c, network/mime_sniff.c and the renderer's
 * core/mime/mime_type.c. Nothing else in the engine is offered to that link, so a renderer-side call to §7 is
 * an undefined symbol rather than a comment somebody has to remember.
 *
 * WHAT LEAVES. `corb.c` is the one caller: SECURITY.md's CORB rule is decided here, beside safe-fetch.js's
 * SOP/CORS, and what crosses back to the trusted zone is the VERDICT plus the computed essence that produced
 * it. A computed type stamped onto a reply record for the renderer to read is the same shape and is not built
 * yet — solver/reply_decode.c says so at the reader's end, and adding the field before the plumbing exists
 * would be the reader-with-no-writer contract CLAUDE.md calls greppable.
 *
 * §4's RECORD IS THE RENDERER'S and stays there: `mime_type_extract` PARSES what a server STATED, and that
 * record is page-observable through `Blob.type`, `File.type`, `DataTransferItem.type` and `accept` matching.
 * Parse is the renderer's; sniff is the network's. Both programs compile that one source, which is what a
 * shared source IS — the same algorithm in two programs, the way net/ links into both of Chromium's.
 */
#ifndef ENGINE_HOST_BROWSER_PROCESS_NETWORK_MIME_SNIFF_H
#define ENGINE_HOST_BROWSER_PROCESS_NETWORK_MIME_SNIFF_H

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
