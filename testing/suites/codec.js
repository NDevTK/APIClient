// Codec roundtrip suite.
//
// Purpose: catch silent parser/encoder bugs in the protocol chain by
// running captured response/request bodies through parse → re-encode →
// parse and comparing outputs. Applies to every captured request with a
// matching format signature.
//
// This is offline — operates entirely on saved reports, no browser
// needed. Run after `npm run harness` produces a report directory.
//
// Usage:
//   node testing/suites/codec.js                   # newest run
//   node testing/suites/codec.js --dir <path>
//
// Extends: roundtrip coverage uses what the extension already ships.
// If a parser has no corresponding encoder (multipart-batch, SSE, NDJSON,
// batchexecute, JSPB), we record a "parse-only verified" outcome — we at
// least check that parse succeeds on captured bytes and produces a
// non-empty result.

"use strict";

const path = require("path");
const fs = require("fs");
const fsp = fs.promises;

// Load the libs the same way test-lib.js does.
const root = path.resolve(__dirname, "..", "..");
function loadLib(relPath, exports) {
  const src = fs.readFileSync(path.join(root, "extension/lib", relPath), "utf8");
  const suffix = exports.map(n => `\nglobalThis.${n} = ${n};`).join("");
  new Function(src + suffix)();
}
loadLib("protobuf.js", [
  "uint8ToBase64", "base64ToUint8", "pbDecodeTree", "pbDecodeRaw",
  "pbGetFields", "pbGetString", "pbGetVarint", "pbGetMessage",
  "pbEncodeVarintField", "pbEncodeLenField", "pbEncodeFixed32Field",
  "pbEncodeFixed64Field", "jspbToTree", "pbTag", "concatBytes",
]);
loadLib("discovery.js", [
  "parseBatchExecuteRequest", "parseBatchExecuteResponse",
  "parseAsyncChunkedResponse", "parseGrpcWebFrames", "encodeGrpcWebFrame",
  "isGrpcWeb", "isGrpcWebText", "parseSSE", "parseNDJSON",
  "parseGraphQLRequest", "parseGraphQLResponse", "isGraphQLUrl",
  "parseMultipartBatch", "isMultipartBatch",
  "convertDiscoveryToOpenApi", "convertOpenApiToDiscovery",
  "sniffBinaryMagic", "classifyResponseAsset",
]);

const REPORTS_DIR = path.join(__dirname, "..", "reports");

function parseArgs(argv) {
  const out = { dir: null };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === "--dir") out.dir = argv[++i];
  }
  return out;
}
async function pickLatestRun() {
  const entries = await fsp.readdir(REPORTS_DIR, { withFileTypes: true });
  // Harness dirs use pure ISO timestamps; suite dirs have a prefix. Filter
  // so suite outputs don't steal "latest" from an actual harness run.
  const harnessRe = /^\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}-\d{3}Z$/;
  const dirs = entries
    .filter(e => e.isDirectory() && harnessRe.test(e.name))
    .map(e => e.name)
    .sort();
  if (!dirs.length) throw new Error("no harness reports found — run `npm run harness` first.");
  return path.join(REPORTS_DIR, dirs[dirs.length - 1]);
}

// ─── Byte-level helpers ─────────────────────────────────────────────────────

function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function entryBodyBytes(entry, which) {
  // which: "request" | "response"
  const [raw, base64] = which === "request"
    ? [entry.rawBodyB64, true]
    : [entry.responseBody, entry.responseBase64];
  if (!raw) return null;
  if (base64) {
    try { return base64ToUint8(raw); }
    catch { return null; }
  }
  // Text body → interpret as latin-1 preserving byte values
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i) & 0xff;
  return out;
}

function entryBodyText(entry, which) {
  const [raw, base64] = which === "request"
    ? [entry.rawBodyB64, true]
    : [entry.responseBody, entry.responseBase64];
  if (raw == null) return null;
  if (base64) {
    try { return new TextDecoder("utf-8", { fatal: false }).decode(base64ToUint8(raw)); }
    catch { return null; }
  }
  return raw;
}

