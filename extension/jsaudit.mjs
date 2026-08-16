/* jsaudit.mjs — the JS-SURFACE GATE. `node extension/jsaudit.mjs`
 *
 * CLAUDE.md §Architecture: the C engine owns ALL semantics; the ONLY irreducible JS is a BRIDGE, and the
 * reasons it may exist are the four SECURITY.md names — (1) relay safeFetch, (2) relay IndexedDB, (3) relay
 * chrome.*, (4) render the popup — plus loading the WASM. Every other line of JS in extension/ is semantics
 * that must become a C component, and the list of which-is-which has to live in ONE place that every file
 * goes through, or a new file joins the surface unclassified and nobody notices.
 *
 * This is that place, and it is a GATE rather than a note:
 *   • a .js on disk with no LEDGER row FAILS   — a new file must declare its zone
 *   • a LEDGER row with no file on disk FAILS  — a stale row is the DFAIL-that-lies failure mode
 *   • any surviving LOGIC row FAILS            — the queue is the work, printed in order with live counts
 * Its only correct trajectory is to zero: when the last LOGIC row is gone the gate goes green, and the file
 * that follows it out is this one. Do NOT soften a row to make it pass — that is choosing the report over
 * the forcing function.
 *
 * ZONES
 *   BRIDGE:<n>  irreducible, where n names the SECURITY.md reason (1 fetch, 2 IDB, 3 chrome.*, 4 popup, 5 WASM load)
 *   LOGIC       semantics that belongs in a C component — `dest` names the component it becomes
 *   MIXED       both, in one file — `split` states the boundary; it is queued as LOGIC until split
 */

import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = dirname(fileURLToPath(import.meta.url));

/* THE MOVE QUEUE IS THE ORDER OF THIS TABLE'S `step` FIELD, and the order is a DEPENDENCY order, not a size
   order: a producer whose consumers still live in JS is not first, because moving it makes every call it
   serves cross the JS↔WASM boundary the architecture rule exists to delete. Callees before callers; leaf
   producers (they consume only a host edge the engine already owns) before aggregators; the popup's own
   offscreen-side compute last, because it is driven by a popup RPC the engine cannot answer until the moat
   itself is in C.

   STEP 0 IS DONE AND ITS ROW IS GONE, and what it turned out to be is worth keeping: `lib/analyze.js` was a
   DELETION rather than a move, but not for the reason this comment used to give. It said step 0 came first
   "because until it is gone the engine does not run at all on a revisit" — that was the content-hash seen-set,
   which had ALREADY been deleted when the sentence was written, so the row was reading as authoritative about
   work already done. What was actually left was per-script finding attribution over `sink.location`,
   `taintPath` and `sanitizerReport` — three names the engine has never emitted — plus a concatenation of a
   script array the CONTENT_HTML handler stopped filling when the engine started sourcing its own scripts. None
   of it moved to result.c: a sink's location is a fact only the running engine holds, and the host's version
   was filing every finding under the first script. The surviving ~40-line handoff sits beside the handler that
   writes the buffer, in offscreen-brain.js, whose own row already queues it.

   STEP 1 IS GONE THE SAME WAY, AND WHAT IT TAUGHT IS THE ORDERING RULE ABOVE, PAID FOR. `lib/discovery.js` sat
   at step 1 holding three components, and its own `why` already said the opposite of its `step`: "what decides
   the next move is the CONSUMER". Checked against the tree, all three had a LIVE JS consumer — the classifier
   gates every learning call in response-decode.js through `_isAsset`/`_isBoringFetch`, the RSC parser fed
   learn.js, and the schema half answers the Send panel — so a whole-file move could only have been completed
   by inventing a host→engine COMPUTE call, which this architecture does not have and must not grow: the engine
   is the driver and the bridge relays. What DID move is the part that needed no JS caller at all, because the
   engine already holds the input: React Flight is now read at `engine_provide`, the one point every fetched
   reply crosses exactly once, and the magic-byte classifier's ALGORITHM became the standard it was a
   hand-rolled copy of (core/mime/mime_sniff.c, WHATWG MIME Sniffing §6/§7). The remainder is re-stepped to the
   files that call it. A row's step is a claim about THIS tree, so a step that disagrees with its own reason is
   the stale-DFAIL failure mode wearing a number. */
