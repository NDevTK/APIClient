// Popup controller: renders captured data, security findings, and sends requests.

/** Extract a flat deduplicated method list from a discovery doc. */
function getDocMethods(doc) {
  if (!doc || !doc.resources) return [];
  const seen = new Set();
  const methods = [];
  function walk(res) {
    for (const rName in res) {
      const r = res[rName];
      if (r.methods) {
        for (const mName in r.methods) {
          const m = r.methods[mName];
          const key = (m.httpMethod || "GET") + " " + m.id;
          if (seen.has(key)) continue;
          seen.add(key);
          methods.push(m);
        }
      }
      if (r.resources) walk(r.resources);
    }
  }
  // Walk probed first so learned wins on dedup (learned is added second, probed filtered by seen)
  if (doc.resources.probed) walk({ probed: doc.resources.probed });
  if (doc.resources.learned) walk({ learned: doc.resources.learned });
  const skip = new Set(["probed", "learned"]);
  const rest = {};
  for (const k in doc.resources) { if (!skip.has(k)) rest[k] = doc.resources[k]; }
  walk(rest);
  return methods;
}

let currentTabId = null;
let tabData = null;
let currentSchema = null;
let currentRequestUrl = "";
let currentRequestMethod = "POST";
let currentContentType = "application/json";
let currentBodyMode = "form"; // "form" | "raw" | "graphql" | "multipart" | "msgconsole"
// mpState: when currentBodyMode === "multipart", tracks one editor per part.
// Each part: { partNumber, method, path, headers{}, contentType, bodyEditor }
// where bodyEditor is { kind: "json" | "graphql" | "form-urlencoded" | "raw", value, meta? }.
// Reassembled into a multipart/mixed envelope on Send.
let mpState = { boundary: null, parts: [] };
// Tab ID for the currently-open Message Console channel. Initially null.
// Set when initMsgConsole() opens a captured WS/PM/MC entry — the channel
// lives in the TAB THAT CAPTURED IT, which may be different from the
// popup's own tab (`currentTabId`) when browsing cross-tab logs. All
// PM_GET_STATUS / MC_SEND_MSG / WS_SEND_MSG calls route through this tab.
let currentChannelTabId = null;
let currentChannelId = null;
let currentChannelType = null; // "WEBSOCKET" | "POSTMESSAGE" | "MSGCHANNEL"
let currentTargetOrigin = null; // For postMessage send
let currentChannelFrameId = null; // Frame where the channel lives
let logFilter = "active"; // "active" | "all" | tabId (number)
let logSearchQuery = ""; // text filter for request log
let allTabsData = null; // { tabId: { meta, requestLog } }
let lastSendResult = null; // Last rendered response for re-render after rename
let gqlState = { ops: [], batched: false, activeIdx: 0 }; // GraphQL operation state
let currentFrameId = 0; // Target frame for sending (0 = main frame)
let availableFrames = []; // Cached frame list for current tab
let currentKeyOverride = null; // null = auto, { key, source, disabled } for override

// Virtual scroll state for request log
const _vs = {
  entries: [],       // full sorted entry list
  estHeight: 85,     // estimated row height (px)
  buffer: 5,         // extra rows above/below viewport
  scrollHandler: null,
  lastStart: -1,     // last rendered start index (skip no-op re-renders)
  lastEnd: -1,       // last rendered end index
  rendering: false,  // prevent re-entrant renders
};

function setSendPanelVisible(visible) {
  var els = [
    document.getElementById("send-headers-section"),
    document.getElementById("send-body-section"),
    document.querySelector(".send-actions"),
  ];
  for (var i = 0; i < els.length; i++) {
    if (!els[i]) continue;
    if (visible) {
      els[i].classList.remove("hidden");
      els[i].style.display = "";
    } else {
      els[i].classList.add("hidden");
      els[i].style.display = "none";
    }
  }
}

function setBodyMode(mode) {
  currentBodyMode = mode;
  const isConsole = mode === "msgconsole";
  // Show the body/actions sections whenever a mode is set
  setSendPanelVisible(true);
  // Toggle each mode panel by BOTH class and inline display. The initial
  // HTML marks these panels with class="hidden" for the static default;
  // leaving the class in place while only flipping `style.display` is
  // inconsistent state that breaks class-based visibility checks. Keep
  // class and display in sync.
  const panels = [
    ["send-form-fields", mode === "form"],
    ["send-raw-body", mode === "raw"],
    ["send-graphql-fields", mode === "graphql"],
    ["send-multipart-fields", mode === "multipart"],
    ["send-ws-console", isConsole],
  ];
  for (const [id, active] of panels) {
    const el = document.getElementById(id);
    if (!el) continue;
    if (active) {
      el.classList.remove("hidden");
      el.style.display = "block";
    } else {
      el.classList.add("hidden");
      el.style.display = "none";
    }
  }
  document.getElementById("send-frame-row").style.display =
    isConsole ? "none" : "";
  document.getElementById("send-key-section").style.display =
    isConsole ? "none" : "";
  document.getElementById("send-headers-section").style.display =
    isConsole ? "none" : "";
  document.getElementById("btn-send").style.display =
    isConsole ? "none" : "";
  document.querySelector(".export-row").style.display =
    isConsole ? "none" : "";
}

// ─── GraphQL Operation UI ────────────────────────────────────────────────────

function gqlBuildOpPanel(idx, op) {
  const div = document.createElement("div");
  div.className = "gql-op-panel" + (idx === gqlState.activeIdx ? " active" : "");
  div.dataset.gqlIdx = idx;
  // Persisted-operation envelopes (Reddit `{operation}`, Apollo APQ
  // `{extensions.persistedQuery}`) don't ship the query text. Show a
  // dedicated "Persisted Operation" field so the user can see + edit it
  // without the Query textarea looking empty/broken.
  const persistedHint = op.operation
    ? `<div class="gql-persisted-hint">Persisted operation — query text is resolved server-side.</div>`
    : "";
  const operationFieldHtml = op.operation
    ? `<div class="gql-field-label">Persisted Operation</div>` +
      `<input type="text" class="gql-operation" value="${esc(op.operation || "")}">`
    : "";
  const varsRaw = op.variables ? (typeof op.variables === "string" ? op.variables : JSON.stringify(op.variables, null, 2)) : "";
  div.innerHTML =
    persistedHint +
    operationFieldHtml +
    `<div class="gql-field-label">Query</div>` +
    `<textarea class="gql-query" placeholder="query { user(id: 1) { name email } }" rows="4">${esc(op.query || "")}</textarea>` +
    `<div class="gql-field-label">Variables</div>` +
    // Tree container is the authoritative editor. The raw textarea below
    // stays in sync so callers (and existing gqlSaveCurrentOp path) can
    // still read it, and power users can flip it open to edit raw JSON.
    `<div class="gql-variables-tree" data-gql-idx="${idx}"></div>` +
    `<details class="gql-variables-raw"><summary>Raw JSON</summary>` +
    `<textarea class="gql-variables" placeholder='{"id": 1}' rows="3">${esc(varsRaw)}</textarea>` +
    `</details>` +
    `<div class="gql-field-label">Operation Name</div>` +
    `<input type="text" class="gql-opname" placeholder="(optional)" value="${esc(op.operationName || "")}">` +
    `<details class="gql-extensions-toggle"><summary>Extensions</summary>` +
    `<textarea class="gql-extensions" placeholder='{"persistedQuery": {...}}' rows="2">${esc(op.extensions ? (typeof op.extensions === "string" ? op.extensions : JSON.stringify(op.extensions, null, 2)) : "")}</textarea>` +
    `</details>`;

  // Populate the contextual tree editor. Uses the same form-field
  // infrastructure the regular body uses, so we get:
  //   - typed inputs (number / bool / string / nested object / array)
  //   - rename buttons per field (alias persisted via RENAME_FIELD)
  //   - AST-value chips and badge bar when applicable
  queueMicrotask(() => { gqlRenderVariablesTree(div, op); });
  return div;
}

// Translate a parsed variables object into a tree of form-fields mounted
// inside `.gql-variables-tree`. Applies persisted aliases from the
// service's discovery doc (schema namespace: `__gqlVars_<operation>`).
function gqlRenderVariablesTree(panelDiv, op) {
  // Two halves: (1) re-render content from op.variables, (2) attach
  // textarea/container sync listeners. The listener setup is one-shot
  // (guarded by dataset markers) so the rebuild closure calls only the
  // pure-render half — gqlRenderVariablesContent — instead of looping
  // back through this entry. Eliminates self-recursion per the lint.
  gqlRenderVariablesContent(panelDiv, op);

  const container = panelDiv.querySelector(".gql-variables-tree");
  if (!container) return;
  if (!container.dataset.gqlSyncAttached) {
    container.dataset.gqlSyncAttached = "1";
    container.addEventListener("input", () => { gqlSyncVariablesFromTree(panelDiv); });
    container.addEventListener("change", () => { gqlSyncVariablesFromTree(panelDiv); });
  }

  const textarea = panelDiv.querySelector(".gql-variables");
  if (textarea && !textarea.dataset.gqlSyncAttached) {
    textarea.dataset.gqlSyncAttached = "1";
    let timer = null;
    const rebuild = () => {
      try {
        const parsed = JSON.parse(textarea.value || "null");
        const opNow = gqlState.ops[gqlState.activeIdx];
        if (!opNow) return;
        opNow.variables = parsed;
        gqlRenderVariablesContent(panelDiv, opNow);
      } catch (_) { /* invalid JSON — leave tree as-is */ }
    };
    textarea.addEventListener("input", () => {
      if (timer) clearTimeout(timer);
      timer = setTimeout(rebuild, 400);
    });
  }
}

// Pure-render half of gqlRenderVariablesTree: clears the container and
// rebuilds the field-input tree from `op.variables`. No listener setup
// (that's the one-shot half in gqlRenderVariablesTree itself). Called
// for every re-render after the initial setup; called by the textarea
// rebuild closure so neither function calls itself.
function gqlRenderVariablesContent(panelDiv, op) {
  const container = panelDiv.querySelector(".gql-variables-tree");
  if (!container) return;
  container.innerHTML = "";

  let parsed = null;
  if (op.variables != null) {
    if (typeof op.variables === "string") {
      try { parsed = JSON.parse(op.variables); } catch (_) { parsed = null; }
    } else {
      parsed = op.variables;
    }
  }
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
    container.innerHTML = '<div class="hint">Variables are not a JSON object — edit via Raw JSON below.</div>';
    return;
  }

  const epSel = document.getElementById("send-ep-select");
  const selectedOpt = epSel?.options?.[epSel.selectedIndex];
  const svc = selectedOpt?.dataset?.svc || null;
  const opName = op.operationName || op.operation || "root";
  const schemaName = "__gqlVars_" + opName;
  let aliasProps = null;
  if (svc && tabData?.discoveryDocs?.[svc]?.doc?.schemas?.[schemaName]?.properties) {
    aliasProps = tabData.discoveryDocs[svc].doc.schemas[schemaName].properties;
  }

  // Iterative aliasing walk over a fieldDef tree. Worklist replaces the
  // previous nested-recursive applyAliases so adversarially-nested
  // GraphQL variables can't blow the JS stack via this code path.
  function applyAliasesIterative(rootDef) {
    const queue = [rootDef];
    while (queue.length > 0) {
      const fd = queue.shift();
      fd.parentSchema = schemaName;
      if (aliasProps?.[fd.name]?.customName && aliasProps[fd.name].name) {
        fd.displayName = aliasProps[fd.name].name;
        fd.customName = true;
      }
      if (Array.isArray(fd.children)) {
        for (const c of fd.children) queue.push(c);
      }
    }
  }

  for (const [key, value] of Object.entries(parsed)) {
    const fieldDef = synthesizeFieldDefFromValue(key, value);
    applyAliasesIterative(fieldDef);
    container.appendChild(createFieldInput(key, fieldDef, "body", 0, value));
  }
}

// Walk the tree and produce a JSON object; write it to the sibling
// `.gql-variables` textarea so existing collection reads a current value.
// `encodeFormToJson` lives in background.js (SW scope); the popup has to
// translate the form-field tree locally.
function gqlFieldTreeToJson(rootFields) {
  // Iterative tree-to-object. Each work item populates a `target` (object
  // or array) from a fields list. Nested objects/arrays pre-allocate
  // sub-targets and queue their children for population. Replaces self-
  // recursion so deeply-nested GraphQL variable trees serialize without
  // growing the JS call stack.
  const root = {};
  const queue = [{ fields: rootFields || [], target: root, mode: "object" }];
  while (queue.length > 0) {
    const { fields, target, mode } = queue.shift();
    if (mode === "object") {
      for (const f of fields) {
        if (f.type === "object" || f.type === "message") {
          if (f.label === "repeated") {
            const list = [];
            target[f.name] = list;
            if (Array.isArray(f.value)) {
              for (const v of f.value) {
                if (v && typeof v === "object" && Array.isArray(v.children)) {
                  const sub = {};
                  list.push(sub);
                  queue.push({ fields: v.children, target: sub, mode: "object" });
                } else {
                  list.push(v);
                }
              }
            } else if (Array.isArray(f.children)) {
              for (const c of f.children) {
                if (Array.isArray(c.children)) {
                  const sub = {};
                  list.push(sub);
                  queue.push({ fields: c.children, target: sub, mode: "object" });
                } else {
                  list.push(c.value);
                }
              }
            }
            continue;
          }
          if (Array.isArray(f.children) && f.children.length) {
            const sub = {};
            target[f.name] = sub;
            queue.push({ fields: f.children, target: sub, mode: "object" });
          } else if (f.value && typeof f.value === "object") {
            target[f.name] = f.value;
          } else {
            target[f.name] = {};
          }
          continue;
        }
        if (f.label === "repeated" && Array.isArray(f.value)) { target[f.name] = f.value.slice(); continue; }
        if (f.value == null && !f.children?.length) continue;
        if (f.type === "bool" || f.type === "boolean") { target[f.name] = f.value === true || f.value === "true"; continue; }
        if (f.type === "number" || f.type === "int32" || f.type === "int64" || f.type === "uint32" || f.type === "uint64" ||
            f.type === "double" || f.type === "float" || f.type === "sint32" || f.type === "sint64") {
          target[f.name] = typeof f.value === "number" ? f.value : Number(f.value);
          continue;
        }
        target[f.name] = f.value;
      }
    }
  }
  return root;
}

function gqlSyncVariablesFromTree(panelDiv) {
  const container = panelDiv.querySelector(".gql-variables-tree");
  const textarea = panelDiv.querySelector(".gql-variables");
  if (!container || !textarea) return;
  const fields = [];
  for (const w of container.querySelectorAll(":scope > .form-field")) {
    const r = collectSingleField(w);
    if (r) fields.push(r);
  }
  const obj = gqlFieldTreeToJson(fields);
  try { textarea.value = JSON.stringify(obj, null, 2); }
  catch (e) {
    // gqlFieldTreeToJson should produce JSON-safe output, but a cycle or
    // BigInt would throw. Surface so a malformed form tree is visible
    // rather than the textarea silently staying stale.
    console.warn("[popup:gql] JSON.stringify failed for form tree:", e && e.message || e);
  }
}

function gqlRenderTabs() {
  const tabsEl = document.getElementById("gql-op-tabs");
  const showTabs = gqlState.ops.length > 1 || gqlState.batched;
  tabsEl.classList.toggle("hidden", !showTabs);
  tabsEl.innerHTML = "";
  for (let i = 0; i < gqlState.ops.length; i++) {
    const tab = document.createElement("span");
    tab.className = "gql-op-tab" + (i === gqlState.activeIdx ? " active" : "");
    const label = gqlState.ops[i].operationName || `Op ${i + 1}`;
    tab.innerHTML = esc(label);
    if (gqlState.ops.length > 1) {
      tab.innerHTML += `<span class="gql-tab-close" data-gql-close="${i}">\u00d7</span>`;
    }
    tab.dataset.gqlTab = i;
    tab.addEventListener("click", (e) => {
      if (e.target.dataset.gqlClose !== undefined) return;
      gqlSaveCurrentOp();
      gqlState.activeIdx = i;
      gqlActivateTab(i);
    });
    tabsEl.appendChild(tab);
  }
  // Close-button click handling moved to a single file-scope delegate
  // (registered at module load, see end of this section). The previous
  // per-button handler called gqlRenderAll directly, creating a static
  // back-edge gqlRenderTabs → gqlRenderAll. With delegation, the
  // handler lives outside the renderer's body — the static call
  // graph is one-way: gqlRenderAll → gqlRenderTabs.
}

function gqlActivateTab(idx) {
  const container = document.getElementById("gql-op-container");
  container.querySelectorAll(".gql-op-panel").forEach((p, i) => {
    p.classList.toggle("active", i === idx);
  });
  document.getElementById("gql-op-tabs").querySelectorAll(".gql-op-tab").forEach((t, i) => {
    t.classList.toggle("active", i === idx);
  });
}

function gqlSaveCurrentOp() {
  const container = document.getElementById("gql-op-container");
  const panel = container.querySelector(`.gql-op-panel[data-gql-idx="${gqlState.activeIdx}"]`);
  if (!panel) return;
  const op = gqlState.ops[gqlState.activeIdx];
  if (!op) return;
  op.query = panel.querySelector(".gql-query").value;
  const varsText = panel.querySelector(".gql-variables").value;
  op.variables = varsText || null;
  op.operationName = panel.querySelector(".gql-opname").value || null;
  const opInput = panel.querySelector(".gql-operation");
  if (opInput) op.operation = opInput.value || null;
  const extText = panel.querySelector(".gql-extensions").value;
  try { op.extensions = extText ? JSON.parse(extText) : null; } catch (_) { op.extensions = extText || null; }
}

function gqlCollectAllOps() {
  gqlSaveCurrentOp();
  return gqlState.ops.map((op) => ({
    query: op.query || "",
    variables: op.variables || null,
    operationName: op.operationName || null,
    operation: op.operation || null,
    extensions: op.extensions || null,
    // Arbitrary top-level keys captured by _parseGqlOp (csrf_token,
    // clientId, rid, etc.). Passed through untouched so rebuild emits the
    // original envelope shape.
    extra: op.extra || null,
  }));
}

function gqlRenderAll() {
  const container = document.getElementById("gql-op-container");
  container.innerHTML = "";
  for (let i = 0; i < gqlState.ops.length; i++) {
    container.appendChild(gqlBuildOpPanel(i, gqlState.ops[i]));
  }
  gqlRenderTabs();
  // Update tab labels when operation name changes
  container.querySelectorAll(".gql-opname").forEach((input) => {
    input.addEventListener("input", () => {
      gqlSaveCurrentOp();
      gqlRenderTabs();
    });
  });
}

// File-scope delegate for the gql tab close button. The delegate is
// registered once at module load — outside any function declaration —
// so the recursion lint correctly does not see this as a call edge
// from inside gqlRenderTabs's body. Each close click runs in a fresh
// task; there is no JS stack growth between the click and the
// re-render.
document.addEventListener("click", (e) => {
  const btn = e.target && e.target.closest && e.target.closest(".gql-tab-close");
  if (!btn) return;
  e.stopPropagation();
  const closeIdx = parseInt(btn.dataset.gqlClose, 10);
  if (Number.isNaN(closeIdx)) return;
  gqlSaveCurrentOp();
  gqlState.ops.splice(closeIdx, 1);
  if (gqlState.activeIdx >= gqlState.ops.length) gqlState.activeIdx = gqlState.ops.length - 1;
  if (gqlState.activeIdx < 0) gqlState.activeIdx = 0;
  gqlRenderAll();
});

function gqlLoadOperations(ops, batched) {
  gqlState.ops = ops.map((o) => ({ ...o }));
  gqlState.batched = batched;
  gqlState.activeIdx = 0;
  gqlRenderAll();
}

function gqlClear() {
  gqlState.ops = [{ query: "", variables: null, operationName: null, extensions: null }];
  gqlState.batched = false;
  gqlState.activeIdx = 0;
  gqlRenderAll();
}

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

// ─── Init ────────────────────────────────────────────────────────────────────

let renderTimer = null;
function throttledLoadState() {
  if (renderTimer) return;
  renderTimer = setTimeout(async () => {
    renderTimer = null;
    await loadState();
  }, 100);
}

// Change-detection fingerprints — skip DOM rebuild when data hasn't changed
let _lastKeysFp = "";
let _lastSecFp = "";
let _lastLogFp = "";
let _lastSendFp = "";

