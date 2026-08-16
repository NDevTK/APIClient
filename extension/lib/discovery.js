// What is LEFT of this file is TWO components with two different consumers, and neither of them is the
// discovery FETCH any more:
//   * schema resolution for the Send panel   (findDiscoveryMethod / findMethodById / resolveDiscoverySchema)
//   * response-asset classification          (classifyResponseAsset — lib/response-decode.js)
//
// The React Server Components parser was the third and is gone — see the note at the foot of this file. What
// keeps the other two here is their CONSUMERS, which is the ordering jsaudit.mjs's own ledger header states:
// "a producer whose consumers still live in JS is not first, because moving it makes every call it serves
// cross the JS↔WASM boundary the architecture rule exists to delete." There is no host→engine compute edge in
// this architecture and there must not be one — the engine is the driver, the bridge relays. So the classifier
// leaves with lib/response-decode.js (jsaudit step 2 → reply_decode.c), whose `_isAsset` / `_isBoringFetch`
// gate its every learning call; and the schema half leaves with lib/send.js (step 7 → moat.c), which is what
// the Send panel actually asks. Its ALGORITHM is already in C on the engine's own side of that line: the
// WHATWG MIME Sniffing standard is core/mime/mime_sniff.c, and the schema of an engine-fetched discovery
// document is read by engine/host/solver/discovery.c.
//
// THE CANDIDATE SET AND ITS FETCH LOOP ARE GONE TO engine/host/solver/discovery.c. `buildDiscoveryUrls` stood
// here and `fetchDiscoveryForService` (lib/discovery-probe.js, deleted with it) walked its candidates in a
// `for` loop, awaiting each in turn — a drive-to-completion in the host, driven by what the page HAPPENED to
// send (response-decode.js called it off passive learning). CLAUDE.md §Attacker sources calls active discovery
// REQUIRED and §Architecture calls it the engine's; the engine now seeds ONE FLOW PER CANDIDATE ADDRESS on the
// one BFS frontier, so the probes are ranked, preempted, parked to the IDB cold tier and resumed in a later
// session like every other flow, and what they learn lands in the same @H endpoint surface as forced
// execution's own findings. It is also GET-only structurally there: the park takes a URL and has no method
// parameter, which is the same shape `pageContextGet` was given here for the same rule.

// ─── Schema Resolution (for Send Request form) ─────────────────────────────

/**
 * Find a discovery method matching an endpoint's path and HTTP method.
 * Walks doc.resources recursively, comparing method paths.
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} endpointPath - URL path (e.g. "/v1/people:search")
 * @param {string} httpMethod - HTTP method (e.g. "POST")
 * @returns {{method: object, resourceName: string}|null}
 */
