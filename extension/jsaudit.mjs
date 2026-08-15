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
   itself is in C. step 0 is a DELETION, not a move, and it comes first because until it is gone the engine
   does not run at all on a revisit — every measurement of every later step is taken through it. */
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
  { f: "lib/analyze.js", zone: "MIXED", step: 0, dest: "engine/host/solver/result.c + bridge.js",
    split: "the second scheduler (_reviewQueue/_analysisInflight, a per-document recency queue with its own in-flight register) and the content-hash seen-set (globalStore.scriptCache/_replayCachedAST, whose hit skipped the engine entirely on a revisit) are DELETED: a delivered document now goes straight to the ONE host WFQ and a revisit resumes its parked flows. What is LEFT is the dispatch, which is bridge.js's, and the per-script finding attribution (_findScriptForLine, the combined→per-script line shift, _markSecurityFindingChanges), which is LOGIC — the engine already knows which script each sink came from, so result.c emits script-local locations and the new/existing/fixed diff instead of the host re-deriving both from line offsets." },
  { f: "lib/req2proto.js", zone: "LOGIC", step: 1, dest: "engine/host/solver/req2proto.c",
    why: "named in CLAUDE.md §Architecture by name. A leaf producer: its only input is a reply the engine already fetches through safeFetch, so nothing it calls stays in JS." },
  { f: "lib/discovery.js", zone: "LOGIC", step: 1, dest: "engine/host/solver/discovery.c",
    why: "candidate discovery-doc URLs + fetch strategies. §Active discovery is REQUIRED and is the engine's." },
  { f: "lib/discovery-probe.js", zone: "LOGIC", step: 1, dest: "engine/host/solver/discovery.c",
    why: "the probe/diff/virtual-doc half of the same component." },
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
    split: "globalStore, the request log, _handleFormSubmit and buildExportRequest are LOGIC. buildLiveDelivery is BRIDGE: the delivery VOCABULARY moved into C (each source declares its mechanism beside its encode set in concolic_declare_source; the @S record carries `delivery`+`deliveryPrefix`), so what is left here is the MECHANISM — window.open, the sandboxed attacker page, the real user gesture — which is exactly what a bridge edge is. It replaced buildPocFromShape, which matched a host-side {hash}|{search}|{pm}|{reply} taxonomy against a `shape` field the engine has never emitted. The chrome.runtime.onMessage router + the sender.tab.url trust gate are BRIDGE:3 and fold into bridge.js." },
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
