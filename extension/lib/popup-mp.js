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

/* THE MULTIPART BODY-PART RECORD — ONE CONTRACT, THREE HOPS, STATED HERE AND ASSERTED AT EACH HOP.
 *
 * A part is minted by exactly three producers — parseMultipartBatchRequest's per-sub-request results, the
 * generic boundary split below, and the Send panel's "+ Part" button — is then edited IN PLACE by the card's
 * inputs, and is finally serialized by mpCollectBody into the record the offscreen's reassembly reads. Every
 * one of those producers writes EVERY field of the shape below, so a reader that fills a hole with `||` is
 * not tolerating an omission that happens: it is substituting for a value the OPERATOR chose, in a request
 * whose entire purpose is to reproduce one the page made, and the substitution is invisible in the envelope
 * that goes out. That is why the contract is asserted rather than defended against.
 *
 * `contentType` is the field that made this worth stating, because it is the one whose EMPTY value is a real
 * state rather than a hole. An empty string is a POSITIVE STATEMENT — this part declares no Content-Type,
 * either because the captured part carried no such header or because the operator cleared the box — and RFC
 * 2046 §5.1.1 Common Syntax says exactly what that means on the wire: "If no Content-Type field is present
 * it is assumed to be `message/rfc822` in a `multipart/digest` and `text/plain` otherwise". So the absence is
 * REPRODUCED (the header is omitted and the receiver applies that default), never filled. Filling it with
 * `application/octet-stream` — which both the loader and the reassembly did, independently — asserted the
 * OPPOSITE of the RFC's own default, and did it in the one place that is meant to be a faithful replay.
 *
 * HOW ITS ABSENCE WOULD SHOW, had these asserts not existed: a captured part with no Content-Type header
 * comes back from the Send panel carrying one, and a server that branches on the part type answers a request
 * the page never made.
 */
function _mpCheckPart(p, where) {
  DCHECK(!!p && typeof p === "object", "a multipart part is not an object " + where);
  DCHECK(typeof p.partNumber === "number" && p.partNumber > 0,
         "a multipart part reached " + where + " with no partNumber — every producer numbers its parts, and " +
         "the number is what the card and the operator refer to the part by");
  /* PAIRED, not independently optional: both come out of one request-line match in the batch parser, and the
     generic split writes both null. A part with a method and no path is a parser that half-matched. */
  DCHECK((p.method === null) === (p.path === null),
         "a multipart part reached " + where + " with method and path disagreeing about whether it is an " +
         "embedded HTTP sub-request — they are matched together or not at all");
  DCHECK(p.method === null || (typeof p.method === "string" && typeof p.path === "string"),
         "a multipart part reached " + where + " with a non-string method/path");
  DCHECK(typeof p.contentType === "string",
         "a multipart part reached " + where + " with no contentType string — empty IS the value when the " +
         "part declares no type (RFC 2046 §5.1.1 Common Syntax), so an absent field is a broken producer");
  DCHECK(!!p.extraHeaders && typeof p.extraHeaders === "object",
         "a multipart part reached " + where + " with no extraHeaders object — every producer writes one " +
         "(empty when the part had no headers beyond Content-Type)");
  DCHECK(p.contentId === null || typeof p.contentId === "string",
         "a multipart part reached " + where + " with a contentId that is neither a string nor the null that " +
         "states the part carried no Content-ID");
  DCHECK(!!p.editor && typeof p.editor === "object" && typeof p.editor.kind === "string" &&
         typeof p.editor.value === "string",
         "a multipart part reached " + where + " with no editor {kind,value} — the editor's value IS the " +
         "part body, so an absent one is a body the operator typed and this panel would send none of");
}