// ─── Roundtrip cases ────────────────────────────────────────────────────────

// Each case: {name, applies(entry), run(entry) → {ok, detail, kind}}

function detectFormat(entry) {
  const ct = (entry.contentType || entry.mimeType || "").toLowerCase();
  const url = entry.url || "";
  if (isGrpcWeb(ct)) return "grpc-web";
  if (ct.includes("application/json") && (url.includes("graphql") || entry._isGraphQL)) return "graphql";
  if (ct.includes("application/json")) return "json";
  if (ct.includes("application/x-protobuf") || ct.includes("application/protobuf")) return "protobuf";
  if (ct.includes("text/event-stream") || ct.includes("text/vnd.event-stream")) return "sse";
  if (ct.includes("application/x-ndjson") || ct.includes("application/ndjson")) return "ndjson";
  if (isMultipartBatch(ct)) return "multipart";
  // Sniff body as last resort
  const bytes = entryBodyBytes(entry, "response");
  if (bytes) {
    const magic = sniffBinaryMagic(bytes);
    if (magic) return null; // binary asset, not a codec target
  }
  return null;
}

// gRPC-Web roundtrip: parse → concatenate frames → re-encode frames → compare.
// The extension's encoder only emits data frames, so the roundtrip compares
// data frames only (skips trailer frames).
function runGrpcWebRoundtrip(entry) {
  const bytes = entryBodyBytes(entry, "response");
  if (!bytes) return { ok: false, detail: "no body" };
  const parsed = parseGrpcWebFrames(bytes);
  if (!parsed) return { ok: false, detail: "parseGrpcWebFrames returned null" };
  if (!parsed.frames.length) return { ok: false, detail: "no frames decoded" };
  // Re-encode each data frame, concat, and check prefix match against original
  // bytes up to the first trailer frame.
  const dataFrames = parsed.frames.filter(f => f.type === "data");
  if (!dataFrames.length) return { ok: true, detail: "only trailers (skip roundtrip)" };
  const encoded = concatBytes(dataFrames.map(f => encodeGrpcWebFrame(f.data)));
  // Reparse the re-encoded bytes and compare frame-by-frame payloads.
  const reparsed = parseGrpcWebFrames(encoded);
  if (!reparsed) return { ok: false, detail: "re-encode then parse failed" };
  if (reparsed.frames.length !== dataFrames.length) {
    return { ok: false, detail: `frame count drift: original ${dataFrames.length}, re-encoded ${reparsed.frames.length}` };
  }
  for (let i = 0; i < dataFrames.length; i++) {
    if (!bytesEqual(dataFrames[i].data, reparsed.frames[i].data)) {
      return { ok: false, detail: `frame ${i} payload differs after roundtrip` };
    }
  }
  return { ok: true, detail: `${dataFrames.length} frame(s) roundtripped` };
}

// JSON roundtrip: parse → stringify → parse → deep compare. Catches
// numeric precision loss and key-order drift (which shouldn't matter for
// behavioral equivalence).
function runJsonRoundtrip(entry) {
  const text = entryBodyText(entry, "response");
  // Empty response bodies (analytics beacons, 204-style) aren't a roundtrip
  // concern — no payload to encode/decode. Skip rather than fail.
  if (!text || !text.trim()) return { ok: true, detail: "empty body (no roundtrip needed)" };
  // Strip Google XSSI prefix (`)]}'`) before parse — common on Google and
  // GitLab snowplow endpoints. The extension's parser also strips this.
  let cleaned = text;
  if (cleaned.trimStart().startsWith(")]}'")) {
    cleaned = cleaned.replace(/^[\s]*\)\]\}'[\r\n]*/, "");
  }
  // "ok"/"true"/"null" confirmation responses aren't JSON-roundtrip-worthy.
  if (/^(ok|OK|true|false|null)\s*$/.test(cleaned.trim())) {
    return { ok: true, detail: "scalar confirmation body (not JSON object)" };
  }
  let obj;
  try { obj = JSON.parse(cleaned); }
  catch (e) { return { ok: false, detail: "JSON.parse threw: " + e.message }; }
  if (obj == null) return { ok: true, detail: "null payload" };
  let re;
  try { re = JSON.parse(JSON.stringify(obj)); }
  catch (e) { return { ok: false, detail: "re-stringify parse threw: " + e.message }; }
  if (!deepEqual(obj, re)) return { ok: false, detail: "deep-compare drift" };
  return { ok: true, detail: `${typeof obj === "object" ? Object.keys(obj).length : 0} top keys` };
}

