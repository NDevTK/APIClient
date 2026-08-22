/* Popup protocol response renderers — extracted from popup.js (classic script, shares the popup global
   scope + DOM). One renderer per response protocol: batch-execute, async, gRPC-Web, SSE, NDJSON, GraphQL
   (single + batched), multipart-batch. Called by renderResponse (popup-response.js) via the global scope. */
function renderBatchExecuteResponse(bodyText, req, overrideDoc = null) {
  const calls = parseBatchExecuteResponse(bodyText);
  if (!calls || calls.length === 0)
    return `<pre class="resp-body">${esc(bodyText)}</pre>`;

  let html = '<div class="pb-tree">';
  const svc = req.service;
  const doc = overrideDoc || tabData?.discoveryDocs?.[svc]?.doc;

  for (const call of calls) {
    const nodes = jspbToTree(
      Array.isArray(call.data) ? call.data : [call.data],
    );
    const schemaName = `${call.rpcId}Response`;
    let schema = null;
    if (doc) {
      schema = doc.schemas?.[schemaName];
    }

    html += `<div class="card card-compact">
      <div class="card-label">RPC ID: <strong>${esc(call.rpcId)}</strong></div>
      <div class="pb-container pb-container-inline">
        ${renderPbTree(nodes, schema, schemaName, doc)}
      </div>
    </div>`;
  }
  html += "</div>";
  return html;
}

function renderAsyncResponse(bodyText, req, overrideDoc = null) {
  const chunks = parseAsyncChunkedResponse(bodyText);
  if (!chunks || chunks.length === 0)
    return `<pre class="resp-body">${esc(bodyText)}</pre>`;

  const svc = req.service;
  const doc = overrideDoc || tabData?.discoveryDocs?.[svc]?.doc;
  const url = req.url ? new URL(req.url) : null;
  const asyncPath =
    url?.pathname
      .split("/")
      .filter(Boolean)
      .pop() || "async";

  let html = '<div class="pb-tree">';
  for (let i = 0; i < chunks.length; i++) {
    const chunk = chunks[i];
    if (chunk.type === "jspb" && Array.isArray(chunk.data)) {
      const nodes = jspbToTree(chunk.data);
      const schemaName = `${asyncPath}_chunk${i}Response`;
      let schema = null;
      if (doc) {
        schema = doc.schemas?.[schemaName];
      }
      html += `<div class="card card-compact">
        <div class="card-label">Chunk ${i} <span class="badge badge-found">JSPB</span></div>
        <div class="pb-container pb-container-inline">
          ${renderPbTree(nodes, schema, schemaName, doc)}
        </div>
      </div>`;
    } else if (chunk.type === "html") {
      html += `<div class="card card-compact">
        <div class="card-label">Chunk ${i} <span class="badge badge-pending">HTML</span></div>
        <pre class="resp-body resp-body-scroll">${esc(chunk.raw)}</pre>
      </div>`;
    } else {
      html += `<div class="card card-compact">
        <div class="card-label">Chunk ${i} <span class="badge">text</span></div>
        <pre class="resp-body">${esc(chunk.raw)}</pre>
      </div>`;
    }
  }
  html += "</div>";
  return html;
}

// ─── gRPC-Web Renderer ──────────────────────────────────────────────────────

function renderGrpcWebResponse(bytes, req, overrideDoc = null) {
  const parsed = parseGrpcWebFrames(bytes);
  if (!parsed || parsed.frames.length === 0)
    return `<pre class="resp-body">[gRPC-Web: no frames decoded]</pre>`;

  const svc = req.service;
  const doc = overrideDoc || tabData?.discoveryDocs?.[svc]?.doc;
  const methodId = req.methodId || document.getElementById("send-ep-select")?.dataset?.discoveryId;
  const grpcFallbackId = methodId ? `${methodId}.response` : "";
  let respSchema = null;
  if (doc && methodId) {
    const methodInfo = findMethodById(doc, methodId);
    if (methodInfo?.method?.response?.$ref) {
      respSchema = resolveDiscoverySchema(doc, methodInfo.method.response.$ref);
    }
    if (!respSchema && grpcFallbackId && doc.schemas?.[grpcFallbackId]) {
      respSchema = doc.schemas[grpcFallbackId];
    }
  }

  let html = '<div class="pb-tree">';

  // Trailers summary
  if (Object.keys(parsed.trailers).length > 0) {
    const grpcStatus = parsed.trailers["grpc-status"] || "?";
    const grpcMsg = parsed.trailers["grpc-message"] || "";
    html += `<div class="card card-compact">
      <div class="card-label">gRPC Status: <strong>${esc(grpcStatus)}</strong>
        ${grpcMsg ? ` &mdash; ${esc((() => { try { return decodeURIComponent(grpcMsg); } catch (_) { return grpcMsg; } })())}` : ""}</div>
    </div>`;
  }

  for (let i = 0; i < parsed.frames.length; i++) {
    const frame = parsed.frames[i];
    if (frame.type === "data") {
      const tree = pbDecodeTree(frame.data);
      html += `<div class="card card-compact">
        <div class="card-label">Data Frame ${i} <span class="badge badge-found">protobuf</span>
          <span class="text-muted-sm ml-4">${frame.data.length} bytes</span></div>
        <div class="pb-container pb-container-inline">
          ${renderPbTree(tree, respSchema, grpcFallbackId, doc)}
        </div>
      </div>`;
    } else if (frame.type === "trailers") {
      html += `<div class="card card-compact">
        <div class="card-label">Trailers</div>
        <pre class="resp-body resp-body-scroll-sm">${esc(frame.data)}</pre>
      </div>`;
    }
  }
  html += "</div>";
  return html;
}

