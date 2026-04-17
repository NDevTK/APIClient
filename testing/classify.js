// Offline analyzer for harness reports.
//
// Reads the newest (or --dir) report directory written by harness.js and
// produces a quality summary that:
//   1. Classifies every captured request as api/static/unknown using ONLY
//      response body content (no Content-Type, no URL extension). This is
//      the user's ground-truth concern: an API can legitimately return an
//      image. So we sniff magic bytes + parseability, nothing else.
//   2. Compares the content-based label to what the extension learned —
//      i.e. was it added to a service's discovery doc? Disagreements are
//      reported per-site as triage lists.
//   3. Scores method naming: collision rate within a service, CRUD-verb
//      presence, bare-number/UUID-looking path segments left in names.
//   4. Scores body-param learning: fields present in request JSON bodies
//      vs fields learned into the service schema.
//   5. Lists every security finding with its source URL, line, sink type,
//      source type, and the 30-line code window from the saved script —
//      for the reviewer to manually decide whether each one is real.
//
// Usage:
//   npm run classify                  # uses newest report dir
//   npm run classify -- --dir testing/reports/<ISO>
//   npm run classify -- --site github_home

"use strict";

const path = require("path");
const fs = require("fs");
const fsp = fs.promises;

const REPORTS_DIR = path.join(__dirname, "reports");

function parseArgs(argv) {
  const out = { dir: null, site: null };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--dir") out.dir = argv[++i];
    else if (a === "--site") out.site = argv[++i];
  }
  return out;
}

async function pickLatestRun() {
  const entries = await fsp.readdir(REPORTS_DIR, { withFileTypes: true });
  // Only consider harness run dirs — their names are pure ISO timestamps
  // (e.g. "2026-04-17T01-47-06-704Z"). Suite output dirs are prefixed
  // ("ui-...", "replay-...") and should not be picked as the latest.
  const harnessRe = /^\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}-\d{3}Z$/;
  const dirs = entries
    .filter(e => e.isDirectory() && harnessRe.test(e.name))
    .map(e => e.name)
    .sort();
  if (!dirs.length) throw new Error("no harness reports found. run `npm run harness` first.");
  return path.join(REPORTS_DIR, dirs[dirs.length - 1]);
}

// ─── Content-only classifier ────────────────────────────────────────────────

function bufFromEntry(entry) {
  const body = entry.responseBody;
  if (body == null) return null;
  if (entry.responseBase64) {
    try { return Buffer.from(body, "base64"); }
    catch { return null; }
  }
  return Buffer.from(body, "utf8");
}

// Magic-byte signatures. First N bytes only.
function sniffMagic(buf) {
  if (buf.length >= 8 && buf[0] === 0x89 && buf[1] === 0x50 && buf[2] === 0x4e && buf[3] === 0x47) return "image/png";
  if (buf.length >= 3 && buf[0] === 0xff && buf[1] === 0xd8 && buf[2] === 0xff) return "image/jpeg";
  if (buf.length >= 6 && buf[0] === 0x47 && buf[1] === 0x49 && buf[2] === 0x46 && buf[3] === 0x38) return "image/gif";
  if (buf.length >= 12 && buf[0] === 0x52 && buf[1] === 0x49 && buf[2] === 0x46 && buf[3] === 0x46 && buf[8] === 0x57 && buf[9] === 0x45 && buf[10] === 0x42 && buf[11] === 0x50) return "image/webp";
  if (buf.length >= 4 && buf[0] === 0x25 && buf[1] === 0x50 && buf[2] === 0x44 && buf[3] === 0x46) return "application/pdf";
  if (buf.length >= 4 && buf[0] === 0x77 && buf[1] === 0x4f && buf[2] === 0x46 && buf[3] === 0x46) return "font/woff";
  if (buf.length >= 4 && buf[0] === 0x77 && buf[1] === 0x4f && buf[2] === 0x46 && buf[3] === 0x32) return "font/woff2";
  if (buf.length >= 4 && buf[0] === 0x00 && buf[1] === 0x01 && buf[2] === 0x00 && buf[3] === 0x00) return "font/ttf";
  if (buf.length >= 4 && buf[0] === 0x1f && buf[1] === 0x8b) return "application/gzip";
  if (buf.length >= 2 && buf[0] === 0x50 && buf[1] === 0x4b) return "application/zip";
  // SVG = xml-ish text, handled in text sniff.
  return null;
}