function findDiscoveryMethod(doc, endpointPath, httpMethod) {
  if (!doc || !doc.resources) return null;

  // canParse guard so the basePath extraction doesn't use throw as a
  // parse-validity test. An empty / malformed baseUrl yields empty basePath,
  // which means no prefix stripping below — correct semantic.
  const baseUrl = doc.baseUrl || doc.rootUrl || "";
  const basePath = (baseUrl && URL.canParse(baseUrl))
    ? new URL(baseUrl).pathname.replace(/\/$/, "")
    : "";

  // Strip basePath prefix from endpointPath for comparison
  let normPath = endpointPath;
  if (basePath && normPath.startsWith(basePath)) {
    normPath = normPath.slice(basePath.length);
  }
  normPath = normPath.replace(/^\//, "");

  function normalizePath(p) {
    // Convert {param} placeholders to a wildcard for matching
    return (p || "").replace(/^\//, "").replace(/\{[^}]+\}/g, "*");
  }

  function matchPath(methodPath, target) {
    const a = normalizePath(methodPath);
    const b = target.replace(/\{[^}]+\}/g, "*");
    if (a === b) return true;
    // Also try matching with path params as segments
    const aParts = a.split("/");
    const bParts = b.split("/");
    if (aParts.length !== bParts.length) return false;
    for (let i = 0; i < aParts.length; i++) {
      if (aParts[i] === "*" || bParts[i] === "*") continue;
      if (aParts[i] !== bParts[i]) return false;
    }
    return true;
  }

  let best = null;

  function walk(res, prefix) {
    for (const [name, r] of Object.entries(res || {})) {
      const fullName = prefix ? prefix + "." + name : name;
      for (const [, m] of Object.entries(r.methods || {})) {
        const mMethod = (m.httpMethod || "").toUpperCase();
        const mPath = m.flatPath || m.path || "";
        if (
          mMethod === httpMethod.toUpperCase() &&
          matchPath(mPath, normPath)
        ) {
          best = { method: m, resourceName: fullName };
          return;
        }
      }
      if (r.resources) walk(r.resources, fullName);
      if (best) return;
    }
  }

  walk(doc.resources, "");

  // Fallback: partial match (endsWith) for flexibility
  if (!best) {
    function walkPartial(res, prefix) {
      for (const [name, r] of Object.entries(res || {})) {
        const fullName = prefix ? prefix + "." + name : name;
        for (const [, m] of Object.entries(r.methods || {})) {
          const mPath = normalizePath(m.flatPath || m.path || "");
          if (
            normPath.endsWith(mPath) ||
            mPath.endsWith(normPath.replace(/\{[^}]+\}/g, "*"))
          ) {
            best = { method: m, resourceName: fullName };
            return;
          }
        }
        if (r.resources) walkPartial(r.resources, fullName);
        if (best) return;
      }
    }
    walkPartial(doc.resources, "");
  }

  return best;
}

/**
 * Find a discovery method by its ID (e.g. "people.people.get").
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} methodId - The method ID to find
 * @returns {{method: object, resourceName: string}|null}
 */
function findMethodById(doc, methodId) {
  if (!doc || !doc.resources) return null;

  let best = null;

  function walk(res, prefix) {
    for (const [name, r] of Object.entries(res || {})) {
      const fullName = prefix ? prefix + "." + name : name;
      for (const [, m] of Object.entries(r.methods || {})) {
        if (m.id === methodId) {
          best = { method: m, resourceName: fullName };
          return;
        }
      }
      if (r.resources) walk(r.resources, fullName);
      if (best) return;
    }
  }

  walk(doc.resources, "");
  return best;
}

/**
 * Resolve a discovery document schema into a recursive field list.
 * Follows $ref pointers in doc.schemas to build the full type tree.
 *
 * Iterative driver — single shared queue processes SCHEMA frames
 * (resolve a named schema's fields into a target array) and PROP
 * frames (populate a single field from a prop definition). Cycles
 * are detected by a per-chain visited set carried on each frame:
 * the set reflects the chain from the root through nested $refs to
 * the current frame, so a back-reference adds a self-circular
 * sentinel field to the children array. No depth cap — visited set
 * is structurally sufficient for any finite OpenAPI schema graph.
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} schemaName - Schema name to resolve (e.g. "Person")
 * @returns {Array<{name, type, required, description, label, children}>}
 */
function resolveDiscoverySchema(doc, schemaName) {
  var fields = [];
  if (!doc || !doc.schemas || !doc.schemas[schemaName]) return fields;
  fields.id = schemaName;
  var queue = [{ kind: "SCHEMA", doc: doc, schemaName: schemaName,
                  visited: new Set(), into: fields }];
  _drainDiscoveryQueue(queue);
  return fields;
}

/* `mapDiscoveryProperty` DELETED — it mapped one discovery property to a field descriptor through the same
   queue `resolveDiscoverySchema` drives, and NOTHING CALLED IT: not this file, not the popup, not send.js.
   A public-looking entry point with no caller reads as a capability the surface has, and the next reader
   builds on it. The queue and its two step functions below have a live caller and stay. */

