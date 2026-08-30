/* Popup response-body rendering — extracted from popup.js (classic script, shares the popup global scope
   + DOM). renderResponse dispatches a Send result to renderResultBody, which renders JSON / a decoded
   protobuf tree (renderPbTree/_renderPbNode/jsonToTree) with the request's schema (findSchemaForRequest).
   Protocol-specific renderers (gRPC-Web/SSE/NDJSON/GraphQL) remain in popup.js and are called globally. */
/* WHAT A SEND RESULT IS, ASSERTED WHERE IT ARRIVES IN THE POPUP. lib/send.js answers a manual replay with
   exactly two shapes and this is the popup's statement of both: an ERROR arm carrying `error` (and `timing`
   on the two arms that got as far as issuing the request), and a SUCCESS record that writes
   `ok`/`status`/`statusText`/`headers`/`body`/`timing` in ONE object literal, downstream of the page-context
   relay contract lib/schema.js asserts. Everything this file renders comes off that record, so a producer
   that stopped writing a field would be rendered rather than reported — `statusText || ""` and `timing || 0`
   are the two that show it best, because "" and 0 are BOTH REAL ANSWERS (an HTTP/2 response carries no reason
   phrase; a cached reply can measure 0 ms), so the fallback said exactly what a healthy answer says.
   THE DISCRIMINANT IS `error`, NOT `status`. It was `result.error && !result.status`, which asks the error
   arm to prove itself with the ABSENCE of a field belonging to the other arm — and the success record carries
   `error: null`, so the two are already distinguishable by the one field that means it. */
function renderResponse(result) {
  DCHECK(!!result && typeof result === "object",
         "the popup was handed a send result that is not a record — lib/send.js returns an object from every " +
         "arm, so an absent one is that path broken rather than a request the page refused");
  if (result.error) {
    DCHECK(typeof result.error === "string" && result.error !== "",
           "a send result reports an error that names nothing — the reason IS the whole of what this arm " +
           "carries, and an empty one is rendered to the reviewer as a failure with no stated cause");
  } else {
    DCHECK(typeof result.ok === "boolean" && typeof result.status === "number" &&
           typeof result.statusText === "string" && !!result.headers && typeof result.headers === "object" &&
           !!result.body && typeof result.body === "object" && typeof result.timing === "number",
           "a send result is missing part of its response record — lib/send.js writes ok/status/statusText/" +
           "headers/body/timing in one literal, and a gap in it renders as the server's own answer");
  }
  lastSendResult = result;
  const container = document.getElementById("send-response");
  container.style.display = "block";

  // Restore child structure if a previous error replaced it via innerHTML
  if (!document.getElementById("send-response-status")) {
    container.innerHTML =
      '<div class="section-header">Manual Send Result</div>' +
      '<div id="send-response-status"></div>' +
      '<details id="send-response-headers-section"><summary>Response Headers</summary>' +
      '<table id="send-response-headers" class="auth-table"></table></details>' +
      '<div id="send-response-body"></div>';
  }

  if (result.error) {
    document.getElementById("send-response-status").innerHTML = "";
    document.getElementById("send-response-headers").innerHTML = "";
    document.getElementById("send-response-body").innerHTML =
      `<div class="card"><div class="card-label">Error</div><div class="card-value">${esc(result.error)}</div></div>`;
    return;
  }

  const statusEl = document.getElementById("send-response-status");
  /* `ok` IS THE WHOLE ANSWER. content.js reads it off a real `Response`, whose `ok` getter (Fetch §5.5
     Response class) returns whether the status is an OK STATUS — "a status in the range 200 to 299,
     inclusive" (Fetch §2.2.3 Statuses). So `result.status >= 200 && result.status < 300` was that same
     predicate written out beside the field that already states it, and its only reachable effect was to
     answer for an ABSENT `ok`. There is no absent `ok` here; the assert at the top says so. */
  const statusClass = result.ok ? "resp-status-ok" : "resp-status-error";
  statusEl.innerHTML =
    `<span class="${statusClass}">${esc(String(result.status))} ${esc(result.statusText)}</span>` +
    ` <span class="resp-timing">${esc(String(result.timing))}ms</span>` +
    ` <span class="resp-size">${esc(String(result.body.size))} bytes</span>`;

  const headersTable = document.getElementById("send-response-headers");
  headersTable.innerHTML = "";
  for (const [k, v] of Object.entries(result.headers)) {
    headersTable.innerHTML += `<tr><td>${esc(k)}</td><td>${esc(v)}</td></tr>`;
  }

  const bodyEl = document.getElementById("send-response-body");
  bodyEl.innerHTML = renderResultBody(result);

  const dlBtn = document.getElementById("btn-download-response");
  if (dlBtn) {
    dlBtn.onclick = () => {
      saveBinaryResponse(
        result.body.raw,
        result.body.bodyEncoding,
        result.body.contentType,
      ).catch(() => {});
    };
  }
}