// ─── SSE Renderer ───────────────────────────────────────────────────────────

function renderSSEResponse(bodyText) {
  const events = parseSSE(bodyText);
  if (!events || events.length === 0)
    return `<pre class="resp-body">${esc(bodyText)}</pre>`;

  let html = `<div class="card-label">Server-Sent Events (${events.length} events)</div><div class="pb-tree">`;
  for (let i = 0; i < events.length; i++) {
    const evt = events[i];
    const typeBadge = evt.event !== "message"
      ? ` <span class="badge badge-pending">${esc(evt.event)}</span>`
      : "";
    const idBadge = evt.id
      ? ` <span class="text-muted-sm">id: ${esc(evt.id)}</span>`
      : "";

    let bodyHtml;
    if (typeof evt.data === "object" && evt.data !== null) {
      bodyHtml = `<pre class="resp-body">${esc(JSON.stringify(evt.data, null, 2))}</pre>`;
    } else {
      bodyHtml = `<pre class="resp-body">${esc(evt.raw)}</pre>`;
    }

    html += `<div class="card card-compact">
      <div class="card-label">Event ${i}${typeBadge}${idBadge}</div>
      ${bodyHtml}
    </div>`;
  }
  html += "</div>";
  return html;
}

// ─── NDJSON Renderer ────────────────────────────────────────────────────────

function renderNDJSONResponse(bodyText) {
  const objects = parseNDJSON(bodyText);
  if (!objects || objects.length === 0)
    return `<pre class="resp-body">${esc(bodyText)}</pre>`;

  let html = `<div class="card-label">NDJSON (${objects.length} records)</div><div class="pb-tree">`;
  for (let i = 0; i < objects.length; i++) {
    html += `<div class="card card-compact">
      <div class="card-label">Record ${i}</div>
      <pre class="resp-body">${esc(JSON.stringify(objects[i], null, 2))}</pre>
    </div>`;
  }
  html += "</div>";
  return html;
}

// ─── GraphQL Renderer ───────────────────────────────────────────────────────

function renderSingleGqlResult(r, respSchema, responseSchemaId, doc) {
  let html = "";
  if (r.errors) {
    html += `<div class="card card-compact-error">
      <div class="card-label card-label-error">Errors (${r.errors.length})</div>
      <pre class="resp-body">${esc(JSON.stringify(r.errors, null, 2))}</pre>
    </div>`;
  }
  if (r.data) {
    const nodes = jsonToTree(r.data);
    html += `<div class="card-label">Data</div>` +
      renderPbTree(nodes, respSchema, responseSchemaId, doc);
  }
  if (r.extensions) {
    html += `<div class="card card-compact">
      <div class="card-label card-label-muted">Extensions</div>
      <pre class="resp-body resp-body-scroll">${esc(JSON.stringify(r.extensions, null, 2))}</pre>
    </div>`;
  }
  return html;
}

function renderGraphQLResponse(bodyText, respSchema, responseSchemaId, doc) {
  const gql = parseGraphQLResponse(bodyText);
  if (!gql) return null; // Fall through to normal JSON rendering

  if (gql.results.length === 1) {
    return renderSingleGqlResult(gql.results[0], respSchema, responseSchemaId, doc);
  }

  let html = "";
  for (let i = 0; i < gql.results.length; i++) {
    html += `<div class="card card-compact">
      <div class="card-label">Operation ${i + 1}</div>
      ${renderSingleGqlResult(gql.results[i], respSchema, responseSchemaId, doc)}
    </div>`;
  }
  return html;
}

// ─── Multipart Batch Renderer ───────────────────────────────────────────────

function renderMultipartBatchResponse(bodyText, contentType) {
  const parts = parseMultipartBatch(bodyText, contentType);
  if (!parts || parts.length === 0)
    return `<pre class="resp-body">${esc(bodyText)}</pre>`;

  let html = `<div class="card-label">Multipart Batch (${parts.length} parts)</div><div class="pb-tree">`;
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const statusBadge = part.status
      ? (part.status >= 200 && part.status < 300
          ? `<span class="badge badge-found">${esc(String(part.status))}</span>`
          : `<span class="badge badge-notfound">${esc(String(part.status))}</span>`)
      : "";

    let bodyHtml;
    try {
      const json = JSON.parse(part.body);
      bodyHtml = `<pre class="resp-body">${esc(JSON.stringify(json, null, 2))}</pre>`;
    } catch (_) {
      bodyHtml = `<pre class="resp-body">${esc(part.body)}</pre>`;
    }

    html += `<div class="card card-compact">
      <div class="card-label">Part ${i} ${statusBadge} ${esc(part.statusText || "")}</div>
      ${bodyHtml}
    </div>`;
  }
  html += "</div>";
  return html;
}