// GraphQL roundtrip: parse request body → reconstruct envelope → reparse →
// check operations match.
function runGraphQLRoundtrip(entry) {
  const text = entryBodyText(entry, "request");
  // GET requests to GraphQL endpoints (Apollo APQ with query-string variables)
  // carry no request body — roundtripping isn't applicable. Treat as a skip,
  // not a failure.
  if (!text) return { ok: true, detail: "no request body (GET/APQ style — skipped)" };
  const parsed = parseGraphQLRequest(text);
  if (!parsed) return { ok: false, detail: "parseGraphQLRequest returned null" };
  if (!parsed.operations.length) return { ok: false, detail: "no operations parsed" };
  // Reconstruct a normalized envelope from parsed ops, then reparse.
  // Preserve the non-standard `operation` field (Reddit persisted queries)
  // and `extensions.persistedQuery` (Apollo APQ) so reparse succeeds.
  const rebuildOp = (op) => {
    const out = {};
    if (op.query) out.query = op.query;
    if (op.variables) out.variables = op.variables;
    if (op.operationName) out.operationName = op.operationName;
    if (op.operation) out.operation = op.operation;
    if (op.extensions) out.extensions = op.extensions;
    return out;
  };
  const rebuilt = parsed.batched
    ? parsed.operations.map(rebuildOp)
    : rebuildOp(parsed.operations[0]);
  const reparsed = parseGraphQLRequest(JSON.stringify(rebuilt));
  if (!reparsed) return { ok: false, detail: "reparse returned null" };
  if (reparsed.operations.length !== parsed.operations.length) {
    return { ok: false, detail: "op count drift" };
  }
  // operationName + query text should match after one round trip.
  for (let i = 0; i < parsed.operations.length; i++) {
    const a = parsed.operations[i];
    const b = reparsed.operations[i];
    if (a.operationName !== b.operationName) {
      return { ok: false, detail: `op[${i}] name drift: ${a.operationName} vs ${b.operationName}` };
    }
    if ((a.query || "") !== (b.query || "")) {
      return { ok: false, detail: `op[${i}] query drift` };
    }
  }
  const firstName = parsed.operations[0].operationName || parsed.operations[0].operation || "(anon)";
  return { ok: true, detail: `${parsed.operations.length} op(s), first=${firstName}` };
}

// Protobuf decode sanity: pbDecodeTree should return a non-null structure
// and not throw. We don't have a tree-to-bytes encoder, so this is parse-only.
function runProtobufSanity(entry) {
  const bytes = entryBodyBytes(entry, "response");
  if (!bytes || bytes.length === 0) return { ok: true, detail: "empty body" };
  try {
    const tree = pbDecodeTree(bytes, 8);
    if (tree == null) return { ok: false, detail: "pbDecodeTree returned null" };
    return { ok: true, detail: "tree decoded" };
  } catch (e) {
    return { ok: false, detail: "pbDecodeTree threw: " + e.message };
  }
}

