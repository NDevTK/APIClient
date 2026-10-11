/* Popup protocol response renderers — extracted from popup.js (classic script, shares the popup global
   scope + DOM). One renderer per response protocol: batch-execute, async, gRPC-Web, SSE, NDJSON, GraphQL
   (single + batched), multipart-batch. Called by renderResponse (popup-response.js) via the global scope.
   Each returns a list of nodes built through lib/dom-builder.js. */

// The response text as-is, for a body no protocol parser could split.
function _respPre(text, cls = "resp-body") {
  return domElement("pre", { class: cls }, [text]);
}

// A compact card: its label row's children, then the card's body nodes.
function _respCard(labelChildren, body, cls = "card card-compact", labelCls = "card-label") {
  return domElement("div", { class: cls }, [domElement("div", { class: labelCls }, labelChildren), body]);
}

// A decoded tree inside the inline container the batch, async and gRPC-Web cards share.
function _respTreeContainer(tree) {
  return domElement("div", { class: "pb-container pb-container-inline" }, [tree]);
}

function renderBatchExecuteResponse(bodyText, req, overrideDoc = null) {
  const calls = parseBatchExecuteResponse(bodyText);
  if (!calls || calls.length === 0)
    return [_respPre(bodyText)];

  const tree = domElement("div", { class: "pb-tree" });
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

    tree.appendChild(_respCard(["RPC ID: ", domElement("strong", {}, [call.rpcId])],
                               _respTreeContainer(renderPbTree(nodes, schema, schemaName, doc))));
  }
  return [tree];
}

function renderAsyncResponse(bodyText, req, overrideDoc = null) {
  const chunks = parseAsyncChunkedResponse(bodyText);
  if (!chunks || chunks.length === 0)
    return [_respPre(bodyText)];

  const svc = req.service;
  const doc = overrideDoc || tabData?.discoveryDocs?.[svc]?.doc;
  const url = req.url ? new URL(req.url) : null;
  const asyncPath =
    url?.pathname
      .split("/")
      .filter(Boolean)
      .pop() || "async";

  const tree = domElement("div", { class: "pb-tree" });
  for (let i = 0; i < chunks.length; i++) {
    const chunk = chunks[i];
    const label = (badgeCls, badgeText) =>
      ["Chunk " + i + " ", domElement("span", { class: badgeCls }, [badgeText])];
    if (chunk.type === "jspb" && Array.isArray(chunk.data)) {
      const nodes = jspbToTree(chunk.data);
      const schemaName = `${asyncPath}_chunk${i}Response`;
      let schema = null;
      if (doc) {
        schema = doc.schemas?.[schemaName];
      }
      tree.appendChild(_respCard(label("badge badge-found", "JSPB"),
                                 _respTreeContainer(renderPbTree(nodes, schema, schemaName, doc))));
    } else if (chunk.type === "html") {
      tree.appendChild(_respCard(label("badge badge-pending", "HTML"), _respPre(chunk.raw, "resp-body resp-body-scroll")));
    } else {
      tree.appendChild(_respCard(label("badge", "text"), _respPre(chunk.raw)));
    }
  }
  return [tree];
}

// ─── gRPC-Web Renderer ──────────────────────────────────────────────────────

function renderGrpcWebResponse(bytes, req, overrideDoc = null) {
  const parsed = parseGrpcWebFrames(bytes);
  if (!parsed || parsed.frames.length === 0)
    return [_respPre("[gRPC-Web: no frames decoded]")];

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

  const tree = domElement("div", { class: "pb-tree" });

  // Trailers summary
  if (Object.keys(parsed.trailers).length > 0) {
    const grpcStatus = parsed.trailers["grpc-status"] || "?";
    const grpcMsg = parsed.trailers["grpc-message"] || "";
    const decodedMsg = (() => { try { return decodeURIComponent(grpcMsg); } catch (_) { return grpcMsg; } })();
    tree.appendChild(domElement("div", { class: "card card-compact" }, [
      domElement("div", { class: "card-label" }, [
        "gRPC Status: ", domElement("strong", {}, [grpcStatus]),
        grpcMsg ? " — " + decodedMsg : "",
      ]),
    ]));
  }

  for (let i = 0; i < parsed.frames.length; i++) {
    const frame = parsed.frames[i];
    if (frame.type === "data") {
      const pbTree = pbDecodeTree(frame.data);
      tree.appendChild(_respCard([
        "Data Frame " + i + " ", domElement("span", { class: "badge badge-found" }, ["protobuf"]), " ",
        domElement("span", { class: "text-muted-sm ml-4" }, [frame.data.length + " bytes"]),
      ], _respTreeContainer(renderPbTree(pbTree, respSchema, grpcFallbackId, doc))));
    } else if (frame.type === "trailers") {
      tree.appendChild(_respCard(["Trailers"], _respPre(frame.data, "resp-body resp-body-scroll-sm")));
    }
  }
  return [tree];
}