const LEDGER = [
  // ── BRIDGE — irreducible platform edge ───────────────────────────────────────────────────────────────
  { f: "bridge.js", zone: "BRIDGE:1,2,5",
    why: "loads the engine WASM, drives qjs_step, relays every byte through safeFetch, persists the frontier to IDB, routes cross-instance notices. The Level-1 WFQ lives here because one WASM instance is one document and no instance can rank the others." },
  { f: "lib/safe-fetch.js", zone: "BRIDGE:1",
    why: "THE network chokepoint. SOP/CORS/PNA/CORB/GET-only cannot live inside the untrusted WASM — SECURITY.md §Network." },
  { f: "lib/persistence.js", zone: "BRIDGE:2",
    why: "IndexedDB open/get/put/clear. The WASM cannot reach IDB." },
  { f: "background.js", zone: "BRIDGE:3",
    why: "stateless SW: owns the offscreen document's lifecycle and performs the chrome.tabs.*/webNavigation.* calls the offscreen cannot." },
  { f: "content.js", zone: "BRIDGE:3",
    why: "the renderer edge: ships the document's own HTML + relays intercept.js bodies + performs page-context fetches. Only a content script can be in the page." },
  { f: "intercept.js", zone: "BRIDGE:3",
    why: "main-world fetch/XHR/WS/postMessage capture + the apiclientsink PoC-fire hook. Only main-world JS can wrap the page's own globals." },
  { f: "check.js", zone: "BRIDGE:3",
    why: "the JS mirror of engine/host/check.h. It exists exactly as long as bridge JS exists, and goes out with the last of it." },
  { f: "popup.js", zone: "BRIDGE:4", why: "the popup's DOM controller." },
  { f: "lib/popup-form.js", zone: "BRIDGE:4", why: "Send-panel field UI." },
  { f: "lib/popup-response.js", zone: "BRIDGE:4", why: "response rendering." },
  { f: "lib/popup-send.js", zone: "BRIDGE:4", why: "Send-panel selectors." },
  { f: "lib/popup-gql.js", zone: "BRIDGE:4", why: "GraphQL operation editor." },
  { f: "lib/popup-mp.js", zone: "BRIDGE:4", why: "multipart sub-part editor." },
  { f: "lib/popup-protocols.js", zone: "BRIDGE:4", why: "per-protocol response renderers." },
  { f: "lib/popup-reqlog.js", zone: "BRIDGE:4", why: "virtual-scrolled request log." },
  { f: "lib/popup-security.js", zone: "BRIDGE:4", why: "@S finding cards + the sandboxed live-verify driver." },
  { f: "lib/popup-console.js", zone: "BRIDGE:4", why: "WS/postMessage console." },

  // ── LOGIC / MIXED — the queue ────────────────────────────────────────────────────────────────────────
  { f: "lib/discovery.js", zone: "LOGIC", step: 2, dest: "engine/host/solver/reply_decode.c + moat.c",
    why: "TWO COMPONENTS LEFT and they belong to two different steps, which is why this row is no longer " +
         "step 1: the RSC parser was the third and is GONE (React Flight is read in the engine at " +
         "engine_provide; `looksLikeRSC` was a body-shape guess §RUN, DON'T MATCH forbids and did not move). " +
         "What is left is classifyResponseAsset — whose `_isAsset`/`_isBoringFetch` gate every learning call " +
         "in lib/response-decode.js, so it moves WITH that file, here at step 2 — and the Send panel's schema " +
         "resolution (findDiscoveryMethod/findMethodById/resolveDiscoverySchema), whose consumers are " +
         "lib/send.js and the popup, so it leaves at step 7. Neither could go earlier: this ledger's own rule " +
         "is that a producer whose consumers still live in JS is not first, and there is no host→engine " +
         "COMPUTE edge for a JS caller to reach a moved callee through — the engine is the driver and adding " +
         "one would be the orchestration layer inverted. The ALGORITHMS are already in C: the magic-byte " +
         "classifier's standard is core/mime/mime_sniff.c (WHATWG MIME Sniffing §6/§7, which also deletes the " +
         "JS's invented SVG/CSS/VTT/HLS/DASH sniffs), and a discovery document's schema graph is read by " +
         "engine/host/solver/discovery.c." },
  { f: "lib/protobuf.js", zone: "LOGIC", step: 2, dest: "engine/host/solver/reply_decode.c",
    why: "a wire CODEC. §A JS-engine encoding builtin is modeled FAITHFULLY — the engine runs the real codec; a second one in JS is the redundant layer." },
  { f: "lib/protocol-parsers.js", zone: "LOGIC", step: 2, dest: "engine/host/solver/reply_decode.c",
    why: "batchExecute/gRPC-Web/SSE/NDJSON/GraphQL/multipart framing. Same component." },
  { f: "lib/response-decode.js", zone: "LOGIC", step: 2, dest: "engine/host/solver/reply_decode.c",
    why: "the protocol-chain unwrap that drives the two above. Must precede learn.js, which consumes its shapes." },
  { f: "lib/schema.js", zone: "LOGIC", step: 3, dest: "engine/host/solver/moat_schema.c",
    why: "schema inference over decoded JSON/JSPB. A CALLEE of learn.js — callees move before callers, or every call crosses the boundary." },
  { f: "lib/stats.js", zone: "LOGIC", step: 3, dest: "engine/host/solver/moat_stats.c", why: "per-parameter observation model. Callee of learn.js." },
  { f: "lib/grouping.js", zone: "LOGIC", step: 3, dest: "engine/host/solver/endpoint.c",
    why: "endpoint identity/service grouping — the component already exists in C and already owns dedup." },
  { f: "lib/chains.js", zone: "LOGIC", step: 3, dest: "engine/host/solver/moat.c", why: "response-value → request-param chaining. Callee of the merge." },
  { f: "lib/keys.js", zone: "LOGIC", step: 3, dest: "engine/host/solver/moat.c", why: "credential/token extraction from decoded bodies. Callee of response-decode." },
  { f: "lib/learn.js", zone: "LOGIC", step: 4, dest: "engine/host/solver/moat.c",
    why: "THE moat aggregation — CLAUDE.md §Architecture names moat aggregation as the engine's. Moves after step 3 because every function it calls is in step 3." },
  { f: "lib/merge.js", zone: "LOGIC", step: 4, dest: "engine/host/solver/moat.c",
    why: "@RESULT → doc-model merge. The engine already dedups its own @RESULT; this is the last host-side re-merge." },
  { f: "lib/serialize.js", zone: "LOGIC", step: 5, dest: "engine/host/solver/result.c",
    why: "the popup snapshot IS the engine's result view once the moat is in C. Cannot precede step 4 — it serializes what step 4 owns." },
  { f: "offscreen-brain.js", zone: "MIXED", step: 6, dest: "engine/host/solver/moat.c + bridge.js",
    split: "globalStore, the request log, _handleFormSubmit and buildExportRequest are LOGIC. buildLiveDelivery is BRIDGE: the delivery VOCABULARY moved into C (each source declares its mechanism beside its encode set in concolic_declare_source; the @S record carries `delivery`+`deliveryPrefix`), so what is left here is the MECHANISM — window.open, the sandboxed attacker page, the real user gesture — which is exactly what a bridge edge is. It replaced buildPocFromShape, which matched a host-side {hash}|{search}|{pm}|{reply} taxonomy against a `shape` field the engine has never emitted. The chrome.runtime.onMessage router + the sender.tab.url trust gate are BRIDGE:3 and fold into bridge.js. _dispatchDocument (what step 0 left of lib/analyze.js) is BRIDGE:3 with it: it reads the browser-stated facts off the document's own buffer, builds the AST_ANALYZE message and hands it to the ONE pool — every field it carries is asserted on arrival in bridge.js." },
  { f: "lib/popup-handlers.js", zone: "MIXED", step: 6, dest: "bridge.js",
    split: "runs in the OFFSCREEN, not the popup, so it cannot claim reason 4: it is the popup RPC surface. The dispatch is BRIDGE:3; every per-command backend it calls is LOGIC that moves with its own step." },
  { f: "lib/send.js", zone: "LOGIC", step: 7, dest: "engine/host/solver/moat.c",
    why: "schema resolution + type coercion for the manual replay. LAST: it is driven by a popup RPC the engine cannot answer until the moat is in C." },
  { f: "lib/encode.js", zone: "LOGIC", step: 7, dest: "engine/host/solver/reply_decode.c", why: "the encode direction of the step-2 codec, driven by the same popup RPC." },
  { f: "lib/openapi-import.js", zone: "LOGIC", step: 7, dest: "engine/host/solver/discovery.c", why: "OpenAPI → discovery conversion; the import half of step 1's component." },
  { f: "lib/openapi-export.js", zone: "LOGIC", step: 7, dest: "engine/host/solver/result.c", why: "discovery → OpenAPI, a projection of the engine's result." },
];