function _buildDiscoveryFieldShell(name, prop, requiredList) {
  var isRequired = (requiredList || []).indexOf(name) >= 0;
  return {
    name: prop.name || name,
    customName: !!prop.customName,
    type: "string",
    required: isRequired,
    description: prop.description || null,
    label: isRequired ? "required" : "optional",
    number: prop.id != null ? prop.id : null,
    messageType: null,
    children: null,
    _defaultValue: prop._defaultValue == null ? null : prop._defaultValue,
    _defaultConfidence: prop._defaultConfidence == null ? null : prop._defaultConfidence,
    _requiredConfidence: prop._requiredConfidence == null ? null : prop._requiredConfidence,
    _range: prop._range || null,
    _detectedEnum: !!prop._detectedEnum,
    _exampleValue: prop._exampleValue === undefined ? null : prop._exampleValue,
    _exampleValueSource: prop._exampleValueSource || null,
    _astValidValues: prop._astValidValues || null,
  };
}

// Sentinel field appended to a children array when a $ref points back
// to a schema already on the current chain. The OpenAPI/JSON-Schema
// spec permits self-referential schemas, so a finite tree
// representation must terminate the cycle somewhere — the sentinel
// makes the truncation point visible to callers (form builders,
// renderers) instead of silently dropping data.
function _circularRefSentinel(schemaName) {
  return {
    name: "...",
    type: "message",
    description: "(circular ref: " + schemaName + ")",
    label: "optional",
  };
}

function _drainDiscoveryQueue(queue) {
  // LIFO drain: the only observable side effect is registration into
  // docSchemas keyed by schema name, so order doesn't matter and pop()
  // avoids the O(N) cost of shift().
  while (queue.length > 0) {
    var item = queue.pop();
    if (item.kind === "SCHEMA") _stepResolveSchema(item, queue);
    else if (item.kind === "PROP") _stepMapProperty(item, queue);
  }
}

// SCHEMA frame: produce field shells for each property of
// `doc.schemas[schemaName]`, append them to `into`, and queue PROP
// frames that populate each shell. Cycle check: if `schemaName` is
// already on the current chain (visited set), append the circular-ref
// sentinel and skip processing.
function _stepResolveSchema(item, queue) {
  var doc = item.doc, schemaName = item.schemaName, visited = item.visited, into = item.into;
  if (!doc || !doc.schemas || !doc.schemas[schemaName]) return;
  if (visited.has(schemaName)) {
    into.push(_circularRefSentinel(schemaName));
    return;
  }
  var nextVisited = new Set(visited);
  nextVisited.add(schemaName);
  var schema = doc.schemas[schemaName];
  var required = schema.required || [];
  var i = 1;
  for (var propName in schema.properties || {}) {
    var prop = schema.properties[propName];
    var shell = _buildDiscoveryFieldShell(propName, prop, required);
    if (shell.number == null) {
      shell.number = i;
      shell.isNumberGuessed = true;
    }
    into.push(shell);
    queue.push({ kind: "PROP", doc: doc, field: shell, prop: prop, visited: nextVisited });
    i++;
  }
}