function sniffText(buf) {
  // Printable check runs on first 64KB, but we return the full buffer as
  // text so downstream parsers (JSON / NDJSON / SSE) see the whole thing —
  // truncating a 1MB JSON response to 64KB breaks JSON.parse and cascades
  // into false CSS matches.
  const probe = buf.slice(0, Math.min(buf.length, 65536));
  let nonPrintable = 0;
  for (let i = 0; i < probe.length; i++) {
    const b = probe[i];
    if (b === 9 || b === 10 || b === 13) continue;
    if (b < 32 || b > 126) {
      // Allow high-ascii (UTF-8 continuation bytes) — treat them as printable.
      if (b >= 128) continue;
      nonPrintable++;
    }
  }
  if (nonPrintable / Math.max(1, probe.length) > 0.05) return null;
  return buf.toString("utf8");
}

function tryJSON(text) {
  let t = text.trim();
  if (!t) return null;
  // Google XSSI prefix: `)]}'` on its own leading line. Strip before parse.
  if (t.startsWith(")]}'")) t = t.replace(/^\)\]\}'[\r\n]*/, "").trim();
  if (t[0] !== "{" && t[0] !== "[" && t[0] !== '"') return null;
  try { return JSON.parse(t); } catch { return null; }
}

function looksLikeGraphQL(obj) {
  if (!obj || typeof obj !== "object") return false;
  return "data" in obj || "errors" in obj || "extensions" in obj;
}

function looksLikeNDJSON(text) {
  const lines = text.split("\n").filter(l => l.trim());
  if (lines.length < 2) return false;
  let ok = 0;
  for (const l of lines.slice(0, 10)) {
    try { JSON.parse(l); ok++; } catch {}
  }
  return ok === Math.min(10, lines.length);
}

function looksLikeSSE(text) {
  // SSE frames are "event: ...\n" or "data: ...\n" separated by blank lines.
  return /^(event:|data:|id:|retry:)/m.test(text) && /\n\n/.test(text);
}

function looksLikeHTML(text) {
  const t = text.trim().toLowerCase();
  if (t.startsWith("<!doctype") || t.startsWith("<html")) return true;
  // HTML fragments: starts with a tag (Reddit returns <faceplate-loader> etc).
  if (/^<[a-z][a-z0-9-]*[\s>]/i.test(t)) return true;
  return /<head[\s>]/.test(t.slice(0, 2000));
}

// React Server Components streaming payload. Line-framed `<hex>:<payload>`
// where <hex> is the row id and <payload> is one of I[...]/HL[...]/T<len>,.../etc.
// Next.js apps emit this with Content-Type `text/x-component` — the response
// IS an API in the functional sense (dynamic, varies by route), just not
// standard JSON. Treating it as static CSS (regex brace-match) is a
// classifier bug we hit on vercel + every other Next app.
function looksLikeRSC(text) {
  if (!text) return false;
  const lines = text.split("\n", 3);
  if (lines.length < 1) return false;
  // First two non-empty lines must match `<hex>:<payload>`.
  let matched = 0;
  for (const line of lines) {
    if (!line) continue;
    if (/^[0-9a-f]+:/.test(line)) matched++;
  }
  return matched >= 1 && /^[0-9a-f]+:/.test(lines[0]);
}