const files = [];
(function walk(d) {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    if (e.name === "qjs" || e.name === "icons") continue;
    const p = join(d, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name.endsWith(".js")) files.push(relative(ROOT, p).split("\\").join("/"));
  }
})(ROOT);

const lines = (f) => readFileSync(join(ROOT, f), "utf8").split("\n").length;
const byFile = new Map(LEDGER.map((r) => [r.f, r]));
const fail = [];

for (const f of files.sort())
  if (!byFile.has(f))
    fail.push(`UNCLASSIFIED  ${f} — no LEDGER row. Every .js on this surface declares whether it is an\n` +
              `              irreducible BRIDGE (and which SECURITY.md reason) or LOGIC (and which C component it becomes).`);
for (const r of LEDGER)
  if (!files.includes(r.f))
    fail.push(`STALE ROW     ${r.f} — the ledger names a file that is not on disk. A row that outlives its file\n` +
              `              reads as authoritative and sends the next reader to move something already gone.`);

const queue = LEDGER.filter((r) => r.zone !== "BRIDGE" && !r.zone.startsWith("BRIDGE:") && files.includes(r.f))
                    .sort((a, b) => (a.step - b.step) || a.f.localeCompare(b.f));
const bridge = LEDGER.filter((r) => r.zone.startsWith("BRIDGE") && files.includes(r.f));
const tot = (rs) => rs.reduce((n, r) => n + lines(r.f), 0);

