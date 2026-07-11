/* Wire-protocol PARSERS — extracted from discovery.js (classic script, globals; loaded in the worker AND
   the popup). Parse a response/request body by protocol: Google batchExecute, gRPC-Web frames, SSE,
   NDJSON, GraphQL, multipart-batch (+ the is<Protocol> content-type sniffers). The pair to the popup's
   protocol RENDERERS (popup-protocols.js): these decode, those display. One concern, one file. */
/**
 * Parse a Google BatchExecute request body (f.req=...).
 * @param {string} bodyText - Raw request body
 * @returns {Array<{rpcId: string, data: any}> | null}
 */
function parseBatchExecuteRequest(bodyText) {
  try {
    const params = new URLSearchParams(bodyText);
    const fReq = params.get("f.req");
    if (!fReq) return null;

    const outer = JSON.parse(fReq);
    if (!Array.isArray(outer)) return null;

    const calls = [];
    for (const call of outer[0]) {
      const [rpcId, innerJson] = call;
      let decodedInner = null;
      try {
        decodedInner = JSON.parse(innerJson);
      } catch (e) {
        decodedInner = innerJson;
      }
      calls.push({ rpcId, data: decodedInner });
    }
    return calls;
  } catch (e) {
    return null;
  }
}

/**
 * Parse a Google BatchExecute response body.
 * Strips security prefix and handles length-prefixed chunks.
 * @param {string} bodyText - Raw response body
 * @returns {Array<{rpcId: string, data: any}> | null}
 */
function parseBatchExecuteResponse(bodyText) {
  try {
    let cleaned = bodyText.trim();
    if (cleaned.startsWith(")]}'")) {
      cleaned = cleaned.substring(4).trim();
    }

    const chunks = [];
    const lines = cleaned.split("\n");
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i].trim();
      if (/^\d+$/.test(line)) {
        if (i + 1 < lines.length) {
          try {
            const chunk = JSON.parse(lines[i + 1]);
            chunks.push(chunk);
            i++;
          } catch (e) {
            // Length-prefix-shaped line followed by non-JSON — malformed
            // BatchExecute envelope. Continue scanning; log so a chunk-format
            // mismatch is diagnosable.
            if (typeof console !== "undefined") console.debug("[discovery:BatchExecute] length-prefix chunk JSON parse failed:", e && e.message || e);
          }
        }
      } else if (line.startsWith("[")) {
        try {
          chunks.push(JSON.parse(line));
        } catch (e) {
          // Bracket-shaped line that isn't JSON — log and skip; the
          // BatchExecute format permits non-chunk noise interleaved with
          // chunks (XSSI prefix etc., already stripped above).
          if (typeof console !== "undefined") console.debug("[discovery:BatchExecute] bracket line JSON parse failed:", e && e.message || e);
        }
      }
    }

    const results = [];
    for (const chunk of chunks) {
      if (!Array.isArray(chunk)) continue;
      for (const item of chunk) {
        if (item[0] === "wrb.fr") {
          const rpcId = item[1];
          const innerJson = item[2];
          let decodedInner = null;
          try {
            decodedInner = JSON.parse(innerJson);
          } catch (e) {
            decodedInner = innerJson;
          }

          // item[3] = error code (null on success)
          // item[6] = error details string (null on success)
          const errorCode = item[3] != null ? item[3] : null;
          let errorDetail = null;
          if (item[6]) {
            try {
              errorDetail = JSON.parse(item[6]);
            } catch (e) {
              errorDetail = item[6];
            }
          }

          const entry = { rpcId, data: decodedInner };
          if (errorCode != null) {
            entry.error = { code: errorCode, detail: errorDetail };
          }
          results.push(entry);
        }
      }
    }
    return results;
  } catch (e) {
    return null;
  }
}

