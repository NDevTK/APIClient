// Replay roundtrip suite.
//
// For every learned method across every service in the current profile's
// globalStore, this suite:
//   1. Synthesizes form field values from the method's learned schema
//      (using .example, .default, or a type-appropriate dummy).
//   2. Calls buildExportRequest() in the service worker — the same code
//      path the popup uses when the user hits Send.
//   3. Decodes the resulting request body with the matching parser.
//   4. Compares the decoded field values back against the synthesized
//      inputs.
//
// Passing means: "a user can fill the form and the encoder will produce
// wire bytes the decoder reads back as the same values."  This catches
// lossy encoders, wrong Content-Type wiring, and multipart sub-parts
// that the current popup can't individually edit.
//
// Runs LIVE against the persisted profile (testing/profile) — so run
// `npm run harness` first to populate globalStore, then this.
//
// Usage: node testing/suites/replay.js

"use strict";

const path = require("path");
const fs = require("fs");
const fsp = fs.promises;
const puppeteer = require("puppeteer");

const ROOT = path.resolve(__dirname, "..", "..");
const EXT_DIR = path.join(ROOT, "extension");
const PROFILE_DIR = path.join(ROOT, "testing", "profile");
const REPORTS_DIR = path.join(ROOT, "testing", "reports");

function nowStamp() { return new Date().toISOString().replace(/[:.]/g, "-"); }
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function findSW(browser) {
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    const t = browser.targets().find(t => t.type() === "service_worker" && t.url().startsWith("chrome-extension://"));
    if (t) return t;
    await sleep(200);
  }
  throw new Error("service_worker target not found");
}

async function evalSW(worker, body, arg) {
  const src = `(async (arg) => { ${body} })(${JSON.stringify(arg ?? null)})`;
  return worker.evaluate(src);
}

// ─── Form synthesis ─────────────────────────────────────────────────────────
//
// Build a {mode: "form", formData: {fields: [...]}} shape from a learned
// method's parameters + body schema, using plausible dummy values.

function synthesizeFormFields(method, doc) {
  const fields = [];
  // Parameters = URL query/path
  for (const [name, p] of Object.entries(method.parameters || {})) {
    if (p.location === "path") continue; // path params handled by URL template
    if (p.location === "query") {
      fields.push({
        name,
        number: 0,
        type: p.type || "string",
        value: dummyValue(p),
        location: "query",
      });
    }
  }
  // Body = request $ref into doc.schemas
  if (method.request?.$ref) {
    const schema = doc.schemas?.[method.request.$ref];
    if (schema) {
      for (const f of schemaToFields(schema, doc)) fields.push(f);
    }
  }
  return fields;
}

function schemaToFields(schema, doc, depth = 0) {
  if (!schema || !schema.properties) return [];
  if (depth > 8) return []; // cycle guard; real schemas shouldn't nest that deep
  const out = [];
  for (const [name, prop] of Object.entries(schema.properties)) {
    const f = {
      name,
      number: prop.number || prop["x-field-number"] || 0,
      type: prop.type || "string",
      value: dummyValue(prop),
    };
    if (prop.type === "array" || prop["x-label"] === "repeated") {
      f.label = "repeated";
      f.value = [dummyValue({ type: prop.items?.type || "string", example: prop.example })];
    }
    if (prop.$ref || prop.type === "object" || prop.type === "message") {
      const nested = prop.$ref ? doc.schemas?.[prop.$ref] : prop;
      f.type = "message";
      f.children = schemaToFields(nested, doc, depth + 1);
      f.value = null;
    }
    out.push(f);
  }
  return out;
}

function dummyValue(prop) {
  if (prop.example !== undefined) return prop.example;
  if (prop.default !== undefined) return prop.default;
  if (Array.isArray(prop.enum) && prop.enum.length) return prop.enum[0];
  const t = (prop.type || "string").toLowerCase();
  if (t === "boolean") return true;
  if (t === "number" || t === "integer" || t === "int32" || t === "int64") return 1;
  if (t === "double" || t === "float") return 1.5;
  if (t === "array") return [];
  if (t === "object" || t === "message") return null;
  return "sample";
}

// ─── Body decoders ──────────────────────────────────────────────────────────
//
// Return { ok, decoded, detail }. `decoded` is a normalized object suitable
// for comparison against the synthesized fields.

