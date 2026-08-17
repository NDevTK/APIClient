/* THE BROWSER PROCESS'S ENTRY — the ABI `extension/browser-process.js` drives through `ccall`, and the whole
 * of it. `engine/host/main.c` is the RENDERER's entry and this is its counterpart: a different program, linked
 * from a different source list into a different artifact, instantiated in a different realm on a different
 * thread. They share no source file, no object, no linear memory and no handle — which is the difference
 * between this and the `browser_process/` directory that was deleted at 58ba66a7, where two wasm-ld
 * invocations over ONE object set produced two file names. (One source WAS shared, core/mime/mime_type.c, and
 * it went with the sniffing entries: nothing left in this program asks a MIME question.)
 *
 * WHY THERE IS NO SCHEDULER HERE, no realm and no quickjs. What makes this program a BROWSER PROCESS is the
 * RENDERER REGISTRY — which agent clusters have an instance, what routing id each was given, and the refusal
 * of a second instance for one cluster. That is STATE, and it lived in `extension/browser-process.js` as a
 * `Map` and a counter, which is the JS orchestration layer CLAUDE.md §Architecture says to delete rather than
 * grow. It is `renderer/registry.c` now. It needs no scheduler: a registry entry answers from a table in
 * this program's own memory and cannot suspend, which is exactly why the ONE part that must suspend — ordering
 * the offscreen's zygote to materialize a frame, because a dedicated Worker has no `document` — stays on the
 * JavaScript side of the pipe and takes its routing id from here. The moment an entry here needs to SUSPEND
 * that stops being true and the answer is the one CLAUDE.md §scheduler already gives, not a second scheduler
 * invented in this file.
 *
 * THE NETWORK SERVICE IS NOT IN THIS PROGRAM, AND THAT IS THE CORRECTION THIS FILE CARRIES. Two entries stood
 * beside the registry — `bp_corb_check` and `bp_classify`, over `network/{mime_sniff,corb,json_sniff,nosniff,
 * resource_kind}.c` — and they were WHATWG MIME Sniffing §7 and Chromium's CORB analyzer transliterated out of
 * working JavaScript that had been shipping. CLAUDE.md §Architecture now rules on that directly: "TYPE
 * SNIFFING STAYS IN JAVASCRIPT, in `safeFetch`, where SECURITY.md puts it." What belongs in C is what a FLOW
 * needs mid-execution, whose answer must fork and park with the flow; a decision the trusted zone takes ONCE
 * about a reply it just fetched is the other kind, and the failure mode of getting it wrong there is a wrong
 * answer rather than a corrupted heap. `extension/lib/safe-fetch.js` holds it again, answers it ONCE per
 * response, and STAMPS what it decided onto the reply record so the renderer is told rather than left to
 * derive a second answer of its own.
 *
 * WHAT CROSSES, AND WHY IT IS TEXT. `extension/renderer-host.js` states the discipline this follows: a record
 * of primitives carrying its TYPE, with BYTES BESIDE IT. Here the bytes go IN — an agent cluster key, placed
 * in this module's linear memory by the worker and passed as a pointer and a LENGTH, never as a
 * NUL-terminated string, because it is a byte sequence that CONTAINS a NUL — and a RECORD comes back, crossing
 * as ONE JSON document that the worker does ONE `JSON.parse` of. That is the shape `qjs_result`'s `@RESULT`
 * has, and for the same reason: a decision with three fields answered as three separate calls is three chances
 * for a caller to read a stale one beside a fresh one.
 */
#include "check.h"
#include "renderer/registry.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define BP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define BP_EXPORT
#endif

/* ────────────────────────────────────────────────────────────────────────────────────────────────────────
   THE RENDERER REGISTRY'S ABI — `content.mojom.RendererHost` implemented, minus the one line that cannot be.
   The interface has three methods and this has five entries, and the difference is precisely the bridge edge:
   `CreateRendererForCluster` is a DECISION followed by an ORDER followed by the ORDER'S OUTCOME, the order
   travels through the offscreen because only a document can create a frame, and the outcome comes back later.
   So the decision is one entry and the two outcomes are two more, and the JavaScript between them holds no
   state at all — it carries an integer out to the zygote and brings a pipe or a reason back.
   THE CLUSTER KEY ARRIVES AS BYTES AND A LENGTH, and the reason is sharp: `clusterKeyOf` joins the
   browsing-context group and the origin with a NUL, so a key marshalled as a C string would arrive truncated
   at the separator and every origin in one tab would answer to one key — the registry whose entire job is to
   refuse a merged agent cluster would perform one.
   ──────────────────────────────────────────────────────────────────────────────────────────────────────── */

/* DECIDE THAT AN AGENT CLUSTER GETS AN INSTANCE. Returns the routing id, which is always positive, and there
   is no refusal to return: the refusals are SECURITY.md's one-instance-per-cluster rule and an empty key, both
   of which abort in registry.c rather than becoming a value a caller may inspect and carry on past. */
BP_EXPORT int bp_renderer_create(const unsigned char *cluster_key, int cluster_key_n)
{
    DCHECK(cluster_key_n >= 0, "an agent cluster key was passed a negative length — the caller measures a byte "
                               "sequence it holds, so a negative one is a subtraction that went past the start");
    DCHECK(cluster_key != NULL || cluster_key_n == 0,
           "an agent cluster key of non-zero length arrived as a null pointer — the worker places the bytes in "
           "this module's memory and passes what it placed, so a null with a length is a placement that failed "
           "silently and would decide a renderer for a cluster this program never saw");
    return renderer_registry_create(cluster_key, (size_t)(cluster_key_n < 0 ? 0 : cluster_key_n));
}

/* THE FORK ORDER'S OUTCOME. A launch that failed frees its agent cluster here — the zygote has already removed
   the frame, and leaving the registration would refuse that cluster a renderer forever. */
BP_EXPORT void bp_renderer_launched(int routing_id) { renderer_registry_launched(routing_id); }
BP_EXPORT void bp_renderer_launch_failed(int routing_id) { renderer_registry_launch_failed(routing_id); }

/* A RENDERER'S DEATH, OBSERVED BY THE ZONE THAT OWNS THE FRAME AND REPORTED HERE. A routing id this process
   never minted CRASHES: it is the one number only this program can produce. */
BP_EXPORT void bp_renderer_terminated(int routing_id) { renderer_registry_terminated(routing_id); }

/* THE TABLE, AS ONE RECORD. It is `GetRegistry`'s declared reply verbatim, built where the table lives so no
   consumer re-derives a field, and it is what makes `rendererPoolProbe`'s cross-check mean something: the ids
   in it were minted in THIS program's memory, and the frames the offscreen holds were counted in another. */
BP_EXPORT const char *bp_registry_snapshot(void) { return renderer_registry_snapshot_json(); }
