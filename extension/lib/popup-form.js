/* Popup form-field builder — extracted from popup.js (classic script, shares the popup global scope +
   DOM). Builds the Send-panel input UI from a request schema: buildFormFields + the recursive field/
   message/repeated builders (_buildFieldStep/_buildMessageGroup/...) + createSingleInput. Called by
   renderFieldsTable / the send panel via the global scope; formFieldsToMap (value collection) stays. */
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