document.addEventListener("DOMContentLoaded", async () => {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  currentTabId = tab?.id ?? null;

  for (const btn of document.querySelectorAll(".tab")) {
    btn.addEventListener("click", () => {
      document.querySelector(".tab.active").classList.remove("active");
      document.querySelector(".panel.active").classList.remove("active");
      btn.classList.add("active");
      document
        .getElementById(`panel-${btn.dataset.panel}`)
        .classList.add("active");
    });
  }

  document.getElementById("btn-clear").addEventListener("click", clearState);

  // ⚙ Settings panel — toggles visibility; values flow to the offscreen brain
  // (IDB-persisted) and propagate to the worker via SET_ANALYSIS_OPTS.
  const settingsPanel = document.getElementById("panel-settings");
  document.getElementById("btn-settings").addEventListener("click", () => {
    const showing = settingsPanel.style.display !== "none";
    settingsPanel.style.display = showing ? "none" : "block";
  });
  const yieldInput = document.getElementById("opt-yield-throttle-ms");
  const yieldOut = document.getElementById("opt-yield-throttle-ms-val");
  const workersInput = document.getElementById("opt-max-workers");
  // Bound the worker count by this machine's logical cores — more workers than
  // cores adds only memory, not parallelism — and surface the count so the
  // control is informative, not a blind number box.
  const _cores = (navigator.hardwareConcurrency | 0) || 4;
  workersInput.max = String(_cores);
  const _whint = document.getElementById("opt-max-workers-hint");
  if (_whint) _whint.textContent += " This machine has " + _cores + " logical cores.";
  // Load current values from the brain (IDB-backed).
  try {
    const opts = await chrome.runtime.sendMessage({ type: "GET_ANALYSIS_OPTS" });
    if (opts && typeof opts === "object") {
      if (typeof opts.yieldThrottleMs === "number") {
        yieldInput.value = String(opts.yieldThrottleMs);
        yieldOut.value = String(opts.yieldThrottleMs);
      }
      if (typeof opts.maxWorkers === "number") {
        workersInput.value = String(opts.maxWorkers);
      }
    }
  } catch (e) {
    /* GET_ANALYSIS_OPTS failed — surface so a brain handler gap isn't hidden.
       The inputs stay at their HTML defaults so the panel is still usable. */
    console.warn("[popup] GET_ANALYSIS_OPTS failed:", e && e.message || e);
  }
  yieldInput.addEventListener("input", () => {
    yieldOut.value = yieldInput.value;
  });
  yieldInput.addEventListener("change", async () => {
    try {
      await chrome.runtime.sendMessage({
        type: "SET_ANALYSIS_OPTS",
        opts: { yieldThrottleMs: parseInt(yieldInput.value, 10) },
      });
    } catch (e) {
      console.warn("[popup] SET_ANALYSIS_OPTS yieldThrottleMs failed:", e && e.message || e);
    }
  });
  workersInput.addEventListener("change", async () => {
    let v = parseInt(workersInput.value, 10);
    if (!(v > 0)) return;
    // Clamp to [1, cores] — a typed value can exceed the input's max, and more
    // workers than cores only burns memory. Reflect the clamp back to the UI.
    v = Math.min(v, _cores);
    if (String(v) !== workersInput.value) workersInput.value = String(v);
    try {
      await chrome.runtime.sendMessage({
        type: "SET_ANALYSIS_OPTS",
        opts: { maxWorkers: v },
      });
    } catch (e) {
      console.warn("[popup] SET_ANALYSIS_OPTS maxWorkers failed:", e && e.message || e);
    }
  });

  // Data panel
  document
    .getElementById("btn-export-data")
    .addEventListener("click", () => copyToClipboard("data", tabData));

  // Send panel
  document
    .getElementById("send-ep-select")
    .addEventListener("change", onSendEndpointSelected);
  document.getElementById("btn-send").addEventListener("click", sendRequest);
  document.getElementById("ws-console-send").addEventListener("click", sendConsoleMessage);
  document.getElementById("ws-console-input").addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); sendConsoleMessage(); }
  });
  document
    .getElementById("btn-add-header")
    .addEventListener("click", addHeaderRow);
  document.getElementById("btn-gql-add-op").addEventListener("click", () => {
    gqlSaveCurrentOp();
    gqlState.ops.push({ query: "", variables: null, operationName: null, extensions: null });
    gqlState.batched = true;
    gqlState.activeIdx = gqlState.ops.length - 1;
    gqlRenderAll();
  });
  const btnAddPart = document.getElementById("btn-mp-add-part");
  if (btnAddPart) {
    btnAddPart.addEventListener("click", () => {
      const num = (mpState.parts[mpState.parts.length - 1]?.partNumber || 0) + 1;
      mpState.parts.push({
        partNumber: num,
        method: null,
        path: null,
        contentType: "application/json",
        extraHeaders: {},
        contentId: null,
        editor: { kind: "json", value: "{}", meta: null },
      });
      mpRenderAll();
    });
  }

  // Frame selector
  document.getElementById("send-frame-select").addEventListener("change", (e) => {
    currentFrameId = parseInt(e.target.value, 10) || 0;
  });

  // API key selector: delegate to radio buttons and custom input
  document.getElementById("send-key-options").addEventListener("change", onKeySelectionChange);
  document.getElementById("send-key-section").addEventListener("change", (e) => {
    if (e.target.name === "key-inject") onKeySelectionChange();
  });
  document.getElementById("send-key-options").addEventListener("input", (e) => {
    if (e.target.id === "send-key-custom") onKeySelectionChange();
  });

  // Service origin hint: "Open" link
  document.getElementById("send-service-origin-hint").addEventListener("click", (e) => {
    if (e.target.classList.contains("service-origin-open")) {
      var url = e.target.dataset.url;
      if (url) chrome.tabs.create({ url: url });
    }
  });

  // Export buttons
  document.getElementById("btn-copy-curl").addEventListener("click", () => copyAsFormat("curl"));
  document.getElementById("btn-copy-fetch").addEventListener("click", () => copyAsFormat("fetch"));
  document.getElementById("btn-copy-python").addEventListener("click", () => copyAsFormat("python"));

  // Service filter + spec export/import
  document.getElementById("spec-service-select").addEventListener("change", () => {
    renderMethodDropdown();
    _renderServiceGrouping();
  });
  document.getElementById("btn-export-spec").addEventListener("click", exportOpenApiSpec);
  document.getElementById("btn-import-spec").addEventListener("click", () => {
    document.getElementById("import-spec-file").click();
  });
  document.getElementById("import-spec-file").addEventListener("change", importOpenApiSpec);

  // Request log tab filter
  document.getElementById("log-tab-filter").addEventListener("change", async (e) => {
    const val = e.target.value;
    logFilter = val === "active" ? "active" : val === "all" ? "all" : parseInt(val, 10);
    _lastLogFp = "";
    await loadRequestLog();
    renderResponsePanel();
  });
  populateTabFilter();

  // Request log search filter
  document.getElementById("log-search").addEventListener("input", (e) => {
    logSearchQuery = e.target.value.toLowerCase().trim();
    _lastLogFp = "";
    renderResponsePanel();
  });

  // HAR export
  document.getElementById("btn-export-har").addEventListener("click", exportHar);

  // Global rename handler
  document.addEventListener("click", async (e) => {
    if (e.target.classList.contains("btn-rename")) {
      const { schema, key } = e.target.dataset;
      const currentName = e.target.previousElementSibling.textContent;
      const newName = prompt(`Rename "${currentName}" to:`, currentName);
      if (newName && newName !== currentName) {
        const select = document.getElementById("send-ep-select");
        const svc = select.dataset.svc;
        const methodId = select.dataset.discoveryId; // This is the ID of the selected method/endpoint
        const url = currentRequestUrl;

        // Rename targeting the GraphQL-variables schema keeps the wire key
        // intact — alias is display-only. The regular form path below
        // rebuilds the whole body form, which would blow away the current
        // tree state; for GQL we just re-render the active op panel.
        const isGqlVarsRename = typeof schema === "string" && schema.startsWith("__gqlVars_");

        // Snapshot GQL op state BEFORE the await below — STATE_UPDATED
        // listeners, render() calls, or content script lifecycles can
        // mutate gqlState mid-await and we'd otherwise re-render from
        // empty defaults.
        const savedGqlOp = isGqlVarsRename && gqlState.ops[gqlState.activeIdx]
          ? JSON.parse(JSON.stringify(gqlState.ops[gqlState.activeIdx]))
          : null;

        const currentData = isGqlVarsRename
          ? null
          : formValuesToInitialData(collectFormValues());

        await chrome.runtime.sendMessage({
          type: "RENAME_FIELD",
          tabId: currentTabId,
          service: svc,
          methodId,
          schemaName: schema,
          fieldKey: key,
          newName,
          url,
        });

        // Refresh tabData so re-renders pick up the new schema
        tabData = await chrome.runtime.sendMessage({
          type: "GET_STATE",
          tabId: currentTabId,
        });

        if (isGqlVarsRename) {
          // Restore the snapshot if something clobbered gqlState during
          // the await, then rebuild the full op panel so rename surfaces
          // everywhere (Query, Persisted Operation, Variables tree).
          if (savedGqlOp) {
            if (gqlState.ops.length === 0) {
              gqlState.ops = [savedGqlOp];
              gqlState.activeIdx = 0;
            } else {
              gqlState.ops[gqlState.activeIdx] = savedGqlOp;
            }
          }
          gqlRenderAll();
        } else {
          // Reload form to reflect change, preserving current body mode
          const savedBodyMode = currentBodyMode;
          await loadVirtualSchema(svc, select.dataset.discoveryId, currentData);
          if (currentBodyMode !== savedBodyMode) setBodyMode(savedBodyMode);
          // Re-render response tree so renamed field is immediately visible
          if (lastSendResult) {
            delete lastSendResult.discovery;
            renderResponse(lastSendResult);
          }
        }
      }
    }
  });

  // ─── Export Functions ──────────────────────────────────────────────────────

  async function buildCurrentRequest() {
    const bodyMode = currentBodyMode;
    let url = currentRequestUrl;
    if (!url) return null;

    const httpMethod = currentRequestMethod;
    const contentType = currentContentType;
    const epKey = document.getElementById("send-ep-select").value;
    const sel = document.getElementById("send-ep-select");
    const selectedOpt = sel.options[sel.selectedIndex];

    const headers = {};
    for (const row of document.querySelectorAll(
      "#send-headers-list .header-row",
    )) {
      const key = row.querySelector(".header-key").value.trim();
      const val = row.querySelector(".header-val").value.trim();
      if (key) headers[key] = val;
    }

    // Local helper: apply form-field params to the URL's query string. canParse
    // guard so an unparseable Send-tab URL stays as-is rather than catching a
    // throw — the URL field's validity is checked upstream when the user types,
    // so reaching here unparseable means a real bug we want surfaced.
    function _applyFormParamsToUrl(rawUrl, params) {
      if (!URL.canParse(rawUrl)) {
        console.warn("[popup:send] URL.canParse(%s) failed — form params not applied", rawUrl);
        return rawUrl;
      }
      const urlObj = new URL(rawUrl);
      for (const [k, v] of Object.entries(params)) urlObj.searchParams.set(k, String(v));
      return urlObj.toString();
    }
    let body;
    if (httpMethod === "GET" || httpMethod === "DELETE") {
      // Collect URL params from form fields even for GET/DELETE
      if (bodyMode === "form") {
        const formValues = collectFormValues();
        if (Object.keys(formValues.params).length > 0) {
          url = _applyFormParamsToUrl(url, formValues.params);
        }
      }
      body = null;
    } else if (bodyMode === "form") {
      const formValues = collectFormValues();
      if (Object.keys(formValues.params).length > 0) {
        url = _applyFormParamsToUrl(url, formValues.params);
      }
      body = {
        mode: "form",
        formData: { fields: formValues.fields },
      };
    } else if (bodyMode === "graphql") {
      body = {
        mode: "graphql",
        operations: gqlCollectAllOps(),
        batched: gqlState.batched,
      };
    } else if (bodyMode === "multipart") {
      body = mpCollectBody();
    } else {
      body = {
        mode: "raw",
        rawBody: document.getElementById("send-raw-body").value,
      };
    }

    try {
      return await chrome.runtime.sendMessage({
        type: "BUILD_REQUEST",
        tabId: currentTabId,
        endpointKey: epKey,
        service: selectedOpt?.dataset?.svc,
        methodId: selectedOpt?.dataset?.discoveryId,
        url,
        httpMethod,
        contentType,
        headers,
        body,
        apiKeyOverride: currentKeyOverride,
      });
    } catch (_) {
      return null;
    }
  }

  function formatCurl(req) {
    const sq = (s) => s.replace(/'/g, "'\\''");
    const parts = [`curl -X '${sq(req.method)}'`];
    for (const [k, v] of Object.entries(req.headers || {})) {
      parts.push(`  -H '${sq(k)}: ${sq(v)}'`);
    }
    if (req.body) {
      const ct = (req.headers || {})["Content-Type"] || "";
      if (ct.includes("protobuf") || ct.includes("grpc")) {
        // Binary body is base64-encoded — pipe through base64 decode
        parts.push(`  --data-binary @- <<< $(echo '${sq(req.body)}' | base64 -d)`);
      } else {
        parts.push(`  -d '${sq(req.body)}'`);
      }
    }
    parts.push(`  '${sq(req.url)}'`);
    return parts.join(" \\\n");
  }

  function formatFetch(req) {
    const opts = { method: req.method };
    if (Object.keys(req.headers || {}).length) opts.headers = req.headers;
    if (req.body) opts.body = req.body;
    return `fetch(${JSON.stringify(req.url)}, ${JSON.stringify(opts, null, 2)});`;
  }

  function formatPython(req) {
    const lines = ["import requests", ""];
    const kwargs = [];
    const ct = (req.headers || {})["Content-Type"] || "";
    const isBinaryBody = ct.includes("protobuf") || ct.includes("grpc");
    const isJson = ct.includes("application/json");
    // For JSON content types, use json= with parsed object.
    // For binary (protobuf/gRPC-Web), body is base64 — decode it.
    // Otherwise use data= with raw string.
    if (isBinaryBody && req.body) {
      lines[0] = "import requests\nimport base64";
      kwargs.push(`    data=base64.b64decode(${JSON.stringify(req.body)})`);
    } else if (isJson && req.body) {
      try {
        const parsed = JSON.parse(req.body);
        kwargs.push(`    json=${JSON.stringify(parsed)}`);
      } catch (_) {
        kwargs.push(`    data=${JSON.stringify(req.body)}`);
      }
    } else if (req.body) {
      kwargs.push(`    data=${JSON.stringify(req.body)}`);
    }
    const headers = { ...(req.headers || {}) };
    // json= sets Content-Type automatically — remove to avoid conflict
    if (isJson && req.body) delete headers["Content-Type"];
    if (Object.keys(headers).length) {
      kwargs.push(`    headers=${JSON.stringify(headers)}`);
    }
    const fn = req.method === "GET" ? "get" : req.method === "POST" ? "post" : req.method === "PUT" ? "put" : req.method === "DELETE" ? "delete" : "request";
    if (fn === "request") {
      kwargs.unshift(`    ${JSON.stringify(req.method)}`);
    }
    const url = `${JSON.stringify(req.url)}`;
    if (kwargs.length) {
      lines.push(`resp = requests.${fn}(`);
      lines.push(`    ${url},`);
      lines.push(kwargs.join(",\n") + ",");
      lines.push(")");
    } else {
      lines.push(`resp = requests.${fn}(${url})`);
    }
    lines.push("print(resp.status_code, resp.text)");
    return lines.join("\n");
  }

  async function copyAsFormat(format) {
    const btn = document.getElementById(`btn-copy-${format}`);
    const req = await buildCurrentRequest();
    if (!req || req.error) {
      btn.textContent = "No request";
      setTimeout(() => { btn.textContent = format === "python" ? "Python" : format; }, 1500);
      return;
    }

    let text;
    if (format === "curl") text = formatCurl(req);
    else if (format === "fetch") text = formatFetch(req);
    else text = formatPython(req);

    try {
      await navigator.clipboard.writeText(text);
      btn.textContent = "Copied!";
    } catch (_) {
      btn.textContent = "Failed";
    }
    setTimeout(() => { btn.textContent = format === "python" ? "Python" : format; }, 1500);
  }

  function exportHar() {
    // Collect the currently visible entries from the virtual scroll state
    const entries = _vs.entries;
    if (!entries || entries.length === 0) {
      alert("No requests to export.");
      return;
    }

    const harEntries = entries.map((r) => {
      const reqHeaders = [];
      if (r.requestHeaders) {
        for (const [k, v] of Object.entries(r.requestHeaders)) {
          reqHeaders.push({ name: k, value: v });
        }
      }

      const respHeaders = [];
      if (r.responseHeaders) {
        for (const [k, v] of Object.entries(r.responseHeaders)) {
          respHeaders.push({ name: k, value: v });
        }
      }

      const request = {
        method: r.method || "GET",
        url: r.url || "",
        httpVersion: "HTTP/1.1",
        cookies: [],
        headers: reqHeaders,
        queryString: [],
        headersSize: -1,
        bodySize: r.rawBodyB64 ? Math.ceil((r.rawBodyB64.length * 3) / 4) : 0,
      };

      // Parse query string from URL — canParse guard so HAR export doesn't
      // catch parse-validity throws. A malformed request.url means the request
      // log captured something unparseable; preserve the entry but skip the
      // queryString breakdown (HAR spec allows empty queryString array).
      if (URL.canParse(r.url)) {
        const u = new URL(r.url);
        u.searchParams.forEach((v, k) => {
          request.queryString.push({ name: k, value: v });
        });
      } else {
        console.debug("[popup:HAR] skipping queryString for unparseable url:", r.url);
      }

      // Request body
      if (r.rawBodyB64) {
        request.postData = {
          mimeType: r.contentType || "application/octet-stream",
          text: r.rawBodyB64,
          encoding: "base64",
        };
      }

      const response = {
        status: r.status || 0,
        statusText: r.statusText || (r.status === 200 ? "OK" : ""),
        httpVersion: "HTTP/1.1",
        cookies: [],
        headers: respHeaders,
        content: {
          size: r.responseBody ? r.responseBody.length : 0,
          mimeType: r.mimeType || r.contentType || "",
        },
        redirectURL: "",
        headersSize: -1,
        bodySize: r.responseBody ? r.responseBody.length : 0,
      };

      // Response body
      if (r.responseBody) {
        response.content.text = r.responseBody;
        if (r.responseBase64) {
          response.content.encoding = "base64";
        }
      }

      return {
        startedDateTime: new Date(r.timestamp || Date.now()).toISOString(),
        time: 0,
        request,
        response,
        cache: {},
        timings: { send: 0, wait: 0, receive: 0 },
      };
    });

    const har = {
      log: {
        version: "1.2",
        creator: { name: "API Security Researcher", version: "1.0" },
        entries: harEntries,
      },
    };

    // Download as file
    const blob = new Blob([JSON.stringify(har, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "api-security-researcher.har";
    a.click();
    URL.revokeObjectURL(url);
  }

  const EXTENSION_ORIGIN = `chrome-extension://${chrome.runtime.id}`;

  // Threat model: popup runs in the extension process (trusted), but broadcast
  // messages are received by all listeners including content scripts. sender.id
  // is spoofable (our content script runs in every renderer), so the real gate
  // is sender.url — set by the browser process, unforgeable by the renderer.
  // See SECURITY.md.
  chrome.runtime.onMessage.addListener((msg, sender) => {
    if (sender.id !== chrome.runtime.id) return;

    const isExtensionPage =
      sender.url && sender.url.startsWith(EXTENSION_ORIGIN + "/");
    if (!isExtensionPage) return;

    if (msg.type === "STATE_UPDATED") {
      // Endpoints, security findings and keys are GLOBAL (cross-tab
      // globalStore), so re-render on ANY tab's update — not just the active
      // tab's. The background deep unused-feature grind and cross-session
      // resumes learn endpoints (e.g. github's login-gated `preheat`) MINUTES
      // after the page loaded and may carry a different tabId than the open
      // popup's active tab; gating on `msg.tabId === currentTabId` hid those,
      // so they only showed on reopen. loadState re-fetches the global store
      // plus the active tab's per-tab request log, so a blanket refresh is
      // correct (and throttled).
      throttledLoadState();
      if (currentBodyMode === "msgconsole" && currentChannelId) {
        refreshMsgConsole();
      }
    }
  });
  loadState();
});

// ─── State ───────────────────────────────────────────────────────────────────

async function loadState() {
  tabData = await chrome.runtime.sendMessage({
    type: "GET_STATE",
    tabId: currentTabId,
  });
  // Fetch available frames for the current tab
  try {
    availableFrames = await chrome.runtime.sendMessage({
      type: "GET_FRAMES",
      tabId: currentTabId,
    }) || [];
  } catch (_) {
    availableFrames = [];
  }
  if (logFilter !== "active") {
    await loadRequestLog();
  }
  render();
}

async function clearState() {
  await chrome.runtime.sendMessage({ type: "CLEAR_TAB", tabId: currentTabId });
  tabData = null;
  allTabsData = null;
  _lastKeysFp = _lastSecFp = _lastLogFp = _lastSendFp = "";
  render();
}

async function loadRequestLog() {
  if (logFilter === "active") {
    allTabsData = null;
    return;
  }
  try {
    allTabsData = await chrome.runtime.sendMessage({
      type: "GET_ALL_LOGS",
      filter: logFilter,
    });
  } catch (_) {
    allTabsData = null;
  }
}

async function populateTabFilter() {
  const select = document.getElementById("log-tab-filter");
  if (!select) return;
  try {
    const tabList = await chrome.runtime.sendMessage({ type: "GET_TAB_LIST", tabId: currentTabId });
    // Remove old dynamic options (keep "Active Tab" and "All Tabs")
    while (select.options.length > 2) {
      select.remove(2);
    }
    if (tabList && tabList.length > 1) {
      const divider = document.createElement("option");
      divider.disabled = true;
      divider.textContent = "──────────";
      select.appendChild(divider);

      for (const t of tabList) {
        const opt = document.createElement("option");
        opt.value = String(t.tabId);
        let label = t.title
          ? (t.title.length > 30 ? t.title.slice(0, 30) + "\u2026" : t.title)
          : `Tab ${t.tabId}`;
        if (t.closed) label += " (closed)";
        opt.textContent = `${label} (${t.count})`;
        if (t.tabId === currentTabId) opt.textContent += " \u2605";
        select.appendChild(opt);
      }
    }
    // Restore selection
    if (logFilter !== "active" && logFilter !== "all") {
      select.value = String(logFilter);
    }
  } catch (e) {
    // GET_TAB_LIST RPC failure or DOM manipulation throw — the popup's tab
    // filter dropdown will be stale. Surface so the user-visible gap (only
    // "Active Tab" / "All Tabs" available, no per-tab choices) is diagnosable.
    console.warn("[popup:populateTabFilter] failed:", e && e.message || e);
  }
}

// ─── Render ──────────────────────────────────────────────────────────────────

function render() {
  renderDeepStatus();
  renderDataPanel();
  renderSecurityPanel();
  renderSendPanel();
  renderFrameSelector();
  renderResponsePanel();
  populateTabFilter();
}

// Background deep unused-feature grind progress. The grind drives lazy-chunk
// orphan functions (e.g. github's login-gated `preheat`) over MINUTES at a low
// CPU duty cycle; without this line the user can't tell whether it's running,
// done, or stuck — and can't tell a not-yet-reached endpoint from a missing one.
function renderDeepStatus() {
  const el = document.getElementById("deep-status");
  if (!el) return;
  const ds = tabData && tabData.deepStats;
  const all = (tabData && Array.isArray(tabData.allDeepStats)) ? tabData.allDeepStats : [];
  // Per-tab summary line — uses the SHARED LearnState classifier (lib/
  // learnstate.js), the same verdict the harness `learnstate` command and the
  // worker heartbeat use, so the UI never drifts from the harness on what
  // "complete" means. The worker's live heartbeat carries the authoritative
  // state (learningState) — prefer it; fall back to classifying the stored
  // per-tab _deepStats when no live heartbeat field is present (e.g. a
  // background tab's last snapshot). The "stalled" verdict is now surfaced
  // (was hidden as a perpetual "X% — learning…") so a prioritization stall is
  // visible to the user, not silently mistaken for slow progress.
  let head = "";
  if (ds && typeof ds.total === "number" && ds.total >= 0) {
    const verdict = (typeof ds.learningState === "string")
      ? { state: ds.learningState, total: ds.total, rem: Math.max(0, ds.rem | 0),
          driven: ds.total - Math.max(0, ds.rem | 0),
          pct: ds.total > 0 ? Math.floor(((ds.total - Math.max(0, ds.rem | 0)) / ds.total) * 100) : -1 }
      : self.LearnState.learningStateOf({ rem: ds.rem, total: ds.total,
          running: ds.running | 0, queued: ds.queued | 0,
          msSinceProgress: (typeof ds.msSinceProgress === "number") ? ds.msSinceProgress : -1 });
    head = self.LearnState.learningLabelOf(verdict);
    el.className = "deep-status " + (verdict.state === "complete" ? "done" : verdict.state === "stalled" ? "stalled" : "active");
  }
  // Cross-tab background work — honest visibility into the rotation (which
  // OTHER tabs have incomplete grinds, whether any paused for a higher-priority
  // review). Display-only; the scheduler isn't user-controllable by design.
  let crossHtml = "";
  if (all.length > 1 || (all.length === 1 && (!ds || all[0].tabId !== ds.tabId))) {
    const rows = all.map((s) => {
      const total = s.total || 0;
      // Same shared classifier as the head line + harness (DRY).
      const v = (typeof s.learningState === "string")
        ? { state: s.learningState, total: total, rem: Math.max(0, s.rem | 0),
            driven: total - Math.max(0, s.rem | 0),
            pct: total > 0 ? Math.floor(((total - Math.max(0, s.rem | 0)) / total) * 100) : -1 }
        : self.LearnState.learningStateOf({ rem: s.rem, total: total, running: s.running | 0, queued: s.queued | 0,
            msSinceProgress: (typeof s.msSinceProgress === "number") ? s.msSinceProgress : -1 });
      let label;
      if (total === 0) label = "queued";
      else if (v.state === "complete") label = "complete";
      else if (v.state === "stalled") label = "STALLED (scheduling)";
      else if (s.stop && s.stop.indexOf("yielded") === 0) label = "paused (preempted by a live review)";
      else label = `${v.pct >= 0 ? v.pct : 0}% (${v.driven >= 0 ? v.driven : 0}/${total})`;
      const page = s.pageUrl ? new URL(s.pageUrl).hostname + new URL(s.pageUrl).pathname.replace(/\/$/, "") : ("tab " + s.tabId);
      return `<div class="deep-row" title="${esc(s.pageUrl || "")}"><span class="deep-page">${esc(page.slice(0, 60))}</span> — <span class="deep-label">${esc(label)}</span></div>`;
    }).join("");
    crossHtml = `<details class="deep-cross-tab"><summary>Background work across tabs (${all.length})</summary>${rows}</details>`;
  }
  // Analyzer gaps (resolverErrors): host calls the forced-exec REACHED but
  // couldn't resolve (fully-opaque URL/method), or host-model gaps (a bundle
  // throw under the host model — a Web API not modelled correctly). Per
  // CLAUDE.md these are P1 to close and MUST be visible, not console-only:
  // they tell the reviewer "there's a fetch here the engine saw but couldn't
  // pin down" — distinguishing a reached-but-unresolved endpoint from one the
  // engine never reached at all.
  let resolverHtml = "";
  const rerrs = (tabData && Array.isArray(tabData.resolverErrors)) ? tabData.resolverErrors : [];
  if (rerrs.length) {
    const rows = rerrs.map((r) =>
      `<div class="deep-row" title="${esc(r.message || "")}"><span class="deep-label">${esc((r.context || "gap") + ": " + (r.message || "").slice(0, 140))}</span></div>`
    ).join("");
    resolverHtml = `<details class="deep-cross-tab"><summary>Analyzer gaps — reached-but-unresolved / host-model (${rerrs.length})</summary>${rows}</details>`;
  }
  // Driving-completeness frontier: of the @T functions the deep grind
  // directed-drove, how many fired NO host call — split threw (an opaque op
  // raised before the fetch) vs returned (the host call's branch arm wasn't
  // forced, or it's an event-gated handler registered-not-dispatched, or the
  // opaque broke a deep call chain to a nested fetch). This is the precise
  // residue the unused-feature surface is still hiding behind; surfacing the
  // shape per CLAUDE.md's no-silent-zero rule turns "explored N functions" into
  // "explored N, M of them reached no endpoint — and here's why".
  let dnfHtml = "";
  if (ds && ((ds.dnfThrew | 0) + (ds.dnfRet | 0)) > 0) {
    const threw = ds.dnfThrew | 0, ret = ds.dnfRet | 0;
    dnfHtml = `<div class="deep-row deep-dnf" title="Driven unused functions that fired no fetch/XHR/WebSocket. threw: an opaque op raised before the host call. returned: the host-call branch wasn't forced, or the handler is event-gated / behind a deep opaque-broken call chain.">${threw + ret} driven functions reached no endpoint (${threw} threw, ${ret} returned) — driving-completeness frontier</div>`;
  }
  if (!head && !crossHtml && !resolverHtml && !dnfHtml) { el.style.display = "none"; return; }
  el.innerHTML = (head ? `<div class="deep-head">${esc(head)}</div>` : "") + dnfHtml + crossHtml + resolverHtml;
  el.style.display = "block";
}

// ─── Data Panel ──────────────────────────────────────────────────────────────

function renderDataPanel() {
  const keysContainer = document.getElementById("data-keys");
  const empty = document.getElementById("data-empty");

  const keys = tabData?.apiKeys ? Object.entries(tabData.apiKeys) : [];
  const fp = keys.length + ":" + keys.map(k => k[0]).join(",");
  if (fp === _lastKeysFp) return;
  _lastKeysFp = fp;

  keysContainer.innerHTML = "";
  const hasData = keys.length > 0;
  empty.style.display = hasData ? "none" : "block";

  // Keys section
  if (keys.length) {
    let html = '<div class="section-header">Discovered API Keys</div>';
    for (const [key, info] of keys) {
      const services = info.services || [];
      const hosts = info.hosts || [];
      const eps = info.endpoints || [];
      const reqCount = info.requestCount || eps.length || 0;

      html += `<div class="card">
        <div class="card-label">${esc(info.name || "API Key")} ${info.source === "page_source" ? '<span class="badge badge-source">page source</span>' : '<span class="badge badge-source">network</span>'}
          ${reqCount > 0 ? `<span class="badge badge-status">${reqCount} req</span>` : ""}
        </div>
        <div class="card-value">${esc(key)}</div>
        <div class="card-meta">
          ${hosts.length ? `${hosts.length === 1 ? "Host" : "Hosts"}: ${hosts.map((h) => `<strong>${esc(h)}</strong>`).join(", ")}` : ""}
        </div>`;

      if (services.length) {
        html += `<div class="card-meta">${services.length === 1 ? "Service" : "Services"}: ${[...services].map((s) => `<code>${esc(s)}</code>`).join(" ")}</div>`;
      }
      const pageUrls = info.pageUrls || [];
      if (pageUrls.length) {
        html += `<div class="card-meta">${pageUrls.length === 1 ? "Page" : "Pages"}: ${[...pageUrls].map((u) => {
          if (/^https?:\/\//i.test(u)) return `<a href="${esc(u)}" target="_blank" title="${esc(u)}">${esc(_shortUrl(u))}</a>`;
          return `<span title="${esc(u)}">${esc(_shortUrl(u))}</span>`;
        }).join(", ")}</div>`;
      }
      html += `</div>`;
    }
    keysContainer.innerHTML = html;
  }
}

// ─── Security Panel ──────────────────────────────────────────────────────────

// Popup-scoped map of findingKey → sessionId for probes kicked off this
// session. Keeps the UI glued to the SW-side _probeSessions Map so that
// after the first click we only show status — no duplicate probes.
const _probeInFlight = new Map();

// Probes whose result has been rendered at least once, used to avoid
// flashing the "Running…" state when the user reopens a card that was
// already verified. Keyed by findingKey → { hits, status, strategy, at }.
const _probeResults = new Map();

// Embedded PoC sandboxes (poc-sandbox.html), keyed by a per-iframe pocId. The
// sandbox is a real cross-origin attacker context that evals the EXACT PoC JS
// on the user's click; we postMessage the PoC to it once it signals POC_READY,
// and watch for POC_RAN to know the user fired it.
const _pocSandboxes = new Map();
let _pocIdSeq = 0;

// One listener for messages FROM the embedded PoC sandbox iframes.
window.addEventListener("message", (e) => {
  const d = e.data;
  if (!d || typeof d !== "object") return;
  if (d.type !== "POC_READY" && d.type !== "POC_RAN") return;
  // Match the sender window to the iframe we created (its contentWindow), then
  // look up the PoC we staged for it. We never trust message content for the
  // PoC body — only for the ready/ran signal.
  for (const [pocId, ent] of _pocSandboxes) {
    const ifr = document.querySelector('iframe[data-poc-id="' + pocId + '"]');
    if (!ifr || ifr.contentWindow !== e.source) continue;
    if (d.type === "POC_READY") {
      try { e.source.postMessage({ type: "POC_SETUP", pocJs: ent.pocJs, marker: ent.marker }, "*"); } catch (_) {}
    } else if (d.type === "POC_RAN") {
      if (ent.statusEl) {
        ent.statusEl.textContent = d.error
          ? "PoC threw in the sandbox: " + d.error
          : "PoC executed — waiting for the sink to fire…";
      }
      // The user fired it; poll the session's hits for the verdict.
      if (ent.resultEl && !_probeInFlight.has(ent.key)) {
        _probeInFlight.set(ent.key, ent.marker);
        _pollProbe(ent.resultEl, ent.key, ent.marker);
      }
    }
    return;
  }
});

// Stable key for a finding INSIDE THE POPUP — independent of the
// harness's SHA-1-based id. We just need something unique within a tab
// so delegated handlers can route clicks back to the right finding.
function _findingKey(entry) {
  const loc = entry.item && entry.item.location;
  const kind = entry.kind === "sink"
    ? (entry.item.type || "?") + ":" + (entry.item.sink || "?")
    : (entry.item.type || "?");
  return (entry.sourceUrl || "(inline)") + "|L" + (loc ? loc.line : "?") + ":C" + (loc ? loc.column : "?") + "|" + entry.kind + "|" + kind;
}

// Render one forced path's data-flow hops (source → ops → sink) as an ordered
// list. Each hop is a psi term step the engine computed.
function _renderDataFlowHops(hops) {
  if (!Array.isArray(hops) || !hops.length) return "";
  let rows = "";
  for (let i = 0; i < hops.length; i++) {
    const h = hops[i];
    const at = h.at ? ("L" + h.at.line + ":C" + h.at.column) : "";
    var snippetHtml = h.code ? '<div class="hop-snippet"><code>' + esc(String(h.code).slice(0, 200)) + '</code></div>' : '';
    var kindHtml = h.kind ? '<code>[' + esc(h.kind) + ']</code> ' : '';
    rows += '<li>' + kindHtml + esc(h.desc || "")
      + (at ? ' <span class="hop-at">' + esc(at) + '</span>' : '') + snippetHtml + '</li>';
  }
  return '<ol class="taint-hops">' + rows + '</ol>';
}
// Render the conditionals (Φ) the forced run took to reach the sink. Each is a
// gate + the branch direction; this is what lets a reviewer judge a candidate
// FP (a gate pins the value) from a live path. The condition expression and
// branch are exactly what QuickJS forced.
function _renderConditions(conds) {
  if (!Array.isArray(conds) || !conds.length) return '<div class="taint-conds-empty">no path conditions — the source reaches the sink unconditionally</div>';
  let rows = "";
  for (let i = 0; i < conds.length; i++) {
    const c = conds[i];
    const took = c.decision ? "true" : "false";
    rows += '<li><span class="cond-branch cond-' + took + '">' + took + '</span> <code>' + esc(c.expr || "?") + '</code></li>';
  }
  return '<ul class="taint-conds">' + rows + '</ul>';
}
// Render the interprocedural call chain — which FUNCTIONS the taint passed
// through on the way to the sink (innermost first, as QuickJS's stack reports).
function _renderCallChain(chain) {
  if (!Array.isArray(chain) || !chain.length) return "";
  let rows = "";
  for (let i = 0; i < chain.length; i++) {
    const f = chain[i];
    const nm = f.name ? esc(f.name) : "(anonymous)";
    const loc = (f.line != null) ? ("L" + f.line + ":C" + (f.column != null ? f.column : f.col)) : "";
    rows += '<li><code>' + nm + '</code> <span class="hop-at">' + esc(loc) + '</span></li>';
  }
  return '<ol class="taint-callchain">' + rows + '</ol>';
}
function _renderTaintPathDetails(item) {
  // Structured forced-execution trace. Built from the engine's psi (data flow),
  // Φ (conditionals), the @S call chain (interprocedural), and the Z3 verdict
  // classification — so a reviewer can tell a PoC from a pinned candidate-FP
  // from a PoC-generation failure, and see EVERY distinct path to the sink.
  const dataFlow = Array.isArray(item.taintPath) ? item.taintPath : [];
  const conds = Array.isArray(item.conditions) ? item.conditions : [];
  const chain = Array.isArray(item.callChain) ? item.callChain : [];
  const paths = Array.isArray(item.paths) ? item.paths : [];
  if (!dataFlow.length && !conds.length && !chain.length && !paths.length) return "";

  const vr = item.verdictReason || null;
  const reasonClass = vr ? ("vr-" + esc(vr.reason)) : "";
  const reasonHtml = vr
    ? '<div class="verdict-reason ' + reasonClass + '" title="What the Z3 static verdict actually means — the Verify probe is the only dynamic confirmation.">' + esc(vr.text) + '</div>'
    : "";

  // Primary path: data flow + conditionals + interprocedural chain.
  let primary = "";
  if (item.source) primary += '<div class="taint-section-h">source</div><div class="taint-source"><code>' + esc(String(item.source)) + '</code></div>';
  if (dataFlow.length) primary += '<div class="taint-section-h">data flow (' + dataFlow.length + ' hop' + (dataFlow.length === 1 ? "" : "s") + ')</div>' + _renderDataFlowHops(dataFlow);
  primary += '<div class="taint-section-h">path conditions (' + conds.length + ')</div>' + _renderConditions(conds);
  if (chain.length) primary += '<div class="taint-section-h">interprocedural call chain (' + chain.length + ' frame' + (chain.length === 1 ? "" : "s") + ')</div>' + _renderCallChain(chain);

  // Alternate forced paths to the SAME sink. Each is a distinct gating path the
  // forced multi-path execution found — represents the multiple interprocedural
  // routes a single linear chain can't show.
  let altHtml = "";
  if (paths.length > 1) {
    let alts = "";
    for (let pi = 0; pi < paths.length; pi++) {
      const p = paths[pi];
      const pvr = p.verdictReason ? '<span class="badge poc-' + esc(String(p.verdict || "").toLowerCase()) + '">' + esc(({ REAL_EXPLOIT: "PoC", TAINT_REACH: "taint", Z3_ERROR: "Z3 error" })[p.verdict] || p.verdict || "") + '</span> ' : "";
      const pconds = Array.isArray(p.conditions) ? p.conditions : [];
      const pchain = Array.isArray(p.callChain) ? p.callChain : [];
      alts += '<li class="alt-path">' + pvr + 'path ' + (pi + 1)
        + '<div class="taint-section-h">conditions (' + pconds.length + ')</div>' + _renderConditions(pconds)
        + (pchain.length ? '<div class="taint-section-h">call chain (' + pchain.length + ')</div>' + _renderCallChain(pchain) : "")
        + '</li>';
    }
    altHtml = '<details class="taint-details alt-paths"><summary>' + paths.length + ' distinct forced paths to this sink</summary><ul class="alt-path-list">' + alts + '</ul></details>';
  }

  const summary = vr ? ("taint trace — " + esc(vr.reason)) : "taint trace (forced execution)";
  return reasonHtml
    + '<details class="taint-details"><summary>' + summary + '</summary>'
    + '<div class="taint-body">' + primary + '</div></details>'
    + altHtml;
}

// Reachability gates the AST extracted between source and sink
// (`if (x === 'exec')`, `switch (x) { case 'add': … }`, early-return
// guards). Each entry tells the reviewer "the probe must pin field X
// to literal Y for this sink to run" — both so the reviewer can
// understand why the finding is real AND so they can read the same
// payload the probe is about to deliver.
function _renderPreconditionsDetails(item) {
  if (!Array.isArray(item.preconditions) || !item.preconditions.length) return "";
  let rows = "";
  for (const p of item.preconditions) {
    const pathLabel = Array.isArray(p.path) && p.path.length
      ? p.path.map(s => JSON.stringify(s)).join(".")
      : "<root>";
    rows += '<li><code>' + esc(pathLabel) + '</code> <span class="hop-at">'
      + esc(String(p.op || "===")) + '</span> <code>' + esc(JSON.stringify(p.value)) + '</code></li>';
  }
  return '<details class="taint-details"><summary>preconditions (' + item.preconditions.length
    + ') — sink reachable only when these fields equal the pinned values</summary>'
    + '<ul class="taint-hops">' + rows + '</ul></details>';
}

// Per-sink sanitizer audit: which sanitizer-shaped calls the
// classifier saw in the enclosing function and whether each matched
// on-path. A reviewer uses this to judge whether the sanitized/not-
// sanitized decision was correct without tracing the minified
// source by hand.
function _renderSanitizerDetails(item) {
  const sr = item.sanitizerReport;
  if (!sr || !Array.isArray(sr.candidates) || !sr.candidates.length) return "";
  let rows = "";
  for (const c of sr.candidates) {
    const at = c.loc ? ("L" + c.loc.line + ":C" + c.loc.column) : "?";
    const verdict = c.matched ? (c.onPath ? "matched, on-path" : "matched, branch-only") : "rejected";
    rows += '<li><code>' + esc(c.label || "?") + '</code> <span class="hop-at">' + esc(at) + '</span> '
      + '[<span class="san-verdict san-' + (c.matched ? (c.onPath ? 'match' : 'branch') : 'reject') + '">' + esc(verdict) + '</span>]'
      + '<div class="san-reason">' + esc(c.matchReason || "") + '</div></li>';
  }
  return '<details class="taint-details"><summary>sanitizer report: ' + esc(sr.decision || "?") + '  ('
    + sr.candidates.length + ' candidate ' + (sr.candidates.length === 1 ? 'call' : 'calls') + ' in scope)</summary>'
    + '<ul class="taint-hops">' + rows + '</ul></details>';
}

// Render a Z3-produced multi-source PoC: shows the ordered attacker
// steps (URL setup, postMessage payloads, storage/cookie injections)
// and a "Run multi-step PoC" button that orchestrates the attack
// against the target tab via background._runStructuredPlan. The
// rendered steps are exactly what the orchestrator dispatches —
// reviewer sees the recipe before executing.
function _renderPocRow(entry, i, poc) {
  const key = _findingKey(entry);
  const stepBlocks = [];
  // URL setup row (hash/search/pathname) — applied at chrome.tabs.create
  if (poc.url && (poc.url.hash || poc.url.search || poc.url.pathname)) {
    const parts = [];
    if (poc.url.hash) parts.push('hash=' + esc(poc.url.hash));
    if (poc.url.search) parts.push('search=' + esc(poc.url.search));
    if (poc.url.pathname) parts.push('path=' + esc(poc.url.pathname));
    stepBlocks.push('<div class="poc-step poc-url">URL: ' + parts.join(' &nbsp; ') + '</div>');
  }
  // Pre-injection state (storage, cookies)
  if (Array.isArray(poc.storage) && poc.storage.length) {
    for (const it of poc.storage)
      stepBlocks.push('<div class="poc-step poc-pre">localStorage.setItem(<b>' + esc(it.key || '') + '</b>, ' + esc(JSON.stringify(it.value || '')) + ')</div>');
  }
  if (Array.isArray(poc.cookies) && poc.cookies.length) {
    for (const it of poc.cookies)
      stepBlocks.push('<div class="poc-step poc-pre">document.cookie = ' + esc(JSON.stringify(it.value || '')) + '</div>');
  }
  // Ordered events (postMessage sequence)
  for (let ei = 0; ei < poc.events.length; ei++) {
    const ev = poc.events[ei];
    const carried = ev.carriesPayload ? ' <span class="poc-marker-tag" title="this event carries the marker that intercept.js detects at the sink">[sink-bearing]</span>' : '';
    stepBlocks.push('<div class="poc-step poc-event">'
      + 'Step ' + (ei + 1) + ' &middot; <code>window.postMessage(</code>'
      + '<pre class="poc-payload">' + esc(JSON.stringify(ev.payload, null, 2)) + '</pre>'
      + '<code>, "*")</code>' + carried + '</div>');
  }
  const stepHtml = stepBlocks.length
    ? '<details class="poc-steps"><summary>PoC plan (' + stepBlocks.length + ' step' + (stepBlocks.length === 1 ? '' : 's') + ')</summary>' + stepBlocks.join('') + '</details>'
    : '<div class="poc-na">PoC has no actionable steps</div>';

  const resultHtml = '<div class="probe-result" data-finding-key="' + esc(key) + '"></div>';
  const probeData = {
    pocPlan: poc,
    sourceUrl: entry.sourceUrl || null,
    pageUrl: entry.pageUrl || null,
    sinkType: entry.item.type || null,
    sinkName: entry.item.sink || null,
  };
  const btnAttrs =
    ' data-finding-key="' + esc(key) + '"'
    + ' data-probe=\'' + esc(JSON.stringify(probeData)) + '\''
    + ' data-finding-idx="' + i + '"';
  // No verdict tag here — the Z3 verdict is a badge at the top of the box
  // (with type + severity). This row holds only the dynamic Run/probe result.
  return '<div class="probe-row poc-row">'
    + stepHtml
    + '<button class="probe-btn poc-run-btn"' + btnAttrs + '>Load PoC</button>'
    + '<span class="probe-hint">compiles the EXACT PoC JavaScript from the Z3 solve (no template) and embeds a cross-origin attacker sandbox — click <b>Run PoC</b> inside it (your click is the user gesture). The payload calls <code>apiclientsink(&lt;finding-id&gt;)</code>, which intercept.js relays only when the browser ACTUALLY executes it → REAL EXPLOIT. CSP blocking the handler → no call → NOT REPRODUCED.</span>'
    + resultHtml + '</div>';
}

function _renderVerifyRow(entry, i) {
  // Offer a PoC ONLY for a proven, solvable exploit. REAL_EXPLOIT means Z3
  // SOUNDLY solved a source-connected exploit; for anything else
  // (EXPLOIT_UNPROVEN / TAINT_REACH / INFEASIBLE / Z3_ERROR) we do NOT offer to
  // build a PoC — there is no soundly-solved exploit to run, so a "Load PoC"
  // button would be a lie. The verdict-reason banner explains the situation.
  if (entry.item && entry.item.verdict && entry.item.verdict !== "REAL_EXPLOIT") {
    return "";
  }
  // Only sinks carry a taint source the probe can exercise. Dangerous
  // patterns (prototype pollution, regex ReDoS, etc.) aren't probed by
  // URL/postMessage strategies — show a subdued row explaining why.
  if (entry.kind !== "sink") {
    return '<div class="probe-row"><span class="probe-na">pattern finding — not probeable via URL/postMessage</span></div>';
  }
  // The PoC is compiled ONLY from the Z3 solve (the @P plan: solved values +
  // channels + order). If a REAL_EXPLOIT carries that plan, offer it; otherwise
  // we offer NOTHING — there is no legacy/template strategy probe. (A sound
  // REAL_EXPLOIT always carries a @P plan, so this is the live path.)
  const poc = entry.item.poc;
  if (poc && Array.isArray(poc.events) && (poc.events.length || (poc.url && (poc.url.hash || poc.url.search || poc.url.pathname)) || (poc.storage && poc.storage.length) || (poc.cookies && poc.cookies.length))) {
    return _renderPocRow(entry, i, poc);
  }
  return "";
}

function _wireVerifyButtons(container) {
  container.querySelectorAll(".probe-btn").forEach((btn) => {
    btn.addEventListener("click", () => _handleVerifyClick(btn));
  });
}

function _resumeInflightProbes(container) {
  // If a previous render kicked off a probe that's still running, or
  // recently completed, restore its result without re-sending. Lets the
  // reviewer close/reopen panels without losing state.
  container.querySelectorAll(".probe-result").forEach((resultEl) => {
    const key = resultEl.dataset.findingKey;
    if (_probeResults.has(key)) {
      _renderProbeResult(resultEl, _probeResults.get(key));
    } else if (_probeInFlight.has(key)) {
      resultEl.textContent = "Running probe…";
      resultEl.className = "probe-result probe-running";
      _pollProbe(resultEl, key, _probeInFlight.get(key));
    }
  });
}

async function _handleVerifyClick(btn) {
  const key = btn.dataset.findingKey;
  let probeData = {};
  try { probeData = JSON.parse(btn.dataset.probe || "{}"); }
  catch (e) {
    // The probe payload was set by renderSecurityPanel; a JSON.parse throw
    // means an earlier serializer wrote malformed data into the data-attribute.
    // Surface so the broken probe definition is visible (probeData stays {}
    // and the exploit-verify dispatches with empty payload, which will fail
    // probe-side — having the parse diagnostic narrows where to look).
    console.warn("[popup:_handleVerifyClick] data-probe parse failed key=%s: %s", key, e && e.message || e);
  }
  const card = btn.closest(".card");
  const resultEl = card ? card.querySelector('.probe-result') : null;
  if (!resultEl) return;

  btn.disabled = true;
  const prevLabel = btn.textContent;
  btn.textContent = "Loading PoC…";
  resultEl.textContent = "Building PoC…";
  resultEl.className = "probe-result probe-running";

  try {
    // PREPARE the PoC: the brain builds the exact attacker JavaScript and
    // registers a session (marker) so a later sink-hit correlates back. It does
    // NOT open any tab — the USER runs the PoC by clicking inside the embedded
    // sandbox (that click is the user activation window.open needs).
    const start = await new Promise((resolve) => {
      chrome.runtime.sendMessage({
        type: "EXPLOIT_PROBE_START",
        strategy: probeData.strategy || null,
        pocPlan: probeData.pocPlan || null,
        paramName: probeData.paramName || null,
        fieldPath: probeData.fieldPath || [],
        sinkType: probeData.sinkType || null,
        sinkName: probeData.sinkName || null,
        decoders: Array.isArray(probeData.decoders) ? probeData.decoders : [],
        preconditions: Array.isArray(probeData.preconditions) ? probeData.preconditions : [],
        pageUrl: probeData.pageUrl || null,
        sourceUrl: probeData.sourceUrl || null,
        findingId: key,
        waitMs: 5000,
      }, (r) => resolve(r));
    });
    if (!start || !start.sessionId || !start.pocJs) {
      resultEl.textContent = "Could not build PoC: " + ((start && start.error) || "no response");
      resultEl.className = "probe-result probe-err";
      btn.disabled = false; btn.textContent = prevLabel;
      return;
    }
    _renderPocSandbox(card, resultEl, key, start.sessionId, start.pocJs);
    btn.textContent = "PoC ready ▾";
  } finally {
    if (btn.textContent === "Loading PoC…") btn.textContent = prevLabel;
  }
}

// Stage the unified PoC: show the EXACT JavaScript (copyable) and embed the
// sandboxed attacker page that will eval that same JS on the user's click. One
// artifact — displayed == executed. No separate run system.
function _renderPocSandbox(card, resultEl, key, marker, pocJs) {
  let host = card.querySelector(".poc-sandbox-host");
  if (!host) {
    host = document.createElement("div");
    host.className = "poc-sandbox-host";
    resultEl.parentNode.insertBefore(host, resultEl);
  }
  const pocId = "poc" + (++_pocIdSeq);
  host.innerHTML =
    '<div class="poc-js-head">PoC JavaScript — runs verbatim on a cross-origin attacker origin (copy &amp; reuse on e.g. https://example.com):</div>'
    + '<pre class="poc-js"><code></code></pre>'
    + '<button class="poc-copy-btn">Copy PoC</button>'
    + '<div class="poc-sandbox-head">Attacker sandbox — click <b>Run PoC</b> below (your click is the user gesture window.open needs):</div>'
    + '<iframe class="poc-sandbox-frame" data-poc-id="' + pocId + '" src="poc-sandbox.html"></iframe>';
  host.querySelector(".poc-js code").textContent = pocJs;
  const copyBtn = host.querySelector(".poc-copy-btn");
  copyBtn.addEventListener("click", () => {
    navigator.clipboard.writeText(pocJs).then(
      () => { copyBtn.textContent = "Copied ✓"; setTimeout(() => { copyBtn.textContent = "Copy PoC"; }, 1500); },
      () => { copyBtn.textContent = "Copy failed"; }
    );
  });
  resultEl.className = "probe-result";
  resultEl.innerHTML = '<span class="poc-status">PoC staged — click Run PoC in the sandbox above.</span>';
  _pocSandboxes.set(pocId, { pocJs, marker, key, resultEl, statusEl: resultEl.querySelector(".poc-status") });
}

async function _pollProbe(resultEl, findingKey, sessionId) {
  // The PoC is fired by the user's click in the sandbox; the verdict arrives as
  // a PROBE_HIT (apiclientsink relayed by marker). We poll the session's hits.
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    const s = await new Promise((resolve) => {
      chrome.runtime.sendMessage({ type: "EXPLOIT_PROBE_STATUS", sessionId }, (r) => resolve(r));
    });
    if (!s || s.error) {
      resultEl.textContent = "Error: " + ((s && s.error) || "session lost");
      resultEl.className = "probe-result probe-err";
      _probeInFlight.delete(findingKey);
      return;
    }
    const hits = s.hits || [];
    if (hits.length || s.executed) {
      const snapshot = { hits, executed: s.executed || null, status: "done", error: s.error || null, strategy: s.strategy };
      _probeResults.set(findingKey, snapshot);
      _probeInFlight.delete(findingKey);
      _renderProbeResult(resultEl, snapshot);
      return;
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  // No hit yet — the user may not have clicked Run, the payload may be blocked,
  // or the gate values may not match. Leave the sandbox staged for a retry.
  resultEl.className = "probe-result probe-miss";
  resultEl.innerHTML = '<span class="poc-status">NOT REPRODUCED yet — apiclientsink never fired. Click Run PoC again, or the target may sanitize / gate the value.</span>';
  _probeInFlight.delete(findingKey);
}

function _renderProbeResult(resultEl, snapshot) {
  if (snapshot.status === "error" || snapshot.error) {
    resultEl.className = "probe-result probe-err";
    resultEl.textContent = "Probe failed: " + (snapshot.error || "unknown");
    return;
  }
  const hits = snapshot.hits || [];
  const executed = snapshot.executed || null;

  // Verdict from the EXACT-PoC run: when the PoC the user fired in the sandbox
  // reaches a sink, intercept.js's apiclientsink relays a PROBE_HIT (by marker).
  // Any hit (or a read exec flag) ⇒ REAL EXPLOIT — the browser actually executed
  // the attacker payload. No hit ⇒ NOT REPRODUCED (the dynamic ground truth that
  // the static "PoC" verdict is UNTESTED against).
  const firedOrigins = executed ? Object.keys(executed).filter(k => executed[k]) : [];
  const fireCount = hits.length || firedOrigins.length;
  if (!fireCount) {
    resultEl.className = "probe-result probe-miss";
    resultEl.innerHTML = '<div class="probe-miss-head">NOT REPRODUCED — apiclientsink never fired. The payload may be blocked by CSP, the gate values may not match, or the target didn\'t parse/execute it.</div>';
    return;
  }

  resultEl.className = "probe-result probe-hit";
  // The payload injects multiple vectors (e.g. <img onerror> + <svg onload>) so
  // a sanitizer that strips one can't hide the finding; fireCount is how many of
  // those vectors actually executed — all carry the same finding id, so any
  // count ≥ 1 is the same confirmed exploit, not N separate bugs.
  let html = '<div class="probe-hit-head">REAL EXPLOIT — payload executed'
    + (fireCount > 1 ? ' (' + fireCount + ' of the injected vectors fired)' : '')
    + '</div><ul class="probe-hits">';
  for (const origin of firedOrigins) {
    html += '<li><code>' + esc(origin) + '</code></li>';
  }
  html += '</ul>';
  if (hits.length) {
    html += '<div class="probe-hit-subhead">apiclientsink invocations (' + hits.length + '):</div><ul class="probe-hits probe-hits-dim">';
    for (const h of hits.slice(0, 6)) {
      const url = h.url ? ' <span class="probe-hit-url">' + esc(String(h.url).slice(0, 80)) + '</span>' : '';
      html += '<li><code>' + esc(h.sink) + '</code>' + url + '</li>';
    }
    if (hits.length > 6) html += '<li>… +' + (hits.length - 6) + ' more</li>';
    html += '</ul>';
  }
  // Show the probe "recipe" — what the SW actually sent. Lets a
  // reviewer audit "is the precondition shape right? did we pre-
  // encode the decoder chain correctly?" without digging into logs.
  const recipe = snapshot.recipe;
  if (recipe) {
    const parts = [];
    if (recipe.sinkType) parts.push('sinkType=' + recipe.sinkType);
    if (recipe.paramName) parts.push('param=' + JSON.stringify(recipe.paramName));
    if (Array.isArray(recipe.fieldPath) && recipe.fieldPath.length) {
      parts.push('fieldPath=' + recipe.fieldPath.map(s => JSON.stringify(s)).join("."));
    }
    if (Array.isArray(recipe.decoders) && recipe.decoders.length) {
      parts.push('decoders=' + recipe.decoders.join("→"));
    }
    if (Array.isArray(recipe.preconditions) && recipe.preconditions.length) {
      const gates = recipe.preconditions.map(p => {
        const k = Array.isArray(p.path) && p.path.length ? p.path.map(s => JSON.stringify(s)).join(".") : "<root>";
        return k + (p.op || "===") + JSON.stringify(p.value);
      }).join(", ");
      parts.push('gates={' + gates + '}');
    }
    if (parts.length) {
      html += '<details class="probe-recipe"><summary>probe recipe — what was actually sent</summary>'
        + '<pre>' + esc(parts.join("\n")) + '</pre></details>';
    }
  }
  resultEl.innerHTML = html;
}

function renderSecurityPanel() {
  const container = document.getElementById("security-findings");
  const empty = document.getElementById("security-empty");

  const findings = tabData?.securityFindings || [];
  // Fingerprint: count of sinks + patterns across all findings
  let secCount = 0;
  for (let i = 0; i < findings.length; i++) {
    secCount += (findings[i].securitySinks || []).length + (findings[i].dangerousPatterns || []).length;
  }
  const fp = findings.length + ":" + secCount;
  if (fp === _lastSecFp) return;
  _lastSecFp = fp;

  container.innerHTML = "";

  // Flatten all sinks and patterns with their source URL
  var allItems = [];
  for (var fi = 0; fi < findings.length; fi++) {
    var f = findings[fi];
    var srcLabel = f.sourceUrl ? _shortUrl(f.sourceUrl) : "(unknown)";
    for (var si = 0; si < (f.securitySinks || []).length; si++) {
      var s = f.securitySinks[si];
      allItems.push({ kind: "sink", item: s, sourceUrl: f.sourceUrl, srcLabel: srcLabel, pageUrl: f.pageUrl });
    }
    for (var di = 0; di < (f.dangerousPatterns || []).length; di++) {
      var d = f.dangerousPatterns[di];
      allItems.push({ kind: "pattern", item: d, sourceUrl: f.sourceUrl, srcLabel: srcLabel, pageUrl: f.pageUrl });
    }
  }

  if (!allItems.length) {
    empty.style.display = "block";
    return;
  }
  empty.style.display = "none";

  // Sort: high first, then medium, then low
  var sevOrder = { high: 0, medium: 1, low: 2 };
  allItems.sort(function(a, b) {
    return (sevOrder[a.item.severity] || 2) - (sevOrder[b.item.severity] || 2);
  });

  var html = '<div class="section-header">Vulnerabilities <span class="badge badge-status">' + allItems.length + '</span></div>';

  for (var i = 0; i < allItems.length; i++) {
    var entry = allItems[i];
    var item = entry.item;
    var sev = item.severity || "low";
    var sevBadge = '<span class="badge badge-' + esc(sev) + '">' + esc(sev.toUpperCase()) + '</span>';
    var loc = item.location ? "L" + item.location.line + ":" + item.location.column : "";


    var srcLink = entry.sourceUrl && /^https?:\/\//i.test(entry.sourceUrl)
      ? '<a href="' + esc(entry.sourceUrl) + '" target="_blank" title="' + esc(entry.sourceUrl) + '">' + esc(entry.srcLabel) + '</a>'
      : esc(entry.srcLabel);
    if (entry.pageUrl && entry.pageUrl !== entry.sourceUrl) {
      srcLink += ' <span class="page-context" title="' + esc(entry.pageUrl) + '">in ' + esc(_shortUrl(entry.pageUrl)) + '</span>';
    }

    if (entry.kind === "sink") {
      var typeBadge = "";
      if (item.type === "xss") typeBadge = '<span class="badge badge-xss">XSS</span>';
      else if (item.type === "eval") typeBadge = '<span class="badge badge-eval">EVAL</span>';
      else if (item.type === "redirect") typeBadge = '<span class="badge badge-redirect">REDIRECT</span>';
      else typeBadge = '<span class="badge badge-danger">' + esc(item.type.toUpperCase()) + '</span>';

      var sourceDesc = item.sourceType === "user-controlled"
        ? "user-controlled" + (item.source ? ": " + esc(item.source) : "")
        : item.sourceType === "dynamic" ? "dynamic value" : "literal value";

      var sinkDimsHtml = Array.isArray(item.sinkDims)
        ? '<div class="card-dims" title="attacker-controlled dims surviving to the sink">sinkDims: {' + esc(item.sinkDims.join(",") || "none") + '}</div>'
        : "";

      // The ONLY verdict badge is "PoC" — and ONLY for REAL_EXPLOIT, where Z3
      // SOUNDLY solved a source-connected exploit (a candidate PoC, untested
      // until Verify fires it). Every other verdict — EXPLOIT_UNPROVEN (SAT only
      // under an unsound over-approx), TAINT_REACH, INFEASIBLE, Z3_ERROR — gets
      // NO badge: the verdict-reason banner explains it in words and no PoC is
      // offered. A badge for those would over-claim solvability.
      var vBadge = item.verdict === "REAL_EXPLOIT"
        ? '<span class="badge poc-real_exploit" title="Z3 soundly solved a source-connected exploit (candidate PoC) — UNTESTED until Verify fires it.">PoC</span> '
        : '';
      var verifyHtmlSink = _renderVerifyRow(entry, i);
      html += '<div class="card" data-finding-key="' + esc(_findingKey(entry)) + '">'
        + '<div class="card-label">' + typeBadge + ' ' + sevBadge + ' ' + vBadge + esc(item.sink) + '</div>'
        + '<div class="card-value">' + esc(sourceDesc) + '</div>'
        + sinkDimsHtml
        + _renderTaintPathDetails(item)
        + _renderPreconditionsDetails(item)
        + _renderSanitizerDetails(item)
        + '<div class="card-meta">' + srcLink + (loc ? " " + esc(loc) : "") + '</div>'
        + verifyHtmlSink
        + '</div>';
    } else {
      var patBadge = '<span class="badge badge-danger">' + esc((item.type || "pattern").toUpperCase().replace(/-/g, " ")) + '</span>';
      var patSinkDimsHtml = Array.isArray(item.sinkDims)
        ? '<div class="card-dims" title="attacker-controlled dims surviving to the sink">sinkDims: {' + esc(item.sinkDims.join(",") || "none") + '}</div>'
        : "";
      var verifyHtmlPat = _renderVerifyRow(entry, i);
      html += '<div class="card" data-finding-key="' + esc(_findingKey(entry)) + '">'
        + '<div class="card-label">' + patBadge + ' ' + sevBadge + '</div>'
        + '<div class="card-value">' + esc(item.description || item.type) + '</div>'
        + patSinkDimsHtml
        + _renderTaintPathDetails(item)
        + '<div class="card-meta">' + srcLink + (loc ? " " + esc(loc) : "") + '</div>'
        + verifyHtmlPat
        + '</div>';
    }
  }

  container.innerHTML = html;
  _wireVerifyButtons(container);
  _resumeInflightProbes(container);
  // The source viewer (+ Prism + Babel) was removed: the taint path now shows
  // the per-hop code inline (each hop's source line, sliced at analysis time),
  // and `harness finding <id>` gives full context — so the standalone viewer's
  // syntax-highlighted source browse is no longer needed. Code snippets render
  // as plain <code>; no Prism.highlightElement, no viewer.html tab.
}

// ─── Send Panel ──────────────────────────────────────────────────────────────

function renderSendPanel() {
  // Fingerprint: skip rebuild if discovery docs haven't changed. Must include
  // PER-DOC method count — when the AST analysis learns new methods on an
  // existing service (e.g. github bundle drops more @H records on the same
  // `github.com` service), docKeys.length stays the same but the dropdown
  // is stale. Without the method-count sum, `renderMethodDropdown()` only
  // fires on a new SERVICE, not new METHODS, so users see "-- select method --"
  // after a fresh nav until manually re-rendering.
  const docKeys = tabData?.discoveryDocs ? Object.keys(tabData.discoveryDocs) : [];
  let methodSum = 0;
  if (tabData?.discoveryDocs) {
    for (const k of docKeys) {
      const svc = tabData.discoveryDocs[k];
      if (svc && svc.doc) methodSum += getDocMethods(svc.doc).length;
    }
  }
  const sendFp = docKeys.length + "/" + methodSum + ":" + (tabData?.requestLog?.length || 0);
  if (sendFp === _lastSendFp) return;
  _lastSendFp = sendFp;

  // Populate service selector
  const svcSelect = document.getElementById("spec-service-select");
  const prevSvc = svcSelect.value;
  svcSelect.innerHTML = '<option value="">All Services</option>';
  if (tabData?.discoveryDocs) {
    for (const [svcName, svcData] of Object.entries(tabData.discoveryDocs).sort((a, b) => a[0].localeCompare(b[0]))) {
      if (svcData.status === "found" && svcData.doc) {
        const methodCount = getDocMethods(svcData.doc).length;
        const opt = document.createElement("option");
        opt.value = svcName;
        opt.textContent = `${svcName} (${methodCount})`;
        svcSelect.appendChild(opt);
      }
    }
  }
  if (prevSvc) svcSelect.value = prevSvc;
  _renderServiceGrouping();

  renderMethodDropdown();
}

// Show which URL-structure rule produced the currently-selected
// service name. Service grouping is heuristic (URL-parsing — no
// server-side fact tells us "this is service X"), so every decision
// must be traceable to the rule that produced it; a reviewer reads
// this row and judges whether the grouping is right for the site.
function _renderServiceGrouping() {
  const el = document.getElementById("spec-service-grouping");
  if (!el) return;
  const svcSelect = document.getElementById("spec-service-select");
  const name = svcSelect.value;
  if (!name || !tabData?.discoveryDocs) { el.textContent = ""; el.className = "service-grouping"; return; }
  const svcData = tabData.discoveryDocs[name];
  if (!svcData) { el.textContent = ""; el.className = "service-grouping"; return; }
  el.className = "service-grouping";

  // Grouping rule line.
  let html = "";
  const g = svcData.grouping;
  if (g) {
    html += '<span class="grouping-label">grouping rule:</span> <code>' + esc(g.rule) + '</code>'
      + (g.matched ? ' <span class="grouping-matched">matched: <code>' + esc(g.matched) + '</code></span>' : '')
      + (g.firstUrl ? '<div class="grouping-first">first request: <code>' + esc(g.firstUrl) + '</code></div>' : '');
  }

  // Bucket quality: count methods by discovery source. AST-discovered
  // methods are the reviewer's primary target — listed first so the
  // reviewer sees the code-analysis yield at a glance.
  if (svcData.doc) {
    let astCount = 0, astLiveCount = 0, liveOnlyCount = 0, assetCount = 0, total = 0;
    for (const bucket of Object.values(svcData.doc.resources || {})) {
      for (const m of Object.values(bucket.methods || {})) {
        total++;
        if (m._responseKind === "asset") { assetCount++; continue; }
        const hasAst = !!m._astInferred;
        const hasLive = !!(m._stats && m._stats.requestCount);
        if (hasAst && hasLive) astLiveCount++;
        else if (hasAst) astCount++;
        else if (hasLive) liveOnlyCount++;
      }
    }
    if (total > 0) {
      const parts = [];
      if (astCount) parts.push(astCount + " AST");
      if (astLiveCount) parts.push(astLiveCount + " AST+live");
      if (liveOnlyCount) parts.push(liveOnlyCount + " live");
      if (assetCount) parts.push(assetCount + " asset");
      html += '<div class="grouping-first">methods: ' + esc(parts.join(", ")) + '  (' + total + ' total)</div>';
    }
  }

  el.innerHTML = html;
}

function renderMethodDropdown() {
  const svcFilter = document.getElementById("spec-service-select").value;
  const select = document.getElementById("send-ep-select");
  const prev = select.value;

  select.innerHTML = '<option value="">-- select method --</option>';

  if (tabData?.discoveryDocs) {
    const services = Object.entries(tabData.discoveryDocs).sort((a, b) =>
      a[0].localeCompare(b[0]),
    );

    for (const [svcName, svcData] of services) {
      if (svcFilter && svcName !== svcFilter) continue;
      if (svcData.status === "found" && svcData.doc) {
        const methods = getDocMethods(svcData.doc);
        methods.sort((a, b) => a.id.localeCompare(b.id));

        if (methods.length > 0) {
          const group = document.createElement("optgroup");
          group.label = svcData.doc.title || svcName;

          for (const m of methods) {
            const opt = document.createElement("option");
            const key = `DISCOVERY ${m.httpMethod} ${svcName} ${m.id}`;
            opt.value = key;
            // Review tags reflect what the reviewer cares about:
            //   [ast]        — discovered via source-code AST, reviewer's
            //                  primary target (endpoint that exists in the
            //                  bundle, may or may not have been exercised).
            //   [ast+live]   — AST-discovered AND real traffic observed.
            //   [live]       — only observed in traffic (no AST origin).
            //   [asset:X]    — response magic-bytes classified as static
            //                  (deprioritise — usually noise).
            let tag = "";
            if (m._responseKind === "asset") {
              tag = " [asset" + (m._responseLabel ? ":" + String(m._responseLabel).split(";")[0].trim() : "") + "]";
            } else {
              const hasAst = !!m._astInferred;
              const hasLive = !!(m._stats && m._stats.requestCount);
              if (hasAst && hasLive) tag = " [ast+live]";
              else if (hasAst) tag = " [ast]";
              else if (hasLive) tag = " [live]";
            }
            // Substitute source-map-resolved param names into the displayed id
            // (e.g. `github.com.{e}_{a}_issues_preheat_index` →
            // `github.com.{owner}_{repo}_issues_preheat_index`) so the reviewer
            // can read the endpoint without having to open the param panel to
            // decode minified single-letter holes. The underlying option value
            // and URL-substitution key stay minified to match the engine's
            // `${e}` hole — only the human-visible textContent is rewritten.
            //
            // Parse the id with a regex-by-hole-name so we only rewrite at
            // declared hole positions, not arbitrary `{e}` substrings — a
            // string-level replace would also rewrite if a longer path
            // template happened to contain `{e}` in another context. The path
            // param is matched as a whole bracketed token bound by either
            // start/end of string or the `.` / `_` separators that the method
            // id uses for `/` (so `{eee}` doesn't get its inner `{e}` touched).
            let displayId = m.id;
            if (m.parameters) {
              for (const pName in m.parameters) {
                const p = m.parameters[pName];
                if (p && p._sourceMapName && p.location === "path" && pName !== p._sourceMapName) {
                  const esc = pName.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
                  const re = new RegExp("(^|[._/])\\{" + esc + "\\}(?=$|[._/])", "g");
                  displayId = displayId.replace(re, "$1{" + p._sourceMapName + "}");
                }
              }
            }
            opt.textContent = `[${m.httpMethod}] ${displayId}${tag}`;
            opt.dataset.method = m.httpMethod;
            opt.dataset.isVirtual = "true";
            opt.dataset.svc = svcName;
            opt.dataset.path = m.path;
            opt.dataset.discoveryId = m.id;
            group.appendChild(opt);
          }
          select.appendChild(group);
        }
      }
    }
  }

  if (prev) select.value = prev;
}

// ─── Frame Selector ──────────────────────────────────────────────────────────

function renderFrameSelector() {
  const row = document.getElementById("send-frame-row");
  const sel = document.getElementById("send-frame-select");
  if (!row || !sel) return;

  if (availableFrames.length <= 1) {
    row.classList.add("hidden");
    currentFrameId = 0;
    renderServiceOriginHint();
    return;
  }

  row.classList.remove("hidden");
  var prevVal = sel.value;
  sel.innerHTML = "";
  for (var i = 0; i < availableFrames.length; i++) {
    var f = availableFrames[i];
    var opt = document.createElement("option");
    opt.value = f.frameId;
    var label = f.isTop ? "Top frame" : "iframe (" + f.frameId + ")";
    try {
      var u = new URL(f.url);
      label += " \u2014 " + u.hostname;
    } catch (_) {
      if (f.origin) label += " \u2014 " + f.origin;
    }
    opt.textContent = label;
    sel.appendChild(opt);
  }
  if (prevVal) sel.value = prevVal;
  currentFrameId = parseInt(sel.value, 10) || 0;
  renderServiceOriginHint();
}

function renderServiceOriginHint() {
  var hint = document.getElementById("send-service-origin-hint");
  if (!hint) return;
  hint.classList.add("hidden");
  hint.innerHTML = "";

  // Get selected service
  var epSelect = document.getElementById("send-ep-select");
  var selectedOpt = epSelect?.options?.[epSelect.selectedIndex];
  var svc = selectedOpt?.dataset?.svc;
  if (!svc || !tabData?.discoveryDocs?.[svc]) return;

  var svcData = tabData.discoveryDocs[svc];
  var pageUrls = svcData.pageUrls || [];
  if (pageUrls.length === 0) return;

  // Check if any known page URL matches the current tab. URL parse failures
  // here are EXPECTED — `tabUrl` may be a chrome:// page or empty, and stored
  // `pageUrls` from older brain versions can be malformed; the fallback is to
  // treat them as non-matching, which is the correct hint behavior. The catches
  // stay bare here precisely because surfacing them would spam on every popup
  // open. The OUTER availableFrames read is the only path where a throw means
  // a real popup state bug (availableFrames should always be an array).
  var tabUrl = "";
  try {
    var tabs = availableFrames;
    if (tabs.length > 0) tabUrl = tabs[0].url;
  } catch (e) {
    console.warn("[popup:renderServiceOriginHint] availableFrames read threw:", e && e.message || e);
  }
  // Use URL.canParse (WHATWG, standard since Chrome 120) so we don't catch
  // throws as a parse-validity test — empty/chrome:// inputs short-circuit
  // explicitly instead of going through exception flow. Root-cause fix for
  // what would otherwise be two silent catches: an unparseable input is
  // KNOWN AT THE GUARD, not discovered via throw.
  var tabOrigin = (tabUrl && URL.canParse(tabUrl)) ? new URL(tabUrl).origin : "";
  var matchesCurrentTab = false;
  for (var i = 0; i < pageUrls.length; i++) {
    if (pageUrls[i] && URL.canParse(pageUrls[i]) && new URL(pageUrls[i]).origin === tabOrigin) {
      matchesCurrentTab = true; break;
    }
  }

  if (matchesCurrentTab) return;

  // Show hint with the most recent page URL — same canParse root-cause fix.
  // Fallback to the raw lastUrl when it isn't a parseable URL (legacy stored
  // entries can be just a hostname) is the correct display semantic.
  var lastUrl = pageUrls[pageUrls.length - 1];
  var lastHostname = (lastUrl && URL.canParse(lastUrl)) ? new URL(lastUrl).hostname : (lastUrl || "");

  var frameNote = "";
  var frameOrigins = svcData.frameOrigins || [];
  if (frameOrigins.length > 0) {
    frameNote = " (iframe: " + esc(frameOrigins[frameOrigins.length - 1]) + ")";
  }

  hint.classList.remove("hidden");
  hint.innerHTML = '<span class="service-origin-hint">Last used from: ' +
    esc(lastHostname) + frameNote +
    ' <a class="service-origin-open" data-url="' + esc(lastUrl) + '">Open \u2197</a></span>';
}

// ─── API Key Selector ────────────────────────────────────────────────────────

function renderKeySelector() {
  var section = document.getElementById("send-key-section");
  var optionsEl = document.getElementById("send-key-options");
  var badge = document.getElementById("send-key-badge");
  if (!section || !optionsEl) return;

  // Get selected service and hostname
  var epSelect = document.getElementById("send-ep-select");
  var selectedOpt = epSelect?.options?.[epSelect.selectedIndex];
  var svc = selectedOpt?.dataset?.svc || "";
  // Explicit canParse guard — same root-cause fix as renderServiceOriginHint.
  var hostname = (currentRequestUrl && URL.canParse(currentRequestUrl)) ? new URL(currentRequestUrl).hostname : "";

  // Collect matching keys (same logic as collectKeysForService in background)
  var matchingKeys = [];
  if (tabData?.apiKeys) {
    for (var k in tabData.apiKeys) {
      var kd = tabData.apiKeys[k];
      var svcMatch = kd.services && kd.services.indexOf(svc) !== -1;
      var hostMatch = kd.hosts && kd.hosts.indexOf(hostname) !== -1;
      if (svcMatch || hostMatch) {
        matchingKeys.push({ key: k, data: kd });
      }
    }
  }

  if (matchingKeys.length === 0) {
    section.classList.add("hidden");
    currentKeyOverride = null;
    return;
  }

  section.classList.remove("hidden");
  badge.textContent = matchingKeys.length + " available";

  var html = '';

  // Auto option (default)
  var autoTruncated = truncateKey(matchingKeys[0].key);
  html += '<label class="key-option">' +
    '<input type="radio" name="send-key-select" value="auto" checked /> ' +
    '<span class="key-option-label">Auto</span> ' +
    '<span class="key-value-truncated">' + esc(autoTruncated) + '</span>' +
    '</label>';

  // One option per matching key
  for (var i = 0; i < matchingKeys.length; i++) {
    var mk = matchingKeys[i];
    var truncated = truncateKey(mk.key);
    var sourceBadge = mk.data.source ? '<span class="key-source-badge">' + esc(mk.data.source) + '</span>' : '';
    html += '<label class="key-option">' +
      '<input type="radio" name="send-key-select" value="key-' + i + '" data-key="' + esc(mk.key) + '" /> ' +
      '<span class="key-value-truncated">' + esc(truncated) + '</span> ' +
      '<span class="key-option-label">' + esc(mk.data.name || "Key") + '</span> ' +
      sourceBadge +
      '</label>';
  }

  // Custom key option
  html += '<label class="key-option">' +
    '<input type="radio" name="send-key-select" value="custom" /> ' +
    '<span class="key-option-label">Custom</span>' +
    '</label>' +
    '<input type="text" class="key-custom-input hidden" id="send-key-custom" placeholder="Paste API key..." />';

  // None option
  html += '<label class="key-option">' +
    '<input type="radio" name="send-key-select" value="none" /> ' +
    '<span class="key-option-label">None (no key injection)</span>' +
    '</label>';

  optionsEl.innerHTML = html;

  // Reset override to auto
  currentKeyOverride = null;
}

function truncateKey(key) {
  if (!key || key.length <= 16) return key || "";
  return key.slice(0, 8) + "\u2026" + key.slice(-4);
}

function onKeySelectionChange() {
  var selected = document.querySelector('input[name="send-key-select"]:checked');
  var customInput = document.getElementById("send-key-custom");
  if (!selected) { currentKeyOverride = null; return; }

  var injectSource = "header";
  var injectRadio = document.querySelector('input[name="key-inject"]:checked');
  if (injectRadio) injectSource = injectRadio.value;

  if (customInput) customInput.classList.add("hidden");

  if (selected.value === "auto") {
    currentKeyOverride = null;
  } else if (selected.value === "none") {
    currentKeyOverride = { disabled: true };
  } else if (selected.value === "custom") {
    if (customInput) customInput.classList.remove("hidden");
    var val = customInput ? customInput.value.trim() : "";
    currentKeyOverride = val ? { key: val, source: injectSource } : null;
  } else if (selected.dataset.key) {
    currentKeyOverride = { key: selected.dataset.key, source: injectSource };
  }
}

function renderFieldsTable(rootFields) {
  // Iterative DFS pre-order: each stack frame holds (entries, idx,
  // depth). Process the top frame: emit one row at entries[idx],
  // bump idx, and if the field has children push a new frame for them
  // (children render immediately after their parent — that's the visual
  // tree layout). Pop empty frames. Replaces self-recursion so deeply-
  // nested field trees render without growing the JS call stack.
  let html = `<table class="fields-table"><thead><tr><th>#</th><th>Field</th><th>Type</th><th>Message Type</th><th>Label</th></tr></thead><tbody>`;
  const stack = [{ entries: rootFields, idx: 0, depth: 0 }];
  while (stack.length > 0) {
    const top = stack[stack.length - 1];
    if (top.idx >= top.entries.length) {
      stack.pop();
      continue;
    }
    const [name, f] = top.entries[top.idx];
    top.idx++;
    const depth = top.depth;
    const indent = depth > 0 ? `padding-left:${depth * 16}px` : "";
    const labelClass = f.required
      ? "f-req"
      : f.label === "repeated"
        ? "f-repeated"
        : "";
    const labelText = f.required ? "required" : f.label || "";
    html += `<tr>
      <td class="f-num">${f.number ?? ""}</td>
      <td class="f-name"${indent ? ` data-indent="${depth}"` : ""}>${depth > 0 ? "&#x2514; " : ""}${esc(name)}</td>
      <td class="f-type">${esc(f.type)}</td>
      <td class="f-msg">${esc(f.messageType || "")}</td>
      <td class="${labelClass}">${esc(labelText)}</td>
    </tr>`;
    if (f.children?.length) {
      const childEntries = f.children.map((c) => [
        c.name || `field_${c.number}`,
        c,
      ]);
      stack.push({ entries: childEntries, idx: 0, depth: depth + 1 });
    }
  }
  html += `</tbody></table>`;
  return html;
}

// ─── Send Panel: Endpoint Selection + Schema ─────────────────────────────────

function onSendEndpointSelected() {
  const select = document.getElementById("send-ep-select");
  const epKey = select.value;

  // Clear previous send result
  const respEl = document.getElementById("send-response");
  if (respEl) respEl.style.display = "none";

  // Handle virtual discovery endpoint
  const selectedOpt = select.options[select.selectedIndex];
  if (selectedOpt?.dataset?.isVirtual === "true") {
    const svc = selectedOpt.dataset.svc;
    const pathTemplate = selectedOpt.dataset.path;
    const validMethod = selectedOpt.dataset.method;
    const discoveryId = selectedOpt.dataset.discoveryId; // Use data-discoveryId
    const svcData = tabData?.discoveryDocs?.[svc];
    const doc = svcData?.doc;

    // baseUrl resolution from doc
    let baseUrl = doc?.baseUrl || doc?.rootUrl;
    if (!baseUrl && doc?.rootUrl) {
      baseUrl = doc.rootUrl + (doc.servicePath || "");
    }
    // Fallback
    if (!baseUrl) {
      console.warn("No baseUrl found for service", svc, svcData);
      // Try to construct from service name if possible, or leave empty
      baseUrl = "";
    }

    // Fix double slashes just in case
    if (baseUrl.endsWith("/") && pathTemplate.startsWith("/")) {
      baseUrl = baseUrl.slice(0, -1);
    }

    currentRequestUrl = baseUrl + pathTemplate;
    currentRequestMethod = validMethod;

    select.dataset.svc = svc;
    select.dataset.discoveryId = discoveryId;

    // Load schema via background
    loadVirtualSchema(svc, discoveryId);
    return;
  }

  // Fallback if no matching endpoint found — hide everything below the dropdowns
  currentRequestUrl = "";
  currentRequestMethod = "POST";
  currentSchema = null;
  // Exit console mode if active
  if (currentBodyMode === "msgconsole") {
    currentChannelId = null;
    currentChannelType = null;
    currentTargetOrigin = null;
    currentChannelFrameId = null;
    document.getElementById("send-ws-console").style.display = "none";
  }
  currentBodyMode = "form";
  setSendPanelVisible(false);
  document.getElementById("send-frame-row").classList.add("hidden");
  document.getElementById("send-key-section").classList.add("hidden");
  document.getElementById("send-form-fields").style.display = "block";
  document.getElementById("send-form-fields").innerHTML =
    '<div class="hint">Select a method to load its schema.</div>';
  renderChainInfo(null);
}

async function loadVirtualSchema(service, methodId, initialData = null) {
  currentSchema = null;
  document.getElementById("send-form-fields").innerHTML =
    '<div class="hint">Loading schema...</div>';

  try {
    const schema = await chrome.runtime.sendMessage({
      type: "GET_ENDPOINT_SCHEMA",
      tabId: currentTabId,
      service,
      methodId,
    });

    if (!schema || !schema.method) {
      document.getElementById("send-form-fields").innerHTML =
        '<div class="hint">Method definition not found.</div>';
      return;
    }

    currentSchema = schema;

    // Auto-determine Content-Type from learned schema
    if (schema.contentTypes?.length) {
      currentContentType = schema.contentTypes[0];
    } else if (schema.endpoint?.contentType) {
      currentContentType = schema.endpoint.contentType;
    } else {
      currentContentType = "application/json";
    }

    // Auto-set body mode: GraphQL if URL matches, otherwise form
    if (isGraphQLUrl(currentRequestUrl)) {
      setBodyMode("graphql");
      if (gqlState.ops.length === 0 || !gqlState.ops[0].query) gqlClear();
    } else {
      setBodyMode("form");
    }

    buildFormFields(schema, initialData);
    renderChainInfo(schema.chains);
    renderKeySelector();
    renderServiceOriginHint();
  } catch (err) {
    console.error("Error loading virtual schema:", err);
    document.getElementById("send-form-fields").innerHTML =
      `<div class="hint">Error loading schema: ${esc(err.message)}</div>`;
  }
}

function renderChainInfo(chains) {
  const container = document.getElementById("send-chain-info");
  if (!container) return;
  if (!chains || (!chains.incoming?.length && !chains.outgoing?.length)) {
    container.classList.add("hidden");
    container.innerHTML = "";
    return;
  }
  container.classList.remove("hidden");
  let html = '<div class="chain-info-box">';
  if (chains.incoming?.length) {
    html += '<div class="chain-section"><span class="chain-section-label">Receives data from:</span>';
    for (const link of chains.incoming) {
      html += `<div class="chain-link chain-incoming">` +
        `<span class="chain-param">${esc(link.paramName)}</span>` +
        `<span class="chain-arrow">&larr;</span>` +
        `<span class="chain-source">${esc(link.sourceMethodId)}</span>` +
        `<span class="chain-field">.${esc(link.sourceFieldPath)}</span>` +
        (link.observedCount > 1 ? `<span class="chain-count">${link.observedCount}x</span>` : "") +
        `</div>`;
    }
    html += '</div>';
  }
  if (chains.outgoing?.length) {
    html += '<div class="chain-section"><span class="chain-section-label">Feeds data to:</span>';
    for (const link of chains.outgoing) {
      html += `<div class="chain-link chain-outgoing">` +
        `<span class="chain-field">${esc(link.sourceFieldPath)}</span>` +
        `<span class="chain-arrow">&rarr;</span>` +
        `<span class="chain-source">${esc(link.targetMethodId)}</span>` +
        `<span class="chain-param">.${esc(link.paramName)}</span>` +
        (link.observedCount > 1 ? `<span class="chain-count">${link.observedCount}x</span>` : "") +
        `</div>`;
    }
    html += '</div>';
  }
  html += '</div>';
  container.innerHTML = html;
}

function pbTreeToMap(rootNodes) {
  // Iterative tree-to-map: each work item populates an object slot
  // (`target[key] = ...`) from a `nodes` list. Nested messages and JSPB
  // sub-arrays pre-allocate empty objects/arrays whose contents are
  // queued for population. Replaces self-recursion so deeply-nested
  // protobuf trees (or adversarial JSPB arrays) convert without growing
  // the JS call stack.
  if (!rootNodes) return null;
  const root = {};
  const queue = [{ kind: "nodes", nodes: rootNodes, target: root }];
  while (queue.length > 0) {
    const job = queue.shift();
    if (job.kind === "nodes") {
      for (const node of job.nodes) {
        if (node.message) {
          const sub = {};
          job.target[node.field] = sub;
          queue.push({ kind: "nodes", nodes: node.message, target: sub });
        } else if (node.isJspb && Array.isArray(node.value)) {
          const list = new Array(node.value.length);
          job.target[node.field] = list;
          for (let i = 0; i < node.value.length; i++) {
            const item = node.value[i];
            if (Array.isArray(item)) {
              const subMap = {};
              list[i] = subMap;
              queue.push({ kind: "nodes", nodes: jspbToTree(item), target: subMap });
            } else {
              list[i] = item;
            }
          }
        } else {
          job.target[node.field] = node.value ?? node.string ?? node.hex ?? node.asFloat;
        }
      }
    }
  }
  return root;
}

function buildFormFields(schema, initialData = null) {
  const container = document.getElementById("send-form-fields");
  container.innerHTML = "";

  if (schema.method && (schema.method.description || schema.method.scopes?.length)) {
    const info = el("div", "card");
    info.style.marginBottom = "8px";
    let html = "";
    if (schema.method.description) {
      html += `<div class="card-meta">${esc(schema.method.description)}</div>`;
    }
    if (schema.method.scopes?.length) {
      html += `<div class="card-meta scopes-row">Scopes: ${schema.method.scopes.map((s) => `<code>${esc(s)}</code>`).join(" ")}</div>`;
    }
    info.innerHTML = html;
    container.appendChild(info);
  }

  // Learned required headers — the header SET the bundle actually attached at
  // the host edge (fetch init.headers / XHR setRequestHeader), per-header
  // literal value vs opaque ("dynamic", set by the app at runtime). Transport
  // metadata, shown read-only so a reviewer knows what the endpoint needs
  // (e.g. github preheat: Accept: application/json + X-GITHUB-PREHEAT dynamic).
  const _rh = (schema.method && schema.method.requiredHeaders)
    || (schema.endpoint && schema.endpoint.requiredHeaders);
  if (_rh && Object.keys(_rh).length > 0) {
    const hsec = el("div", "form-section");
    let hh = '<div class="form-section-label">Required Headers <span class="card-meta">(learned)</span></div>';
    for (const [hn, hv] of Object.entries(_rh)) {
      if (hv && hv.kind === "literal") {
        /* Literal: read-only display. sendRequest auto-attaches the
           value from currentSchema.method.requiredHeaders so the user
           doesn't need to retype it. */
        hh += `<div class="card-meta" style="display:flex;gap:8px;align-items:center;"><code>${esc(hn)}</code>: <code>${esc(String(hv.value))}</code></div>`;
      } else {
        /* Opaque: editable input so the reviewer can paste in the
           runtime value (CSRF token, signature, bearer) the analyzer
           couldn't compute. The input carries data-required-header so
           sendRequest's auto-attach loop reads it as a learned header
           override; user-typed form headers still win above. */
        hh += `<div class="card-meta" style="display:flex;gap:8px;align-items:center;"><code>${esc(hn)}</code>: <input type="text" class="opaque-header-input" data-required-header="${esc(hn)}" placeholder="dynamic — paste runtime value" style="flex:1;min-width:0"></div>`;
      }
    }
    hsec.innerHTML = hh;
    container.appendChild(hsec);
  }

  if (schema.parameters && Object.keys(schema.parameters).length > 0) {
    const section = el("div", "form-section");
    section.innerHTML = '<div class="form-section-label">URL Parameters</div>';
    for (const [name, param] of Object.entries(schema.parameters)) {
      section.appendChild(
        createFieldInput(
          name,
          {
            name: param.name, // Pass the name (which might be an alias)
            // Show the source-map-resolved real name (e.g. `e`→`owner`) as the
            // label; the field key (first arg) stays the minified name so URL
            // path substitution still matches the `{e}` hole.
            displayName: param._sourceMapName || undefined,
            type:
              param.type === "integer"
                ? "int32"
                : param.type === "boolean"
                  ? "bool"
                  : "string",
            required: param.required,
            description: param.description,
            label: param.required ? "required" : "optional",
            number: null,
            messageType: null,
            children: null,
            enum: param.enum || null,
            location: param.location,
            parentSchema: "params",
            _astValidValues: param._astValidValues || null,
            _astValueSource: param._astValueSource || null,
            _detectedEnum: param._detectedEnum || false,
            _defaultValue: param._defaultValue ?? null,
            _defaultConfidence: param._defaultConfidence ?? null,
            _requiredConfidence: param._requiredConfidence ?? null,
            _exampleValue: param._exampleValue === undefined ? null : param._exampleValue,
            _exampleValueSource: param._exampleValueSource || null,
            _range: param._range || null,
          },
          "param",
          0,
          // Prefer the captured value (last real request's data), then
          // fall back to the AST/stats-derived example value. This lets
          // a reviewer open a method they've never replayed and still
          // get a sendable form — no blank fields — without us making
          // up values. The source is an observed default or an AST
          // constraint, never a guess.
          initialData && initialData[name] !== undefined
            ? initialData[name]
            : (param._exampleValue !== undefined ? param._exampleValue : null),
        ),
      );
    }
    container.appendChild(section);
  }

  if (schema.requestBody?.fields?.length > 0) {
    const section = el("div", "form-section");
    const label = schema.requestBody.schemaName
      ? `Request Body (${esc(schema.requestBody.schemaName)})`
      : "Request Body";
    section.innerHTML = `<div class="form-section-label">${label}</div>`;
    const renderedTop = new Set();
    for (const field of schema.requestBody.fields) {
      renderedTop.add(field.name);
      // A JSPB body's initialData is keyed by FIELD NUMBER (the protobuf
      // tree produces `map[node.field]`). Claim that number as well, so
      // the drift loop below doesn't re-render the same field under its
      // numeric key as a second anonymous entry next to the named one.
      if (field.number != null) renderedTop.add(String(field.number));
      // Same prefer-captured-then-example logic as parameters. When
      // the reviewer opens a body-carrying method they haven't
      // replayed, the form is populated from the schema's AST /
      // observed-default values so Send produces a plausible request
      // the server will actually accept (or reject with a useful
      // error). No made-up values.
      const fieldVal = (initialData && (initialData[field.number] !== undefined || initialData[field.name] !== undefined))
        ? (initialData[field.number] ?? initialData[field.name] ?? null)
        : (field._exampleValue !== undefined ? field._exampleValue : null);
      section.appendChild(
        createFieldInput(
          field.name,
          { ...field, parentSchema: schema.requestBody.schemaName },
          "body",
          0,
          fieldVal,
        ),
      );
    }
    // Top-level schema drift: fields present in the captured body but
    // not in the learned schema still need to reach the outgoing request.
    // Without this, anything the extension hasn't learned yet gets
    // silently stripped on replay — which is the opposite of what the
    // user wants when they hit Send.
    if (initialData && typeof initialData === "object" && !Array.isArray(initialData)) {
      for (const [k, v] of Object.entries(initialData)) {
        if (renderedTop.has(k)) continue;
        // initialData may include path/query params keyed by string — skip
        // keys that match any URL parameter definition.
        if (schema.parameters && schema.parameters[k]) continue;
        section.appendChild(
          createFieldInput(
            k,
            synthesizeFieldDefFromValue(k, v),
            "body",
            0,
            v,
          ),
        );
      }
    }
    container.appendChild(section);
  }

  if (!schema.parameters && !schema.requestBody?.fields?.length) {
    container.innerHTML = '<div class="hint">No schema available.</div>';
  }

  // Show raw body textarea alongside form when schema has no body fields
  // but the method has a body (POST/PUT/PATCH) — allows editing unknown body formats
  if (!schema.requestBody?.fields?.length) {
    const method = (currentRequestMethod || "").toUpperCase();
    if (method !== "GET" && method !== "DELETE") {
      document.getElementById("send-raw-body").style.display = "block";
    }
  }
}

// Build a field descriptor from a captured value when no schema field
// exists. Lets the form render + collect every JSON property, not just
// the ones the extension has already learned. Scalars get typed by their
// JS type; objects become `type: "object"`; arrays become `label: "repeated"`.
function synthesizeFieldDefFromValue(rootName, rootValue) {
  // Iterative tree-builder. Each work item pairs (name, value) with the
  // destination FieldDef shell to populate. Object/array values queue
  // their children for later population; scalars finalize in-place.
  // Replaces self-recursion so deeply-nested JSON variables (or
  // adversarial server payloads) synthesize without growing the JS
  // call stack.
  function makeShell(name) {
    return { name, number: null, required: false, description: null,
      label: "optional", messageType: null, children: null };
  }
  const root = makeShell(rootName);
  const queue = [{ name: rootName, value: rootValue, dst: root }];
  while (queue.length > 0) {
    const { value, dst } = queue.shift();
    if (value === null || value === undefined) {
      dst.type = "string";
      continue;
    }
    if (Array.isArray(value)) {
      const firstObj = value.find(v => v && typeof v === "object" && !Array.isArray(v));
      if (firstObj) {
        dst.type = "object";
        dst.label = "repeated";
        dst.children = [];
        for (const [k, v] of Object.entries(firstObj)) {
          const childShell = makeShell(k);
          dst.children.push(childShell);
          queue.push({ name: k, value: v, dst: childShell });
        }
      } else {
        dst.type = value.length ? typeOfScalar(value[0]) : "string";
        dst.label = "repeated";
      }
      continue;
    }
    if (typeof value === "object") {
      dst.type = "object";
      dst.children = [];
      for (const [k, v] of Object.entries(value)) {
        const childShell = makeShell(k);
        dst.children.push(childShell);
        queue.push({ name: k, value: v, dst: childShell });
      }
      continue;
    }
    dst.type = typeOfScalar(value);
  }
  return root;
}
function typeOfScalar(v) {
  if (typeof v === "number") return Number.isInteger(v) ? "int64" : "double";
  if (typeof v === "boolean") return "bool";
  return "string";
}

// Form-builder iterative driver. The three public entry points
// (createFieldInput, _buildRepeatedMessageItem, _buildMessageGroup)
// each seed a build queue, build the root wrapper synchronously, and
// drain the queue (which dispatches each enqueued item to its step
// function). Step functions only enqueue children — they never call
// each other or the public entry points — so the static call graph
// is acyclic.
//
// Dynamic-add-item / remove-item user interactions don't reference
// the renderer from inside the renderer. Each "+ Add item" button is
// registered in `_addItemTargets` (a WeakMap from button to its
// build context); a single document-level click delegate at file
// scope reads the registry and invokes _buildRepeatedMessageItem.
// Each remove "×" button gets `data-form-remove-item="1"`. The
// file-scope listener is outside any function declaration, so the
// recursion lint correctly does not see this as a call edge from
// inside the renderer's body.
const _addItemTargets = new WeakMap();

function createFieldInput(name, fieldDef, category, depth, initialValue = null) {
  const queue = [];
  const wrapper = _buildFieldStep(name, fieldDef, category, depth || 0, initialValue, queue);
  _drainBuildQueue(queue);
  return wrapper;
}

function _drainBuildQueue(queue) {
  while (queue.length > 0) {
    const item = queue.shift();
    let wrapper;
    if (item.kind === "FIELD") {
      wrapper = _buildFieldStep(item.name, item.fieldDef, item.category, item.depth, item.initialValue, queue);
    } else if (item.kind === "REPEATED_ITEM") {
      wrapper = _buildRepeatedItemStep(item.fieldDef, item.category, item.depth, item.itemValue, queue);
    } else if (item.kind === "MESSAGE_GROUP") {
      wrapper = _buildMessageStep(item.fieldDef, item.category, item.depth, item.initialValue, item.hasSchema, queue);
    }
    if (wrapper) item.parent.appendChild(wrapper);
  }
}

function _buildFieldStep(name, fieldDef, category, depth, initialValue, queue) {
  depth = depth || 0;
  const wrapper = el("div", "form-field");
  wrapper.style.paddingLeft = depth * 16 + "px";

  wrapper.dataset.name = name;
  wrapper.dataset.type = fieldDef.type || "string";
  wrapper.dataset.category = category;
  if (fieldDef.number) wrapper.dataset.number = fieldDef.number;
  if (fieldDef.label) wrapper.dataset.label = fieldDef.label;
  if (fieldDef.location) wrapper.dataset.location = fieldDef.location;

  const labelEl = el("label", "form-field-label");
  // `displayName` is the rendered label only — an explicit `displayName`
  // override (used by the GraphQL variables tree to render aliases while
  // keeping the wire key intact) wins, then the fieldDef's own custom
  // name, then the caller's positional `name`.
  const displayName = fieldDef.displayName || fieldDef.name || name;
  let labelHtml = `<span class="field-name">${esc(displayName)}</span>`;

  // Add rename button for learned/indexed fields, parameters, or any
  // field where a caller has explicitly opted in by setting parentSchema
  // to a non-empty value (used by the GraphQL variables tree to persist
  // per-operation aliases under `__gqlVars_<op>`).
  if (fieldDef.number || name.startsWith("field") || category === "param" || (fieldDef.parentSchema && fieldDef.parentSchema !== "params")) {
    labelHtml += ` <span class="btn-rename" title="Rename field" data-schema="${esc(fieldDef.parentSchema || "params")}" data-key="${esc(name)}">✎</span>`;
  }

  if (fieldDef.number)
    labelHtml += ` <span class="field-number">#${fieldDef.number}</span>`;
  labelHtml += ` <span class="field-type">${esc(fieldDef.type || "string")}</span>`;
  if (fieldDef.required)
    labelHtml += ` <span class="field-required">required</span>`;
  if (fieldDef.label === "repeated")
    labelHtml += ` <span class="field-repeated">repeated</span>`;

  // Stats-derived badges
  if (fieldDef._requiredConfidence != null && !fieldDef.required) {
    labelHtml += ` <span class="field-stat badge-optional">seen ${Math.round(fieldDef._requiredConfidence * 100)}%</span>`;
  }
  if (fieldDef._detectedEnum && fieldDef.enum) {
    labelHtml += ` <span class="field-stat badge-enum-detected">enum detected</span>`;
  }
  if (fieldDef._defaultValue != null) {
    labelHtml += ` <span class="field-stat badge-default">default: ${esc(String(fieldDef._defaultValue))}</span>`;
  }
  if (fieldDef._range) {
    labelHtml += ` <span class="field-stat badge-range">${fieldDef._range.min}–${fieldDef._range.max}</span>`;
  }
  // When the form was prefilled from an example value (vs a captured
  // request's initialData), show the provenance so the user knows
  // whether the value came from observed traffic, AST analysis, or a
  // type-default fallback. Never a guess without attribution.
  if (fieldDef._exampleValueSource && initialValue != null && (fieldDef._exampleValue === initialValue || String(fieldDef._exampleValue) === String(initialValue))) {
    labelHtml += ` <span class="field-stat badge-prefill" title="Prefilled from ${esc(fieldDef._exampleValueSource)}">prefill: ${esc(fieldDef._exampleValueSource)}</span>`;
  }
  if (fieldDef.format && fieldDef.format !== fieldDef.type) {
    labelHtml += ` <span class="field-stat badge-format">${esc(fieldDef.format)}</span>`;
  }

  labelEl.innerHTML = labelHtml;
  wrapper.appendChild(labelEl);

  if (fieldDef.description) {
    const desc = el("div", "field-description");
    desc.textContent = fieldDef.description;
    wrapper.appendChild(desc);
  }

  // Show AST-discovered valid values as clickable chips
  if (fieldDef._astValidValues && fieldDef._astValidValues.length > 0 && !fieldDef.enum) {
    const valHint = el("div", "field-ast-values");
    valHint.innerHTML = '<span class="ast-values-label">Values found in JS:</span> '
      + fieldDef._astValidValues.map(v => '<span class="ast-value-chip">' + esc(String(v)) + '</span>').join(' ');
    valHint.addEventListener("click", function(e) {
      if (!e.target.classList.contains("ast-value-chip")) return;
      var input = wrapper.querySelector(".form-input");
      if (input) { input.value = e.target.textContent; input.dispatchEvent(new Event("input")); }
    });
    wrapper.appendChild(valHint);
  }

  if (fieldDef.label === "repeated" && (fieldDef.type === "message" || fieldDef.type === "object")) {
    // Repeated message / array of objects — render each captured item as its
    // own collapsible message sub-editor. Without this, arrays like
    // `events: [{ts: 1, kind: "x"}, {ts: 2, kind: "y"}]` had no UI at all
    // and the popup fell back to the raw textarea, defeating the form
    // editor. Matches the encodeFormToJson repeated-message path.
    const listContainer = el("div", "form-repeated-list form-repeated-message-list");
    listContainer.dataset.fieldType = fieldDef.type;

    // Repeated-message branch — enqueue each item onto the build
    // queue with this listContainer as its DOM parent. The driver
    // drains them in order, calling _buildRepeatedItemStep for each.
    const items = Array.isArray(initialValue) ? initialValue : [];
    for (const it of items) {
      queue.push({ kind: "REPEATED_ITEM", parent: listContainer, fieldDef, category, depth, itemValue: it });
    }
    wrapper.appendChild(listContainer);

    const addBtn = el("button", "btn-small");
    addBtn.textContent = "+ Add item";
    addBtn.type = "button";
    addBtn.dataset.formAddRepeated = "1";
    _addItemTargets.set(addBtn, { kind: "repeatedMessage", listContainer, fieldDef, category, depth });
    wrapper.appendChild(addBtn);
  } else if ((fieldDef.type === "message" || fieldDef.type === "object") && fieldDef.children?.length) {
    queue.push({ kind: "MESSAGE_GROUP", parent: wrapper, fieldDef, category, depth, initialValue, hasSchema: true });
  } else if (fieldDef.type === "message" || fieldDef.type === "object") {
    if (initialValue && typeof initialValue === "object" && !Array.isArray(initialValue)) {
      queue.push({ kind: "MESSAGE_GROUP", parent: wrapper, fieldDef, category, depth, initialValue, hasSchema: false });
    } else {
      wrapper.appendChild(createSingleInput({ type: "string" }, "", category));
    }
  } else if (fieldDef.label === "repeated" && fieldDef.type !== "message") {
    const listContainer = el("div", "form-repeated-list");
    listContainer.dataset.fieldType = fieldDef.type;

    if (Array.isArray(initialValue) && initialValue.length > 0) {
      for (const val of initialValue) {
        listContainer.appendChild(createSingleInput(fieldDef, val, category));
      }
    } else {
      listContainer.appendChild(createSingleInput(fieldDef, initialValue, category));
    }
    wrapper.appendChild(listContainer);

    const addBtn = el("button", "btn-small");
    addBtn.textContent = "+ Add";
    addBtn.type = "button";
    addBtn.dataset.formAddRepeated = "1";
    _addItemTargets.set(addBtn, { kind: "repeatedScalar", listContainer, fieldDef, category });
    wrapper.appendChild(addBtn);
  } else if (fieldDef.type !== "message") {
    wrapper.appendChild(createSingleInput(fieldDef, initialValue, category));
  }

  return wrapper;
}

function _buildRepeatedMessageItem(fieldDef, category, depth, itemValue) {
  const queue = [];
  const wrapper = _buildRepeatedItemStep(fieldDef, category, depth, itemValue, queue);
  _drainBuildQueue(queue);
  return wrapper;
}

function _buildRepeatedItemStep(fieldDef, category, depth, itemValue, queue) {
  const itemWrapper = el("div", "form-repeated-item form-message-group");
  const summary = document.createElement("div");
  summary.className = "form-repeated-item-summary";
  summary.textContent = (fieldDef.messageType || fieldDef.name || "item");
  const removeBtn = el("button", "btn-small");
  removeBtn.textContent = "×";
  removeBtn.type = "button";
  removeBtn.title = "Remove item";
  removeBtn.dataset.formRemoveItem = "1";
  summary.appendChild(removeBtn);
  itemWrapper.appendChild(summary);

  const childContainer = el("div", "form-message-children");
  const schemaChildren = fieldDef.children || [];
  const rendered = new Set();
  for (const child of schemaChildren) {
    rendered.add(child.name);
    if (child.number != null) rendered.add(String(child.number));
    const childVal = itemValue && typeof itemValue === "object"
      ? (itemValue[child.name] ?? itemValue[child.number] ?? null)
      : null;
    queue.push({
      kind: "FIELD", parent: childContainer,
      name: child.name,
      fieldDef: { ...child, parentSchema: fieldDef.messageType || fieldDef.parentSchema },
      category, depth: depth + 1, initialValue: childVal,
    });
  }
  if (itemValue && typeof itemValue === "object" && !Array.isArray(itemValue)) {
    for (const [k, v] of Object.entries(itemValue)) {
      if (rendered.has(k)) continue;
      queue.push({
        kind: "FIELD", parent: childContainer,
        name: k, fieldDef: synthesizeFieldDefFromValue(k, v),
        category, depth: depth + 1, initialValue: v,
      });
    }
  }
  itemWrapper.appendChild(childContainer);
  return itemWrapper;
}

function _buildMessageGroup(fieldDef, category, depth, initialValue, hasSchema) {
  const queue = [];
  const wrapper = _buildMessageStep(fieldDef, category, depth, initialValue, hasSchema, queue);
  _drainBuildQueue(queue);
  return wrapper;
}

function _buildMessageStep(fieldDef, category, depth, initialValue, hasSchema, queue) {
  const details = document.createElement("details");
  details.open = hasSchema ? (initialValue !== null || depth < 1) : (depth < 1);
  details.className = "form-message-group";
  const summary = document.createElement("summary");
  summary.textContent = hasSchema
    ? (fieldDef.messageType || fieldDef.name || "message")
    : (fieldDef.name || "object");
  details.appendChild(summary);
  const childContainer = el("div", "form-message-children");
  const inheritedSchema = fieldDef.messageType || fieldDef.parentSchema;
  const rendered = new Set();
  if (hasSchema) {
    for (const child of fieldDef.children) {
      rendered.add(child.name);
      if (child.number != null) rendered.add(String(child.number));
      const childVal = initialValue
        ? (initialValue[child.name] ?? initialValue[child.number] ?? null)
        : null;
      queue.push({
        kind: "FIELD", parent: childContainer,
        name: child.name,
        fieldDef: { ...child, parentSchema: inheritedSchema },
        category, depth: depth + 1, initialValue: childVal,
      });
    }
  }
  if (initialValue && typeof initialValue === "object" && !Array.isArray(initialValue)) {
    for (const [k, v] of Object.entries(initialValue)) {
      if (rendered.has(k)) continue;
      const synthDef = synthesizeFieldDefFromValue(k, v);
      if (inheritedSchema) synthDef.parentSchema = inheritedSchema;
      queue.push({
        kind: "FIELD", parent: childContainer,
        name: k, fieldDef: synthDef,
        category, depth: depth + 1, initialValue: v,
      });
    }
  }
  details.appendChild(childContainer);
  return details;
}

// File-scope event delegation for dynamic add/remove buttons. The
// addEventListener call lives outside any function declaration; the
// recursion lint walks function bodies, so this listener's invocation
// of the public entry point is correctly NOT seen as a call edge from
// inside the renderer. Architecturally: state mutation (clicking the
// button) is decoupled from rendering — the click handler resolves
// build context from the WeakMap registry and asks the renderer to
// build the new node.
document.addEventListener("click", (e) => {
  const target = e.target;
  if (!target || !target.closest) return;
  const removeBtn = target.closest("[data-form-remove-item]");
  if (removeBtn) {
    const item = removeBtn.closest(".form-repeated-item");
    if (item) item.remove();
    return;
  }
  const addBtn = target.closest("[data-form-add-repeated]");
  if (!addBtn) return;
  const ctx = _addItemTargets.get(addBtn);
  if (!ctx) return;
  if (ctx.kind === "repeatedMessage") {
    ctx.listContainer.appendChild(_buildRepeatedMessageItem(ctx.fieldDef, ctx.category, ctx.depth, null));
  } else if (ctx.kind === "repeatedScalar") {
    ctx.listContainer.appendChild(createSingleInput(ctx.fieldDef, null, ctx.category));
  }
});

function createSingleInput(fieldDef, initialValue = null, category = null) {
  const type = fieldDef.type || "string";

  if ((type === "enum" || fieldDef.enum) && fieldDef.enum?.length) {
    const sel = document.createElement("select");
    sel.className = "form-input form-input-select";
    const emptyOpt = document.createElement("option");
    emptyOpt.value = "";
    emptyOpt.textContent = "-- select --";
    sel.appendChild(emptyOpt);
    for (let i = 0; i < fieldDef.enum.length; i++) {
      const opt = document.createElement("option");
      opt.value = fieldDef.enum[i];
      opt.textContent =
        fieldDef.enum[i] +
        (fieldDef.enumDescriptions?.[i]
          ? " - " + fieldDef.enumDescriptions[i]
          : "");
      if (
        initialValue !== null &&
        String(initialValue) === String(fieldDef.enum[i])
      ) {
        opt.selected = true;
      }
      sel.appendChild(opt);
    }
    return sel;
  }

  // AST-discovered valid values (e.g. role=admin/role=guest in different
  // code branches) — render as autocomplete via <input list> + <datalist>.
  // Per user direction: 'I think it should be autocomplete suggestions'
  // — keeps free-text entry while offering the AST-observed values as
  // suggestions the user can pick from.
  if (fieldDef._astValidValues && fieldDef._astValidValues.length > 0) {
    const wrap = document.createDocumentFragment();
    const inp = document.createElement("input");
    inp.type = "text";
    inp.className = "form-input";
    inp.autocomplete = "off";
    // Stable id derived from field name so re-renders reuse the same
    // datalist (no orphaned <datalist> nodes accumulating in the DOM).
    const dlId = "astvals-" + (fieldDef.name || "field").replace(/[^A-Za-z0-9]/g, "_") + "-" + (category || "");
    inp.setAttribute("list", dlId);
    if (initialValue !== null && initialValue !== undefined) {
      inp.value = String(initialValue);
    } else if (fieldDef._astValidValues.length === 1) {
      /* Single observed AST value AND no initialValue → prefill it.
         The bundle only set this param one way during forced execution,
         so the analyzer's confidence on this single value is highest.
         For multi-valued params (>=2 distinct observed values) the
         input stays empty so the user picks from the datalist; per
         CLAUDE.md never auto-pick one of multiple observations as if
         it were "the" value. */
      inp.value = String(fieldDef._astValidValues[0]);
    }
    const dl = document.createElement("datalist");
    dl.id = dlId;
    for (let i = 0; i < fieldDef._astValidValues.length; i++) {
      const opt = document.createElement("option");
      opt.value = String(fieldDef._astValidValues[i]);
      dl.appendChild(opt);
    }
    wrap.appendChild(inp);
    wrap.appendChild(dl);
    return wrap;
  }

  switch (type) {
    case "bool": {
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.className = "form-input form-input-bool";
      if (initialValue === true || initialValue === 1 || initialValue === "1")
        cb.checked = true;
      return cb;
    }
    case "enum": {
      const inp = document.createElement("input");
      inp.type = "number";
      inp.className = "form-input form-input-enum";
      inp.placeholder = "enum value (integer)";
      inp.min = "0";
      if (initialValue !== null) inp.value = initialValue;
      return inp;
    }
    case "int32":
    case "int64":
    case "uint32":
    case "uint64":
    case "sint32":
    case "sint64":
    case "double":
    case "float":
    case "fixed32":
    case "fixed64":
    case "sfixed32":
    case "sfixed64": {
      const inp = document.createElement("input");
      inp.type = "number";
      inp.className = "form-input form-input-number";
      inp.placeholder = type;
      if (type === "double" || type === "float") inp.step = "any";
      if (initialValue !== null) inp.value = initialValue;
      return inp;
    }
    case "bytes": {
      const ta = document.createElement("textarea");
      ta.className = "form-input form-input-bytes";
      ta.placeholder = "base64-encoded bytes";
      ta.rows = 2;
      if (initialValue !== null) ta.value = initialValue;
      return ta;
    }
    default: {
      const inp = document.createElement("input");
      inp.type = "text";
      inp.className = "form-input form-input-string";
      inp.placeholder = type || "value";
      if (initialValue !== null) {
        inp.value =
          typeof initialValue === "object"
            ? JSON.stringify(initialValue)
            : initialValue;
      }
      return inp;
    }
  }
}

// ─── Send Panel: Value Collection + Request ──────────────────────────────────

function formFieldsToMap(rootFields) {
  // Iterative tree-to-map. Each work item populates a `target` object
  // from a fields list; nested message fields enqueue empty sub-objects
  // for later population. Replaces self-recursion on form-field trees.
  const root = {};
  const queue = [{ fields: rootFields, target: root }];
  while (queue.length > 0) {
    const { fields, target } = queue.shift();
    for (const f of fields) {
      if (f.number === null || f.number === undefined) continue;
      if (f.type === "message" && f.children) {
        const sub = {};
        target[f.number] = sub;
        queue.push({ fields: f.children, target: sub });
      } else {
        target[f.number] = f.value;
      }
    }
  }
  return root;
}

function formValuesToInitialData(formValues) {
  if (!formValues) return null;
  const data = { ...formValues.params };
  const fieldMap = formFieldsToMap(formValues.fields);
  Object.assign(data, fieldMap);
  return data;
}

function collectFormValues() {
  const params = {};
  const pathParams = {};
  const fields = [];
  const topFields = document.querySelectorAll(
    "#send-form-fields > .form-section > .form-field",
  );

  for (const wrapper of topFields) {
    const result = collectSingleField(wrapper);
    if (!result) continue;
    if (wrapper.dataset.category === "param") {
      if (result.value !== "" && result.value != null) {
        // Path-template params (e.g. /{owner}/{repo}/…) substitute INTO the
        // URL path; everything else is a query param. Separating them is what
        // makes the learned template's editable owner/repo actually target the
        // right resource instead of being appended as ?owner=…&repo=….
        if (wrapper.dataset.location === "path") pathParams[result.name] = result.value;
        else params[result.name] = result.value;
      }
    } else {
      fields.push(result);
    }
  }

  return { params, pathParams, fields };
}

// Substitute editable path-template holes — /{owner}/{repo}/… — with the
// values the researcher typed. Unfilled holes are left as-is so an invalid
// URL surfaces (the user must supply required path params), never silently
// sent with a literal "{owner}".
function applyPathParams(url, pathParams) {
  if (!pathParams || !url) return url;
  return url.replace(/\{([^}\/]+)\}/g, (m, name) =>
    Object.prototype.hasOwnProperty.call(pathParams, name)
      ? encodeURIComponent(String(pathParams[name]))
      : m,
  );
}

function collectSingleField(rootWrapper) {
  // Iterative DOM-walker. Each work item carries a wrapper to collect
  // and an `attach` callback that places the resulting field-result into
  // the right slot of its parent (root, parent.children, or repeated
  // item.children). Nested message wrappers enqueue further work for
  // their own children. Replaces self-recursion so deeply-nested form
  // structures collect without growing the JS call stack.
  let rootResult = null;
  const queue = [{ wrapper: rootWrapper, attach: (r) => { rootResult = r; } }];
  while (queue.length > 0) {
    const { wrapper, attach } = queue.shift();
    const r = _collectShallow(wrapper, queue);
    if (r) attach(r);
  }
  return rootResult;
}

// Process one form-field wrapper: build its result and queue any nested
// children for the outer driver. Returns the result (without recursing)
// or null if the field has no value/items.
function _collectShallow(wrapper, queue) {
  const name = wrapper.dataset.name;
  const type = wrapper.dataset.type;
  const number = wrapper.dataset.number
    ? parseInt(wrapper.dataset.number)
    : null;
  const label = wrapper.dataset.label || "optional";

  // Repeated message (array of objects) — must be checked BEFORE the
  // scalar `label === "repeated"` branch so the right list container
  // gets walked.
  if (label === "repeated" && (type === "message" || type === "object")) {
    const list = wrapper.querySelector(":scope > .form-repeated-list.form-repeated-message-list");
    if (!list) return null;
    const itemEls = list.querySelectorAll(":scope > .form-repeated-item");
    if (!itemEls.length) return null;
    const items = [];
    for (const itemEl of itemEls) {
      const childContainer = itemEl.querySelector(":scope > .form-message-children");
      const itemChildren = [];
      items.push({ children: itemChildren });
      if (childContainer) {
        for (const childEl of childContainer.querySelectorAll(":scope > .form-field")) {
          queue.push({ wrapper: childEl, attach: (r) => { itemChildren.push(r); } });
        }
      }
    }
    return { name, type, number, label, value: items, children: null };
  }

  if (type === "message" || type === "object") {
    const childContainer = wrapper.querySelector(
      ":scope > .form-message-group > .form-message-children",
    );
    if (!childContainer) return null;
    const children = [];
    for (const childEl of childContainer.querySelectorAll(":scope > .form-field")) {
      queue.push({ wrapper: childEl, attach: (r) => { children.push(r); } });
    }
    return { name, type, number, label, value: null, children };
  }

  if (label === "repeated") {
    const inputs = wrapper.querySelectorAll(".form-repeated-list .form-input");
    const values = [];
    for (const inp of inputs) {
      const v = getInputValue(inp, type);
      if (v !== "" && v != null) values.push(v);
    }
    if (!values.length) return null;
    return { name, type, number, label, value: values, children: null };
  }

  const input = wrapper.querySelector(":scope > .form-input");
  if (!input) return null;
  const value = getInputValue(input, type);
  if (value === "" || value == null) return null;
  return { name, type, number, label, value, children: null };
}

function getInputValue(input, type) {
  if (type === "bool") return input.checked;
  if (input.value === "") return null;
  if (type === "enum") {
    // Enum values may be strings (AST-detected constraints) or integers (protobuf enums).
    // Return as number only if the value is numeric.
    var numVal = Number(input.value);
    return isNaN(numVal) ? input.value : numVal;
  }
  if (
    [
      "int32",
      "int64",
      "uint32",
      "uint64",
      "double",
      "float",
      "sint32",
      "sint64",
      "fixed32",
      "fixed64",
      "sfixed32",
      "sfixed64",
    ].includes(type)
  ) {
    return Number(input.value);
  }
  return input.value;
}

async function sendRequest() {
  const btn = document.getElementById("btn-send");
  btn.disabled = true;
  btn.textContent = "Sending...";

  const bodyMode = currentBodyMode;
  let url = currentRequestUrl;
  const httpMethod = currentRequestMethod;
  const contentType = currentContentType;
  const epKey = document.getElementById("send-ep-select").value;

  const headers = {};
  for (const row of document.querySelectorAll(
    "#send-headers-list .header-row",
  )) {
    const key = row.querySelector(".header-key").value.trim();
    const val = row.querySelector(".header-val").value.trim();
    if (key) headers[key] = val;
  }
  /* Auto-attach learned required headers — the engine captured these
     from the bundle's own fetch init.headers / XHR setRequestHeader
     (per-header literal/opaque provenance). Without this, the popup
     SHOWED the required headers in the form section but DIDN'T send
     them, so replays of (for example) github preheat went out without
     `Accept: application/json` and the server returned HTML instead of
     JSON — the reviewer thought the endpoint was broken when actually
     the replay was missing a header the analyzer had already learned.
     Precedence (last-write-wins after this point): form-row headers >
     opaque-input runtime values > learned literals. */
  const _learnedRH = (currentSchema && currentSchema.method && currentSchema.method.requiredHeaders)
                  || (currentSchema && currentSchema.endpoint && currentSchema.endpoint.requiredHeaders);
  if (_learnedRH && typeof _learnedRH === "object") {
    const _userKeysLc = new Set(Object.keys(headers).map(k => k.toLowerCase()));
    for (const [hn, hv] of Object.entries(_learnedRH)) {
      if (!hv || hv.kind !== "literal" || typeof hv.value !== "string") continue;
      if (_userKeysLc.has(hn.toLowerCase())) continue;   // user form-row override wins
      headers[hn] = hv.value;
    }
  }
  /* Opaque-required-header inputs: the reviewer pastes runtime values
     (CSRF token, bearer, signature) directly into the Required Headers
     section. Each input carries data-required-header with the canonical
     name. Empty values are skipped — the analyzer can't compute these,
     and sending an empty header is worse than omitting it. User-typed
     form-row headers still win (case-insensitive). */
  for (const inp of document.querySelectorAll(".opaque-header-input")) {
    const name = inp.dataset.requiredHeader;
    const val = inp.value;
    if (!name || !val) continue;
    const lc = name.toLowerCase();
    let overridden = false;
    for (const k of Object.keys(headers)) {
      if (k.toLowerCase() === lc) { overridden = true; break; }
    }
    if (overridden) continue;
    headers[name] = val;
  }

  let body;
  if (httpMethod === "GET" || httpMethod === "DELETE") {
    // Collect URL params from form fields even for GET/DELETE
    if (bodyMode === "form") {
      const formValues = collectFormValues();
      url = applyPathParams(url, formValues.pathParams);
      if (Object.keys(formValues.params).length > 0) {
        try {
          const urlObj = new URL(url);
          for (const [k, v] of Object.entries(formValues.params)) {
            urlObj.searchParams.set(k, String(v));
          }
          url = urlObj.toString();
        } catch (_) {
          console.warn("[Send] URL construction failed:", _);
        }
      }
      currentRequestUrl = url;
    }
    body = { mode: "raw", formData: null, rawBody: null, frameId: currentReplayRequest?.frameId };
  } else if (bodyMode === "form") {
    const formValues = collectFormValues();
    url = applyPathParams(url, formValues.pathParams);
    if (Object.keys(formValues.params).length > 0) {
      try {
        const urlObj = new URL(url);
        for (const [k, v] of Object.entries(formValues.params)) {
          urlObj.searchParams.set(k, String(v));
        }
        url = urlObj.toString();
      } catch (_) {
        console.warn("[Send] URL construction failed:", _);
      }
    }
    currentRequestUrl = url;
    if (formValues.fields.length === 0) {
      // No body fields in schema — fall back to raw body (e.g. replayed form-urlencoded body)
      const rawFallback = document.getElementById("send-raw-body").value;
      body = rawFallback
        ? { mode: "raw", formData: null, rawBody: rawFallback, frameId: currentReplayRequest?.frameId }
        : { mode: "form", formData: { fields: [] }, rawBody: null, frameId: currentReplayRequest?.frameId };
    } else {
      body = {
        mode: "form",
        formData: { fields: formValues.fields },
        rawBody: null,
        frameId: currentReplayRequest?.frameId,
      };
    }
  } else if (bodyMode === "graphql") {
    body = {
      mode: "graphql",
      operations: gqlCollectAllOps(),
      batched: gqlState.batched,
      frameId: currentReplayRequest?.frameId,
    };
  } else if (bodyMode === "multipart") {
    body = { ...mpCollectBody(), frameId: currentReplayRequest?.frameId };
  } else {
    body = {
      mode: "raw",
      formData: null,
      rawBody: document.getElementById("send-raw-body").value,
      frameId: currentReplayRequest?.frameId,
    };
  }

  const sel = document.getElementById("send-ep-select");
  const selectedOpt = sel.options[sel.selectedIndex];

  // Route the SEND through the tab+frame that originally captured this
  // request when available. The popup's own active tab has different
  // cookies, CORS origin, and iframe tree — firing there silently breaks
  // cross-tab replay (requests against site B from a popup opened on site
  // A) and iframe-captured requests. Matches the routing already used for
  // WebSocket/postMessage consoles (currentChannelTabId).
  const replayTabId = currentReplayRequest?._tabId != null
    ? currentReplayRequest._tabId
    : currentTabId;
  const replayFrameId = currentReplayRequest?.frameId != null
    ? currentReplayRequest.frameId
    : currentFrameId;

  const msg = {
    type: "SEND_REQUEST",
    tabId: replayTabId,
    endpointKey: epKey,
    service: selectedOpt?.dataset?.svc,
    methodId: selectedOpt?.dataset?.discoveryId,
    url,
    httpMethod,
    contentType,
    headers,
    body,
    frameId: replayFrameId,
    apiKeyOverride: currentKeyOverride,
  };

  try {
    const result = await chrome.runtime.sendMessage(msg);
    renderResponse(result);

    // Scroll result into view
    setTimeout(() => {
      document
        .getElementById("send-response")
        .scrollIntoView({ behavior: "smooth", block: "start" });
    }, 100);
  } catch (err) {
    renderResponse({ error: err.message });
  }

  btn.disabled = false;
  btn.textContent = "Send Request";
}

// ─── Message Console (WebSocket + postMessage) ─────────────────────────────

async function initMsgConsole(req) {
  currentChannelId = req.channelId;
  currentChannelType = req.method; // "WEBSOCKET" or "POSTMESSAGE"
  // For PM reply: target is the sourceOrigin (who sent to us, we reply back to them)
  currentTargetOrigin = req.sourceOrigin || null;
  currentChannelFrameId = req.frameId ?? null;
  // Bind the channel to the tab that captured it. When logFilter=="all"
  // or we're viewing a closed-tab log, `req._tabId` is set to the origin
  // tab during log flattening. Default back to currentTabId when missing
  // (same-tab log view).
  currentChannelTabId = (req._tabId != null) ? req._tabId : currentTabId;
  setBodyMode("msgconsole");

  // Dynamic label based on channel type
  const labelEl = document.querySelector("#send-ws-console .ws-label");
  const urlEl = document.getElementById("ws-console-url");
  if (currentChannelType === "POSTMESSAGE") {
    labelEl.textContent = "postMessage";
    urlEl.textContent = (req.sourceOrigin || "?") + " \u2192 " + (req.targetOrigin || "?");
  } else if (currentChannelType === "MSGCHANNEL") {
    labelEl.textContent = "MessageChannel";
    urlEl.textContent = (req.sourceOrigin || "?") + " \u2192 " + (req.targetOrigin || "?");
  } else {
    labelEl.textContent = "WebSocket";
    urlEl.textContent = req.url;
  }

  // Render all messages from the combined entry
  const historyEl = document.getElementById("ws-console-history");
  historyEl.innerHTML = "";
  const messages = req.messages || [];
  _renderConsoleMessages(historyEl, messages);
  historyEl.scrollTop = historyEl.scrollHeight;

  // Pre-fill input with last sent message for easy re-send
  const input = document.getElementById("ws-console-input");
  const lastSent = messages.filter((m) => m.dir === "sent").pop();
  if (lastSent) {
    let bodyText = lastSent.body;
    if (lastSent.base64) {
      try { bodyText = new TextDecoder().decode(base64ToUint8(lastSent.body)); }
      catch (_) {}
    }
    input.value = bodyText;
  } else {
    input.value = "";
  }

  await refreshMsgConsole();
}

function _renderConsoleMessages(container, messages) {
  let html = "";
  for (const msg of messages) {
    if (msg.dir === "close") {
      const time = new Date(msg.time).toLocaleTimeString();
      html +=
        `<div class="ws-msg ws-msg-close">` +
        `<span class="ws-msg-dir">closed</span> ` +
        `<span class="ws-msg-time">${esc(time)}</span>` +
        (msg.body ? `<pre class="ws-msg-body">${esc(String(msg.body))}</pre>` : "") +
        `</div>`;
      continue;
    }
    let bodyText = msg.body;
    if (msg.base64) {
      try { bodyText = new TextDecoder().decode(base64ToUint8(msg.body)); }
      catch (_) {}
    }
    const dirLabel = msg.dir === "sent" ? "sent" : "received";
    const dirClass = msg.dir === "sent" ? "ws-msg-sent" : "ws-msg-recv";
    const time = new Date(msg.time).toLocaleTimeString();
    html +=
      `<div class="ws-msg ${dirClass}">` +
      `<span class="ws-msg-dir">${esc(dirLabel)}</span> ` +
      `<span class="ws-msg-time">${esc(time)}</span>` +
      `<pre class="ws-msg-body">${esc(bodyText)}</pre></div>`;
  }
  container.innerHTML = html;
}

async function refreshMsgConsole() {
  const statusEl = document.getElementById("ws-console-status");
  const sendBtn = document.getElementById("ws-console-send");
  if (!currentChannelId) {
    statusEl.innerHTML = '<span class="ws-status-closed">NO CONNECTION</span>';
    sendBtn.disabled = true;
    return;
  }

  // Pick the right status message type based on channel
  const statusType = currentChannelType === "POSTMESSAGE" ? "PM_GET_STATUS"
    : currentChannelType === "MSGCHANNEL" ? "MC_GET_STATUS" : "WS_GET_STATUS";
  try {
    const routedTab = currentChannelTabId != null ? currentChannelTabId : currentTabId;
    const result = await chrome.runtime.sendMessage({
      type: statusType, tabId: routedTab, channelId: currentChannelId,
    });

    if (currentChannelType === "POSTMESSAGE" || currentChannelType === "MSGCHANNEL") {
      // postMessage / MessageChannel — always "active", no connection lifecycle
      statusEl.innerHTML = '<span class="ws-status-open">ACTIVE</span>';
      sendBtn.disabled = false;
    } else {
      const names = { 0: "CONNECTING", 1: "OPEN", 2: "CLOSING", 3: "CLOSED" };
      const classes = { 0: "ws-status-connecting", 1: "ws-status-open",
        2: "ws-status-closing", 3: "ws-status-closed" };
      const rs = result.readyState;
      statusEl.innerHTML = `<span class="${classes[rs] || "ws-status-closed"}">${names[rs] || "CLOSED"}</span>`;
      sendBtn.disabled = rs !== 1;
    }

    // Live-update message history
    if (result.messages) {
      const historyEl = document.getElementById("ws-console-history");
      const wasAtBottom = historyEl.scrollTop + historyEl.clientHeight >= historyEl.scrollHeight - 20;
      _renderConsoleMessages(historyEl, result.messages);
      if (wasAtBottom) historyEl.scrollTop = historyEl.scrollHeight;
    }
  } catch (_) {
    statusEl.innerHTML = '<span class="ws-status-closed">UNKNOWN</span>';
    sendBtn.disabled = true;
  }
}

async function sendConsoleMessage() {
  const input = document.getElementById("ws-console-input");
  const sendBtn = document.getElementById("ws-console-send");
  const data = input.value;
  if (!data || !currentChannelId) return;

  sendBtn.disabled = true;
  sendBtn.textContent = "Sending...";
  try {
    // Route through the channel's OWN tab (captured) not the popup's tab.
    const routedTab = currentChannelTabId != null ? currentChannelTabId : currentTabId;
    let msgPayload;
    if (currentChannelType === "POSTMESSAGE") {
      msgPayload = {
        type: "PM_SEND_MSG", tabId: routedTab, channelId: currentChannelId,
        data: data, targetOrigin: currentTargetOrigin || "*",
        frameId: currentChannelFrameId,
      };
    } else if (currentChannelType === "MSGCHANNEL") {
      msgPayload = {
        type: "MC_SEND_MSG", tabId: routedTab, channelId: currentChannelId,
        data: data, frameId: currentChannelFrameId,
      };
    } else {
      msgPayload = {
        type: "WS_SEND_MSG", tabId: routedTab,
        channelId: currentChannelId, data: data,
        frameId: currentChannelFrameId,
      };
    }
    const result = await chrome.runtime.sendMessage(msgPayload);
    if (result.error) {
      const historyEl = document.getElementById("ws-console-history");
      historyEl.innerHTML +=
        `<div class="ws-msg ws-msg-error">` +
        `<span class="ws-msg-dir">error</span> ${esc(result.error)}</div>`;
    }
  } catch (err) {
    const historyEl = document.getElementById("ws-console-history");
    historyEl.innerHTML +=
      `<div class="ws-msg ws-msg-error">` +
      `<span class="ws-msg-dir">error</span> ${esc(err.message)}</div>`;
  }
  sendBtn.textContent = "Send Message";
  await refreshMsgConsole();
}

function renderResponse(result) {
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

  if (result.error && !result.status) {
    document.getElementById("send-response-status").innerHTML = "";
    document.getElementById("send-response-headers").innerHTML = "";
    document.getElementById("send-response-body").innerHTML =
      `<div class="card"><div class="card-label">Error</div><div class="card-value">${esc(result.error)}</div></div>`;
    return;
  }

  const statusEl = document.getElementById("send-response-status");
  const statusClass =
    result.ok || (result.status >= 200 && result.status < 300)
      ? "resp-status-ok"
      : "resp-status-error";
  statusEl.innerHTML =
    `<span class="${statusClass}">${esc(String(result.status))} ${esc(result.statusText || "")}</span>` +
    ` <span class="resp-timing">${result.timing || 0}ms</span>` +
    ` <span class="resp-size">${result.body?.size || 0} bytes</span>`;

  const headersTable = document.getElementById("send-response-headers");
  headersTable.innerHTML = "";
  if (result.headers) {
    for (const [k, v] of Object.entries(result.headers)) {
      headersTable.innerHTML += `<tr><td>${esc(k)}</td><td>${esc(v)}</td></tr>`;
    }
  }

  const bodyEl = document.getElementById("send-response-body");
  if (!result.body) {
    bodyEl.innerHTML = '<div class="hint">No response body</div>';
    return;
  }

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

function renderResultBody(result) {
  // Use schema-aware rendering if possible
  const methodId =
    result.methodId ||
    document.getElementById("send-ep-select").dataset.discoveryId;
  const svc =
    result.service || document.getElementById("send-ep-select").dataset.svc;
  let respSchema = null;

  // Prioritize discovery info returned in the result (most up-to-date)
  const discoveryInfo = result.discovery || tabData?.discoveryDocs?.[svc];

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

  // Check raw body for async chunked format (takes priority — it wraps JSPB)
  const rawBody = result.body.raw || "";
  const respContentType = result.headers?.["content-type"] || "";

  if (isAsyncChunkedResponse(rawBody)) {
    return renderAsyncResponse(
      rawBody,
      { service: result.service || svc, url: currentRequestUrl },
      discoveryInfo?.doc,
    );
  }

  // gRPC-Web: unwrap frames, render protobuf trees
  if (result.body.format === "grpc_web") {
    const grpcBytes = result.body.bytes || (result.body.bytesB64 ? base64ToUint8(result.body.bytesB64) : null);
    if (grpcBytes) {
      return renderGrpcWebResponse(
        grpcBytes,
        { service: result.service || svc, methodId: result.methodId },
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
      { service: result.service || svc },
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
      if (svcData.status === "found") services.push(svcName);
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
        tabId: currentTabId,
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
      tabId: currentTabId,
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

function _shortUrl(u) {
  try {
    var parsed = new URL(u);
    var path = parsed.pathname.replace(/\/$/, "");
    if (!path) return parsed.hostname;
    return parsed.hostname + path;
  } catch (_) {
    return u;
  }
}

function esc(s) {
  if (s == null) return "";
  const d = document.createElement("div");
  d.textContent = String(s);
  return d.innerHTML.replace(/"/g, "&quot;").replace(/'/g, "&#39;");
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
  function pushNested(builder) {
    // builder receives the stack to push into. Inside builder we push
    // items in REVERSE order so they pop correctly.
    builder();
  }

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

// ─── Response Panel (Request Log) — Virtual Scroll ──────────────────────────

function _renderLogCard(req, showTabLabel) {
  const hasProto = !!req.decodedBody;

  // Combined WebSocket entry — show message counts and connection status
  if (req.method === "WEBSOCKET") {
    const msgs = req.messages || [];
    const sentCount = msgs.filter((m) => m.dir === "sent").length;
    const recvCount = msgs.filter((m) => m.dir === "recv").length;
    const statusClass = req.wsOpen ? "ws-status-open" : "ws-status-closed";
    const statusText = req.wsOpen ? "OPEN" : "CLOSED";
    return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge badge-ws">WEBSOCKET</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      <span class="badge ${esc(statusClass)}">${esc(statusText)}</span>
    </div>
    <div class="card-value card-value-mono">${esc(req.url)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      <span class="ws-msg-counts">${sentCount} sent / ${recvCount} received</span>
    </div>
  </div>`;
  }

  // Combined postMessage entry — show origin pair and message counts
  if (req.method === "POSTMESSAGE") {
    const msgs = req.messages || [];
    const sentCount = msgs.filter((m) => m.dir === "sent").length;
    const recvCount = msgs.filter((m) => m.dir === "recv").length;
    const origins = (req.sourceOrigin || "?") + " \u2192 " + (req.targetOrigin || "?");
    return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge badge-pm">POSTMESSAGE</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      <span class="badge ws-status-open">ACTIVE</span>
    </div>
    <div class="card-value card-value-mono">${esc(origins)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      <span class="ws-msg-counts">${recvCount} received${sentCount ? ` / ${sentCount} replied` : ""}</span>
    </div>
  </div>`;
  }

  // Combined MessageChannel entry — show origin pair and message counts
  if (req.method === "MSGCHANNEL") {
    const msgs = req.messages || [];
    const sentCount = msgs.filter((m) => m.dir === "sent").length;
    const recvCount = msgs.filter((m) => m.dir === "recv").length;
    const origins = (req.sourceOrigin || "?") + " \u2192 " + (req.targetOrigin || "?");
    return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge badge-mc">MSGCHANNEL</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      <span class="badge ws-status-open">ACTIVE</span>
    </div>
    <div class="card-value card-value-mono">${esc(origins)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      <span class="ws-msg-counts">${recvCount} received${sentCount ? ` / ${sentCount} sent` : ""}</span>
    </div>
  </div>`;
  }

  return `<div class="card request-card clickable-card mb-8" data-id="${esc(String(req.id))}" data-tab-id="${esc(String(req._tabId))}">
    <div class="card-label flex-between">
      <span>
        <span class="badge ${esc(req.method)}">${esc(req.method)}</span>
        <span class="text-timestamp">${new Date(req.timestamp).toLocaleTimeString()}</span>
        ${showTabLabel ? `<span class="badge badge-tab">${esc(req._tabTitle || "Tab " + req._tabId)}</span>` : ""}
      </span>
      ${getStatusBadge(req.status)}
    </div>
    <div class="card-value card-value-mono">${esc(req.url)}</div>
    <div class="card-meta">
      ${req.service ? `Service: <strong>${esc(req.service)}</strong>` : ""}
      ${hasProto ? ' <span class="badge badge-found">PROTOBUF</span>' : ""}
      ${req.url.includes("batchexecute") ? ' <span class="badge badge-batch">BATCHEXECUTE</span>' : ""}
      ${isGrpcWeb(req.mimeType || req.contentType || "") ? ' <span class="badge badge-grpc">gRPC-WEB</span>' : ""}
      ${isSSE(req.mimeType || "") ? ' <span class="badge badge-sse">SSE</span>' : ""}
      ${isNDJSON(req.mimeType || "") ? ' <span class="badge badge-ndjson">NDJSON</span>' : ""}
      ${isGraphQLUrl(req.url) ? ' <span class="badge badge-graphql">GRAPHQL</span>' : ""}
      ${isMultipartBatch(req.mimeType || "") ? ' <span class="badge badge-multipart">MULTIPART</span>' : ""}
      ${/\/async\//.test(req.url) ? ' <span class="badge badge-batch">ASYNC</span>' : ""}
      ${req.method === "SSE" ? ' <span class="badge badge-sse">SSE</span>' : ""}
    </div>
  </div>`;
}

function _renderVisibleSlice() {
  if (_vs.rendering) return;
  const container = document.getElementById("response-log");
  const scrollEl = document.getElementById("panel-response");
  const n = _vs.entries.length;
  if (!n) return;

  const rh = _vs.estHeight;
  const buf = _vs.buffer;
  // Subtract the header/search offset so index math is relative to the first card
  const logOffset = container.offsetTop;
  const scrollTop = Math.max(0, scrollEl.scrollTop - logOffset);
  const viewH = scrollEl.clientHeight;

  // Calculate visible range using fixed row height (no feedback loop)
  let startIdx = Math.max(0, Math.floor(scrollTop / rh) - buf);
  let endIdx = Math.min(n - 1, Math.ceil((scrollTop + viewH) / rh) + buf);

  // Skip re-render if visible range unchanged
  if (startIdx === _vs.lastStart && endIdx === _vs.lastEnd) return;

  _vs.lastStart = startIdx;
  _vs.lastEnd = endIdx;

  _vs.rendering = true;

  const topPad = startIdx * rh;
  const totalHeight = n * rh;
  const showTabLabel = logFilter !== "active";

  // Fixed minHeight keeps scroll range stable regardless of actual card heights
  container.style.minHeight = totalHeight + "px";

  let html = '<div id="vs-top-spacer"></div>';
  for (let i = startIdx; i <= endIdx; i++) {
    html += _renderLogCard(_vs.entries[i], showTabLabel);
  }

  container.innerHTML = html;
  document.getElementById("vs-top-spacer").style.height = topPad + "px";

  // Attach click handlers
  container.querySelectorAll(".request-card").forEach((c) => {
    c.onclick = () => {
      const sourceTabId = c.dataset.tabId ? parseInt(c.dataset.tabId, 10) : undefined;
      replayRequest(c.dataset.id, sourceTabId);
    };
  });

  // Self-correct estHeight from measured card heights to prevent scroll oscillation
  const cards = container.querySelectorAll(".request-card");
  if (cards.length > 0) {
    let totalH = 0;
    cards.forEach((c) => { totalH += c.offsetHeight + 8; });
    const avgH = Math.round(totalH / cards.length);
    if (Math.abs(avgH - _vs.estHeight) > 10) {
      _vs.estHeight = avgH;
      container.style.minHeight = (n * avgH) + "px";
    }
  }

  _vs.rendering = false;
}

function renderResponsePanel() {
  const container = document.getElementById("response-log");
  const scrollEl = document.getElementById("panel-response");

  // Build entry list based on filter mode
  let entries = [];
  if (logFilter === "active") {
    entries = (tabData?.requestLog || []).map((r) => ({ ...r, _tabId: currentTabId }));
  } else if (allTabsData) {
    for (const [tidStr, data] of Object.entries(allTabsData)) {
      const tid = parseInt(tidStr, 10);
      const meta = data.meta || {};
      for (const req of data.requestLog) {
        entries.push({ ...req, _tabId: tid, _tabTitle: meta.title || `Tab ${tid}` });
      }
    }
    entries.sort((a, b) => b.timestamp - a.timestamp);
  }

  // Apply text search filter
  if (logSearchQuery) {
    entries = entries.filter((r) => {
      return (r.url && r.url.toLowerCase().includes(logSearchQuery)) ||
        (r.method && r.method.toLowerCase().includes(logSearchQuery)) ||
        (r.service && r.service.toLowerCase().includes(logSearchQuery)) ||
        (r.mimeType && r.mimeType.toLowerCase().includes(logSearchQuery)) ||
        (r._tabTitle && r._tabTitle.toLowerCase().includes(logSearchQuery));
    });
  }

  // Fingerprint: skip full rebuild if entries unchanged
  const lastId = entries.length > 0 ? (entries[entries.length - 1].id || entries[entries.length - 1].timestamp) : "";
  const fp = entries.length + ":" + lastId;
  if (fp === _lastLogFp && _vs.scrollHandler) return;
  _lastLogFp = fp;

  // Detach old scroll handler if entries changed
  if (_vs.scrollHandler) {
    scrollEl.removeEventListener("scroll", _vs.scrollHandler);
    _vs.scrollHandler = null;
  }

  _vs.entries = entries;
  _vs.lastStart = -1;
  _vs.lastEnd = -1;

  if (entries.length === 0) {
    container.style.minHeight = "";
    container.innerHTML = '<div class="empty">No requests captured yet.</div>';
    return;
  }

  // Render initial visible slice
  _renderVisibleSlice();

  // Attach scroll-driven rendering
  let rafPending = false;
  _vs.scrollHandler = () => {
    if (rafPending) return;
    rafPending = true;
    requestAnimationFrame(() => {
      rafPending = false;
      _renderVisibleSlice();
    });
  };
  scrollEl.addEventListener("scroll", _vs.scrollHandler, { passive: true });
}

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
          ? `<span class="badge badge-found">${part.status}</span>`
          : `<span class="badge badge-notfound">${part.status}</span>`)
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

function getStatusBadge(status) {
  if (status === "pending")
    return '<span class="badge badge-pending">pending</span>';
  if (status === "error") return '<span class="badge badge-error">error</span>';
  const code = parseInt(status);
  if (code >= 200 && code < 300)
    return `<span class="badge badge-found">${code}</span>`;
  if (code >= 400) return `<span class="badge badge-notfound">${code}</span>`;
  return `<span class="badge">${code}</span>`;
}

let currentReplayRequest = null;

async function replayRequest(reqId, sourceTabId) {
  // Clear previous send result
  lastSendResult = null;
  const sendResp = document.getElementById("send-response");
  if (sendResp) sendResp.style.display = "none";

  // Search the correct log source
  let req;
  if (sourceTabId && allTabsData && allTabsData[sourceTabId]) {
    req = allTabsData[sourceTabId].requestLog.find((r) => String(r.id) === String(reqId));
  }
  if (!req) {
    req = tabData?.requestLog?.find((r) => String(r.id) === String(reqId));
  }
  if (!req) {
    console.error(`[Replay] Request ${reqId} not found in log`);
    return;
  }
  // Stamp the origin tab so downstream sendRequest routes through it.
  // requestLog entries inside allTabsData are NOT pre-annotated — only the
  // flattened view in renderResponsePanel adds _tabId. Without this stamp,
  // currentReplayRequest._tabId is undefined and Send falls back to the
  // popup's own active tab, silently sending cross-tab replays to the
  // wrong origin.
  if (sourceTabId != null && req._tabId == null) req._tabId = sourceTabId;
  currentReplayRequest = req;

  // Auto-select the frame this request came from (if available)
  if (req.frameId != null && availableFrames.length > 1) {
    var _frameMatch = availableFrames.find(function(f) { return f.frameId === req.frameId; });
    if (_frameMatch) {
      currentFrameId = req.frameId;
      var _frameSel = document.getElementById("send-frame-select");
      if (_frameSel) _frameSel.value = String(req.frameId);
    }
  }

  // Message console mode: WebSocket or postMessage
  if (req.method === "WEBSOCKET" || req.method === "POSTMESSAGE" || req.method === "MSGCHANNEL") {
    // Ensure the captured channel's origin tab is propagated — the click
    // path reaches here with the sourceTabId argument, which is what
    // initMsgConsole needs to route all subsequent SW calls correctly.
    // Without this, the popup's own tab id would be used, and the SW
    // couldn't find the captured entry.
    if (sourceTabId != null && req._tabId == null) req._tabId = sourceTabId;
    await initMsgConsole(req);
    document.querySelector(".tab[data-panel='send']").click();
    return;
  }

  // Try to find and select the matching endpoint in the dropdown to load the schema
  const epSelect = document.getElementById("send-ep-select");
  let found = false;

  if (epSelect) {
    // Check if it's a batchexecute request
    const isBatch = req.url.includes("batchexecute");
    let targetRpcId = null;

    if (isBatch && req.rawBodyB64) {
      const bytes = base64ToUint8(req.rawBodyB64);
      const text = new TextDecoder().decode(bytes);
      const calls = parseBatchExecuteRequest(text);
      if (calls && calls.length > 0) {
        targetRpcId = calls[0].rpcId;
      }
    }

    // Set current state from the replayed request
    currentRequestUrl = req.url;
    currentRequestMethod = "POST"; // Default for replayed backend requests or detect from req
    if (req.method) currentRequestMethod = req.method;

    for (const opt of epSelect.options) {
      if (opt.dataset.isVirtual === "true" && opt.dataset.svc === req.service) {
        // 1. For batch, attempt strict RPC ID match first
        if (isBatch && targetRpcId) {
          if (
            opt.dataset.discoveryId &&
            opt.dataset.discoveryId.endsWith("." + targetRpcId)
          ) {
            opt.selected = true;
            found = true;
            break;
          }
          // If it's a batch request but this option isn't the right RPC ID,
          // skip further checks for this option to avoid incorrect path matching.
          continue;
        }

        // 2. Match by methodId AND HTTP verb — the same methodId covers
        //    both GET and POST probe variants of an endpoint, so a methodId-
        //    only match picks the first declared variant (often POST) even
        //    when replaying a captured GET, misattributing the schema.
        if (req.methodId && opt.dataset.discoveryId === req.methodId &&
            opt.dataset.method === req.method) {
          opt.selected = true;
          found = true;
          break;
        }

        // 3. Fallback for path-based matching (Non-batch only) — also
        //    filtered by HTTP verb, same rationale as (2).
        if (!isBatch) {
          try {
            const reqPath = new URL(req.url).pathname;
            if (opt.dataset.path && reqPath.endsWith(opt.dataset.path) &&
                opt.dataset.method === req.method) {
              opt.selected = true;
              found = true;
              break;
            }
          } catch (e) {
            console.warn("[Replay] URL path resolution failed:", e);
          }
        }
      }
    }

    if (found) {
      const svc = req.service;
      const selectedOpt = epSelect.options[epSelect.selectedIndex];
      const discoveryId = selectedOpt.dataset.discoveryId;

      epSelect.dataset.svc = svc;
      epSelect.dataset.discoveryId = discoveryId;

      // Extract the correct initial data for the selected RPC call
      let initialData = null;
      if (isBatch && req.rawBodyB64) {
        const bytes = base64ToUint8(req.rawBodyB64);
        const text = new TextDecoder().decode(bytes);
        const calls = parseBatchExecuteRequest(text);
        if (calls && calls.length > 0) {
          initialData = jspbToTree(
            Array.isArray(calls[0].data) ? calls[0].data : [calls[0].data],
          );
          initialData = pbTreeToMap(initialData);
        }
      } else if (req.isJson && req.decodedBody) {
        // JSON body — use parsed object directly as named-key initialData
        initialData = req.decodedBody;
      } else if (req.decodedBody) {
        initialData = pbTreeToMap(req.decodedBody) || {};
      } else if (req.rawBodyB64) {
        // Try to extract f.req JSPB from form-urlencoded body
        try {
          const bodyBytes = base64ToUint8(req.rawBodyB64);
          const bodyText = new TextDecoder().decode(bodyBytes);
          const bodyParams = new URLSearchParams(bodyText);
          const fReq = bodyParams.get("f.req");
          if (fReq) {
            const parsed = JSON.parse(fReq);
            if (Array.isArray(parsed)) {
              initialData = pbTreeToMap(jspbToTree(parsed));
            }
          }
        } catch (e) {
          // f.req JSPB extraction failed — expected for non-Google bodies.
          // Falls through to plain-JSON attempt below; debug-log so a real
          // serialization-shape bug is visible without spamming.
          console.debug("[popup:send_load] f.req JSPB parse failed:", e && e.message || e);
        }
        // Also try plain JSON body
        if (!initialData) {
          try {
            const bodyBytes = base64ToUint8(req.rawBodyB64);
            const bodyText = new TextDecoder().decode(bodyBytes);
            const json = JSON.parse(bodyText);
            if (json && typeof json === "object" && !Array.isArray(json)) {
              initialData = json;
            }
          } catch (e) {
            // Plain JSON parse failed — body might be form-encoded or binary.
            // The fallthrough `initialData = {}` below is the correct empty
            // form state for "couldn't parse anything meaningful."
            console.debug("[popup:send_load] plain JSON parse failed:", e && e.message || e);
          }
        }
        if (!initialData) initialData = {};
      } else {
        initialData = {};
      }

      // Merge query parameters from URL for pre-filling
      try {
        const urlObj = new URL(req.url);
        urlObj.searchParams.forEach((val, key) => {
          // $httpHeaders is a gRPC-Web transport param with CRLF separators —
          // putting it through a text input strips \r\n and corrupts the URL.
          if (key === "$httpHeaders" || key === "$ct") return;
          // Prefer body data if it already provides this key, but for GET it's usually just URL params
          if (initialData[key] === undefined || initialData[key] === null) {
            initialData[key] = val;
          }
        });
      } catch (e) {
        console.warn("[Replay] Failed to extract URL parameters:", e);
      }

      await loadVirtualSchema(svc, discoveryId, initialData);
    }
  }

  // Auto-determine Content-Type from the original request (AFTER schema load)
  if (req.requestHeaders) {
    const ctHeader = Object.keys(req.requestHeaders).find(
      (k) => k.toLowerCase() === "content-type",
    );
    if (ctHeader) {
      currentContentType = req.requestHeaders[ctHeader];
    } else {
      currentContentType = "application/json";
    }
  }

  // Auto-determine body mode
  let gqlDetected = false;
  let mpDetected = false;

  // Multipart has precedence when the captured body's content-type is
  // multipart/*. Each sub-part gets its own contextual editor; the flat
  // form builder can only express one part so we must bypass it.
  const reqCt = req.contentType || req.mimeType || "";
  if (reqCt.toLowerCase().startsWith("multipart/") && req.rawBodyB64) {
    if (mpLoadFromCapturedRequest(req)) {
      mpDetected = true;
      setBodyMode("multipart");
      gqlClear();
    }
  }

  if (!mpDetected && isGraphQLUrl(req.url) && req.rawBodyB64) {
    try {
      const bytes = base64ToUint8(req.rawBodyB64);
      const text = new TextDecoder().decode(bytes);
      const gqlReq = parseGraphQLRequest(text);
      if (gqlReq) {
        gqlDetected = true;
        setBodyMode("graphql");
        gqlLoadOperations(gqlReq.operations, gqlReq.batched);
      }
    } catch (e) {
      // isGraphQLUrl heuristically matched (often false-positive for
      // /graphql-shaped URLs that aren't real GraphQL), so the body might
      // not be a GraphQL envelope. parseGraphQLRequest returning null is the
      // normal "not GraphQL" signal; a THROW here means something unexpected.
      console.debug("[popup:send_load] GraphQL body load threw:", e && e.message || e);
    }
  }
  if (!gqlDetected && !mpDetected) {
    // Form mode if schema was loaded, otherwise raw
    setBodyMode(currentSchema ? "form" : "raw");
    gqlClear();
    mpClear();
  }
  // Add headers (filtering out Content-Type which is auto-determined)
  const headersList = document.getElementById("send-headers-list");
  headersList.innerHTML = "";
  if (req.requestHeaders) {
    for (const [k, v] of Object.entries(req.requestHeaders)) {
      if (k.toLowerCase() === "content-type") continue;
      addHeaderRow(k, v);
    }
  }

  // Populate raw body textarea with original body as fallback
  document.getElementById("send-raw-body").value = "";
  if (req.rawBodyB64 && !gqlDetected) {
    try {
      const bytes = base64ToUint8(req.rawBodyB64);
      document.getElementById("send-raw-body").value = new TextDecoder().decode(bytes);
    } catch (e) {
      // base64 decode or UTF-8 decode failed — the rawBodyB64 is malformed.
      // The textarea stays empty (correct fallback for "can't display") but
      // surface so a binary/corrupt body capture is diagnosable.
      console.warn("[popup:send] raw body decode failed:", e && e.message || e);
    }
  }
  // Populate historical response if available
  if (req.responseBody || req.status) {
    const historicalResult = {
      ok: parseInt(req.status) < 400,
      status: req.status,
      headers: req.responseHeaders,
      timing: 0,
      service: req.service,
      methodId: req.methodId,
      body: null,
    };

    if (req.responseBody) {
      const mimeType = req.mimeType || "";
      const isBinaryProtobuf =
        (mimeType.includes("protobuf") && !mimeType.includes("json")) ||
        (req.requestHeaders &&
          Object.entries(req.requestHeaders).some(
            ([k, v]) =>
              k.toLowerCase() === "content-type" &&
              v.toLowerCase().includes("protobuf") &&
              !v.toLowerCase().includes("json"),
          ));
      const isJspb = mimeType.includes("json+protobuf") ||
        mimeType.includes("json; protobuf");

      let bodyText = req.responseBody;
      if (req.responseBase64) {
        try {
          const bytes = base64ToUint8(req.responseBody);
          bodyText = new TextDecoder().decode(bytes);
        } catch (e) {
          // base64 / UTF-8 decode of historical body failed — bodyText stays
          // as the raw base64 string. Downstream format parsing will likely
          // also fail (and log), but surface here for traceability.
          console.warn("[popup:historical] base64 decode failed for %s body: %s", mimeType, e && e.message || e);
        }
      }

      if (isGrpcWeb(mimeType)) {
        // gRPC-Web: pass raw bytes for frame parsing
        try {
          let bytes;
          if (isGrpcWebText(mimeType)) {
            bytes = base64ToUint8(
              req.responseBase64 ? req.responseBody : btoa(req.responseBody),
            );
          } else {
            bytes = req.responseBase64
              ? base64ToUint8(req.responseBody)
              : new TextEncoder().encode(req.responseBody);
          }
          historicalResult.body = {
            format: "grpc_web",
            bytes,
            raw: bodyText,
            size: bytes.length,
          };
        } catch (e) {
          // gRPC-Web frame extraction threw — surface so a corrupt captured
          // body that the MIME type promised but couldn't deliver is visible.
          console.debug("[popup:historical] gRPC-Web parse failed:", e && e.message || e);
        }
      } else if (isJspb) {
        // JSPB (JSON+Protobuf): parse as JSON, convert to protobuf tree
        try {
          const parsed = JSON.parse(bodyText);
          if (Array.isArray(parsed)) {
            historicalResult.body = {
              format: "protobuf_tree",
              parsed: jspbToTree(parsed),
              raw: bodyText,
              size: bodyText.length,
              isJspb: true,
            };
          }
        } catch (e) {
          console.debug("[popup:historical] JSPB parse failed:", e && e.message || e);
        }
      } else if (isBinaryProtobuf) {
        try {
          const bytes = req.responseBase64
            ? base64ToUint8(req.responseBody)
            : new TextEncoder().encode(req.responseBody);
          historicalResult.body = {
            format: "protobuf_tree",
            parsed: pbDecodeTree(bytes),
            raw: req.responseBody,
            size: bytes.length,
          };
        } catch (e) {
          console.debug("[popup:historical] binary protobuf parse failed:", e && e.message || e);
        }
      } else if (mimeType.includes("json") || mimeType.includes("text/plain") || mimeType.includes("javascript")) {
        try {
          // Strip Google XSSI prefix before parsing
          let jsonText = bodyText;
          if (jsonText.trimStart().startsWith(")]}'")) {
            jsonText = jsonText.trimStart().substring(4).trimStart();
          }
          // Strip JSONP wrapper: callbackName({...}) → {...}
          if (mimeType.includes("javascript")) {
            var _jpM = /^[a-zA-Z_$][\w$.]*\s*\(\s*/.exec(jsonText);
            if (_jpM) {
              var _jpInner = jsonText.slice(_jpM[0].length);
              var _jpEnd = _jpInner.lastIndexOf(")");
              if (_jpEnd !== -1) jsonText = _jpInner.slice(0, _jpEnd).trim();
            }
          }
          const parsed = JSON.parse(jsonText);
          // Detect JSPB in text/plain responses (Google returns these)
          if (Array.isArray(parsed) && parsed.length > 0 &&
              parsed.some((item) => item === null || Array.isArray(item) || typeof item !== "object")) {
            historicalResult.body = {
              format: "protobuf_tree",
              parsed: jspbToTree(parsed),
              raw: bodyText,
              size: bodyText.length,
              isJspb: true,
            };
          } else {
            historicalResult.body = {
              format: "json",
              parsed,
              raw: bodyText,
              size: bodyText.length,
            };
          }
        } catch (e) {
          // JSON parse failure on a body the MIME type tagged as JSON/text/JS
          // — common (servers misreport content-type), surface at debug so it's
          // diagnosable without spamming on every misreported body.
          console.debug("[popup:historical] JSON parse failed for %s body: %s", mimeType, e && e.message || e);
        }
      }

      // SSE, NDJSON, multipart, and async chunked are detected by renderResultBody()
      // via content-type headers and body inspection — just need raw text preserved.
      if (!historicalResult.body) {
        historicalResult.body = {
          format: "text",
          raw: bodyText,
          size: bodyText.length,
        };
      }
    }

    renderResponse(historicalResult);
    document.getElementById("send-response-status").innerHTML +=
      ' <span class="badge badge-source ml-8">Historical</span>';
  } else {
    document.getElementById("send-response").style.display = "none";
  }

  // Switch tab
  document.querySelector(".tab[data-panel='send']").click();
}

document.getElementById("btn-clear-log").addEventListener("click", async () => {
  if (logFilter === "active") {
    if (tabData) tabData.requestLog = [];
    await chrome.runtime.sendMessage({ type: "CLEAR_LOG", tabId: currentTabId });
  } else if (logFilter === "all") {
    allTabsData = null;
    if (tabData) tabData.requestLog = [];
    await chrome.runtime.sendMessage({ type: "CLEAR_LOG", clearAll: true });
  } else {
    // Clearing a specific tab
    const targetTabId = logFilter;
    if (allTabsData && allTabsData[targetTabId]) {
      delete allTabsData[targetTabId];
    }
    if (targetTabId === currentTabId && tabData) {
      tabData.requestLog = [];
    }
    await chrome.runtime.sendMessage({ type: "CLEAR_LOG", tabId: targetTabId });
  }
  renderResponsePanel();
  populateTabFilter();
});