// ─── SSE Renderer ───────────────────────────────────────────────────────────

function renderSSEResponse(bodyText) {
  const events = parseSSE(bodyText);
  if (!events || events.length === 0)
    return [_respPre(bodyText)];

  const tree = domElement("div", { class: "pb-tree" });
  for (let i = 0; i < events.length; i++) {
    const evt = events[i];
    const typeBadge = evt.event !== "message"
      ? [" ", domElement("span", { class: "badge badge-pending" }, [evt.event])]
      : [];
    const idBadge = evt.id
      ? [" ", domElement("span", { class: "text-muted-sm" }, ["id: " + evt.id])]
      : [];

    const body = (typeof evt.data === "object" && evt.data !== null)
      ? _respPre(JSON.stringify(evt.data, null, 2))
      : _respPre(evt.raw);

    tree.appendChild(_respCard(["Event " + i, typeBadge, idBadge], body));
  }
  return [domElement("div", { class: "card-label" }, ["Server-Sent Events (" + events.length + " events)"]), tree];
}

// ─── NDJSON Renderer ────────────────────────────────────────────────────────

function renderNDJSONResponse(bodyText) {
  const objects = parseNDJSON(bodyText);
  if (!objects || objects.length === 0)
    return [_respPre(bodyText)];

  const tree = domElement("div", { class: "pb-tree" });
  for (let i = 0; i < objects.length; i++) {
    tree.appendChild(_respCard(["Record " + i], _respPre(JSON.stringify(objects[i], null, 2))));
  }
  return [domElement("div", { class: "card-label" }, ["NDJSON (" + objects.length + " records)"]), tree];
}

// ─── GraphQL Renderer ───────────────────────────────────────────────────────

function renderSingleGqlResult(r, respSchema, responseSchemaId, doc) {
  const out = [];
  if (r.errors) {
    out.push(_respCard(["Errors (" + r.errors.length + ")"], _respPre(JSON.stringify(r.errors, null, 2)),
                       "card card-compact-error", "card-label card-label-error"));
  }
  if (r.data) {
    const nodes = jsonToTree(r.data);
    out.push(domElement("div", { class: "card-label" }, ["Data"]),
             renderPbTree(nodes, respSchema, responseSchemaId, doc));
  }
  if (r.extensions) {
    out.push(_respCard(["Extensions"], _respPre(JSON.stringify(r.extensions, null, 2), "resp-body resp-body-scroll"),
                       "card card-compact", "card-label card-label-muted"));
  }
  return out;
}

function renderGraphQLResponse(bodyText, respSchema, responseSchemaId, doc) {
  const gql = parseGraphQLResponse(bodyText);
  if (!gql) return null; // Fall through to normal JSON rendering

  if (gql.results.length === 1) {
    return renderSingleGqlResult(gql.results[0], respSchema, responseSchemaId, doc);
  }

  const out = [];
  for (let i = 0; i < gql.results.length; i++) {
    out.push(_respCard(["Operation " + (i + 1)],
                       renderSingleGqlResult(gql.results[i], respSchema, responseSchemaId, doc)));
  }
  return out;
}

// ─── Multipart Batch Renderer ───────────────────────────────────────────────

function renderMultipartBatchResponse(bodyText, contentType) {
  const parts = parseMultipartBatch(bodyText, contentType);
  if (!parts || parts.length === 0)
    return [_respPre(bodyText)];

  const tree = domElement("div", { class: "pb-tree" });
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const statusBadge = part.status
      ? domElement("span", { class: part.status >= 200 && part.status < 300 ? "badge badge-found" : "badge badge-notfound" },
                   [String(part.status)])
      : [];

    let body;
    try {
      const json = JSON.parse(part.body);
      body = _respPre(JSON.stringify(json, null, 2));
    } catch (_) {
      body = _respPre(part.body);
    }

    tree.appendChild(_respCard(["Part " + i + " ", statusBadge, " " + (part.statusText || "")], body));
  }
  return [domElement("div", { class: "card-label" }, ["Multipart Batch (" + parts.length + " parts)"]), tree];
}