/* THE THREE METADATA NAMES ARE ECHOED BACK ON EVERY SUCCESS RECORD, AND `null` IS WHAT EACH SAYS WHEN THERE
   IS NOTHING TO ECHO. lib/send.js writes `service`, `methodId` and `discovery` in the same literal as
   ok/status/headers/body — so the question these three `||`s were answering is not "did the producer write
   it" (it always does) but "did the reviewer have a discovery method selected when they pressed Send", which
   is a state the panel is legitimately in and which `null` states outright. Written as `||`, that legitimate
   null and a producer that had stopped echoing were the same bytes, and the fallback — the CURRENT contents
   of the endpoint select — is a different question's answer: what is selected NOW, which after a re-select is
   not what this response came from. So each is read as itself, and the select is consulted only where the
   record says there was nothing to echo. */
function renderResultBody(result) {
  DCHECK((result.service === null || typeof result.service === "string") &&
         (result.methodId === null || typeof result.methodId === "string"),
         "a send result's service/methodId is neither a name nor a stated absence — lib/send.js echoes both " +
         "on every success record and asserts `null` for \"no discovery method was chosen\" where the request " +
         "was composed, so a third spelling here is that echo broken and this panel would resolve the " +
         "response against whichever method the select happens to be showing now");
  DCHECK(result.discovery === null || (!!result.discovery && typeof result.discovery === "object"),
         "a send result's `discovery` is neither a doc entry nor a stated absence — `null` MEANS the " +
         "offscreen store held no discovery document for this service, and `undefined` cannot mean it: " +
         "chrome.runtime.sendMessage DROPS an undefined property, so that spelling arrives as a record with " +
         "no `discovery` key and is indistinguishable from a producer that stopped writing one");

  const sel = document.getElementById("send-ep-select");
  const methodId = result.methodId === null ? sel.dataset.discoveryId : result.methodId;
  const svc = result.service === null ? sel.dataset.svc : result.service;
  let respSchema = null;

  /* The result's own doc is the one this response was resolved against; the popup's cached copy is consulted
     only where the offscreen store had none. (popup.js clears this to `null` after a field rename — saying
     "that doc is stale, re-read mine" — where it used to `delete` the property, which is the same statement
     made in the one spelling this record cannot carry.) */
  const discoveryInfo = result.discovery === null ? tabData?.discoveryDocs?.[svc] : result.discovery;

  // Deterministic schema ID for storing response field renames
  const responseSchemaId = methodId ? `${methodId}.response` : "";

  const doc = discoveryInfo?.doc || null;
  if (svc && methodId && doc) {
    const methodInfo = findMethodById(doc, methodId);
    if (methodInfo?.method?.response?.$ref) {
      respSchema = resolveDiscoverySchema(doc, methodInfo.method.response.$ref);
    }
    // Fallback: check for manually-created schema when no probed schema exists
    if (!respSchema && responseSchemaId && doc.schemas?.[responseSchemaId]) {
      respSchema = doc.schemas[responseSchemaId];
    }
  }

  /* `raw` IS THE RESPONSE TEXT AND IT IS ALWAYS WRITTEN — every arm of lib/send.js's bodyResult carries it,
     and an empty response is the empty string rather than the absence of one. `headers` is proven by the
     assert at the top of this file; what is legitimately absent is a HEADER, which is a fact about the server
     and is what the `|| ""` on the key lookup reads. */
  const rawBody = result.body.raw;
  const respContentType = result.headers["content-type"] || "";

  /* `svc`, NOT `result.service || svc`, AT ALL THREE PROTOCOL RENDERERS. `svc` IS `result.service` wherever
     that is stated, so the `||` could only ever yield `svc` — it had no reachable second arm at all. It read
     as a safety net over the same field the line above had already resolved, which is why it survived three
     copies: a default nobody can see the effect of, because it has none. The gRPC renderer below now takes
     the resolved `methodId` beside it rather than the raw echo: it was handed a RESOLVED service and an
     UNRESOLVED method id as one pair, and the two halves of that pair name one method. */
  if (isAsyncChunkedResponse(rawBody)) {
    return renderAsyncResponse(
      rawBody,
      { service: svc, url: currentRequestUrl },
      discoveryInfo?.doc,
    );
  }

  // gRPC-Web: unwrap frames, render protobuf trees
  if (result.body.format === "grpc_web") {
    const grpcBytes = result.body.bytes || (result.body.bytesB64 ? base64ToUint8(result.body.bytesB64) : null);
    if (grpcBytes) {
      return renderGrpcWebResponse(
        grpcBytes,
        { service: svc, methodId },
        discoveryInfo?.doc,
      );
    }
  }

  // SSE: split events, render individually
  if (isSSE(respContentType)) {
    return renderSSEResponse(rawBody);
  }

  // NDJSON: split lines, render as records
  if (isNDJSON(respContentType)) {
    return renderNDJSONResponse(rawBody);
  }

  // Multipart batch: parse MIME parts
  if (isMultipartBatch(respContentType)) {
    return renderMultipartBatchResponse(rawBody, respContentType);
  }

  // GraphQL: enhanced display with data/errors/extensions sections
  if (isGraphQLUrl(currentRequestUrl) && result.body.format === "json") {
    const gqlHtml = renderGraphQLResponse(rawBody, respSchema, responseSchemaId, doc);
    if (gqlHtml) return gqlHtml;
  }

  // batchexecute: detect by content (wrb.fr markers), not just URL
  if (isBatchExecuteResponse(rawBody)) {
    return renderBatchExecuteResponse(
      rawBody,
      { service: svc },
      discoveryInfo?.doc,
    );
  }

  if (result.body.format === "binary_download") {
    const ct = result.body.contentType || "application/octet-stream";
    const sizeKB = (result.body.size / 1024).toFixed(1);
    const ext = mimeToExt(ct);
    return `<div class="card card-compact">
      <div class="card-label">Binary Response</div>
      <div class="card-value">${esc(ct)} — ${sizeKB} KB</div>
      <button class="btn-action" id="btn-download-response">Save As${ext ? " (." + ext + ")" : ""}</button>
    </div>`;
  }

  if (result.body.format === "json") {
    const nodes = jsonToTree(result.body.parsed);
    return (
      `<div class="card-label">Decoded JSON</div>` +
      renderPbTree(nodes, respSchema, responseSchemaId, doc)
    );
  } else if (result.body.format === "protobuf_tree") {
    return (
      `<div class="card-label">Decoded Protobuf</div>` +
      renderPbTree(result.body.parsed, respSchema, responseSchemaId, doc)
    );
  } else {
    return `<pre class="resp-body">${esc(result.body.raw || "")}</pre>`;
  }
}