// PROP frame: populate the pre-allocated `field` from `prop`. $ref →
// queue a SCHEMA frame whose `into` is the field's children array.
// Inline object / array-of-object → queue child PROP frames. The
// visited set carried into sub-frames is the chain from root to the
// current node; when a sub-frame recurses through a $ref, it gets a
// fresh clone via _stepResolveSchema's `nextVisited`.
function _stepMapProperty(item, queue) {
  var doc = item.doc, f = item.field, p = item.prop, v = item.visited;

  if (p.$ref) {
    f.type = "message";
    f.messageType = p.$ref;
    f.children = [];
    queue.push({ kind: "SCHEMA", doc: doc, schemaName: p.$ref,
                  visited: v, into: f.children });
    return;
  }

  if (p.type === "array" && p.items) {
    f.label = "repeated";
    if (p.items.$ref) {
      f.type = "message";
      f.messageType = p.items.$ref;
      f.children = [];
      queue.push({ kind: "SCHEMA", doc: doc, schemaName: p.items.$ref,
                    visited: v, into: f.children });
    } else if (p.items.type === "object" && p.items.properties) {
      f.type = "message";
      f.children = [];
      var arrRequired = p.items.required || [];
      for (var ipn in p.items.properties) {
        var arrShell = _buildDiscoveryFieldShell(ipn, p.items.properties[ipn], arrRequired);
        f.children.push(arrShell);
        queue.push({ kind: "PROP", doc: doc, field: arrShell,
                      prop: p.items.properties[ipn], visited: v });
      }
    } else {
      f.type = mapJsonSchemaType(p.items);
    }
    return;
  }

  if (p.type === "object" && p.properties) {
    f.type = "message";
    f.children = [];
    var nestedRequired = p.required || [];
    for (var pn in p.properties) {
      var nestShell = _buildDiscoveryFieldShell(pn, p.properties[pn], nestedRequired);
      f.children.push(nestShell);
      queue.push({ kind: "PROP", doc: doc, field: nestShell,
                    prop: p.properties[pn], visited: v });
    }
    return;
  }

  if (p.type === "object" && p.additionalProperties) {
    f.type = "string";
    f.description =
      (f.description || "") +
      " (map<string, " +
      (p.additionalProperties.type || "string") +
      ">)";
    return;
  }

  // Scalar
  f.type = mapJsonSchemaType(p);
  if (p.label === "repeated") f.label = "repeated";
  if (p.enum) {
    f.type = "enum";
    f.enum = p.enum;
    f.enumValues = p.enum;
    f.enumDescriptions = p.enumDescriptions || null;
  }
}

/**
 * Map a JSON schema type+format to unified protobuf-style type.
 */
function mapJsonSchemaType(prop) {
  if (!prop) return "string";
  var t = prop.type || "string";
  var f = prop.format || "";

  // Pass through protobuf-native types (from JSPB-learned schemas)
  var pbTypes = [
    "int32", "int64", "uint32", "uint64", "sint32", "sint64",
    "double", "float", "fixed32", "fixed64", "sfixed32", "sfixed64",
    "bool", "bytes", "enum",
  ];
  if (pbTypes.indexOf(t) >= 0) return t;

  if (t === "string") {
    if (f === "byte") return "bytes";
    if (f === "int64" || f === "uint64") return f;
    return "string";
  }
  if (t === "integer") {
    if (f === "int32" || f === "uint32") return f;
    return "int32";
  }
  if (t === "number") {
    if (f === "float") return "float";
    return "double";
  }
  if (t === "boolean") return "bool";
  return "string";
}