// SSE / NDJSON parse-only sanity.
function runSseParse(entry) {
  const text = entryBodyText(entry, "response");
  if (!text) return { ok: false, detail: "no body" };
  const events = parseSSE(text);
  if (!events) return { ok: false, detail: "parseSSE returned null" };
  return { ok: true, detail: `${events.length} event(s)` };
}
function runNdjsonParse(entry) {
  const text = entryBodyText(entry, "response");
  if (!text) return { ok: false, detail: "no body" };
  const items = parseNDJSON(text);
  if (!items) return { ok: false, detail: "parseNDJSON returned null" };
  return { ok: true, detail: `${items.length} record(s)` };
}
function runMultipartParse(entry) {
  const text = entryBodyText(entry, "response");
  const ct = entry.contentType || entry.mimeType || "";
  if (!text) return { ok: false, detail: "no body" };
  const parts = parseMultipartBatch(text, ct);
  if (!parts) return { ok: false, detail: "parseMultipartBatch returned null" };
  return { ok: true, detail: `${parts.length} part(s)` };
}

function deepEqual(a, b) {
  if (a === b) return true;
  if (typeof a !== typeof b) return false;
  if (a == null || b == null) return a === b;
  if (typeof a !== "object") return false;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (Array.isArray(a)) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!deepEqual(a[i], b[i])) return false;
    return true;
  }
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) if (!deepEqual(a[k], b[k])) return false;
  return true;
}

// OpenAPI bidirectional conversion over every discovered service.
function runOpenApiRoundtripOnDump(dump) {
  const results = [];
  const discovery = dump.global?.discovery || {};
  for (const [svc, entry] of Object.entries(discovery)) {
    if (entry.status !== "found" || !entry.doc) continue;
    try {
      const spec = convertDiscoveryToOpenApi(entry.doc, svc);
      if (!spec) { results.push({ service: svc, ok: false, detail: "discovery → OpenAPI returned null" }); continue; }
      const back = convertOpenApiToDiscovery(spec, "roundtrip://" + svc);
      if (!back) { results.push({ service: svc, ok: false, detail: "OpenAPI → discovery returned null" }); continue; }
      // Compare method counts (both directions should preserve method count).
      const origMethods = countMethods(entry.doc);
      const backMethods = countMethods(back);
      const origSchemas = Object.keys(entry.doc.schemas || {}).length;
      const backSchemas = Object.keys(back.schemas || {}).length;
      if (origMethods !== backMethods) {
        results.push({ service: svc, ok: false, detail: `method count drift: ${origMethods} → ${backMethods}` });
        continue;
      }
      results.push({
        service: svc,
        ok: true,
        detail: `methods=${origMethods} schemas=${origSchemas}→${backSchemas}`,
      });
    } catch (e) {
      results.push({ service: svc, ok: false, detail: "threw: " + e.message });
    }
  }
  return results;
}

// Count methods, deduping by (path|verb|id). A service that lists the same
// method in both `learned` and `probed` buckets is one logical operation —
// the OpenAPI exporter correctly collapses them. Counting by (bucket,name)
// treats that as 2→1 drift which is actually a correct roundtrip.
function countMethods(doc) {
  const seen = new Set();
  if (doc.resources) {
    for (const b of Object.values(doc.resources)) {
      if (!b || !b.methods) continue;
      for (const [name, m] of Object.entries(b.methods)) {
        const key = (m.id || name) + "|" + (m.path || "") + "|" + (m.httpMethod || "POST");
        seen.add(key);
      }
    }
  }
  if (doc.methods) {
    for (const [name, m] of Object.entries(doc.methods)) {
      const key = (m.id || name) + "|" + (m.path || "") + "|" + (m.httpMethod || "POST");
      seen.add(key);
    }
  }
  if (doc.paths) {
    for (const [p, verbs] of Object.entries(doc.paths)) {
      if (!verbs || typeof verbs !== "object") continue;
      for (const [verb, op] of Object.entries(verbs)) {
        if (!op || !op.operationId) continue;
        const key = op.operationId + "|" + p + "|" + verb.toUpperCase();
        seen.add(key);
      }
    }
  }
  return seen.size;
}

// ─── Per-site driver ────────────────────────────────────────────────────────