console.log(`\nextension/ JS surface: ${files.length} files, ${files.reduce((n, f) => n + lines(f), 0)} lines`);
console.log(`  BRIDGE (irreducible): ${bridge.length} files, ${tot(bridge)} lines`);
console.log(`  LOGIC/MIXED (queued): ${queue.length} files, ${tot(queue)} lines\n`);
let step = -1;
for (const r of queue) {
  if (r.step !== step) { step = r.step; console.log(`--- step ${step} ---`); }
  console.log(`  ${String(lines(r.f)).padStart(5)}  ${r.f.padEnd(28)} → ${r.dest}`);
  console.log(`         ${r.why || r.split}`);
}
if (queue.length)
  fail.push(`QUEUE NON-EMPTY  ${queue.length} file(s), ${tot(queue)} lines of semantics still in JS. CLAUDE.md\n` +
            `                 §Architecture: "A JS orchestration layer is unwanted context-switching across the\n` +
            `                 JS↔WASM boundary; DELETE it." Take step ${queue[0].step}. When the queue empties,\n` +
            `                 this gate goes green and is deleted with the last row.`);

if (fail.length) { console.error("\n" + fail.join("\n") + "\n"); process.exit(1); }
console.log("\nJS surface is bridge-only. Delete extension/jsaudit.mjs.\n");
