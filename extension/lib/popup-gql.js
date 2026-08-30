/* Popup GraphQL operation editor — extracted from popup.js (classic script, shares the popup's global
   scope: gqlState + the DOM). Builds/renders the per-operation query+variables panels and collects them
   into the send request. One concern, one file — the popup.js de-monolithization (was 5445 lines). */
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
  /* THE ALIAS IS A THIRD-PARTY DOCUMENT'S TEXT. `aliasProps` is a discovery-document schema this zone stored
     — the reviewer's own renames land in it, but so does whatever an imported spec or a fetched document put
     there — so the name is REFUSED through lib/field-def.js rather than taken: a `name` that is not text is
     a document making no rename claim, and the wire key is then what the panel renders (`displayName: null`,
     which the record already states). The rest of the record is synthesizeFieldDefFromValue's, built through
     makeFieldDef; this walk only writes names that record declares. */
  function applyAliasesIterative(rootDef) {
    const queue = [rootDef];
    while (queue.length > 0) {
      const fd = queue.shift();
      fd.parentSchema = schemaName;
      const ap = aliasProps === null ? null : fdDocRecord(aliasProps[fd.name]);
      const alias = (ap !== null && ap.customName === true) ? fdDocString(ap.name) : null;
      if (alias !== null) {
        fd.displayName = alias;
        fd.customName = true;
      }
      if (fd.children !== null) {
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