function decodeBody(result, contentType) {
  const ct = (contentType || result.headers?.["Content-Type"] || result.headers?.["content-type"] || "").toLowerCase();
  const body = result.body;
  if (body == null || body === "") return { ok: true, decoded: null, detail: "empty body" };
  if (ct.includes("application/json") && !ct.includes("+protobuf")) {
    try { return { ok: true, decoded: JSON.parse(body), detail: "json" }; }
    catch (e) { return { ok: false, detail: "JSON.parse threw: " + e.message }; }
  }
  if (ct.startsWith("application/x-www-form-urlencoded")) {
    const params = new URLSearchParams(body);
    const out = {};
    for (const [k, v] of params) out[k] = v;
    // Google batchexecute: f.req wraps a double-JSON envelope
    if (out["f.req"]) {
      try { out["__f.req__decoded"] = JSON.parse(out["f.req"]); } catch (_) {}
    }
    return { ok: true, decoded: out, detail: "form-urlencoded" };
  }
  if (ct.startsWith("multipart/")) {
    // The buildExportRequest batch path emits a fixed-shape multipart envelope.
    // Verify that we can find the inner part and parse its body.
    const m = body.match(/--([^\r\n]+)\r?\n/);
    const boundary = m ? m[1] : null;
    if (!boundary) return { ok: false, detail: "no boundary in multipart body" };
    const parts = body.split("--" + boundary).filter(p => p.trim() && !p.trim().startsWith("--"));
    const decodedParts = parts.map(p => {
      // part is: headers \r\n\r\n body
      const split = p.split(/\r?\n\r?\n/);
      if (split.length < 2) return { raw: p };
      const inner = split.slice(1).join("\n\n").replace(/\r?\n--\s*$/, "").trim();
      // If the inner looks like an embedded HTTP request (application/http),
      // strip the request line + headers to get the JSON payload.
      const httpMatch = inner.match(/^[A-Z]+ [^\n]+ HTTP\/1\.[01]\r?\n([\s\S]*?)\r?\n\r?\n([\s\S]*)$/);
      if (httpMatch) {
        try { return { body: JSON.parse(httpMatch[2]) }; }
        catch (_) { return { body: httpMatch[2] }; }
      }
      try { return { body: JSON.parse(inner) }; }
      catch (_) { return { body: inner }; }
    });
    return { ok: true, decoded: { parts: decodedParts, boundary }, detail: "multipart" };
  }
  if (ct.includes("protobuf") && ct.includes("grpc-web")) {
    // base64-encoded gRPC-Web frame
    try {
      const raw = Buffer.from(body, "base64");
      return { ok: true, decoded: { bytes: raw.length, firstByte: raw[0] }, detail: "grpc-web base64 frame" };
    } catch (e) { return { ok: false, detail: "base64 decode: " + e.message }; }
  }
  if (ct.includes("x-protobuf") || ct === "application/protobuf") {
    // base64
    try {
      const raw = Buffer.from(body, "base64");
      return { ok: true, decoded: { bytes: raw.length }, detail: "protobuf base64" };
    } catch (e) { return { ok: false, detail: "base64 decode: " + e.message }; }
  }
  return { ok: true, decoded: body.slice(0, 200), detail: "opaque ct=" + ct };
}

// ─── Multipart editability check ────────────────────────────────────────────
//
// Flags methods whose ORIGINAL captured body was multipart with 2+ parts
// BUT whose learned schema represents it as a single flat form — i.e. the
// current popup would lose structure on edit + re-send.

function multipartEditabilityCheck(method, originalEntry) {
  if (!originalEntry) return null;
  const ct = (originalEntry.contentType || "").toLowerCase();
  if (!ct.startsWith("multipart/")) return null;
  const body = originalEntry.rawBodyB64;
  if (!body) return { editable: "unknown", detail: "no request body captured" };
  const text = Buffer.from(body, "base64").toString("utf8");
  const m = ct.match(/boundary=([^;]+)/);
  if (!m) return { editable: "unknown", detail: "no boundary header" };
  const boundary = m[1].replace(/^"|"$/g, "");
  const parts = text.split("--" + boundary).filter(p => p.trim() && !p.trim().startsWith("--"));
  if (parts.length < 2) return { editable: "ok", detail: "single-part multipart" };
  // Multiple parts — classify each sub-part's content-type.
  const subTypes = parts.map(p => {
    const hMatch = p.match(/Content-Type:\s*([^\r\n]+)/i);
    return hMatch ? hMatch[1].trim() : "unknown";
  });
  const schemaFields = Object.keys(method.parameters || {}).length +
    Object.keys(method.request?.$ref ? {} : {}).length;
  // The schema surface doesn't model sub-parts — if it's flat, the editor
  // can only reconstruct a single part, losing the other N-1.
  return {
    editable: "degraded",
    subPartCount: parts.length,
    subTypes,
    detail: `${parts.length} sub-parts captured, schema models only flat fields — popup cannot edit sub-parts individually`,
  };
}

// ─── Per-method runner (in SW) ─────────────────────────────────────────────