function addHeaderRow(initialKey = "", initialValue = "") {
  const list = document.getElementById("send-headers-list");
  const row = el("div", "header-row");
  row.innerHTML =
    `<input class="header-key" type="text" placeholder="Header-Name" value="${esc(initialKey)}" />` +
    `<input class="header-val" type="text" placeholder="value" value="${esc(initialValue)}" />` +
    `<button class="btn-remove-header" type="button" title="Remove">&times;</button>`;
  row
    .querySelector(".btn-remove-header")
    .addEventListener("click", () => row.remove());
  list.appendChild(row);
}

// ─── Export / Copy ────────────────────────────────────────────────────────────

function copyToClipboard(panelName, data) {
  const btn = document.getElementById(`btn-export-${panelName}`);
  const text = JSON.stringify(data ?? null, null, 2);

  navigator.clipboard.writeText(text).then(() => {
    btn.textContent = "Copied!";
    btn.classList.add("copied");
    setTimeout(() => {
      btn.textContent = "Copy";
      btn.classList.remove("copied");
    }, 1500);
  });
}

// ─── Spec Export / Import ────────────────────────────────────────────────────

async function exportOpenApiSpec() {
  const svcFilter = document.getElementById("spec-service-select").value;

  // Collect services to export
  const services = [];
  if (svcFilter) {
    services.push(svcFilter);
  } else if (tabData?.discoveryDocs) {
    for (const [svcName, svcData] of Object.entries(tabData.discoveryDocs)) {
      if (svcData.doc) services.push(svcName);   // a service with a learned doc is exportable whatever the published fetch answered
    }
  }
  if (!services.length) {
    alert("No services discovered yet.");
    return;
  }

  const btn = document.getElementById("btn-export-spec");
  btn.disabled = true;
  btn.textContent = "...";

  try {
    // Collect all specs
    const specs = [];
    for (const svc of services) {
      const result = await chrome.runtime.sendMessage({
        type: "EXPORT_OPENAPI",
        service: svc,
      });
      if (result?.error && services.length === 1) { alert(result.error); return; }
      if (result?.spec) specs.push({ svc, spec: result.spec });
    }
    if (!specs.length) { alert("No specs to export."); return; }

    let combined;
    let filename;
    if (specs.length === 1) {
      combined = specs[0].spec;
      filename = specs[0].svc.replace(/[^a-zA-Z0-9.-]/g, "_") + ".openapi.json";
    } else {
      // Merge all specs into one
      combined = {
        openapi: "3.0.3",
        info: {
          title: "API Security Researcher — Combined Export",
          description: `Merged from ${specs.length} services: ${specs.map(s => s.svc).join(", ")}`,
          version: "v1",
        },
        servers: [],
        paths: {},
        components: { schemas: {} },
      };
      const seenServers = new Set();
      for (const { spec } of specs) {
        for (const srv of spec.servers || []) {
          if (!seenServers.has(srv.url)) {
            seenServers.add(srv.url);
            combined.servers.push(srv);
          }
        }
        Object.assign(combined.paths, spec.paths || {});
        Object.assign(combined.components.schemas, spec.components?.schemas || {});
        if (spec.components?.securitySchemes) {
          combined.components.securitySchemes = {
            ...combined.components.securitySchemes,
            ...spec.components.securitySchemes,
          };
        }
      }
      filename = "combined.openapi.json";
    }
    await downloadJson(combined, filename);
    btn.textContent = "Done!";
    setTimeout(() => { btn.textContent = "Export"; btn.disabled = false; }, 1500);
  } catch (err) {
    alert("Export failed: " + err.message);
    btn.textContent = "Export";
    btn.disabled = false;
  }
}

