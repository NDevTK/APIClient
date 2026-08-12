/* Popup multipart body editor — extracted from popup.js (classic script, shares the popup's global scope:
   mpState + the DOM). One contextual sub-editor per multipart sub-part (json/graphql/urlencoded/raw),
   reassembled on Send. Includes the file-scope click/input delegates for the part cards. */
// ─── Multipart Editor ────────────────────────────────────────────────────────
//
// Renders one contextual editor per sub-part of a captured multipart request.
// Each part's body editor is chosen by its sub-Content-Type:
//   application/json            → JSON textarea with parse/format feedback
//   application/graphql OR body parses as GraphQL → GraphQL composer snippet
//   application/x-www-form-urlencoded → URL-encoded key/value list
//   anything else               → raw textarea, labeled with the sub-CT so
//                                  the user knows what they're editing
//
// Reassembly happens on Send via buildExportRequest with body.mode="multipart".
// Structure is preserved: N input parts → N output parts, each encoded per
// its chosen editor. No information is silently lost.

function mpClassifyPartBody(ct, bodyText) {
  const lct = (ct || "").toLowerCase();
  if (lct.includes("graphql") || lct.includes("application/json")) {
    // Distinguish JSON-GraphQL from plain JSON by probing for op structure.
    if (lct.includes("graphql")) return "graphql";
    const parsed = typeof parseGraphQLRequest === "function" ? parseGraphQLRequest(bodyText) : null;
    if (parsed && parsed.operations && parsed.operations.length) return "graphql";
    return "json";
  }
  if (lct.includes("application/x-www-form-urlencoded")) return "form-urlencoded";
  return "raw";
}