// ─── Content-based asset classification ──────────────────────────────────────
//
// Decide whether a captured response body is binary media (image, video,
// font, archive, 3d model, wasm) purely from magic bytes — no URL extension,
// no content-type. An API endpoint that returns a PNG is still an API
// (its URL/query/auth are meaningful); this classifier only decides whether
// to attempt structured-schema extraction from the response body. Binary
// media has no JSON/protobuf schema to learn, so we skip response parsing
// and annotate the method entry with the detected media type.
//
// Nothing is hidden from the user — every captured response is logged whatever
// this answers; the classifier only decides how much schema to synthesize
// around it. (This paragraph used to say the log carried each response's
// `_assetKind` / `_assetLabel`. It never did: response-decode.js wrote both
// fields and nothing on either side of the popup boundary read them, so the
// sentence described a surface that did not exist. Both writes are deleted.)
//
// THIS COMPONENT IS SUPERSEDED AND IS WAITING FOR ITS CALLER. What it does is
// the WHATWG MIME Sniffing standard, and that standard is implemented in C at
// engine/host/browser/core/mime/mime_sniff.c — §6's pattern tables and §7's
// sniffing algorithm, read by engine/host/solver/reply_decode.c. The hand-
// rolled table below survives only because lib/response-decode.js is still JS
// and calls it; it goes out with that file at jsaudit step 2. Do not extend it.
//
// Sniff magic bytes on a Uint8Array. Returns a MIME-like label or null.
function sniffBinaryMagic(bytes) {
  if (!bytes || bytes.length < 2) return null;
  var b = bytes;
  if (b.length >= 8 && b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) return "image/png";
  if (b.length >= 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff) return "image/jpeg";
  if (b.length >= 6 && b[0] === 0x47 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x38) return "image/gif";
  if (b.length >= 12 && b[0] === 0x52 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x46 && b[8] === 0x57 && b[9] === 0x45 && b[10] === 0x42 && b[11] === 0x50) return "image/webp";
  if (b.length >= 4 && b[0] === 0x25 && b[1] === 0x50 && b[2] === 0x44 && b[3] === 0x46) return "application/pdf";
  if (b.length >= 4 && b[0] === 0x77 && b[1] === 0x4f && b[2] === 0x46 && b[3] === 0x46) return "font/woff";
  if (b.length >= 4 && b[0] === 0x77 && b[1] === 0x4f && b[2] === 0x46 && b[3] === 0x32) return "font/woff2";
  if (b.length >= 4 && b[0] === 0x00 && b[1] === 0x01 && b[2] === 0x00 && b[3] === 0x00) return "font/ttf";
  if (b.length >= 4 && b[0] === 0x4f && b[1] === 0x54 && b[2] === 0x54 && b[3] === 0x4f) return "font/otf";
  if (b.length >= 2 && b[0] === 0x1f && b[1] === 0x8b) return "application/gzip";
  if (b.length >= 4 && b[0] === 0x50 && b[1] === 0x4b && b[2] === 0x03 && b[3] === 0x04) return "application/zip";
  if (b.length >= 4 && b[0] === 0x25 && b[1] === 0x21 && b[2] === 0x50 && b[3] === 0x53) return "application/postscript";
  if (b.length >= 4 && b[0] === 0x00 && b[1] === 0x61 && b[2] === 0x73 && b[3] === 0x6d) return "application/wasm";
  // MP4 / QuickTime: bytes 4..7 are "ftyp"
  if (b.length >= 8 && b[4] === 0x66 && b[5] === 0x74 && b[6] === 0x79 && b[7] === 0x70) return "video/mp4";
  if (b.length >= 4 && b[0] === 0x1a && b[1] === 0x45 && b[2] === 0xdf && b[3] === 0xa3) return "video/webm";
  // glTF (.glb) — little-endian "glTF" magic
  if (b.length >= 4 && b[0] === 0x67 && b[1] === 0x6c && b[2] === 0x54 && b[3] === 0x46) return "model/gltf-binary";
  // RIFF container (wav/avi) — webp already matched above
  if (b.length >= 4 && b[0] === 0x52 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x46) return "application/octet-stream";
  // ID3/MP3
  if (b.length >= 3 && b[0] === 0x49 && b[1] === 0x44 && b[2] === 0x33) return "audio/mpeg";
  // OGG
  if (b.length >= 4 && b[0] === 0x4f && b[1] === 0x67 && b[2] === 0x67 && b[3] === 0x53) return "audio/ogg";
  return null;
}