async function downloadJson(obj, filename) {
  const json = JSON.stringify(obj, null, 2);
  const handle = await window.showSaveFilePicker({
    suggestedName: filename,
    types: [{
      description: "JSON",
      accept: { "application/json": [".json"] },
    }],
  });
  const writable = await handle.createWritable();
  await writable.write(json);
  await writable.close();
}

function mimeToExt(ct) {
  const map = {
    "image/png": "png", "image/jpeg": "jpg", "image/gif": "gif",
    "image/webp": "webp", "image/svg+xml": "svg",
    "video/mp4": "mp4", "video/webm": "webm",
    "audio/mpeg": "mp3", "audio/ogg": "ogg", "audio/wav": "wav",
    "application/pdf": "pdf", "application/zip": "zip",
    "application/octet-stream": "bin",
  };
  for (const [mime, ext] of Object.entries(map)) {
    if (ct.includes(mime)) return ext;
  }
  return "";
}

async function saveBinaryResponse(base64Data, bodyEncoding, contentType) {
  const ext = mimeToExt(contentType);
  const handle = await window.showSaveFilePicker({
    suggestedName: `response${ext ? "." + ext : ""}`,
    types: [{
      description: contentType,
      accept: { [contentType.split(";")[0].trim()]: ext ? ["." + ext] : [] },
    }],
  });
  const writable = await handle.createWritable();
  const bytes = bodyEncoding === "base64"
    ? base64ToUint8(base64Data)
    : new TextEncoder().encode(base64Data);
  await writable.write(bytes);
  await writable.close();
}