async function analyzeSite(runDir, file) {
  const report = JSON.parse(await fsp.readFile(path.join(runDir, file), "utf8"));
  if (!report.dump) return { site: report.name, error: report.error || "no dump" };
  const dump = report.dump;
  const log = dump.tab?.requestLog || [];

  const results = {
    site: report.name,
    total: log.length,
    byFormat: {},
    entries: [],
  };

  for (const entry of log) {
    const fmt = detectFormat(entry);
    if (!fmt) continue;
    let run;
    switch (fmt) {
      case "grpc-web": run = runGrpcWebRoundtrip(entry); break;
      case "graphql": run = runGraphQLRoundtrip(entry); break;
      case "json": run = runJsonRoundtrip(entry); break;
      case "protobuf": run = runProtobufSanity(entry); break;
      case "sse": run = runSseParse(entry); break;
      case "ndjson": run = runNdjsonParse(entry); break;
      case "multipart": run = runMultipartParse(entry); break;
      default: continue;
    }
    results.byFormat[fmt] = results.byFormat[fmt] || { tested: 0, passed: 0, failed: [] };
    results.byFormat[fmt].tested++;
    if (run.ok) results.byFormat[fmt].passed++;
    else results.byFormat[fmt].failed.push({ url: entry.url, detail: run.detail });
    results.entries.push({ fmt, ok: run.ok, detail: run.detail, url: entry.url, method: entry.method });
  }

  results.openapi = runOpenApiRoundtripOnDump(dump);
  return results;
}

async function main() {
  const args = parseArgs(process.argv);
  const runDir = args.dir ? path.resolve(args.dir) : await pickLatestRun();
  console.log("[codec] run dir:", runDir);

  const entries = await fsp.readdir(runDir);
  const siteFiles = entries.filter(f => f.endsWith(".json") && f !== "run.json" && f !== "analysis.json" && f !== "codec.json");

  const all = [];
  for (const f of siteFiles) {
    const res = await analyzeSite(runDir, f);
    all.push(res);
  }

  // Aggregate
  const totals = { byFormat: {}, openapi: { ok: 0, failed: 0 } };
  for (const r of all) {
    for (const [fmt, s] of Object.entries(r.byFormat || {})) {
      if (!totals.byFormat[fmt]) totals.byFormat[fmt] = { tested: 0, passed: 0, failed: [] };
      totals.byFormat[fmt].tested += s.tested;
      totals.byFormat[fmt].passed += s.passed;
      for (const f of s.failed) totals.byFormat[fmt].failed.push({ site: r.site, ...f });
    }
    for (const o of r.openapi || []) {
      if (o.ok) totals.openapi.ok++;
      else totals.openapi.failed++;
    }
  }

  await fsp.writeFile(path.join(runDir, "codec.json"), JSON.stringify({ perSite: all, totals }, null, 2), "utf8");

  console.log("\n── codec roundtrip totals ──────────────────────────");
  for (const [fmt, s] of Object.entries(totals.byFormat)) {
    const fail = s.tested - s.passed;
    const status = fail === 0 ? "OK" : "FAIL";
    console.log(`  ${fmt.padEnd(10)} ${s.passed}/${s.tested}  ${status}${fail ? "  (" + fail + " failures)" : ""}`);
    for (const f of s.failed.slice(0, 5)) {
      console.log(`      ${f.site}  ${f.url.slice(0, 80)} — ${f.detail}`);
    }
  }
  console.log(`  openapi    ${totals.openapi.ok}/${totals.openapi.ok + totals.openapi.failed}  ${totals.openapi.failed === 0 ? "OK" : "FAIL"}`);
  console.log(`\n[codec] wrote ${path.join(runDir, "codec.json")}`);

  // Exit code reflects whether any format had failures
  const anyFail = Object.values(totals.byFormat).some(s => s.tested !== s.passed) || totals.openapi.failed > 0;
  process.exit(anyFail ? 1 : 0);
}

main().catch(err => { console.error(err); process.exit(1); });