function looksLikeJS(text) {
  // Very rough — we only need to rule out static JS from "API that returned JS".
  // Real APIs rarely ship function definitions + module syntax at the top.
  const head = text.slice(0, 2000).trimStart();
  return /^(import |export |"use strict"|\/\*|\(function|!function|var |let |const |class )/m.test(head);
}

function looksLikeCSS(text) {
  const head = text.slice(0, 2000);
  return /[\.#@a-zA-Z][^{}\n]{0,200}\{[^}]{0,500}\}/.test(head) && !/<html/i.test(head.slice(0, 200));
}

function looksLikeProtobuf(buf) {
  // Varint-tag first-byte heuristic. Not perfect; enough to separate binary
  // protobuf from binary media (which magic bytes already caught).
  if (buf.length < 2) return false;
  const b = buf[0];
  const wireType = b & 0x07;
  // Valid wire types: 0,1,2,5.  Common field numbers 1..15 → byte in range 0x08..0x7d.
  if (wireType !== 0 && wireType !== 1 && wireType !== 2 && wireType !== 5) return false;
  // Length-delimited is the common shape; sanity-check the declared length.
  if (wireType === 2 && buf.length >= 3) {
    // read varint at byte 1
    let len = 0, shift = 0, i = 1;
    while (i < buf.length && i < 6) {
      const x = buf[i];
      len |= (x & 0x7f) << shift;
      if (!(x & 0x80)) { i++; break; }
      shift += 7; i++;
    }
    if (len > 0 && len < buf.length) return true;
  }
  return wireType === 0 || wireType === 1 || wireType === 5;
}

function looksLikeGrpcFrame(buf) {
  // gRPC-Web: 1-byte flag (0x00 or 0x80) + 4-byte big-endian length.
  if (buf.length < 5) return false;
  if (buf[0] !== 0x00 && buf[0] !== 0x80) return false;
  const len = (buf[1] << 24) | (buf[2] << 16) | (buf[3] << 8) | buf[4];
  return len >= 0 && len + 5 <= buf.length + 1024; // allow slight slack for trailers
}

function classifyBody(entry) {
  const buf = bufFromEntry(entry);
  if (!buf || !buf.length) return { label: "empty", why: "no body", confidence: 1 };

  const magic = sniffMagic(buf);
  if (magic) return { label: "static", kind: magic, why: "magic-bytes", confidence: 1 };

  if (looksLikeGrpcFrame(buf)) return { label: "api", kind: "grpc-web", why: "grpc-frame", confidence: 0.9 };

  const text = sniffText(buf);
  if (text == null) {
    if (looksLikeProtobuf(buf)) return { label: "api", kind: "protobuf", why: "wire-format", confidence: 0.7 };
    return { label: "binary", kind: "unknown", why: "non-printable", confidence: 0.3 };
  }

  const obj = tryJSON(text);
  if (obj) {
    if (looksLikeGraphQL(obj)) return { label: "api", kind: "graphql", why: "graphql-shape", confidence: 1 };
    return { label: "api", kind: "json", why: "json-parse", confidence: 0.95 };
  }
  if (looksLikeNDJSON(text)) return { label: "api", kind: "ndjson", why: "ndjson-lines", confidence: 0.9 };
  if (looksLikeSSE(text)) return { label: "api", kind: "sse", why: "sse-frames", confidence: 0.9 };
  if (looksLikeRSC(text)) return { label: "api", kind: "rsc", why: "react-server-components", confidence: 0.95 };
  if (looksLikeHTML(text)) return { label: "static", kind: "html", why: "html-doctype", confidence: 0.95 };
  if (looksLikeJS(text)) return { label: "static", kind: "js", why: "js-module-shape", confidence: 0.85 };
  if (looksLikeCSS(text)) return { label: "static", kind: "css", why: "css-rules", confidence: 0.85 };
  // Plaintext that isn't any known format — could still be an API.
  return { label: "unknown", kind: "text", why: "unrecognized-text", confidence: 0.2 };
}

// ─── Extension-learned-it probe ────────────────────────────────────────────

function collectMethods(doc) {
  // Discovery-style doc: doc.resources.<bucket>.methods.<methodName>
  // OpenAPI-style: doc.paths[path].<verb>
  // Flat fallback: doc.methods
  if (!doc) return {};
  const out = {};
  if (doc.resources) {
    for (const [bucket, b] of Object.entries(doc.resources)) {
      const m = b && b.methods;
      if (m) for (const [name, v] of Object.entries(m)) out[`${bucket}.${name}`] = v;
    }
  }
  if (doc.methods) for (const [k, v] of Object.entries(doc.methods)) out[k] = v;
  if (doc.paths) {
    for (const [p, verbs] of Object.entries(doc.paths)) {
      if (verbs && typeof verbs === "object") {
        for (const [verb, op] of Object.entries(verbs)) out[`${verb.toUpperCase()} ${p}`] = op;
      }
    }
  }
  return out;
}

function extensionLearnedService(dump, service) {
  const d = dump.global.discovery || {};
  const entry = d[service];
  if (!entry || entry.status !== "found" || !entry.doc) return false;
  return Object.keys(collectMethods(entry.doc)).length > 0;
}

function extensionLearnedRequest(dump, entry) {
  // "Learned" = the service that this URL maps to has a non-empty discovery
  // doc AND the request log entry was assigned that service (the extension
  // classified it as an API worth tracking).
  if (!entry.service || entry.service === "unknown") return false;
  return extensionLearnedService(dump, entry.service);
}

// ─── Naming ────────────────────────────────────────────────────────────────

const CRUD_WORDS = ["get", "list", "create", "update", "delete", "post", "put", "patch", "fetch", "search", "query", "mutate", "send"];

function scoreNames(dump) {
  const discovery = dump.global.discovery || {};
  const perService = [];
  for (const [svc, entry] of Object.entries(discovery)) {
    if (entry.status !== "found" || !entry.doc) continue;
    const methods = collectMethods(entry.doc);
    const names = Object.keys(methods);
    // Strip bucket prefix ("learned.", "probed.") for naming quality —
    // those are organizational, not user-facing.
    const bare = names.map(n => n.split(".").pop());
    const byBaseName = new Map();
    for (const n of bare) byBaseName.set(n, (byBaseName.get(n) || 0) + 1);
    const collisions = bare.length - byBaseName.size;
    const crudy = bare.filter(n => {
      const lower = n.toLowerCase();
      return CRUD_WORDS.some(w => lower.includes(w));
    }).length;
    const uuid = bare.filter(n => /[a-f0-9]{8}-[a-f0-9]{4}/i.test(n) || /^\d+$/.test(n)).length;
    const generic = bare.filter(n => /^(root|index|default|home|main|api|v\d+)$/i.test(n)).length;
    perService.push({
      service: svc,
      methodCount: bare.length,
      collisions,
      crudyPct: bare.length ? Math.round((crudy / bare.length) * 100) : 0,
      uuidInName: uuid,
      genericInName: generic,
      sampleNames: bare.slice(0, 6),
    });
  }
  return perService.sort((a, b) => b.methodCount - a.methodCount);
}

// ─── Body param learning ──────────────────────────────────────────────────

function countJsonLeaves(obj, prefix, out) {
  if (obj == null) return;
  if (Array.isArray(obj)) {
    // Treat index 0 as a representative element for counting purposes.
    if (obj.length) countJsonLeaves(obj[0], prefix + "[]", out);
    return;
  }
  if (typeof obj === "object") {
    for (const k of Object.keys(obj)) countJsonLeaves(obj[k], prefix ? `${prefix}.${k}` : k, out);
    return;
  }
  out.add(prefix);
}

function extractRequestJsonFields(entry) {
  if (!entry.rawBodyB64) return null;
  let text;
  try {
    text = Buffer.from(entry.rawBodyB64, "base64").toString("utf8");
  } catch { return null; }
  const t = text.trim();
  if (!t || (t[0] !== "{" && t[0] !== "[")) return null;
  let obj;
  try { obj = JSON.parse(t); } catch { return null; }
  const fields = new Set();
  countJsonLeaves(obj, "", fields);
  return [...fields];
}

function countLearnedFields(dump, service, methodName) {
  const entry = dump.global.discovery?.[service];
  if (!entry || entry.status !== "found" || !entry.doc) return null;
  const methods = collectMethods(entry.doc);
  const m = methods[methodName] || methods[`learned.${methodName}`] || methods[`probed.${methodName}`];
  if (!m) return null;
  // Parameters are URL/query params. The request body shape is in doc.schemas[$ref].
  let count = Object.keys(m.parameters || {}).length;
  const bodyRef = m.request && m.request.$ref;
  if (bodyRef) {
    const schema = entry.doc.schemas?.[bodyRef];
    if (schema && schema.properties) count += Object.keys(schema.properties).length;
  }
  return count;
}

function scoreParams(dump) {
  const log = dump.tab.requestLog || [];
  const rows = [];
  for (const entry of log) {
    const presentFields = extractRequestJsonFields(entry);
    if (!presentFields || !presentFields.length) continue;
    const learned = entry.methodId
      ? countLearnedFields(dump, entry.service, entry.methodId.split(".").pop())
      : null;
    rows.push({
      url: entry.url,
      method: entry.method,
      service: entry.service,
      methodId: entry.methodId,
      presentFields: presentFields.length,
      learnedFields: learned,
    });
  }
  return rows;
}

// ─── Security triage ──────────────────────────────────────────────────────

async function loadScriptSource(runDir, siteName, sourceUrl) {
  const dir = path.join(runDir, `${siteName}.scripts`);
  try {
    const idx = JSON.parse(await fsp.readFile(path.join(dir, "index.json"), "utf8"));
    const row = idx.find(x => x.url === sourceUrl && x.ok);
    if (!row) return null;
    return await fsp.readFile(path.join(dir, row.file), "utf8");
  } catch { return null; }
}

function byteToLineCol(text, offset) {
  if (text == null || offset == null) return { line: null, col: null };
  let line = 1, col = 1;
  for (let i = 0; i < offset && i < text.length; i++) {
    if (text.charCodeAt(i) === 10) { line++; col = 1; } else col++;
  }
  return { line, col };
}

function codeWindow(text, line, col, radius = 4) {
  if (text == null || line == null) return null;
  const lines = text.split("\n");
  const lo = Math.max(0, line - 1 - radius);
  const hi = Math.min(lines.length, line + radius);
  // For minified code a single line can be 100KB+. If the target line is
  // huge, show a ±250 char window around the column instead.
  const target = lines[line - 1] || "";
  if (target.length > 500 && typeof col === "number") {
    const lo2 = Math.max(0, col - 250);
    const hi2 = Math.min(target.length, col + 250);
    const pre = lo2 > 0 ? "…" : "";
    const post = hi2 < target.length ? "…" : "";
    const snippet = target.slice(lo2, hi2);
    const caretPos = col - lo2 + pre.length;
    const caret = " ".repeat(Math.max(0, caretPos)) + "^";
    return `line ${line} col ${col}:\n${pre}${snippet}${post}\n${caret}`;
  }
  return lines.slice(lo, hi).map((l, i) => {
    const n = lo + i + 1;
    const marker = n === line ? ">" : " ";
    return `${marker} ${String(n).padStart(5)}  ${l}`;
  }).join("\n");
}

function resolveLoc(loc) {
  if (!loc) return { line: null, col: null };
  // Extension stores {line, column}; accept byte offset as fallback.
  if (typeof loc.line === "number") return { line: loc.line, col: loc.column ?? null };
  if (typeof loc.start === "number") return null; // caller falls through to byte math
  return { line: null, col: null };
}

async function buildSecurityTriage(runDir, siteName, dump) {
  const findings = dump.global.findings || {};
  const items = [];
  for (const [url, f] of Object.entries(findings)) {
    const source = await loadScriptSource(runDir, siteName, url);
    for (const sink of f.securitySinks || []) {
      const resolved = resolveLoc(sink.location) || byteToLineCol(source, sink.location?.start);
      const { line, col } = resolved;
      items.push({
        kind: "sink",
        sourceUrl: url,
        pageUrl: f.pageUrl || null,
        type: sink.type,
        sink: sink.sink,
        severity: sink.severity,
        sourceType: sink.sourceType,
        taintSource: sink.source,
        sanitized: !!sink.sanitized,
        line, col,
        codeWindow: codeWindow(source, line, col),
        extensionContext: sink.codeContext || null,
      });
    }
    for (const p of f.dangerousPatterns || []) {
      const resolved = resolveLoc(p.location) || byteToLineCol(source, p.location?.start);
      const { line, col } = resolved;
      items.push({
        kind: "pattern",
        sourceUrl: url,
        pageUrl: f.pageUrl || null,
        type: p.type,
        description: p.description,
        severity: p.severity,
        line, col,
        codeWindow: codeWindow(source, line, col),
        extensionContext: p.codeContext || null,
      });
    }
  }
  const rank = { high: 0, medium: 1, low: 2, info: 3 };
  items.sort((a, b) => (rank[a.severity] ?? 9) - (rank[b.severity] ?? 9));
  return items;
}

// ─── Per-site analysis ────────────────────────────────────────────────────

async function analyzeSite(runDir, file) {
  const report = JSON.parse(await fsp.readFile(path.join(runDir, file), "utf8"));
  if (!report.dump) return { name: report.name, url: report.url, error: report.error || "no dump" };
  const dump = report.dump;

  const requests = dump.tab.requestLog || [];
  const classified = requests.map(r => ({
    url: r.url,
    method: r.method,
    service: r.service,
    advertisedType: r.contentType || r.mimeType || null,
    extensionLearned: extensionLearnedRequest(dump, r),
    content: classifyBody(r),
    _hasBody: r.responseBody != null,
  }));

  // WS / postMessage / MessageChannel have no HTTP body shape — exclude.
  const isTransport = c => /^(WEBSOCKET|POSTMESSAGE|MSGCHANNEL)$/i.test(c.method || "");
  // Empty-body POST/PUT is fine (fire-and-forget APIs) — exclude from disagreement.
  const httpRequests = classified.filter(c => !isTransport(c));
  const extSaysApiBodyStatic = httpRequests.filter(c => c.extensionLearned && c.content.label === "static");
  const bodySaysApiExtMiss = httpRequests.filter(c => !c.extensionLearned && c.content.label === "api" && c.content.confidence >= 0.9);

  // Coverage — what each extension feature produced for this site.
  const ast = dump.tab.astResults || [];
  const coverage = {
    scriptsBuffered: (dump.tab.scripts || []).length,
    astScripts: ast.length,
    astCallSites: ast.reduce((n, a) => n + (a.fetchCallSites?.length || 0), 0),
    astValueConstraints: ast.reduce((n, a) => n + (a.valueConstraints?.length || 0), 0),
    astProtoFieldMaps: ast.reduce((n, a) => n + (a.protoFieldMaps?.length || 0), 0),
    astProtoEnums: ast.reduce((n, a) => n + (a.protoEnums?.length || 0), 0),
    astSourceMaps: ast.filter(a => a.sourceMap).length,
    frames: (dump.tab.frames || []).length,
    wsLogs: requests.filter(r => r.method === "WEBSOCKET").length,
    postMessageLogs: requests.filter(r => r.method === "POSTMESSAGE").length,
    msgChannelLogs: requests.filter(r => r.method === "MSGCHANNEL").length,
    sseLogs: classified.filter(c => c.content.kind === "sse").length,
    graphqlLogs: classified.filter(c => c.content.kind === "graphql").length,
    protobufLogs: classified.filter(c => c.content.kind === "protobuf" || c.content.kind === "grpc-web").length,
    probes: Object.keys(dump.global.probeResults || {}).length,
    apiKeys: Object.keys(dump.global.apiKeys || {}).length,
    discoveryChanges: Object.values(dump.global.discoveryChanges || {}).reduce((n, list) => n + (list?.length || 0), 0),
  };

  // APIs discovered by AST but never exercised during this run.
  const seenUrls = new Set(requests.map(r => r.url));
  const astOnlyEndpoints = [];
  for (const a of ast) {
    for (const cs of a.fetchCallSites || []) {
      const url = cs.url || cs.urlTemplate || null;
      if (!url) continue;
      if (!seenUrls.has(url)) {
        astOnlyEndpoints.push({ sourceScript: a.sourceUrl, method: cs.method || null, url });
      }
    }
  }

  const summary = {
    name: report.name,
    url: report.url,
    requestCount: requests.length,
    withBody: classified.filter(c => c._hasBody).length,
    byContentLabel: classified.reduce((acc, c) => {
      acc[c.content.label] = (acc[c.content.label] || 0) + 1;
      return acc;
    }, {}),
    extensionLearned: classified.filter(c => c.extensionLearned).length,
    disagreements: {
      extSaysApiBodyStatic: extSaysApiBodyStatic.length,
      bodySaysApiExtMiss: bodySaysApiExtMiss.length,
    },
    coverage,
    astOnlyEndpointCount: astOnlyEndpoints.length,
    naming: scoreNames(dump),
    params: scoreParams(dump),
  };

  const triage = await buildSecurityTriage(runDir, report.name, dump);

  return {
    summary,
    classified,
    disagreements: { extSaysApiBodyStatic, bodySaysApiExtMiss },
    astOnlyEndpoints,
    triage,
  };
}

// ─── Driver ───────────────────────────────────────────────────────────────

async function main() {
  const args = parseArgs(process.argv);
  const runDir = args.dir ? path.resolve(args.dir) : await pickLatestRun();
  console.log(`[classify] run dir: ${runDir}`);

  const entries = await fsp.readdir(runDir);
  const siteFiles = entries.filter(f => f.endsWith(".json") && f !== "run.json" && f !== "analysis.json");

  const analyses = [];
  for (const file of siteFiles) {
    const siteName = file.replace(/\.json$/, "");
    if (args.site && siteName !== args.site) continue;
    console.log(`[classify] → ${siteName}`);
    try {
      const a = await analyzeSite(runDir, file);
      analyses.push(a);
    } catch (e) {
      console.error(`[classify]   error on ${siteName}: ${e.message}`);
      analyses.push({ summary: { name: siteName, error: String(e) } });
    }
  }

  await fsp.writeFile(path.join(runDir, "analysis.json"), JSON.stringify(analyses, null, 2), "utf8");

  // Console summary
  console.log("\n── summary ──────────────────────────────────────────");
  for (const a of analyses) {
    const s = a.summary;
    if (!s || s.error) {
      console.log(`  ${s?.name || "?"}: ERROR — ${s?.error || "(unknown)"}`);
      continue;
    }
    const labels = Object.entries(s.byContentLabel).map(([k, v]) => `${k}=${v}`).join(" ");
    console.log(
      `  ${s.name.padEnd(22)}  reqs=${String(s.requestCount).padStart(4)}  learned=${String(s.extensionLearned).padStart(3)}  ${labels}  disagree={api-but-static:${s.disagreements.extSaysApiBodyStatic}, miss:${s.disagreements.bodySaysApiExtMiss}}  findings=${a.triage?.length ?? 0}`
    );
  }
  console.log(`\n[classify] wrote ${path.join(runDir, "analysis.json")}`);
  console.log("[classify] security triage is inline in analysis.json → triage[]. Each item has a codeWindow for manual review.");
}

main().catch(err => {
  console.error(err);
  process.exit(1);
});