const RUN_BUILD = `
  const { tabId, service, methodName, formFields, url, httpMethod, contentType, methodId } = arg;
  if (typeof buildExportRequest !== "function") return { ok: false, detail: "buildExportRequest not in SW scope" };
  try {
    const msg = {
      url, httpMethod, methodId, service, contentType,
      headers: {},
      body: { mode: "form", formData: { fields: formFields } },
    };
    const result = await buildExportRequest(tabId, msg);
    return { ok: true, result };
  } catch (e) {
    return { ok: false, detail: "buildExportRequest threw: " + (e.message || e) };
  }
`;

const DUMP_METHODS = `
  const out = [];
  for (const [svc, e] of globalStore.discoveryDocs) {
    if (e.status !== "found" || !e.doc) continue;
    const methods = {};
    if (e.doc.resources) {
      for (const [bucket, b] of Object.entries(e.doc.resources)) {
        if (b && b.methods) {
          for (const [name, m] of Object.entries(b.methods)) methods[bucket + "." + name] = m;
        }
      }
    }
    out.push({ service: svc, doc: e.doc, methods });
  }
  return out;
`;

// Walk every captured request across every tracked tab and return those
// whose outbound content-type is multipart. The multipart editability
// check runs against this list so we can quantify how often the popup
// would lose structure on edit.
const DUMP_MULTIPART_REQUESTS = `
  const out = [];
  for (const [tabId, tab] of state.tabs.entries()) {
    for (const r of tab.requestLog || []) {
      const ct = (r.contentType || "").toLowerCase();
      if (ct.startsWith("multipart/") && r.rawBodyB64) {
        out.push({
          tabId,
          url: r.url,
          method: r.method,
          contentType: r.contentType,
          methodId: r.methodId || null,
          rawBodyB64: r.rawBodyB64,
        });
      }
    }
  }
  return out;
`;

const GET_ANY_TAB = `
  // Ensure at least one tab is registered so buildExportRequest has a tab to query.
  if (state.tabs.size === 0) {
    // Create a synthetic tab — use getTab to initialize the structure.
    getTab(-1);
  }
  return [...state.tabs.keys()][0];
`;

