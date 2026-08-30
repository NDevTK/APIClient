/* Popup form-field builder — extracted from popup.js (classic script, shares the popup global scope +
   DOM). Builds the Send-panel input UI from a request schema: buildFormFields + the recursive field/
   message/repeated builders (_buildFieldStep/_buildMessageGroup/...) + createSingleInput. Called by
   the send panel via the global scope; formFieldsToMap (value collection) stays. */
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
      /* THE HEADER VOCABULARY IS ASSERTED, NOT PATTERN-MATCHED. `hv.kind === "literal"` on an untranslated
         value is FALSE rather than an error, so the header renders as "dynamic — paste runtime value" and the
         auto-attach loop below drops it — a value the engine actually COMPUTED, shown as one it could not.
         That is not hypothetical: lib/learn.js records it happening, because merge.js put endpoint.c's flat
         name -> STRING record on the endpoint entry untranslated while this reader spoke {kind, value}. One
         translation now exists (`astHeaderRecord`, at the boundary, for both producers), so a value reaching
         here in the other vocabulary is that boundary being bypassed again and it says so. */
      DCHECK(!!hv && (hv.kind === "literal" || hv.kind === "opaque") && typeof hv.value === "string",
             "a learned required header reached the Send panel in the wrong vocabulary (" + hn + " = "
             + JSON.stringify(hv) + ") — lib/learn.js `astHeaderRecord` translates endpoint.c's flat "
             + "name -> string record into {kind:\"literal\"|\"opaque\", value} for BOTH producers, so anything "
             + "else here bypassed it, and this panel would show a header the engine computed as one it could not");
      if (hv.kind === "literal") {
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
    /* EVERY `param.*` BELOW IS READ AS ITSELF, because lib/send.js's `resolveEndpointSchema` now declares the
       WHOLE parameter record from BOTH of its producers — the discovery-doc branch and the AST-learned
       path-segment branch — instead of one shape from one and five fields from the other. The `|| null` /
       `?? null` that used to stand on `enum`, `_range`, `_defaultValue`, the confidences and the example
       source were reading that disagreement, and they answered it with the same bytes the doc-derived branch
       uses for "unconstrained": so a path parameter, about whose domain that branch says NOTHING, rendered
       identically to a parameter a discovery document declared as unconstrained. Two different claims, one
       appearance — §@S's own rule about a shape that carries provenance and drops the domain, performed on
       the panel that renders it. `null` now MEANS "no such constraint was observed", from either producer,
       and an absent field means the producer is broken. */
    for (const [name, param] of Object.entries(schema.parameters)) {
      section.appendChild(
        createFieldInput(
          name,
          makeFieldDef({
            name: param.name, // Pass the name (which might be an alias)
            /* NO `displayName` FROM `param._sourceMapName`. It promised the minified-to-declared rename
               (`e` → `owner`) on this label and nothing has ever written the field — no engine emits a
               source-map name — so the rename has never been applied to a parameter. `displayName` itself is
               real and stays: lib/popup-gql.js writes it for a GraphQL alias, which createFieldInput renders. */
            type:
              param.type === "integer"
                ? "int32"
                : param.type === "boolean"
                  ? "bool"
                  : "string",
            required: param.required,
            description: param.description,
            label: param.required ? "required" : "optional",
            /* THE PARAMETER'S OWN NUMBER, where an imported spec declared one. It used to be a hard `null`
               here, which stated that a parameter is never numbered — false of exactly the case
               lib/openapi-import.js exists to serve, and the reason an imported `x-field-numbers` entry
               survived the store and vanished at the panel. `null` still means "not numbered"; it now comes
               from the producer rather than from this line's assumption about it. */
            number: param.number,
            /* `format` IS THE PARAMETER'S, NOT THE RECORD'S DECLARED ABSENCE. resolveEndpointSchema carries
               it from the discovery declaration, and omitting it here rendered every declared `date-time` /
               `int64` parameter as one whose document said nothing about its form. */
            format: param.format,
            messageType: null,
            children: null,
            enum: param.enum,
            location: param.location,
            parentSchema: "params",
            _astValidValues: param._astValidValues,
            /* NO `_astValueSource`. lib/send.js stopped projecting it (its only writer wrote it onto its own
               path-param entries, and nothing ever rendered it), so this line carried undefined into the
               field def and no reader looked. */
            _detectedEnum: param._detectedEnum,
            _defaultValue: param._defaultValue,
            _defaultConfidence: param._defaultConfidence,
            _requiredConfidence: param._requiredConfidence,
            _exampleValue: param._exampleValue === undefined ? null : param._exampleValue,
            _exampleValueSource: param._exampleValueSource,
            _range: param._range,
            /* THE DOMAIN THE CODE'S OWN GATES STATED, beside the range the observations spanned. The two are
               different measurements of the same kind of fact and both are constraints, never values: an
               `_excludedValues` entry is a token the forced execution PROVED this parameter is not on every
               path it saw reach the request. `undefined` and `null` are one statement here (nothing was
               observed), which is why this is written the way `_exampleValue` is and not with a `||` that
               would also swallow an empty array arriving as "unconstrained". */
            _excludedValues: param._excludedValues === undefined ? null : param._excludedValues,
            /* …and the ORDERING gate's interval, which is the same kind of fact and a different measurement
               from `_range` above: `_range` is what live traffic's values happened to span, this is what the
               bundle's own predicates REQUIRE on every path the engine saw reach the request. */
            _bounds: param._bounds === undefined ? null : param._bounds,
          }, "lib/popup-form.js URL parameter `" + name + "`"),
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
      /* THE RECORD IS RE-CONSTRUCTED ON ARRIVAL, WHICH IS THE BOUNDARY CHECK AND NOT A REDUNDANT ONE. These
         fields were built in the OFFSCREEN zone (lib/discovery.js, lib/send.js) and crossed
         chrome.runtime.sendMessage to get here, and a crossing is exactly where a record can arrive short of
         a name — the serialization drops an `undefined` property, so a producer that stopped writing a field
         looks identical on this side to one that never had it. Rebuilding through makeFieldDef asserts the
         crossed record against the ONE definition instead of letting the builder read the gap through `||`. */
      section.appendChild(
        createFieldInput(
          field.name,
          makeFieldDef({ ...field, parentSchema: schema.requestBody.schemaName },
                       "lib/popup-form.js request-body field `" + field.name + "`"),
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
/* THE VALUE DECIDES THE WHOLE SHAPE, AND IT IS IN HAND BEFORE THE RECORD EXISTS. The previous form allocated
   a typeless shell, pushed it into its parent's children, and typed it a queue-turn later — which meant the
   record was briefly a field with no type, and `type` could not be a fact this producer STATES. It never had
   to be: the value that decides the type is the same value that decides whether there are children, so both
   are read here, once, and the driver below only POPULATES the children a container already declared. */
function _synthShapeOfValue(value) {
  if (value === null || value === undefined) return { type: "string", label: "optional", children: null };
  if (Array.isArray(value)) {
    // An array of objects is a REPEATED MESSAGE whose fields are the first object's; an array of scalars is
    // a repeated scalar and has no children to descend into.
    const firstObj = value.find(v => v && typeof v === "object" && !Array.isArray(v));
    if (firstObj) return { type: "object", label: "repeated", children: [] };
    return { type: value.length ? typeOfScalar(value[0]) : "string", label: "repeated", children: null };
  }
  if (typeof value === "object") return { type: "object", label: "optional", children: [] };
  return { type: typeOfScalar(value), label: "optional", children: null };
}

function synthesizeFieldDefFromValue(rootName, rootValue) {
  // Iterative tree-builder. Each work item pairs a container's value with the record whose `children` it
  // fills. Replaces self-recursion so deeply-nested JSON variables (or adversarial server payloads)
  // synthesize without growing the JS call stack.
  function mk(name, value) {
    /* EVERY OTHER NAME TAKES ITS DECLARED ABSENCE, and that is a STATEMENT rather than a gap: a field
       synthesized out of a captured value has no wire number, no schema to rename under, no declared enum
       and no domain, because nothing observed one — this producer read a value, not a declaration. */
    return makeFieldDef(Object.assign({ name, required: false }, _synthShapeOfValue(value)),
                        "lib/popup-form.js synthesized field `" + name + "`");
  }
  const root = mk(rootName, rootValue);
  const queue = [{ value: rootValue, dst: root }];
  while (queue.length > 0) {
    const { value, dst } = queue.shift();
    if (dst.children === null) continue;
    const src = Array.isArray(value)
      ? value.find(v => v && typeof v === "object" && !Array.isArray(v))
      : value;
    for (const [k, v] of Object.entries(src)) {
      const child = mk(k, v);
      dst.children.push(child);
      queue.push({ value: v, dst: child });
    }
  }
  return root;
}
function typeOfScalar(v) {
  if (typeof v === "number") return Number.isInteger(v) ? "int64" : "double";
  if (typeof v === "boolean") return "bool";
  return "string";
}

/* AN ORDERING GATE'S INTERVAL, SPELLED ONCE — read by the badge and by both placeholders, because a domain
   rendered two ways in one form is two claims a reviewer has to reconcile. It states a CONSTRAINT and never a
   member of it: `> 5` cannot be typed into a box and sent, which is exactly why §@H requires it where
   inventing `6` is forbidden.
   The keys are JSON Schema Validation 2020-12 §6.2's own, carried unrenamed from endpoint.c so there is one
   vocabulary from the C emission to the OpenAPI export. Returns "" when there is no bound to state, which
   every caller reads as the positive statement it is (no ordering gate's claim survived every observed
   path). */
function boundsPhrase(b) {
  if (!b || typeof b !== "object") return "";
  const parts = [];
  if ("minimum" in b) parts.push("≥ " + b.minimum);
  else if ("exclusiveMinimum" in b) parts.push("> " + b.exclusiveMinimum);
  if ("maximum" in b) parts.push("≤ " + b.maximum);
  else if ("exclusiveMaximum" in b) parts.push("< " + b.exclusiveMaximum);
  return parts.join(" and ");
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

  /* EVERY `fieldDef.*` BELOW IS READ AS ITSELF. lib/field-def.js is the ONE definition of this record and
     `makeFieldDef` is the only way one is built, so a name that is absent here is a producer that stopped
     writing it — which crashes at that producer — and a name whose value is `null` is the producer STATING
     that it observed none. The `||` that used to stand on `type`, `displayName`, `name`, `number`,
     `parentSchema` and `children` could not tell those two apart, and answered both with the bytes one
     particular producer happens to use, so a probe-learned field and a discovery-declared unconstrained one
     rendered identically while only the second had ever been asked. */
  wrapper.dataset.name = name;
  wrapper.dataset.type = fieldDef.type;
  wrapper.dataset.category = category;
  if (fieldDef.number !== null) wrapper.dataset.number = fieldDef.number;
  wrapper.dataset.label = fieldDef.label;
  if (fieldDef.location !== null) wrapper.dataset.location = fieldDef.location;

  const labelEl = el("label", "form-field-label");
  /* `displayName: null` MEANS "render the wire name" — the GraphQL variables tree is the one producer that
     writes an alias, and every other one states its absence. The caller's positional `name` is a genuine
     ALTERNATIVE and not a filler: `_buildMessageStep` passes a captured body's own key for a field the
     schema does not declare, which is a different fact from the record's `name` and the reason both are
     read. */
  const displayName = fieldDef.displayName !== null ? fieldDef.displayName
    : (fieldDef.name !== "" ? fieldDef.name : name);
  let labelHtml = `<span class="field-name">${esc(displayName)}</span>`;

  /* THE RENAME TARGET IS THE RECORD'S, and `parentSchema: null` means this field has none — which is a
     statement, so it is read as one. It used to be `esc(fieldDef.parentSchema || "params")`, which wrote
     every unowned field's rename into the URL-PARAMETER schema: a body field synthesized from a captured
     JSON value carries no schema at all, and clicking its ✎ persisted the rename against `params`, where
     nothing would ever read it back. The button is only offered where there is somewhere to persist it. */
  if (fieldDef.parentSchema !== null &&
      (fieldDef.number !== null || name.startsWith("field") || category === "param" ||
       fieldDef.parentSchema !== "params")) {
    labelHtml += ` <span class="btn-rename" title="Rename field" data-schema="${esc(fieldDef.parentSchema)}" data-key="${esc(name)}">✎</span>`;
  }

  // esc(): `number` is not guaranteed numeric — openapi-import.js takes it verbatim from an imported spec's
  // `x-field-numbers`, which is a file the researcher was handed. Every value on this path came from
  // somewhere hostile; none of them is escaped by being "probably a number".
  if (fieldDef.number !== null)
    labelHtml += ` <span class="field-number">#${esc(String(fieldDef.number))}</span>`;
  labelHtml += ` <span class="field-type">${esc(fieldDef.type)}</span>`;
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
  // A DOMAIN IS RENDERED AS A DOMAIN. `5–100` on its own reads like a pair of observed values; it is the
  // range the field's observations spanned, which is a constraint on what the value can be, never a value the
  // code produced. Saying "range" is the difference between a shape and an example — CLAUDE.md §@H: a param
  // known only to satisfy a range stays a domain-annotated shape, and inventing a member of it fabricates an
  // observed key. The input below is NOT prefilled from it (pickExampleValue deleted its "range-min" tier).
  if (fieldDef._range) {
    labelHtml += ` <span class="field-stat badge-range" title="the domain this field's observations spanned — a constraint, not a value the code computed">range ${esc(String(fieldDef._range.min))}–${esc(String(fieldDef._range.max))}</span>`;
  }
  /* AND THE DOMAIN THE CODE ITSELF STATED. `_range` is what the observed values spanned; this is what the
     bundle's own equality gates PROVED the value is not, on the arms the forced execution took. It renders
     as a constraint and never as a value — `≠ admin` cannot be mistaken for a key to send — and its ABSENCE
     is not rendered at all, because "nothing was proved" and "proved to be nothing in particular" are the
     same statement about what the reviewer may assume. Without it a param this run proved is neither
     "admin" nor "prod" looked exactly like a param nothing ever tested. */
  if (Array.isArray(fieldDef._excludedValues) && fieldDef._excludedValues.length > 0) {
    labelHtml += ` <span class="field-stat badge-excluded" title="values the forced execution proved this parameter is NOT, on every observed path to this request — a constraint the code stated, never a value it computed">${fieldDef._excludedValues.map((v) => "\u2260 " + esc(String(v))).join(", ")}</span>`;
  }
  /* …AND THE ORDERING GATE'S HALF OF THE SAME RULE. `_range` above is a statistic over observed traffic; this
     is a CONSTRAINT the bundle's own `x > 5` stated, and the two must not be read in one voice — so it gets
     its own badge, its own colour, and a title that says which of the two it is. Without it a parameter this
     run proved must exceed 5 rendered exactly like a parameter nothing had ever ordered, and §Solver-half
     calls that silence a wrong report rather than a partial one. It renders a CONSTRAINT and never a value:
     `> 5` is not something a reviewer can mistake for a key to send, which is the whole reason @H may state
     it where inventing `6` is forbidden. */
  const _bp = boundsPhrase(fieldDef._bounds);
  if (_bp) {
    labelHtml += ` <span class="field-stat badge-bounds" title="the interval the forced execution proved this parameter obeys, on every observed path to this request — a constraint the code stated, never a value it computed">${esc(_bp)}</span>`;
  }
  // A PREFILLED BOX ALWAYS CARRIES ITS PROVENANCE. The badge is driven by the SAME resolvePrefill() the input
  // reads, so the box can never show a value the label does not attribute — which is what happened for a
  // single AST-observed value: createSingleInput typed it into the field on its own, and this badge (keyed on
  // _exampleValueSource) stayed silent, so a value the code merely LISTS rendered exactly like a value
  // observed on the wire.
  var _pf = resolvePrefill(fieldDef, initialValue);
  if (_pf.source) {
    labelHtml += ` <span class="field-stat badge-prefill" title="Prefilled from ${esc(_pf.source)}">prefill: ${esc(_pf.source)}</span>`;
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
  /* `_astValidValues: null` MEANS the bundle was never observed setting this field, and `enum: null` MEANS
     no document declared a membership — two different silences, each read as itself. The chips are the
     OBSERVED set and the select above is the DECLARED one, so a field that has both renders the declared
     one and nothing is doubled. */
  if (fieldDef._astValidValues !== null && fieldDef._astValidValues.length > 0 && fieldDef.enum === null) {
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
  } else if ((fieldDef.type === "message" || fieldDef.type === "object") &&
             fieldDef.children !== null && fieldDef.children.length > 0) {
    queue.push({ kind: "MESSAGE_GROUP", parent: wrapper, fieldDef, category, depth, initialValue, hasSchema: true });
  } else if (fieldDef.type === "message" || fieldDef.type === "object") {
    if (initialValue && typeof initialValue === "object" && !Array.isArray(initialValue)) {
      queue.push({ kind: "MESSAGE_GROUP", parent: wrapper, fieldDef, category, depth, initialValue, hasSchema: false });
    } else {
      /* A MESSAGE WITH NEITHER A SCHEMA NOR A CAPTURED VALUE STILL NEEDS A BOX TO TYPE INTO, and the box
         is a FIELD like any other — a bare `{ type: "string" }` was a fifth vocabulary reaching the one
         consumer, so every domain and example createSingleInput reads was answered by the `||` this record
         exists to delete. It states what it is: the same field, rendered as raw text because nothing
         described its shape. */
      wrapper.appendChild(createSingleInput(
        makeFieldDef({ name: fieldDef.name, type: "string", label: fieldDef.label,
                       required: fieldDef.required },
                     "lib/popup-form.js schemaless message `" + fieldDef.name + "`"),
        "", category));
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
  /* THE SUMMARY NAMES THE MESSAGE IF THERE IS ONE TO NAME. `messageType: null` MEANS this repeated item
     came from no named schema, which is the case for an array synthesized out of a captured body — so the
     wire name is used, and "item" only where the record carries neither. Three alternatives, not two
     fallbacks past a hole. */
  summary.textContent = fieldDef.messageType !== null ? fieldDef.messageType
    : (fieldDef.name !== "" ? fieldDef.name : "item");
  const removeBtn = el("button", "btn-small");
  removeBtn.textContent = "×";
  removeBtn.type = "button";
  removeBtn.title = "Remove item";
  removeBtn.dataset.formRemoveItem = "1";
  summary.appendChild(removeBtn);
  itemWrapper.appendChild(summary);

  const childContainer = el("div", "form-message-children");
  /* `children: null` MEANS this field is not a message, so it declares NO fields for an item to render —
     which is a statement about the schema and not a gap: the item's own captured keys are rendered by the
     loop below either way, and that is the whole content of an item whose shape nothing described. */
  const schemaChildren = fieldDef.children === null ? [] : fieldDef.children;
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
      fieldDef: makeFieldDef(
        { ...child, parentSchema: fieldDef.messageType !== null ? fieldDef.messageType : fieldDef.parentSchema },
        "lib/popup-form.js repeated-item child `" + child.name + "`"),
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
    ? (fieldDef.messageType !== null ? fieldDef.messageType
       : (fieldDef.name !== "" ? fieldDef.name : "message"))
    : (fieldDef.name !== "" ? fieldDef.name : "object");
  details.appendChild(summary);
  const childContainer = el("div", "form-message-children");
  /* A CHILD INHERITS THE MESSAGE IT CAME FROM, and where this field names none (`messageType: null`) it
     inherits whatever schema THIS field renames under — which may itself be `null`, meaning the children
     have no rename target either. Reading the chain as three statements rather than as two fallbacks is
     what stops a schemaless child being renamed into the URL-parameter schema. */
  const inheritedSchema = fieldDef.messageType !== null ? fieldDef.messageType : fieldDef.parentSchema;
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
        fieldDef: makeFieldDef({ ...child, parentSchema: inheritedSchema },
                               "lib/popup-form.js message child `" + child.name + "`"),
        category, depth: depth + 1, initialValue: childVal,
      });
    }
  }
  if (initialValue && typeof initialValue === "object" && !Array.isArray(initialValue)) {
    for (const [k, v] of Object.entries(initialValue)) {
      if (rendered.has(k)) continue;
      /* A CAPTURED KEY THE SCHEMA DOES NOT DECLARE STILL RENAMES UNDER THE MESSAGE IT WAS FOUND IN, where
         there is one — `inheritedSchema === null` MEANS there is none, and the synthesized record already
         states that, so the assignment is made only where it carries a fact. */
      const synthDef = synthesizeFieldDefFromValue(k, v);
      if (inheritedSchema !== null) synthDef.parentSchema = inheritedSchema;
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

// ONE ANSWER to "what value will this input carry, and where did that value come from" — read by the label
// badge and by the input itself. Two places used to decide it independently: the label attributed
// `_exampleValue` and the input separately typed in a lone `_astValidValues[0]`, so the one prefill the
// analyzer is LEAST sure of (a value the bundle lists as valid, which the server may never accept) was the
// one that arrived with no attribution at all. Provenance is a fact about the value; it belongs where the
// value is decided.
//
// It never INVENTS: a field whose only knowledge is a domain (`_range`, a multi-valued `_astValidValues`)
// resolves to no value, so the box stays empty and the domain shows as a badge/placeholder beside it.
// CLAUDE.md §@H — a range never picks a member.
function resolvePrefill(fieldDef, initialValue) {
  if (initialValue !== null && initialValue !== undefined) {
    // A captured request's value is the value itself; it is attributed only when it IS the analyzer's example
    // (otherwise it came off the wire in front of the user and needs no badge).
    const matches = fieldDef._exampleValue === initialValue
      || String(fieldDef._exampleValue) === String(initialValue);
    return { value: initialValue, source: (fieldDef._exampleValueSource && matches) ? fieldDef._exampleValueSource : null };
  }
  const vv = fieldDef._astValidValues;
  // ONE observed value in the bundle = the bundle only ever set this param one way. >= 2 is a set to choose
  // from, never a pick: auto-selecting one would report an observation the code never made.
  if (Array.isArray(vv) && vv.length === 1) return { value: vv[0], source: "ast-constraint" };
  return { value: null, source: null };
}

function createSingleInput(fieldDef, initialValue = null, category = null) {
  const type = fieldDef.type;
  const pf = resolvePrefill(fieldDef, initialValue);

  /* `enum: null` MEANS no document declared a membership. The `type === "enum"` disjunct that stood here
     could never reach a select on its own — the `?.length` behind it already required the list — so it read
     as a second way in and was one. */
  if (fieldDef.enum !== null && fieldDef.enum.length > 0) {
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
        /* `enumDescriptions: null` MEANS the document described no member; a list SHORTER than `enum` is
           that document describing only some, which is why the per-index read stays. */
        (fieldDef.enumDescriptions !== null && fieldDef.enumDescriptions[i]
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
  if (fieldDef._astValidValues !== null && fieldDef._astValidValues.length > 0) {
    const wrap = document.createDocumentFragment();
    const inp = document.createElement("input");
    inp.type = "text";
    inp.className = "form-input";
    inp.autocomplete = "off";
    // Stable id derived from field name so re-renders reuse the same
    // datalist (no orphaned <datalist> nodes accumulating in the DOM).
    const dlId = "astvals-" + (fieldDef.name !== "" ? fieldDef.name : "field").replace(/[^A-Za-z0-9]/g, "_")
      + "-" + (category === null ? "" : category);
    inp.setAttribute("list", dlId);
    /* The prefill (and its attribution in the label) is resolvePrefill's ONE decision — a lone AST value is
       taken, a set of >= 2 is not, per CLAUDE.md's never-auto-pick rule. The datalist below offers the whole
       set either way, so nothing observed is hidden by not being chosen. */
    if (pf.value !== null && pf.value !== undefined) inp.value = String(pf.value);
    /* The DOMAIN, where one is known and no value was resolved: shown as a placeholder, which is greyed and
       never submitted, so the box states what the value must satisfy without asserting a member of it. */
    else if (boundsPhrase(fieldDef._bounds)) inp.placeholder = (type || "value") + " " + boundsPhrase(fieldDef._bounds);
    else if (fieldDef._range) inp.placeholder = (type || "value") + " in " + fieldDef._range.min + "–" + fieldDef._range.max;
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
      if (pf.value !== null) inp.value = pf.value;
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
      /* The DOMAIN in the placeholder where one is known — a constraint stated, never a member picked. The
         PROVED interval is preferred over the observed span: the first is what the code requires, the second
         is only what a sample happened to contain, and a box can state one thing. */
      inp.placeholder = boundsPhrase(fieldDef._bounds) ? (type + " " + boundsPhrase(fieldDef._bounds))
        : fieldDef._range ? (type + " in " + fieldDef._range.min + "\u2013" + fieldDef._range.max) : type;
      if (type === "double" || type === "float") inp.step = "any";
      if (pf.value !== null) inp.value = pf.value;
      return inp;
    }
    case "bytes": {
      const ta = document.createElement("textarea");
      ta.className = "form-input form-input-bytes";
      ta.placeholder = "base64-encoded bytes";
      ta.rows = 2;
      if (pf.value !== null) ta.value = pf.value;
      return ta;
    }
    default: {
      const inp = document.createElement("input");
      inp.type = "text";
      inp.className = "form-input form-input-string";
      /* The DOMAIN in the placeholder, which is greyed and never submitted — the box states what the value
         must satisfy without asserting a member of it, exactly as the numeric input does for `_range`. */
      inp.placeholder = (Array.isArray(fieldDef._excludedValues) && fieldDef._excludedValues.length > 0)
        ? (type || "value") + " other than " + fieldDef._excludedValues.map(String).join(", ")
        : boundsPhrase(fieldDef._bounds) ? (type || "value") + " " + boundsPhrase(fieldDef._bounds)
        : (type || "value");
      if (pf.value !== null) {
        inp.value =
          typeof pf.value === "object"
            ? JSON.stringify(pf.value)
            : pf.value;
      }
      return inp;
    }
  }
}

/* ─── Value collection (the inverse of buildFormFields): read the form inputs back into a values map/
   initialData for the Send request. formFieldsToMap/collectFormValues/getInputValue + path-param apply. */
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
      /* THE NUMBER IS THIS MAP'S KEY, SO IT IS ASSERTED WHERE IT BECOMES ONE. `_collectShallow` is the only
         producer of these records and it states the wrapper's own `data-number` text or `null`, so `undefined`
         is not a second spelling of "not numbered" — it is that producer having stopped writing the name, and
         `|| f.number === undefined` was the hole that would have hidden it. A number this reader cannot trust
         is not a rendering bug: it becomes a KEY of the outgoing request's field map, which is the one place
         a wrong value is silently a request the researcher did not compose. */
      DCHECK(f.number === null || typeof f.number === "string",
             "a collected form field's `number` is neither the text its wrapper carries nor `null` — " +
             "_collectShallow reads `data-number` and states its absence as `null`, so anything else here is " +
             "that collector broken and this line would key the outgoing request's field map on it");
      if (f.number === null) continue;
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
  /* THE NUMBER IS THE TEXT THE BUILDER WROTE, AND ITS ABSENCE IS A STATEMENT — not a value to coerce one out
     of. `FieldDef.number` (lib/field-def.js) is a string, a number, or `null` MEANING "this field is not
     numbered", and the STRING spelling need not be numeric: lib/openapi-import.js takes it verbatim from an
     imported spec's `x-field-numbers`, a file the researcher was handed, and `fdDocKey` refuses only the
     non-scalars. `_buildFieldStep` writes `data-number` only where the record stated one, so by HTML §3.2.6.6
     "Embedding custom non-visible data with the data-* attributes" — whose `dataset` IDL attribute "provides
     convenient accessors for all the data-* attributes on an element" — an ABSENT attribute reads back as
     `undefined` and IS that `null`, exactly and with nothing to guess.
     `parseInt` ANSWERED THREE DIFFERENT QUESTIONS WITH ONE COERCION, and the damage is not cosmetic because
     formFieldsToMap uses this value as a KEY of the outgoing request's field map. `"seven"` became `NaN` and
     then the literal key `"NaN"` — so a non-numeric field was sent under a name no document ever used, and two
     of them collided onto one key with the second overwriting the first. `"1e3"` became `1`, colliding with
     the field the document genuinely numbered 1. And the truthiness test read a stated `""` as absence.
     Reading the text as ITSELF also round-trips, which the coercion did not: every other reader in this file
     already uses the number as a string key (`renderedTop.add(String(field.number))`, `initialData[field.number]`),
     because a property key is a string however it was spelled. */
  const number = wrapper.dataset.number === undefined ? null : wrapper.dataset.number;
  /* THE WRAPPER IS `_buildFieldStep`'s OWN PRODUCT, so a missing `data-label` is THIS FILE broken and not a
     producer being silent — it writes the record's `label` unconditionally, which lib/field-def.js requires
     every producer to state. `|| "optional"` stood here while that write was conditional, and it answered
     "the builder wrote no cardinality" with the word for "not repeated": a repeated field whose wrapper lost
     its label collected as a single value, silently dropping every item but one. */
  DCHECK(typeof wrapper.dataset.label === "string" && wrapper.dataset.label !== "",
         "a form-field wrapper carries no `data-label` — _buildFieldStep writes the FieldDef's own label on " +
         "every wrapper it builds, so an absent one is that builder broken and this collection would read a " +
         "repeated field as a single value");
  const label = wrapper.dataset.label;

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
  // Readiness + re-entrancy backstop for the _refreshSendEnabled() gate: refuse
  // unless a valid send target is READY (a live pinned documentId or a replay's
  // captured one), the pin isn't a confirmed cross-origin swap, we're past the
  // brief post-change settle, and no send is already running.
  const _target = (currentReplayRequest && currentReplayRequest.documentId) || currentDocumentId();
  if (_sendInProgress || _pinStale || !_target || Date.now() < _navBlockUntil) {
    _refreshSendEnabled();
    return;
  }
  btn.disabled = true;
  btn.textContent = "Sending...";
  _sendInProgress = true;

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
      /* The SAME assert as the renderer above, because this is the half with teeth: a value in the wrong
         vocabulary was silently `continue`d here, so the replay went out WITHOUT a header the engine had
         computed and the endpoint looked broken. Skipping an OPAQUE header is a real decision — the analyzer
         could not compute it and the reviewer pastes it into the input the renderer made — so that arm stays;
         what is gone is the arm that treated an unrecognised SHAPE as an opaque header. */
      DCHECK(!!hv && (hv.kind === "literal" || hv.kind === "opaque") && typeof hv.value === "string",
             "a learned required header reached the send builder in the wrong vocabulary (" + hn + " = "
             + JSON.stringify(hv) + ") — lib/learn.js `astHeaderRecord` is the one translation of endpoint.c's "
             + "flat name -> string record, and a value that bypassed it is dropped here, so the replay goes "
             + "out missing a header the engine had already computed");
      if (hv.kind !== "literal") continue;               // opaque: the reviewer's input supplies it below
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

  /* A dataset entry an option does not carry reads as `undefined`, and the consumer branches on "did the
     operator choose a discovery method". `undefined` is not a statement of that — `null` is, and the offscreen
     then has a fact to test rather than a hole to survive. An option with no selection at all is the same
     answer, so `selectedOpt` being absent is not a second case. */
  const _svc = selectedOpt && typeof selectedOpt.dataset.svc === "string" && selectedOpt.dataset.svc !== ""
    ? selectedOpt.dataset.svc : null;
  const _mid = selectedOpt && typeof selectedOpt.dataset.discoveryId === "string" &&
    selectedOpt.dataset.discoveryId !== "" ? selectedOpt.dataset.discoveryId : null;
  const msg = {
    type: "SEND_REQUEST",
    tabId: replayTabId,
    documentId: currentReplayRequest?.documentId ?? currentDocumentId(),
    endpointKey: epKey,
    service: _svc,
    methodId: _mid,
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

  btn.textContent = "Send Request";
  _sendInProgress = false;
  _refreshSendEnabled();   // re-evaluate readiness — don't blindly re-enable
}