/**
 * Parse a Google async chunked response (used by /async/* endpoints).
 * Format: XSSI prefix `)]}'`, then hex-length-prefixed chunks: `<hex>;<payload>`.
 * Payloads are JSPB arrays, HTML fragments, or plain text.
 * @param {string} bodyText - Raw response body
 * @returns {Array<{type: string, data: any, raw: string}>} Parsed chunks
 */
function parseAsyncChunkedResponse(bodyText) {
  try {
    let text = bodyText;
    // Strip XSSI prefix
    if (text.startsWith(")]}'")) {
      text = text.substring(4);
    }
    // Trim leading whitespace/newlines
    text = text.replace(/^\s+/, "");

    const chunks = [];
    let pos = 0;

    while (pos < text.length) {
      // Read hex length up to semicolon
      const semi = text.indexOf(";", pos);
      if (semi < 0) break;

      const hexStr = text.substring(pos, semi).trim();
      const len = parseInt(hexStr, 16);
      if (isNaN(len)) break;
      if (len === 0) break; // terminator

      const payload = text.substring(semi + 1, semi + 1 + len);
      pos = semi + 1 + len;

      // Skip whitespace between chunks
      while (pos < text.length && (text[pos] === "\n" || text[pos] === "\r"))
        pos++;

      // Classify payload
      const trimmed = payload.trim();
      if (trimmed.startsWith("[")) {
        try {
          const parsed = JSON.parse(trimmed);
          chunks.push({ type: "jspb", data: parsed, raw: payload });
          continue;
        } catch (e) {
          // Starts with `[` but isn't valid JSON — fall through to text
          // classification. Debug-log so a malformed [-bracketed payload that
          // pretends to be JSPB is diagnosable.
          if (typeof console !== "undefined") console.debug("[discovery:chunked] JSON.parse(JSPB-shaped) failed:", e && e.message || e);
        }
      }
      if (trimmed.startsWith("<")) {
        chunks.push({ type: "html", data: null, raw: payload });
      } else {
        chunks.push({ type: "text", data: null, raw: payload });
      }
    }

    return chunks.length > 0 ? chunks : null;
  } catch (e) {
    return null;
  }
}

/**
 * Detect whether a response body uses Google's async chunked format.
 * @param {string} bodyText - Raw response body
 * @returns {boolean}
 */
function isAsyncChunkedResponse(bodyText) {
  if (!bodyText) return false;
  const stripped = bodyText.trimStart();
  // Must start with XSSI prefix followed by hex;
  if (!stripped.startsWith(")]}'")) return false;
  const after = stripped.substring(4).trimStart();
  // Next token should be a hex number followed by semicolon
  const semi = after.indexOf(";");
  if (semi < 1 || semi > 10) return false;
  const hex = after.substring(0, semi).trim();
  return /^[0-9a-fA-F]+$/.test(hex);
}

/**
 * Detect whether a response body is a batchexecute response (wrb.fr format).
 * Format: optional `)]}'` XSSI prefix, decimal chunk lengths on own lines,
 * JSON arrays containing `["wrb.fr", ...]` items.
 * @param {string} bodyText - Raw response body
 * @returns {boolean}
 */
function isBatchExecuteResponse(bodyText) {
  if (!bodyText) return false;
  let text = bodyText.trimStart();
  if (text.startsWith(")]}'")) text = text.substring(4).trimStart();
  // First non-empty line should be a decimal chunk length
  const firstNewline = text.indexOf("\n");
  if (firstNewline < 1) return false;
  const firstLine = text.substring(0, firstNewline).trim();
  return /^\d+$/.test(firstLine) && bodyText.includes('"wrb.fr"');
}

// ─── gRPC-Web Frame Parser ──────────────────────────────────────────────────

/**
 * Parse gRPC-Web framed response.
 * Each frame: 1-byte flag (0=data, 0x80=trailers) + 4-byte big-endian length + payload.
 * Data frames contain protobuf; trailer frames contain HTTP/2-style headers.
 * @param {Uint8Array} bytes - Raw response bytes
 * @returns {{frames: Array<{type: string, data: Uint8Array|string}>, trailers: Object}|null}
 */