function mpClassifyPartBody(ct, bodyText) {
  /* Both operands are the part record's own fields, so both are strings by the contract above; the empty
     Content-Type falls through to "raw", which is the right editor for a part that declares no type. */
  DCHECK(typeof ct === "string" && typeof bodyText === "string",
         "mpClassifyPartBody was asked to classify a part whose Content-Type or body is not a string");
  const lct = ct.toLowerCase();
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
      /* THE PARSER'S OWN CONTRACT, ASSERTED WHERE IT IS ADOPTED. parseMultipartBatchRequest writes all five
         names on every result it returns — `contentId` already as the string-or-null this record wants — so
         re-stating `|| null` here was a SECOND spelling of a decision the parser had already made, and the
         one place where a parser that stopped writing the field would have gone unnoticed. */
      DCHECK(typeof bp.method === "string" && typeof bp.path === "string" &&
             !!bp.headers && typeof bp.headers === "object" && typeof bp.body === "string" &&
             (bp.contentId === null || typeof bp.contentId === "string"),
             "parseMultipartBatchRequest returned a sub-request that is not {method,path,headers,body," +
             "contentId} — the Send panel adopts those five names as a part and can substitute for none of them");
      /* AN ABSENT PART Content-Type IS REPRODUCED AS ABSENT (""), never stamped. See the record contract above:
         RFC 2046 §5.1.1 Common Syntax makes a typeless body part text/plain, and octet-stream said the
         opposite about a part the page sent with no header at all. */
      const partCt = "content-type" in bp.headers ? bp.headers["content-type"] : "";
      const kind = mpClassifyPartBody(partCt, bp.body);
      parts.push({
        partNumber: i + 1,
        method: bp.method,
        path: bp.path,
        contentType: partCt,
        extraHeaders: { ...bp.headers },
        contentId: bp.contentId,
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
      const partCt = "content-type" in headers ? headers["content-type"] : "";   // absent stays absent — RFC 2046 §5.1.1
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

  for (const p of parts) _mpCheckPart(p, "out of mpLoadFromCapturedRequest");
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
  _mpCheckPart(p, "at the part card for index " + index);
  const card = document.createElement("div");
  card.className = "mp-part-card";
  card.dataset.mpPartIdx = String(index);

  const head = document.createElement("div");
  head.className = "mp-part-head";
  head.innerHTML =
    '<span class="mp-part-num">Part ' + esc(String(p.partNumber)) + '</span>' +
    (p.method !== null ? ' <span class="badge">' + esc(p.method) + ' ' + esc(p.path) + '</span>' : '') +
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
  /* Pretty-print on load if valid JSON. AN EMPTY PART BODY IS EMPTY, not `null`: `|| "null"` here parsed the
     empty string as the JSON literal and pretty-printed the word "null" into the box, which the first
     keystroke then committed as the part's body — a value nothing observed, typed into a request meant to
     reproduce one the page made. */
  if (part.editor.value === "") {
    ta.value = "";
  } else {
    try {
      const obj = JSON.parse(part.editor.value);
      ta.value = JSON.stringify(obj, null, 2);
    } catch (_) { ta.value = part.editor.value; }
  }
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
  /* `|| "{}"` handed the parser a fabricated empty envelope for an empty part body; an empty body simply has
     no operations to split, which is what the `op` fallback below already says. */
  try { parsed = part.editor.value === "" ? null : parseGraphQLRequest(part.editor.value); }
  catch (e) {
    // The body might not be a GraphQL request at all (user typing free-form
    // before structured editing kicks in) — that's expected and routes through
    // the `op` fallback below using the raw editor value. But a real parse
    // throw (vs returning null) means something unexpected; surface at debug
    // so a malformed-envelope diagnosis is possible without spamming the
    // common "free-form input" case (parseGraphQLRequest returns null there).
    console.debug("[popup:gql_editor] parseGraphQLRequest threw:", e && e.message || e);
  }
  const op = parsed?.operations?.[0] || { query: part.editor.value, variables: null, operationName: null };

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
  const params = new URLSearchParams(part.editor.value);
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
  /* The typeless part gets told what it IS rather than shown a blank where a media type would be — the two
     states reach this editor by the same route and would otherwise read as one. */
  hint.textContent = part.contentType === ""
    ? "This part declares no Content-Type (RFC 2046 §5.1.1 makes it text/plain); editing as raw text."
    : "No typed editor for Content-Type " + part.contentType + "; editing as raw text.";
  const ta = document.createElement("textarea");
  ta.rows = 6;
  ta.value = part.editor.value;
  ta.oninput = () => { part.editor.value = ta.value; };
  wrap.appendChild(hint);
  wrap.appendChild(ta);
  return wrap;
}

/* Serialize mpState into the WIRE part record buildExportRequest reassembles: the editor-side record above
   with `editor` flattened into `body`+`kind`, and NOTHING ELSE changed. This is the last place the popup can
   state the contract before it crosses sendMessage, so it states it in full for every part — including the
   empty `contentType` that says the part declares no type, which the reassembly reproduces by omitting the
   header rather than by choosing one. `extraHeaders || {}` stood here and was the same substitution one hop
   early: every producer writes the object, so the `||` could only ever have hidden one that stopped. */
function mpCollectBody() {
  return {
    mode: "multipart",
    parts: mpState.parts.map((p, i) => {
      _mpCheckPart(p, "on its way onto the wire (part index " + i + ")");
      return {
        contentType: p.contentType,
        method: p.method,
        path: p.path,
        extraHeaders: p.extraHeaders,
        contentId: p.contentId,
        body: p.editor.value,
        kind: p.editor.kind,
      };
    }),
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