// Classify the response body. Returns { kind, label }:
//   "asset"   → binary media; skip RESPONSE-body schema extraction (request
//                still learned as normal). label is the sniffed MIME.
//   "empty"   → no body captured; learn as a fire-and-forget API (204-style).
//   "api"     → structured or text body; learn normally.
// Text-format asset signatures (SVG, plain CSS from CDN, etc.). SVG in
// particular is text but structurally a static image — icon CDNs like
// fonts.gstatic.com serve thousands of per-icon GET responses that
// shouldn't populate the discovery doc. Sniff on leading bytes only.
function _sniffTextAssetSignature(text) {
  if (!text) return null;
  var head = text.trimStart();
  var t = head.slice(0, 512);
  var lower = t.toLowerCase();
  // HLS playlist — literal "#EXTM3U" on the first line.
  if (head.startsWith("#EXTM3U")) return "application/vnd.apple.mpegurl";
  // WebVTT subtitles — "WEBVTT" header line.
  if (head.startsWith("WEBVTT")) return "text/vtt";
  // SVG (two entry shapes)
  if (lower.startsWith("<?xml") && /<svg\b/.test(lower)) return "image/svg+xml";
  if (lower.startsWith("<svg")) return "image/svg+xml";
  // HTML — doctype declaration or root <html> tag. Page fragments and
  // full documents fetched via fetch() are assets, not APIs.
  if (lower.startsWith("<!doctype html")) return "text/html";
  if (lower.startsWith("<html")) return "text/html";
  // DASH manifest — XML with <MPD as root element.
  if (lower.startsWith("<?xml") && /<mpd\b/.test(lower)) return "application/dash+xml";
  // SMIL / SRT — some streamers use these.
  if (lower.startsWith("<?xml") && /<smil\b/.test(lower)) return "application/smil+xml";
  // Plain CSS (CDN icon fonts often ship CSS with @font-face rules).
  // Require a @-rule at the head to avoid matching HTML with inline <style>.
  if (/^@(font-face|import|charset|media|keyframes|supports)\b/.test(t)) return "text/css";
  return null;
}