function parseGrpcWebFrames(bytes) {
  try {
    const frames = [];
    const trailers = {};
    let pos = 0;

    while (pos + 5 <= bytes.length) {
      const flag = bytes[pos];
      const len =
        (bytes[pos + 1] << 24) |
        (bytes[pos + 2] << 16) |
        (bytes[pos + 3] << 8) |
        bytes[pos + 4];
      pos += 5;

      if (len < 0 || pos + len > bytes.length) break;
      const payload = bytes.subarray(pos, pos + len);
      pos += len;

      if (flag & 0x80) {
        // Trailer frame — HTTP/2 header block as ASCII
        const text = new TextDecoder().decode(payload);
        for (const line of text.split("\r\n")) {
          const idx = line.indexOf(":");
          if (idx > 0) {
            trailers[line.slice(0, idx).trim().toLowerCase()] =
              line.slice(idx + 1).trim();
          }
        }
        frames.push({ type: "trailers", data: text });
      } else {
        frames.push({ type: "data", data: payload });
      }
    }

    return frames.length > 0 ? { frames, trailers } : null;
  } catch (e) {
    return null;
  }
}

/**
 * Detect gRPC-Web content type.
 * @param {string} contentType
 * @returns {boolean}
 */
function isGrpcWeb(contentType) {
  if (!contentType) return false;
  const ct = contentType.toLowerCase();
  return (
    ct.includes("grpc-web") ||
    (ct.includes("grpc") && !ct.includes("json"))
  );
}

/**
 * Check if gRPC-Web response uses base64 text encoding (grpc-web-text).
 * @param {string} contentType
 * @returns {boolean}
 */
function isGrpcWebText(contentType) {
  return contentType ? contentType.toLowerCase().includes("grpc-web-text") : false;
}

/**
 * Encode a protobuf payload into a gRPC-Web data frame.
 * Frame format: 1-byte flag (0x00=uncompressed) + 4-byte big-endian length + payload.
 * @param {Uint8Array} protobufBytes - Encoded protobuf message
 * @returns {Uint8Array} gRPC-Web framed message
 */
function encodeGrpcWebFrame(protobufBytes) {
  const frame = new Uint8Array(5 + protobufBytes.length);
  frame[0] = 0x00; // Uncompressed
  frame[1] = (protobufBytes.length >> 24) & 0xff;
  frame[2] = (protobufBytes.length >> 16) & 0xff;
  frame[3] = (protobufBytes.length >> 8) & 0xff;
  frame[4] = protobufBytes.length & 0xff;
  frame.set(protobufBytes, 5);
  return frame;
}

// ─── SSE (Server-Sent Events) Parser ────────────────────────────────────────

/**
 * Parse a text/event-stream response into individual events.
 * @param {string} bodyText - Raw SSE response
 * @returns {Array<{event: string, data: any, id: string|null, raw: string}>|null}
 */
function parseSSE(bodyText) {
  try {
    const events = [];
    // Split on double newlines (event boundaries)
    const blocks = bodyText.split(/\n\n+/);

    for (const block of blocks) {
      const trimmed = block.trim();
      if (!trimmed) continue;

      let eventType = "message";
      let dataLines = [];
      let id = null;

      for (const line of trimmed.split("\n")) {
        if (line.startsWith("event:")) {
          eventType = line.slice(6).trim();
        } else if (line.startsWith("data:")) {
          dataLines.push(line.slice(5).trimStart());
        } else if (line.startsWith("id:")) {
          id = line.slice(3).trim();
        } else if (line.startsWith(":")) {
          // Comment — skip
        } else if (line.includes(":")) {
          // Unknown field — treat as data
          dataLines.push(line);
        } else if (line.trim()) {
          dataLines.push(line);
        }
      }

      if (dataLines.length === 0) continue;

      const rawData = dataLines.join("\n");
      // Try JSON; fall through to raw string when SSE `data:` isn't JSON.
      // SSE data is OFTEN plain text (status updates, comments), so debug-
      // log the parse failure rather than warn — it's expected for text events.
      let parsed = rawData;
      try { parsed = JSON.parse(rawData); }
      catch (e) {
        if (typeof console !== "undefined") console.debug("[discovery:SSE] data JSON parse failed (treating as text):", e && e.message || e);
      }

      events.push({ event: eventType, data: parsed, id, raw: rawData });
    }

    return events.length > 0 ? events : null;
  } catch (e) {
    return null;
  }
}