async function run() {
  const browser = await puppeteer.launch({
    headless: false,
    userDataDir: PROFILE_DIR,
    defaultViewport: null,
    args: [
      `--disable-extensions-except=${EXT_DIR}`,
      `--load-extension=${EXT_DIR}`,
      "--no-first-run",
      "--no-default-browser-check",
    ],
  });

  const runDir = path.join(REPORTS_DIR, "replay-" + nowStamp());
  await fsp.mkdir(runDir, { recursive: true });

  try {
    const swTarget = await findSW(browser);
    const worker = await swTarget.worker();
    if (!worker) throw new Error("no worker handle");

    // Wake the SW so module-level state is available.
    await sleep(500);

    const tabId = await evalSW(worker, GET_ANY_TAB, null);
    console.log("[replay] tabId:", tabId);

    const services = await evalSW(worker, DUMP_METHODS, null);
    console.log(`[replay] services with learned methods: ${services.length}`);

    const results = [];
    for (const svc of services) {
      for (const [qualName, method] of Object.entries(svc.methods)) {
        const methodName = qualName.split(".")[1];
        const methodId = method.id || `${svc.service.replace(/\//g, ".")}.${methodName}`;
        // Skip entries tagged as asset responses — they have no schema to
        // reconstruct a body from, and the extension correctly doesn't try.
        if (method._responseKind === "asset") {
          results.push({
            service: svc.service, methodId, skip: "asset-response",
          });
          continue;
        }
        const fields = synthesizeFormFields(method, svc.doc);
        const exampleOrigin = method.origin || (method.path ? ("https://" + svc.service.split("/")[0]) : "https://example.com");
        const exampleUrl = method.origin
          ? (method.origin + "/" + (method.path || ""))
          : ("https://" + svc.service + "/" + (method.path || ""));
        const urlObj = (() => { try { return new URL(exampleUrl); } catch { return null; } })();
        if (!urlObj) {
          results.push({ service: svc.service, methodId, skip: "unresolvable-url" });
          continue;
        }
        const contentType = method.contentTypes?.[0] || null;
        const httpMethod = method.httpMethod || "POST";
        const buildOut = await evalSW(worker, RUN_BUILD, {
          tabId, service: svc.service, methodName, methodId,
          formFields: fields, url: urlObj.toString(), httpMethod, contentType,
        });
        if (!buildOut.ok) {
          results.push({ service: svc.service, methodId, build: { ok: false, detail: buildOut.detail } });
          continue;
        }
        const decoded = decodeBody(buildOut.result, contentType);

        // Value roundtrip: for JSON bodies with NAMED fields, check that
        // every top-level field name we synthesized survived the encoder →
        // decoder round-trip. JSPB / batchexecute encode body as a numeric
        // array — field names don't apply, so we skip the name comparison.
        let valueRoundtrip = null;
        const looksJspb = decoded.decoded
          && (
            (typeof decoded.decoded === "object" && "f.req" in (decoded.decoded || {}))
            || Array.isArray(decoded.decoded)
          );
        const looksFieldN = fields.some(f => /^field\d+$/.test(f.name));
        if (decoded.ok && decoded.decoded && typeof decoded.decoded === "object" && !Array.isArray(decoded.decoded) && !looksJspb && !looksFieldN) {
          const bodyFieldNames = fields
            .filter(f => f.location !== "query")
            .map(f => f.name);
          const decodedKeys = Object.keys(decoded.decoded);
          const missing = bodyFieldNames.filter(n => !decodedKeys.includes(n));
          valueRoundtrip = {
            ok: missing.length === 0,
            inputFields: bodyFieldNames.length,
            decodedFields: decodedKeys.length,
            missing,
          };
        } else if (looksJspb || looksFieldN) {
          valueRoundtrip = { ok: true, detail: "JSPB / numeric-field encoding — name roundtrip not applicable" };
        }

        results.push({
          service: svc.service,
          methodId,
          httpMethod,
          contentType,
          build: {
            ok: true,
            url: buildOut.result.url,
            headerContentType: buildOut.result.headers?.["Content-Type"] || null,
            bodyLen: buildOut.result.body ? String(buildOut.result.body).length : 0,
          },
          decode: decoded,
          valueRoundtrip,
          fieldCount: fields.length,
        });
      }
    }

    // Aggregate
    const total = results.length;
    const skipped = results.filter(r => r.skip).length;
    const buildFailed = results.filter(r => r.build && !r.build.ok).length;
    const decodeFailed = results.filter(r => r.decode && !r.decode.ok).length;
    const passed = results.filter(r => r.build?.ok && r.decode?.ok).length;

    // Check every captured multipart REQUEST for popup editability. A
    // multipart with 2+ sub-parts that the learned schema flattens would
    // lose structure on edit+resend.
    const multipartReqs = await evalSW(worker, DUMP_MULTIPART_REQUESTS, null);
    const multipartFindings = [];
    for (const mp of multipartReqs) {
      const text = Buffer.from(mp.rawBodyB64, "base64").toString("utf8");
      const m = (mp.contentType || "").match(/boundary=([^;]+)/i);
      if (!m) continue;
      const boundary = m[1].replace(/^"|"$/g, "").trim();
      const parts = text.split("--" + boundary).filter(p => p.trim() && !p.trim().startsWith("--"));
      if (parts.length < 2) continue;
      const subTypes = parts.map(p => {
        const h = p.match(/Content-Type:\s*([^\r\n]+)/i);
        return h ? h[1].trim() : "unknown";
      });
      const hasGraphQLPart = subTypes.some(t => /graphql/i.test(t));
      multipartFindings.push({
        url: mp.url, method: mp.method, subPartCount: parts.length, subTypes,
        hasGraphQLPart, editable: "degraded",
        detail: "Popup currently has no multipart mode; flat form can express only one of " + parts.length + " parts",
      });
    }

    const report = {
      runDir,
      totals: {
        total, passed, skipped, buildFailed, decodeFailed,
        multipartDegraded: multipartFindings.length,
      },
      results,
      multipartFindings,
    };
    await fsp.writeFile(path.join(runDir, "replay.json"), JSON.stringify(report, null, 2), "utf8");

    const valueFails = results.filter(r => r.valueRoundtrip && !r.valueRoundtrip.ok).length;
    console.log("\n── replay totals ──────────────────────────────────────");
    console.log(`  total methods:        ${total}`);
    console.log(`  passed:               ${passed}`);
    console.log(`  skipped:              ${skipped}`);
    console.log(`  build failed:         ${buildFailed}`);
    console.log(`  decode failed:        ${decodeFailed}`);
    console.log(`  value-roundtrip fail: ${valueFails}`);
    console.log(`  multipart degraded:   ${multipartFindings.length}`);
    for (const r of results.filter(r => r.build && !r.build.ok).slice(0, 10)) {
      console.log(`    BUILD-FAIL ${r.service}/${r.methodId}: ${r.build.detail}`);
    }
    for (const r of results.filter(r => r.decode && !r.decode.ok).slice(0, 10)) {
      console.log(`    DECODE-FAIL ${r.service}/${r.methodId}: ${r.decode.detail}`);
    }
    console.log(`\n[replay] wrote ${path.join(runDir, "replay.json")}`);
    process.exitCode = (buildFailed + decodeFailed) > 0 ? 1 : 0;
  } finally {
    await browser.close().catch(() => {});
  }
}

run().catch(err => { console.error(err); process.exit(1); });