async function importOpenApiSpec(e) {
  const file = e.target.files[0];
  if (!file) return;
  e.target.value = "";

  const btn = document.getElementById("btn-import-spec");
  btn.disabled = true;
  btn.textContent = "Importing...";

  try {
    const text = await file.text();
    let spec;
    try {
      spec = JSON.parse(text);
    } catch (_) {
      alert("Only JSON format is supported. Convert YAML to JSON first.");
      btn.textContent = "Import";
      btn.disabled = false;
      return;
    }

    const result = await chrome.runtime.sendMessage({
      type: "IMPORT_OPENAPI",
      spec,
    });

    if (result?.error) {
      alert(result.error);
      btn.textContent = "Import";
      btn.disabled = false;
      return;
    }

    btn.textContent = "Imported!";
    setTimeout(() => { btn.textContent = "Import"; btn.disabled = false; }, 1500);

    // Refresh the UI to show imported methods
    await loadState();
  } catch (err) {
    alert("Import failed: " + err.message);
    btn.textContent = "Import";
    btn.disabled = false;
  }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

/* THE LABEL THAT NAMES WHICH PAGE A ROW IS ABOUT — the engine-run list, the @S finding's source and page, the
   request log. It read `parsed.hostname`, which DROPS THE PORT, and a port is part of the ORIGIN: three probe
   documents on 8890, 8896 and 8850 are three different origins with three different frontiers, and all three
   rendered as the single word "127.0.0.1". MEASURED in the engine-runs panel: eight rows, three pages, one
   label — the panel exists to say which run belongs to which page and could not. `host` is the same string
   with the port when there is a non-default one, which is what the browser's own UI shows. */
function _shortUrl(u) {
  try {
    var parsed = new URL(u);
    var path = parsed.pathname.replace(/\/$/, "");
    if (!path) return parsed.host;
    return parsed.host + path;
  } catch (_) {
    return u;
  }
}

function esc(s) {
  if (s == null) return "";
  const d = document.createElement("div");
  d.textContent = String(s);
  return d.innerHTML.replace(/"/g, "&quot;").replace(/'/g, "&#39;").replace(/`/g, "&#96;");
}

function el(tag, className) {
  const e = document.createElement(tag);
  if (className) e.className = className;
  return e;
}

function renderPbTree(rootNodes, rootSchema = null, rootFallbackSchemaId = "", rootDoc = null) {
  // Iterative HTML builder. The stack holds work items processed LIFO;
  // each is either a "literal" string fragment or a "render" job for
  // (nodes, schema, fallback) which emits one node and pushes nested
  // children/items as further jobs interleaved with their wrapper
  // open/close fragments. Replaces self-recursion so deeply-nested
  // protobuf trees render without growing the JS call stack.
  if (!rootNodes || rootNodes.length === 0) return '<div class="pb-empty">Empty body</div>';

  const out = [];
  const stack = [];
  // Push initial scope: open pb-tree, render each node, close pb-tree.
  // Pushed in REVERSE so LIFO pop yields correct visual order.
  function pushNodes(nodes, schema, fallbackSchemaId, doc) {
    if (schema && schema.request) schema = schema.request;
    let fieldMap = null;
    if (schema) {
      if (schema.properties) fieldMap = schema.properties;
      else if (schema.parameters) fieldMap = schema.parameters;
      else fieldMap = schema;
    }
    stack.push({ kind: "literal", text: '</div>' });
    for (let i = nodes.length - 1; i >= 0; i--) {
      stack.push({ kind: "node", node: nodes[i], schema, fieldMap, fallbackSchemaId, doc });
    }
    stack.push({ kind: "literal", text: '<div class="pb-tree">' });
  }
  pushNodes(rootNodes, rootSchema, rootFallbackSchemaId, rootDoc);

  while (stack.length > 0) {
    const item = stack.pop();
    if (item.kind === "literal") { out.push(item.text); continue; }
    _renderPbNode(item, out, stack);
  }
  return out.join("");
}

// Render one pb-tree node into `out` and push any nested children/items
// onto the stack as interleaved literal+node entries. The original
// recursive form returned a string per call; here we stream fragments
// into `out` and defer nested expansion to the outer driver loop.
function _renderPbNode(item, out, stack) {
  const { node, schema, fieldMap, fallbackSchemaId, doc } = item;
  // Field definition lookup mirrors the original logic exactly.
  let fieldDef = null;
  let fieldName = node.isJson ? String(node.field) : `Field ${node.field}`;
  if (fieldMap) {
    if (Array.isArray(fieldMap)) {
      fieldDef = fieldMap.find(
        (f) => !f.isNumberGuessed &&
          (String(f.number) == String(node.field) || String(f.id) == String(node.field)),
      );
      if (!fieldDef) {
        fieldDef = fieldMap.find(
          (f) => String(f.number) == String(node.field) || String(f.id) == String(node.field),
        );
      }
      if (fieldDef) fieldName = fieldDef.name || fieldName;
    } else {
      const entries = Object.entries(fieldMap);
      let foundEntry = entries.find(([k, v]) =>
        !v.isNumberGuessed &&
        (String(v.id) == String(node.field) || String(v.number) == String(node.field)),
      );
      if (!foundEntry) {
        foundEntry = entries.find(([k, v]) =>
          String(v.id) == String(node.field) || String(v.number) == String(node.field),
        );
      }
      if (foundEntry) {
        fieldName = foundEntry[0];
        fieldDef = foundEntry[1];
        if (fieldDef.name) fieldName = fieldDef.name;
      }
    }
  }
  let typeLabel;
  if (fieldDef) {
    typeLabel = `<span class="pb-type-badge">${esc(fieldDef.type || "")}</span>`;
  } else if (node.isJson) {
    const jsType = node.jsType || (node.string != null ? "string" : typeof node.value);
    typeLabel = `<span class="pb-type-badge">${esc(jsType)}</span>`;
  } else {
    typeLabel = `<span class="pb-wire-badge">${node.wire === 0 ? "varint" : node.wire === 1 ? "64bit" : node.wire === 2 ? "len" : "32bit"}</span>`;
  }
  const currentSchemaId = schema?.id || (schema?.$ref) || fallbackSchemaId;
  const renameAttr = `data-schema="${esc(currentSchemaId)}" data-key="${esc(fieldDef ? (fieldDef.id || fieldDef.number || fieldName) : node.field)}" data-is-raw="${!fieldDef}"`;
  const renameBtn = currentSchemaId ? ` <span class="btn-rename" title="Rename field" ${renameAttr}>✎</span>` : "";

  out.push(`<div class="pb-node">
      <span class="pb-field">${esc(fieldName)}</span>${renameBtn}
      ${typeLabel}: `);

  // Build the per-node inner HTML. Nested-tree fragments push themselves
  // onto the stack (followed by their wrapper close), and the node-close
  // fragment is queued LIFO so it lands last.
  // Pushed in REVERSE order: closing div, then nested content, then any
  // pre-content (none here — all the literal HTML before nested calls is
  // emitted via out.push directly above).
  // To preserve original ordering: the parent's closing </div> goes
  // LAST in output, so it's pushed FIRST onto the stack.
  // Helper: push a render job for child nodes inside a wrapper div.
  // Order on output: <wrapperOpen>...<child rendering>...</wrapper>
  function pushNestedTree(wrapperOpen, wrapperClose, childNodes, childSchema, childFallback) {
    // Stack pops LIFO; to get [wrapperOpen, child*, wrapperClose] in
    // output, push them in REVERSE: close, child*, open.
    // Children themselves expand to multiple frames each; push them in
    // reverse too so they pop in original order.
    stack.push({ kind: "literal", text: wrapperClose });
    // Resolve fieldMap for childSchema right here (matches pushNodes).
    let cs = childSchema;
    if (cs && cs.request) cs = cs.request;
    let cfm = null;
    if (cs) {
      if (cs.properties) cfm = cs.properties;
      else if (cs.parameters) cfm = cs.parameters;
      else cfm = cs;
    }
    for (let i = childNodes.length - 1; i >= 0; i--) {
      stack.push({ kind: "node", node: childNodes[i], schema: cs, fieldMap: cfm, fallbackSchemaId: childFallback, doc });
    }
    stack.push({ kind: "literal", text: wrapperOpen });
  }

  // The closing </div> for the current pb-node. Push BEFORE any nested
  // content so it lands at the bottom of this node's output.
  stack.push({ kind: "literal", text: "</div>" });

  if (node.message) {
    let childrenSchema = fieldDef?.children || null;
    const childFallback = currentSchemaId ? `${currentSchemaId}.${node.field}` : "";
    if (!childrenSchema && childFallback && doc?.schemas?.[childFallback]) {
      childrenSchema = doc.schemas[childFallback];
    }
    pushNestedTree('<div class="pb-nested"><div class="pb-tree">', '</div></div>',
      node.message, childrenSchema, childFallback);
  } else if (node.packed) {
    let frag = '<div class="pb-repeated">';
    for (const val of node.packed) {
      frag += `<span class="pb-scalar-item">${esc(String(val))}</span> `;
    }
    frag += "</div>";
    stack.push({ kind: "literal", text: frag });
  } else if (node.isRepeatedScalar && Array.isArray(node.value)) {
    let frag = '<div class="pb-repeated">';
    for (const val of node.value) {
      if (val === null || val === undefined) continue;
      frag += `<span class="pb-scalar-item">${esc(JSON.stringify(val))}</span> `;
    }
    frag += "</div>";
    stack.push({ kind: "literal", text: frag });
  } else if (Array.isArray(node.value) && node.isJspb) {
    const isRepeated = fieldDef?.label === "repeated";
    const isMessage = fieldDef?.type === "message";
    if (isRepeated) {
      // Sequence: <div class="pb-repeated">[items]</div>
      stack.push({ kind: "literal", text: "</div>" });
      // Items in REVERSE so they pop in original order.
      for (let vi = node.value.length - 1; vi >= 0; vi--) {
        const itemv = node.value[vi];
        if (itemv === null || itemv === undefined) continue;
        if (isMessage && Array.isArray(itemv)) {
          const itemNodes = jspbToTree(itemv);
          const childFallback = currentSchemaId ? `${currentSchemaId}.${node.field}` : "";
          pushNestedTree('<div class="pb-nested-item"><div class="pb-tree">', '</div></div>',
            itemNodes, fieldDef?.children, childFallback);
        } else {
          stack.push({ kind: "literal", text: `<span class="pb-scalar-item">${esc(JSON.stringify(itemv))}</span>` });
        }
      }
      stack.push({ kind: "literal", text: '<div class="pb-repeated">' });
    } else if (isMessage) {
      const nestedNodes = jspbToTree(node.value);
      const childFallback = currentSchemaId ? `${currentSchemaId}.${node.field}` : "";
      pushNestedTree('<div class="pb-nested"><div class="pb-tree">', '</div></div>',
        nestedNodes, fieldDef?.children, childFallback);
    } else {
      stack.push({ kind: "literal", text: `<span class="pb-string">${esc(JSON.stringify(node.value))}</span>` });
    }
  } else if (node.string !== undefined) {
    stack.push({ kind: "literal", text: `<span class="pb-string">"${esc(node.string)}"</span>` });
  } else if (node.value !== undefined) {
    if (typeof node.value === "object" && node.value !== null) {
      const childNodes = jsonToTree(node.value);
      let childrenSchema = fieldDef?.children || null;
      const childFallback = currentSchemaId ? `${currentSchemaId}.${node.field}` : "";
      if (!childrenSchema && childFallback && doc?.schemas?.[childFallback]) {
        childrenSchema = doc.schemas[childFallback];
      }
      pushNestedTree('<div class="pb-nested"><div class="pb-tree">', '</div></div>',
        childNodes, childrenSchema, childFallback);
    } else {
      stack.push({ kind: "literal", text: `<span class="pb-number">${esc(String(node.value))}</span>` });
    }
  } else if (node.hex) {
    stack.push({ kind: "literal", text: `<span class="pb-hex">0x${esc(node.hex)}</span>` });
  } else if (node.asFloat !== undefined) {
    stack.push({ kind: "literal", text: `<span class="pb-number">${node.asFloat.toFixed(4)}</span>` });
  }
}

function jsonToTree(rootObj) {
  // Iterative tree builder. Each worklist entry pairs the source value
  // with the destination message-array to populate. Nested objects/arrays
  // get pre-allocated nodes whose .message is queued for population.
  if (rootObj === null || rootObj === undefined) return [];
  const root = [];
  const queue = [{ src: rootObj, dst: root }];
  while (queue.length > 0) {
    const { src, dst } = queue.shift();
    if (Array.isArray(src)) {
      for (let i = 0; i < src.length; i++) {
        const item = src[i];
        if (item && typeof item === "object") {
          const child = { field: i, wire: 2, message: [], isJson: true };
          dst.push(child);
          queue.push({ src: item, dst: child.message });
        } else if (typeof item === "string") {
          dst.push({ field: i, wire: 2, string: item, isJson: true });
        } else {
          dst.push({ field: i, wire: 0, value: item, isJson: true });
        }
      }
      continue;
    }
    if (typeof src !== "object" || src === null) continue;
    const entries = Object.entries(src);
    for (let ei = 0; ei < entries.length; ei++) {
      const key = entries[ei][0], val = entries[ei][1];
      if (val && typeof val === "object") {
        // Both arrays and objects route through the same nested branch.
        const child = { field: key, wire: 2, message: [], isJson: true };
        dst.push(child);
        queue.push({ src: val, dst: child.message });
      } else if (typeof val === "string") {
        dst.push({ field: key, wire: 2, string: val, isJson: true });
      } else {
        dst.push({ field: key, wire: 0, value: val, isJson: true });
      }
    }
  }
  return root;
}

function findSchemaForRequest(req) {
  if (!tabData?.discoveryDocs || !req.service) return null;
  const svcInfo = tabData.discoveryDocs[req.service];
  if (!svcInfo) return null;

  // We need the full discovery doc
  const doc = svcInfo.doc;
  if (!doc) {
    return null;
  }
  const url = new URL(req.url);

  // 1. Try matching by methodId if background annotated it
  if (req.methodId) {
    const methodMatch = findMethodById(doc, req.methodId);
    if (methodMatch && methodMatch.method.request?.$ref) {
      return resolveDiscoverySchema(doc, methodMatch.method.request.$ref);
    }
  }

  // 2. Fallback: match by URL path
  const match = findDiscoveryMethod(doc, url.pathname, req.method);
  if (match && match.method.request?.$ref) {
    return resolveDiscoverySchema(doc, match.method.request.$ref);
  }

  return null;
}