/**
 * Detect SSE content type.
 * @param {string} contentType
 * @returns {boolean}
 */
function isSSE(contentType) {
  return contentType ? contentType.toLowerCase().includes("event-stream") : false;
}

// ─── NDJSON (Newline-Delimited JSON) Parser ─────────────────────────────────

/**
 * Parse NDJSON (one JSON object per line).
 * @param {string} bodyText
 * @returns {Array<any>|null}
 */
function parseNDJSON(bodyText) {
  try {
    const lines = bodyText.split("\n").filter((l) => l.trim());
    if (lines.length < 2) return null; // Need at least 2 lines to be NDJSON
    const objects = [];
    let parsed = 0;
    for (const line of lines) {
      try {
        objects.push(JSON.parse(line));
        parsed++;
      } catch (_) {
        // Allow a few unparseable lines (e.g. trailing newlines)
      }
    }
    // At least 2 valid JSON lines to qualify as NDJSON
    return parsed >= 2 ? objects : null;
  } catch (e) {
    return null;
  }
}

/**
 * Detect NDJSON content type.
 * @param {string} contentType
 * @returns {boolean}
 */
function isNDJSON(contentType) {
  if (!contentType) return false;
  const ct = contentType.toLowerCase();
  return ct.includes("ndjson") || ct.includes("jsonl") || ct.includes("json-seq");
}

// ─── GraphQL Decoder ────────────────────────────────────────────────────────

/**
 * Extract a single GraphQL operation from a parsed object.
 * @param {Object} obj - Parsed JSON object
 * @returns {{query: string, variables: Object|null, operationName: string|null, extensions: Object|null}|null}
 */
function _parseGqlOp(obj) {
  if (!obj || typeof obj !== "object") return null;
  // Standard GraphQL over HTTP: { query, operationName?, variables? }
  // Apollo APQ (persisted query, no query sent): { operationName, extensions: { persistedQuery: { sha256Hash } } }
  // Reddit-style persisted: { operation: "Name", variables, ... } (their `operation` field IS the name — server maps to stored doc)
  var hasQuery = !!(obj.query || obj.mutation);
  var hasPersisted = obj.extensions && obj.extensions.persistedQuery;
  var hasRedditOp = typeof obj.operation === "string" && obj.operation.length > 0;
  if (!hasQuery && !hasPersisted && !hasRedditOp) return null;
  // Preserve any extra top-level fields (csrf_token, clientId, rid, etc.) so
  // byte-roundtrip through the GraphQL encoder stays faithful. Without this,
  // Reddit's envelope ({operation, variables, csrf_token}) rebuilt as just
  // {operation, variables} — a silent authentication stripping the server
  // rejects. Known fields are popped from the extras bag.
  var standard = { query: 1, mutation: 1, variables: 1, operationName: 1, operation: 1, extensions: 1 };
  var extra = null;
  for (var k in obj) {
    if (Object.prototype.hasOwnProperty.call(obj, k) && !standard[k]) {
      if (!extra) extra = {};
      extra[k] = obj[k];
    }
  }
  return {
    query: obj.query || obj.mutation || "",
    variables: obj.variables || null,
    operationName: obj.operationName || null,
    operation: hasRedditOp ? obj.operation : null,
    extensions: obj.extensions || null,
    extra: extra,
  };
}