function mpLoadFromCapturedRequest(req) {
  // req.rawBodyB64 is the captured multipart envelope; req.contentType/mimeType
  // has the boundary. parseMultipartBatchRequest splits and returns per-part
  // { method, path, headers, body, contentId } — but that parser is tuned for
  // the Google batch "application/http" subtype. For generic multipart we
  // fall back to a simple split on the boundary.
  const ct = req.contentType || req.mimeType || "";
  if (!req.rawBodyB64) return false;
  let text;
  try { text = new TextDecoder().decode(base64ToUint8(req.rawBodyB64)); }
  catch (_) { return false; }
  const bm = ct.match(/boundary=["']?([^"';\s]+)/i);
  if (!bm) return false;
  const boundary = bm[1];

  // Try the batch-aware parser first.
  const batchParts = (typeof parseMultipartBatchRequest === "function")
    ? parseMultipartBatchRequest(text, ct)
    : null;

  const parts = [];
  if (batchParts && batchParts.length) {
    for (let i = 0; i < batchParts.length; i++) {
      const bp = batchParts[i];
      const partCt = bp.headers["content-type"] || "application/octet-stream";
      const kind = mpClassifyPartBody(partCt, bp.body);
      parts.push({
        partNumber: i + 1,
        method: bp.method,
        path: bp.path,
        contentType: partCt,
        extraHeaders: { ...bp.headers },
        contentId: bp.contentId || null,
        editor: { kind, value: bp.body, meta: null },
      });
    }
  } else {
    // Generic multipart split: each section between "--boundary" markers.
    const raw = text.split("--" + boundary);
    let idx = 0;
    for (const segment of raw) {
      const t = segment.trim();
      if (!t || t === "--") continue;
      // Header/body split on blank line.
      const sep = t.search(/\r?\n\r?\n/);
      const headerBlock = sep >= 0 ? t.slice(0, sep) : t;
      const bodyBlock = sep >= 0 ? t.slice(sep).replace(/^[\r\n]+/, "") : "";
      const headers = {};
      for (const line of headerBlock.split(/\r?\n/)) {
        const ci = line.indexOf(":");
        if (ci > 0) headers[line.slice(0, ci).trim().toLowerCase()] = line.slice(ci + 1).trim();
      }
      const partCt = headers["content-type"] || "application/octet-stream";
      const kind = mpClassifyPartBody(partCt, bodyBlock);
      parts.push({
        partNumber: ++idx,
        method: null,
        path: null,
        contentType: partCt,
        extraHeaders: headers,
        contentId: null,
        editor: { kind, value: bodyBlock, meta: null },
      });
    }
  }

  mpState = { boundary, parts };
  mpRenderAll();
  return parts.length > 0;
}

function mpRenderAll() {
  const container = document.getElementById("mp-parts-container");
  const info = document.getElementById("mp-boundary-info");
  if (!container) return;
  container.innerHTML = "";
  if (info) info.textContent = mpState.parts.length + " part" + (mpState.parts.length === 1 ? "" : "s") + " · boundary=" + (mpState.boundary || "auto");

  for (let i = 0; i < mpState.parts.length; i++) {
    container.appendChild(mpRenderPart(i));
  }
}

function mpRenderPart(index) {
  return _mpBuildPartCard(index);
}

// Builder that produces a fresh card element for parts[index].
// Click/input handlers for Remove / Content-Type are delegated at
// file scope (see end of section) so this body never references
// mpRenderAll or mpRenderPart by name. Each handler stashes the
// part index on its DOM element via data-mp-part-idx; the file-scope
// delegate reads the index and invokes the renderer in a fresh task.
function _mpBuildPartCard(index) {
  const p = mpState.parts[index];
  const card = document.createElement("div");
  card.className = "mp-part-card";
  card.dataset.mpPartIdx = String(index);

  const head = document.createElement("div");
  head.className = "mp-part-head";
  head.innerHTML =
    '<span class="mp-part-num">Part ' + esc(String(p.partNumber)) + '</span>' +
    (p.method ? ' <span class="badge">' + esc(p.method) + ' ' + esc(p.path || "") + '</span>' : '') +
    ' <span class="mp-part-kind">[' + esc(p.editor.kind) + ']</span>';
  const del = document.createElement("button");
  del.className = "btn-small mp-part-del";
  del.type = "button";
  del.textContent = "Remove";
  del.dataset.mpPartIdx = String(index);
  head.appendChild(del);
  card.appendChild(head);

  const ctRow = document.createElement("div");
  ctRow.className = "mp-ct-row";
  ctRow.innerHTML = '<label>Content-Type</label>';
  const ctInput = document.createElement("input");
  ctInput.type = "text";
  ctInput.value = p.contentType;
  ctInput.classList.add("mp-part-ct-input");
  ctInput.dataset.mpPartIdx = String(index);
  ctRow.appendChild(ctInput);
  card.appendChild(ctRow);

  // Body editor for this part.
  const editorWrap = document.createElement("div");
  editorWrap.className = "mp-editor-" + p.editor.kind;
  if (p.editor.kind === "json") {
    editorWrap.appendChild(mpJsonEditor(p));
  } else if (p.editor.kind === "graphql") {
    editorWrap.appendChild(mpGraphqlEditor(p));
  } else if (p.editor.kind === "form-urlencoded") {
    editorWrap.appendChild(mpFormUrlEncodedEditor(p));
  } else {
    editorWrap.appendChild(mpRawEditor(p));
  }
  card.appendChild(editorWrap);

  return card;
}

function mpJsonEditor(part) {
  const wrap = document.createElement("div");
  const ta = document.createElement("textarea");
  ta.className = "mp-json-editor";
  ta.rows = 6;
  // Pretty-print on load if valid JSON.
  try {
    const obj = JSON.parse(part.editor.value || "null");
    ta.value = JSON.stringify(obj, null, 2);
  } catch (_) { ta.value = part.editor.value || ""; }
  const status = document.createElement("span");
  status.className = "mp-json-status";
  status.textContent = "valid JSON";
  ta.oninput = () => {
    part.editor.value = ta.value;
    try { JSON.parse(ta.value); status.textContent = "valid JSON"; status.classList.remove("mp-invalid"); }
    catch (e) { status.textContent = "invalid: " + e.message; status.classList.add("mp-invalid"); }
  };
  wrap.appendChild(ta);
  wrap.appendChild(status);
  return wrap;
}

function mpGraphqlEditor(part) {
  const wrap = document.createElement("div");
  // Parse current value as GraphQL envelope to split query/variables/operationName.
  let parsed = null;
  try { parsed = parseGraphQLRequest(part.editor.value || "{}"); }
  catch (e) {
    // The body might not be a GraphQL request at all (user typing free-form
    // before structured editing kicks in) — that's expected and routes through
    // the `op` fallback below using the raw editor value. But a real parse
    // throw (vs returning null) means something unexpected; surface at debug
    // so a malformed-envelope diagnosis is possible without spamming the
    // common "free-form input" case (parseGraphQLRequest returns null there).
    console.debug("[popup:gql_editor] parseGraphQLRequest threw:", e && e.message || e);
  }
  const op = parsed?.operations?.[0] || { query: part.editor.value || "", variables: null, operationName: null };

  function field(label, value, rows) {
    const row = document.createElement("div");
    row.className = "mp-gql-field";
    const lab = document.createElement("label"); lab.textContent = label;
    const ta = document.createElement("textarea");
    ta.rows = rows || 3;
    ta.value = value || "";
    ta.dataset.mpGqlField = label;
    ta.oninput = () => serialize();
    row.appendChild(lab); row.appendChild(ta);
    return row;
  }
  const qRow = field("query", op.query, 6);
  const vRow = field("variables (JSON)", op.variables ? JSON.stringify(op.variables, null, 2) : "", 4);
  const nRow = field("operationName", op.operationName || "", 1);
  wrap.appendChild(qRow); wrap.appendChild(vRow); wrap.appendChild(nRow);

  function serialize() {
    const q = wrap.querySelector("[data-mp-gql-field='query']").value;
    const v = wrap.querySelector("[data-mp-gql-field='variables (JSON)']").value;
    const n = wrap.querySelector("[data-mp-gql-field='operationName']").value;
    const out = { query: q };
    if (v.trim()) {
      try { out.variables = JSON.parse(v); }
      catch (_) { /* leave raw; user sees red status elsewhere */ out.variables = v; }
    }
    if (n.trim()) out.operationName = n;
    part.editor.value = JSON.stringify(out, null, 2);
  }
  serialize(); // initial sync
  return wrap;
}

function mpFormUrlEncodedEditor(part) {
  const wrap = document.createElement("div");
  wrap.className = "mp-fue-editor";
  const params = new URLSearchParams(part.editor.value || "");
  function serialize() {
    const out = new URLSearchParams();
    for (const row of wrap.querySelectorAll(".mp-fue-row")) {
      const k = row.querySelector("[data-mp-fue=key]").value;
      const v = row.querySelector("[data-mp-fue=value]").value;
      if (k) out.append(k, v);
    }
    part.editor.value = out.toString();
  }
  function addRow(k, v) {
    const row = document.createElement("div"); row.className = "mp-fue-row";
    const ki = document.createElement("input"); ki.dataset.mpFue = "key"; ki.value = k;
    const vi = document.createElement("input"); vi.dataset.mpFue = "value"; vi.value = v;
    const rm = document.createElement("button"); rm.type = "button"; rm.className = "btn-small"; rm.textContent = "×";
    rm.onclick = () => { row.remove(); serialize(); };
    ki.oninput = serialize; vi.oninput = serialize;
    row.appendChild(ki); row.appendChild(vi); row.appendChild(rm);
    wrap.appendChild(row);
  }
  for (const [k, v] of params) addRow(k, v);
  const addBtn = document.createElement("button");
  addBtn.type = "button"; addBtn.className = "btn-small"; addBtn.textContent = "+ Pair";
  addBtn.onclick = () => { addRow("", ""); };
  wrap.appendChild(addBtn);
  return wrap;
}

function mpRawEditor(part) {
  const wrap = document.createElement("div");
  const hint = document.createElement("div");
  hint.className = "hint";
  hint.textContent = "No typed editor for Content-Type " + part.contentType + "; editing as raw text.";
  const ta = document.createElement("textarea");
  ta.rows = 6;
  ta.value = part.editor.value || "";
  ta.oninput = () => { part.editor.value = ta.value; };
  wrap.appendChild(hint);
  wrap.appendChild(ta);
  return wrap;
}

// Serialize mpState into the payload buildExportRequest expects for
// body.mode = "multipart". The background.js handler encodes the envelope
// with a fresh boundary.
function mpCollectBody() {
  return {
    mode: "multipart",
    parts: mpState.parts.map(p => ({
      contentType: p.contentType,
      method: p.method,
      path: p.path,
      extraHeaders: p.extraHeaders || {},
      contentId: p.contentId,
      body: p.editor.value,
      kind: p.editor.kind,
    })),
  };
}

function mpClear() {
  mpState = { boundary: null, parts: [] };
  const container = document.getElementById("mp-parts-container");
  if (container) container.innerHTML = "";
  const info = document.getElementById("mp-boundary-info");
  if (info) info.textContent = "";
}

// File-scope delegates for the multipart part card. Each delegate is
// registered once at module load — outside any function declaration —
// so the recursion lint correctly does not see them as call edges
// from inside _mpBuildPartCard's body. The handlers run in fresh
// tasks (separate JS stack from the original render).
document.addEventListener("click", (e) => {
  const btn = e.target && e.target.closest && e.target.closest(".mp-part-del");
  if (!btn) return;
  const idx = parseInt(btn.dataset.mpPartIdx, 10);
  if (Number.isNaN(idx)) return;
  mpState.parts.splice(idx, 1);
  mpRenderAll();
});

document.addEventListener("input", (e) => {
  const input = e.target;
  if (!input || !input.classList || !input.classList.contains("mp-part-ct-input")) return;
  const idx = parseInt(input.dataset.mpPartIdx, 10);
  if (Number.isNaN(idx)) return;
  const p = mpState.parts[idx];
  if (!p) return;
  p.contentType = input.value;
  p.editor.kind = mpClassifyPartBody(p.contentType, p.editor.value);
  const card = input.closest(".mp-part-card");
  if (card) card.replaceWith(mpRenderPart(idx));
});