function classifyResponseAsset(responseBody, responseBase64, opts) {
  // Opaque cross-origin responses (fetch mode:"no-cors") can't be read,
  // so body is always empty. These are overwhelmingly fire-and-forget
  // tracking pixels / preconnect beacons — not API endpoints.
  if (opts && opts.responseType === "opaque") {
    return { kind: "asset", label: "opaque-cross-origin" };
  }
  // Server-declared content type (stripped to the bare MIME). Used only
  // as a weaker cross-check — magic bytes are authoritative; the header
  // is a server claim that can lie or be misconfigured.
  var declaredCt = null;
  if (opts && typeof opts.responseContentType === "string") {
    declaredCt = opts.responseContentType.toLowerCase().split(";")[0].trim() || null;
  }

  if (responseBody == null || responseBody === "") {
    return { kind: "empty", label: null };
  }
  if (responseBase64) {
    var bytes;
    try { bytes = base64ToUint8(responseBody); }
    catch (_) { return { kind: "api", label: null }; }
    if (bytes.length === 0) return { kind: "empty", label: null };
    var magic = sniffBinaryMagic(bytes);
    if (magic) {
      var note1 = declaredCt && declaredCt !== magic ? " (declared " + declaredCt + ")" : "";
      return { kind: "asset", label: magic + note1 };
    }
    // If the base64 decodes to printable text, run the text-asset sniff
    // too — misconfigured CDNs occasionally serve SVG as application/
    // octet-stream, triggering binary capture.
    try {
      var decoded = new TextDecoder("utf-8", { fatal: false }).decode(bytes);
      if (decoded) {
        var textMagic = _sniffTextAssetSignature(decoded);
        if (textMagic) {
          var note2 = declaredCt && declaredCt !== textMagic ? " (declared " + declaredCt + ")" : "";
          return { kind: "asset", label: textMagic + note2 };
        }
      }
    } catch (e) {
      // TextDecoder with fatal:false shouldn't throw on arbitrary bytes —
      // a throw here means the bytes input wasn't a valid Uint8Array shape.
      // Falls through to "binary-structured" classification.
      if (typeof console !== "undefined") console.debug("[discovery:classify] TextDecoder threw on binary bytes:", e && e.message || e);
    }
    // Base64 bytes with no magic match: could be protobuf, gRPC-Web, or any
    // structured binary format. These have schemas; don't skip learning.
    return { kind: "api", label: "binary-structured" };
  }
  // Text body. Sniff text-format assets first (SVG, CSS, HTML).
  var textAsset = _sniffTextAssetSignature(responseBody);
  if (textAsset) {
    var note3 = declaredCt && declaredCt !== textAsset ? " (declared " + declaredCt + ")" : "";
    return { kind: "asset", label: textAsset + note3 };
  }
  // Also run the binary sniff on raw bytes — servers sometimes ship
  // binary under a text content-type, which intercept captures as text.
  var probe = responseBody.length > 64 ? responseBody.slice(0, 64) : responseBody;
  var textBytes = new Uint8Array(probe.length);
  for (var i = 0; i < probe.length; i++) textBytes[i] = probe.charCodeAt(i) & 0xff;
  var magicText = sniffBinaryMagic(textBytes);
  if (magicText) {
    var note4 = declaredCt && declaredCt !== magicText ? " (declared " + declaredCt + ")" : "";
    return { kind: "asset", label: magicText + note4 };
  }
  // Asset content-types whose bodies have no unique structural prefix
  // (or where the @-rule sniff above misses common shapes). Trust the
  // server-declared MIME when the body is NOT a JSON root shape (`{`/`[`)
  // — JSON-shape under a JS MIME is JSONP/API data; under a CSS MIME it
  // wouldn't be valid CSS anyway, but the same gate keeps the path
  // symmetric. The set is restricted to MIMEs where servers have no
  // legitimate reason to ship API payloads (browsers execute JS, parse
  // CSS, render fonts — these aren't structured data formats).
  var ctAssetMimes = {
    "application/javascript": 1, "text/javascript": 1,
    "application/ecmascript": 1, "text/ecmascript": 1,
    "application/x-javascript": 1,
    "text/css": 1,
  };
  if (ctAssetMimes[declaredCt]) {
    var trimmed = responseBody.trimStart();
    var firstCh = trimmed.charCodeAt(0);
    // 0x7B = '{', 0x5B = '[' — JSON root shapes. Anything else starts
    // with a CSS selector / JS statement / comment and is asset content,
    // not API payload.
    if (firstCh !== 0x7B && firstCh !== 0x5B) {
      return { kind: "asset", label: declaredCt };
    }
  }
  return { kind: "api", label: null };
}

// Explicit self-binding for SW global scope — some extension loader configs
// don't hoist late function declarations into `self` reliably; attach so
// background.js can call them via importScripts.
if (typeof self !== "undefined") {
  self.sniffBinaryMagic = sniffBinaryMagic;
  self.classifyResponseAsset = classifyResponseAsset;
}

// ─── React Server Components — GONE TO engine/host/solver/reply_decode.c ────────────────────────────────
//
// `isRSC` / `looksLikeRSC` / `parseRSC` parsed a React Flight stream (`text/x-component`, line-framed
// `<hex row id>:<payload>`) so lib/learn.js could register each client reference's chunk as an endpoint and
// merge the json rows into a response schema. The ENDPOINT half is the @H surface and is now learned in the
// engine, at `engine_provide` — the one point every fetched reply crosses exactly once — keyed on the reply's
// COMPUTED MIME type (WHATWG MIME Sniffing, core/mime/mime_sniff.c). `looksLikeRSC` did not move at all: it
// guessed the protocol from a body whose first two lines matched `^[0-9a-f]+:`, which is also the shape of a
// JSON object keyed by digits, and CLAUDE.md §RUN, DON'T MATCH names that ("no regex/name/identifier
// matching, scoring, heuristics"). The schema half went with learn.js's branch, into the moat the engine is
// taking over at jsaudit step 4 rather than being re-hosted twice.