// Resolve a method name from a parsed GraphQL operation.
//
// Preference order (most-specific wins):
//   1. Explicit `operationName` field in the request JSON.
//   2. Named operation declaration in the query text:
//        `query Foo { ... }` / `mutation Foo { ... }` / `subscription Foo { ... }`
//   3. First root field in an anonymous op: `{ getUser(id: 1) { ... } }` → getUser.
//   4. `anonymous_<operationType>` fallback.
//
// Strips whitespace/comment noise before matching. This is a structural scan
// over GraphQL syntax, not user JS, so string matching on the grammar is
// safe here (GraphQL has a fixed, simple surface). An operationName with
// spaces or other invalid identifier characters is rejected — a server
// would reject it too, so we don't want to propagate it as a method name.
function _stripGqlNoise(q) {
  if (typeof q !== "string") return "";
  // Strip # line comments and then collapse whitespace.
  return q
    .replace(/#[^\n]*/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}
function _isValidGqlName(s) {
  return typeof s === "string" && /^[A-Za-z_][A-Za-z0-9_]*$/.test(s);
}
function deriveGraphQLMethodName(op) {
  if (!op) return null;
  if (op.operationName && _isValidGqlName(op.operationName)) {
    return op.operationName;
  }
  // Reddit-style persisted-operation envelope: `{operation: "Name", ...}`.
  // Their shreddit backend maps the name to a stored GraphQL doc server-side,
  // so the client never sends the query text. `operation` is still the
  // canonical name we want to key methods under.
  if (op.operation && _isValidGqlName(op.operation)) {
    return op.operation;
  }
  var q = _stripGqlNoise(op.query);
  if (!q) return null;
  // Match `query|mutation|subscription <Name>` at the head of the document.
  // The name is followed by either `(` (variables), `@` (directive), or `{`.
  var m = q.match(/^(query|mutation|subscription)\s+([A-Za-z_][A-Za-z0-9_]*)\s*[({@]/);
  if (m) return m[2];
  // Anonymous operation — find the first selection in the top-level `{ ... }`.
  // Skip a leading operation-type keyword (`query`, `mutation`, `subscription`)
  // if present but unnamed.
  var head = q.replace(/^(query|mutation|subscription)\s*/, "");
  var braceIdx = head.indexOf("{");
  if (braceIdx === -1) return null;
  // Scan forward after the brace for the first identifier token.
  var after = head.slice(braceIdx + 1).trim();
  // Strip optional alias: `aliasName:` → ""
  var aliasMatch = after.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*/);
  if (aliasMatch) after = after.slice(aliasMatch[0].length).trim();
  var firstMatch = after.match(/^([A-Za-z_][A-Za-z0-9_]*)/);
  if (firstMatch) return firstMatch[1];
  // Truly anonymous and no root field readable — stamp the operation type.
  var opType = (q.match(/^(query|mutation|subscription)\b/) || [])[1];
  return opType ? "anonymous_" + opType : null;
}

/**
 * Extract GraphQL structure from a JSON request body.
 * Handles both single operations and batched arrays.
 * @param {string} bodyText - JSON request body
 * @returns {{operations: Array, batched: boolean}|null}
 */
function parseGraphQLRequest(bodyText) {
  try {
    const json = JSON.parse(bodyText);
    if (Array.isArray(json)) {
      const ops = [];
      for (const item of json) {
        const op = _parseGqlOp(item);
        if (op) ops.push(op);
      }
      if (ops.length === 0) return null;
      return { operations: ops, batched: true };
    }
    const op = _parseGqlOp(json);
    if (!op) return null;
    return { operations: [op], batched: false };
  } catch (e) {
    return null;
  }
}

/**
 * Extract a single GraphQL response result.
 * @param {Object} obj
 * @returns {{data: any, errors: Array|null, extensions: Object|null}|null}
 */
function _parseGqlResp(obj) {
  if (!obj || typeof obj !== "object") return null;
  if (!("data" in obj) && !("errors" in obj)) return null;
  return {
    data: obj.data ?? null,
    errors: obj.errors || null,
    extensions: obj.extensions || null,
  };
}

/**
 * Decode a GraphQL response, extracting data, errors, and extensions.
 * Handles both single responses and batched arrays.
 * @param {string} bodyText - JSON response body
 * @returns {{results: Array, batched: boolean}|null}
 */
function parseGraphQLResponse(bodyText) {
  try {
    const json = JSON.parse(bodyText);
    if (Array.isArray(json)) {
      const results = [];
      for (const item of json) {
        const r = _parseGqlResp(item);
        if (r) results.push(r);
      }
      if (results.length === 0) return null;
      return { results, batched: true };
    }
    const r = _parseGqlResp(json);
    if (!r) return null;
    return { results: [r], batched: false };
  } catch (e) {
    return null;
  }
}

/**
 * Detect if a URL looks like a GraphQL endpoint.
 * @param {string} url
 * @returns {boolean}
 */
function isGraphQLUrl(url) {
  if (!url) return false;
  // Path/host tokens that real-world GraphQL endpoints actually use.
  // Observed during the harness audit:
  //   /graphql, /api/graphql, /svc/shreddit/graphql (reddit), /dml/graphql (booking)
  //   gql-fed.reddit.com (hostname uses `gql-` prefix with no "graphql")
  //   shopify, github etc. tend to use /graphql explicitly.
  if (/graphql/i.test(url)) return true;
  // canParse guard — non-URL inputs simply aren't GraphQL by these tests.
  if (URL.canParse(url)) {
    var u = new URL(url);
    // Hostname-level indicators.
    if (/\bgql(-|\.|$)/i.test(u.hostname)) return true;
    // Path-level indicators beyond plain "graphql".
    if (/\/gql(\/|\?|$)/i.test(u.pathname)) return true;
  }
  return false;
}

// ─── Multipart Batch Response Parser ────────────────────────────────────────

/**
 * Parse a multipart/mixed (or multipart/batch) response.
 * Each part contains a full HTTP response (status line, headers, body).
 * @param {string} bodyText - Raw multipart response body
 * @param {string} contentType - Content-Type header (contains boundary)
 * @returns {Array<{status: number, statusText: string, headers: Object, body: string}>|null}
 */
function parseMultipartBatch(bodyText, contentType) {
  try {
    // Extract boundary from content-type
    const boundaryMatch = contentType.match(/boundary=["']?([^"';\s]+)/i);
    if (!boundaryMatch) return null;
    const boundary = boundaryMatch[1];

    const parts = bodyText.split("--" + boundary);
    const results = [];

    for (const part of parts) {
      const trimmed = part.trim();
      if (!trimmed || trimmed === "--") continue; // Preamble or closing

      // Each part: optional part headers, blank line, then HTTP response
      // Find the HTTP response within the part
      const httpStart = trimmed.indexOf("HTTP/");
      if (httpStart < 0) {
        // No HTTP response line — might be a raw body part
        const blankLine = trimmed.indexOf("\r\n\r\n");
        if (blankLine >= 0) {
          results.push({
            status: 0,
            statusText: "",
            headers: {},
            body: trimmed.substring(blankLine + 4),
          });
        }
        continue;
      }

      const responseText = trimmed.substring(httpStart);
      // Split status line from headers+body
      const firstLine = responseText.split(/\r?\n/)[0];
      const statusMatch = firstLine.match(/HTTP\/[\d.]+\s+(\d+)\s*(.*)/);
      const status = statusMatch ? parseInt(statusMatch[1]) : 0;
      const statusText = statusMatch ? statusMatch[2] : "";

      // Parse headers (between status line and blank line)
      const headerEnd = responseText.search(/\r?\n\r?\n/);
      const headers = {};
      if (headerEnd > 0) {
        const headerBlock = responseText.substring(
          responseText.indexOf("\n") + 1,
          headerEnd,
        );
        for (const line of headerBlock.split(/\r?\n/)) {
          const idx = line.indexOf(":");
          if (idx > 0) {
            headers[line.slice(0, idx).trim().toLowerCase()] =
              line.slice(idx + 1).trim();
          }
        }
      }

      // Body is everything after the blank line
      const bodyStart = responseText.search(/\r?\n\r?\n/);
      const body =
        bodyStart >= 0
          ? responseText.substring(
              bodyStart + responseText.substring(bodyStart).match(/\r?\n\r?\n/)[0].length,
            )
          : "";

      results.push({ status, statusText, headers, body });
    }

    return results.length > 0 ? results : null;
  } catch (e) {
    return null;
  }
}

/**
 * Parse a multipart batch REQUEST body (each part is an HTTP request).
 * Google batch APIs send multiple HTTP sub-requests in one body.
 * @param {string} bodyText
 * @param {string} contentType - includes boundary parameter
 * @returns {Array<{method:string, path:string, headers:Object, body:string, contentId:string|null}>|null}
 */
function parseMultipartBatchRequest(bodyText, contentType) {
  try {
    const boundaryMatch = contentType.match(/boundary=["']?([^"';\s]+)/i);
    if (!boundaryMatch) return null;
    const boundary = boundaryMatch[1];
    const parts = bodyText.split("--" + boundary);
    const results = [];

    for (const part of parts) {
      const trimmed = part.trim();
      if (!trimmed || trimmed === "--") continue;

      // Find the HTTP request line: METHOD /path HTTP/1.x
      const reqMatch = trimmed.match(
        /^(GET|POST|PUT|PATCH|DELETE|HEAD|OPTIONS)\s+(\S+)\s+HTTP\//m,
      );
      if (!reqMatch) continue;

      const method = reqMatch[1];
      const path = reqMatch[2];
      const reqLineIdx = trimmed.indexOf(reqMatch[0]);

      // Everything after the request line
      const afterReqLine = trimmed.substring(
        reqLineIdx + reqMatch[0].length,
      );
      // Skip rest of request line (e.g. "1.1")
      const firstNl = afterReqLine.indexOf("\n");
      const afterFirstLine =
        firstNl >= 0 ? afterReqLine.substring(firstNl + 1) : "";

      // Headers + body separated by blank line
      const blankLine = afterFirstLine.search(/\r?\n\r?\n/);
      const headers = {};
      // If no blank line, all remaining text is headers (no body, e.g. GET requests)
      const headerBlock =
        blankLine >= 0
          ? afterFirstLine.substring(0, blankLine)
          : afterFirstLine;
      for (const line of headerBlock.split(/\r?\n/)) {
        const idx = line.indexOf(":");
        if (idx > 0) {
          headers[line.slice(0, idx).trim().toLowerCase()] = line
            .slice(idx + 1)
            .trim();
        }
      }
      const body =
        blankLine >= 0
          ? afterFirstLine.substring(blankLine).replace(/^[\r\n]+/, "")
          : "";

      // Content-ID from part envelope headers (above the HTTP line)
      const partHeaders = trimmed.substring(0, reqLineIdx);
      const cidMatch = partHeaders.match(/Content-ID:\s*<?([^>\r\n]+)/i);

      results.push({
        method,
        path,
        headers,
        body: body.trim(),
        contentId: cidMatch ? cidMatch[1] : null,
      });
    }

    return results.length > 0 ? results : null;
  } catch (e) {
    return null;
  }
}

/**
 * Detect multipart content type.
 * @param {string} contentType
 * @returns {boolean}
 */
function isMultipartBatch(contentType) {
  if (!contentType) return false;
  const ct = contentType.toLowerCase();
  return (ct.includes("multipart/mixed") || ct.includes("multipart/batch")) &&
    ct.includes("boundary");
}
