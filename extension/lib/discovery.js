// What is LEFT of this file is ONE component, and the header above once listed three:
//   * schema resolution for the Send panel   (findDiscoveryMethod / findMethodById / resolveDiscoverySchema)
//
// The React Server Components parser was the second and is gone — see the note at the foot of this file. The
// response-asset classifier was the third and is gone too, to the BROWSER PROCESS
// (engine/host/browser_process/network/resource_kind.c); the note where it stood says what moved and what was
// deleted rather than ported. What keeps the last one here is its CONSUMER, which is the ordering jsaudit.mjs's
// own ledger header states: "a producer whose consumers still live in JS is not first, because moving it makes
// every call it serves cross the JS↔WASM boundary the architecture rule exists to delete." There is no
// host→engine compute edge in this architecture and there must not be one — the engine is the driver, the
// bridge relays — so the schema half leaves with lib/send.js (step 7 → moat.c), which is what the Send panel
// actually asks. The classifier could go earlier precisely because its destination was NOT the engine: the
// browser process is a program the trusted zone CALLS, so moving it added no inverted edge.
//
// THE CANDIDATE SET AND ITS FETCH LOOP ARE GONE TO engine/host/solver/discovery.c. `buildDiscoveryUrls` stood
// here and `fetchDiscoveryForService` (lib/discovery-probe.js, deleted with it) walked its candidates in a
// `for` loop, awaiting each in turn — a drive-to-completion in the host, driven by what the page HAPPENED to
// send (response-decode.js called it off passive learning). CLAUDE.md §Attacker sources calls active discovery
// REQUIRED and §Architecture calls it the engine's; the engine now seeds ONE FLOW PER CANDIDATE ADDRESS on the
// one BFS frontier, so the probes are ranked, preempted, parked to the IDB cold tier and resumed in a later
// session like every other flow, and what they learn lands in the same @H endpoint surface as forced
// execution's own findings. It is also GET-only structurally there: the park takes a URL and has no method
// parameter, which is the same shape `pageContextGet` was given here for the same rule.

// ─── Schema Resolution (for Send Request form) ─────────────────────────────

/**
 * Find a discovery method matching an endpoint's path and HTTP method.
 * Walks doc.resources recursively, comparing method paths.
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} endpointPath - URL path (e.g. "/v1/people:search")
 * @param {string} httpMethod - HTTP method (e.g. "POST")
 * @returns {{method: object, resourceName: string}|null}
 */
function findDiscoveryMethod(doc, endpointPath, httpMethod) {
  if (!doc || !doc.resources) return null;

  // canParse guard so the basePath extraction doesn't use throw as a
  // parse-validity test. An empty / malformed baseUrl yields empty basePath,
  // which means no prefix stripping below — correct semantic.
  const baseUrl = doc.baseUrl || doc.rootUrl || "";
  const basePath = (baseUrl && URL.canParse(baseUrl))
    ? new URL(baseUrl).pathname.replace(/\/$/, "")
    : "";

  // Strip basePath prefix from endpointPath for comparison
  let normPath = endpointPath;
  if (basePath && normPath.startsWith(basePath)) {
    normPath = normPath.slice(basePath.length);
  }
  normPath = normPath.replace(/^\//, "");

  function normalizePath(p) {
    // Convert {param} placeholders to a wildcard for matching
    return (p || "").replace(/^\//, "").replace(/\{[^}]+\}/g, "*");
  }

  function matchPath(methodPath, target) {
    const a = normalizePath(methodPath);
    const b = target.replace(/\{[^}]+\}/g, "*");
    if (a === b) return true;
    // Also try matching with path params as segments
    const aParts = a.split("/");
    const bParts = b.split("/");
    if (aParts.length !== bParts.length) return false;
    for (let i = 0; i < aParts.length; i++) {
      if (aParts[i] === "*" || bParts[i] === "*") continue;
      if (aParts[i] !== bParts[i]) return false;
    }
    return true;
  }

  let best = null;

  function walk(res, prefix) {
    for (const [name, r] of Object.entries(res || {})) {
      const fullName = prefix ? prefix + "." + name : name;
      for (const [, m] of Object.entries(r.methods || {})) {
        const mMethod = (m.httpMethod || "").toUpperCase();
        const mPath = m.flatPath || m.path || "";
        if (
          mMethod === httpMethod.toUpperCase() &&
          matchPath(mPath, normPath)
        ) {
          best = { method: m, resourceName: fullName };
          return;
        }
      }
      if (r.resources) walk(r.resources, fullName);
      if (best) return;
    }
  }

  walk(doc.resources, "");

  // Fallback: partial match (endsWith) for flexibility
  if (!best) {
    function walkPartial(res, prefix) {
      for (const [name, r] of Object.entries(res || {})) {
        const fullName = prefix ? prefix + "." + name : name;
        for (const [, m] of Object.entries(r.methods || {})) {
          const mPath = normalizePath(m.flatPath || m.path || "");
          if (
            normPath.endsWith(mPath) ||
            mPath.endsWith(normPath.replace(/\{[^}]+\}/g, "*"))
          ) {
            best = { method: m, resourceName: fullName };
            return;
          }
        }
        if (r.resources) walkPartial(r.resources, fullName);
        if (best) return;
      }
    }
    walkPartial(doc.resources, "");
  }

  return best;
}

/**
 * Find a discovery method by its ID (e.g. "people.people.get").
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} methodId - The method ID to find
 * @returns {{method: object, resourceName: string}|null}
 */
function findMethodById(doc, methodId) {
  if (!doc || !doc.resources) return null;

  let best = null;

  function walk(res, prefix) {
    for (const [name, r] of Object.entries(res || {})) {
      const fullName = prefix ? prefix + "." + name : name;
      for (const [, m] of Object.entries(r.methods || {})) {
        if (m.id === methodId) {
          best = { method: m, resourceName: fullName };
          return;
        }
      }
      if (r.resources) walk(r.resources, fullName);
      if (best) return;
    }
  }

  walk(doc.resources, "");
  return best;
}

/**
 * Resolve a discovery document schema into a recursive field list.
 * Follows $ref pointers in doc.schemas to build the full type tree.
 *
 * Iterative driver — single shared queue processes SCHEMA frames
 * (resolve a named schema's fields into a target array) and PROP
 * frames (populate a single field from a prop definition). Cycles
 * are detected by a per-chain visited set carried on each frame:
 * the set reflects the chain from the root through nested $refs to
 * the current frame, so a back-reference adds a self-circular
 * sentinel field to the children array. No depth cap — visited set
 * is structurally sufficient for any finite OpenAPI schema graph.
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} schemaName - Schema name to resolve (e.g. "Person")
 * @returns {Array<{name, type, required, description, label, children}>}
 */
function resolveDiscoverySchema(doc, schemaName) {
  var fields = [];
  if (!doc || !doc.schemas || !doc.schemas[schemaName]) return fields;
  fields.id = schemaName;
  var queue = [{ kind: "SCHEMA", doc: doc, schemaName: schemaName,
                  visited: new Set(), into: fields }];
  _drainDiscoveryQueue(queue);
  return fields;
}

/* `mapDiscoveryProperty` DELETED — it mapped one discovery property to a field descriptor through the same
   queue `resolveDiscoverySchema` drives, and NOTHING CALLED IT: not this file, not the popup, not send.js.
   A public-looking entry point with no caller reads as a capability the surface has, and the next reader
   builds on it. The queue and its two step functions below have a live caller and stay. */

function _buildDiscoveryFieldShell(name, prop, requiredList) {
  var isRequired = (requiredList || []).indexOf(name) >= 0;
  return {
    name: prop.name || name,
    customName: !!prop.customName,
    type: "string",
    required: isRequired,
    description: prop.description || null,
    label: isRequired ? "required" : "optional",
    number: prop.id != null ? prop.id : null,
    messageType: null,
    children: null,
    _defaultValue: prop._defaultValue == null ? null : prop._defaultValue,
    _defaultConfidence: prop._defaultConfidence == null ? null : prop._defaultConfidence,
    _requiredConfidence: prop._requiredConfidence == null ? null : prop._requiredConfidence,
    _range: prop._range || null,
    _detectedEnum: !!prop._detectedEnum,
    _exampleValue: prop._exampleValue === undefined ? null : prop._exampleValue,
    _exampleValueSource: prop._exampleValueSource || null,
    _astValidValues: prop._astValidValues || null,
  };
}

// Sentinel field appended to a children array when a $ref points back
// to a schema already on the current chain. The OpenAPI/JSON-Schema
// spec permits self-referential schemas, so a finite tree
// representation must terminate the cycle somewhere — the sentinel
// makes the truncation point visible to callers (form builders,
// renderers) instead of silently dropping data.
function _circularRefSentinel(schemaName) {
  return {
    name: "...",
    type: "message",
    description: "(circular ref: " + schemaName + ")",
    label: "optional",
  };
}

function _drainDiscoveryQueue(queue) {
  // LIFO drain: the only observable side effect is registration into
  // docSchemas keyed by schema name, so order doesn't matter and pop()
  // avoids the O(N) cost of shift().
  while (queue.length > 0) {
    var item = queue.pop();
    if (item.kind === "SCHEMA") _stepResolveSchema(item, queue);
    else if (item.kind === "PROP") _stepMapProperty(item, queue);
  }
}

// SCHEMA frame: produce field shells for each property of
// `doc.schemas[schemaName]`, append them to `into`, and queue PROP
// frames that populate each shell. Cycle check: if `schemaName` is
// already on the current chain (visited set), append the circular-ref
// sentinel and skip processing.
function _stepResolveSchema(item, queue) {
  var doc = item.doc, schemaName = item.schemaName, visited = item.visited, into = item.into;
  if (!doc || !doc.schemas || !doc.schemas[schemaName]) return;
  if (visited.has(schemaName)) {
    into.push(_circularRefSentinel(schemaName));
    return;
  }
  var nextVisited = new Set(visited);
  nextVisited.add(schemaName);
  var schema = doc.schemas[schemaName];
  var required = schema.required || [];
  var i = 1;
  for (var propName in schema.properties || {}) {
    var prop = schema.properties[propName];
    var shell = _buildDiscoveryFieldShell(propName, prop, required);
    if (shell.number == null) {
      shell.number = i;
      shell.isNumberGuessed = true;
    }
    into.push(shell);
    queue.push({ kind: "PROP", doc: doc, field: shell, prop: prop, visited: nextVisited });
    i++;
  }
}

// PROP frame: populate the pre-allocated `field` from `prop`. $ref →
// queue a SCHEMA frame whose `into` is the field's children array.
// Inline object / array-of-object → queue child PROP frames. The
// visited set carried into sub-frames is the chain from root to the
// current node; when a sub-frame recurses through a $ref, it gets a
// fresh clone via _stepResolveSchema's `nextVisited`.
function _stepMapProperty(item, queue) {
  var doc = item.doc, f = item.field, p = item.prop, v = item.visited;

  if (p.$ref) {
    f.type = "message";
    f.messageType = p.$ref;
    f.children = [];
    queue.push({ kind: "SCHEMA", doc: doc, schemaName: p.$ref,
                  visited: v, into: f.children });
    return;
  }

  if (p.type === "array" && p.items) {
    f.label = "repeated";
    if (p.items.$ref) {
      f.type = "message";
      f.messageType = p.items.$ref;
      f.children = [];
      queue.push({ kind: "SCHEMA", doc: doc, schemaName: p.items.$ref,
                    visited: v, into: f.children });
    } else if (p.items.type === "object" && p.items.properties) {
      f.type = "message";
      f.children = [];
      var arrRequired = p.items.required || [];
      for (var ipn in p.items.properties) {
        var arrShell = _buildDiscoveryFieldShell(ipn, p.items.properties[ipn], arrRequired);
        f.children.push(arrShell);
        queue.push({ kind: "PROP", doc: doc, field: arrShell,
                      prop: p.items.properties[ipn], visited: v });
      }
    } else {
      f.type = mapJsonSchemaType(p.items);
    }
    return;
  }

  if (p.type === "object" && p.properties) {
    f.type = "message";
    f.children = [];
    var nestedRequired = p.required || [];
    for (var pn in p.properties) {
      var nestShell = _buildDiscoveryFieldShell(pn, p.properties[pn], nestedRequired);
      f.children.push(nestShell);
      queue.push({ kind: "PROP", doc: doc, field: nestShell,
                    prop: p.properties[pn], visited: v });
    }
    return;
  }

  if (p.type === "object" && p.additionalProperties) {
    f.type = "string";
    f.description =
      (f.description || "") +
      " (map<string, " +
      (p.additionalProperties.type || "string") +
      ">)";
    return;
  }

  // Scalar
  f.type = mapJsonSchemaType(p);
  if (p.label === "repeated") f.label = "repeated";
  if (p.enum) {
    f.type = "enum";
    f.enum = p.enum;
    f.enumValues = p.enum;
    f.enumDescriptions = p.enumDescriptions || null;
  }
}

/**
 * Map a JSON schema type+format to unified protobuf-style type.
 */
function mapJsonSchemaType(prop) {
  if (!prop) return "string";
  var t = prop.type || "string";
  var f = prop.format || "";

  // Pass through protobuf-native types (from JSPB-learned schemas)
  var pbTypes = [
    "int32", "int64", "uint32", "uint64", "sint32", "sint64",
    "double", "float", "fixed32", "fixed64", "sfixed32", "sfixed64",
    "bool", "bytes", "enum",
  ];
  if (pbTypes.indexOf(t) >= 0) return t;

  if (t === "string") {
    if (f === "byte") return "bytes";
    if (f === "int64" || f === "uint64") return f;
    return "string";
  }
  if (t === "integer") {
    if (f === "int32" || f === "uint32") return f;
    return "int32";
  }
  if (t === "number") {
    if (f === "float") return "float";
    return "double";
  }
  if (t === "boolean") return "bool";
  return "string";
}


// ─── Content-based asset classification — GONE TO engine/host/browser_process/network/resource_kind.c ────────
//
// `sniffBinaryMagic`, `_sniffTextAssetSignature` and `classifyResponseAsset` answered "is this captured reply a
// static asset to skip, or API data to learn from" for lib/response-decode.js, which gated every learning call
// on the answer. All three are deleted. The ruling they were living against is one sentence — type checking is
// safeFetch's job and safeFetch is the only source of sniffing — and this file was the second source: a
// hand-rolled magic-byte table beside WHATWG MIME Sniffing §6, a two-row `<!doctype html` / `<html` test beside
// §7.1's nineteen, and sniffs for SVG, plain CSS, WebVTT, HLS and DASH that are in no standard and that no
// browser performs. §RUN, DON'T MATCH names that last group exactly ("no regex/name/identifier matching,
// scoring, heuristics"), and a server that serves an SVG STATES `image/svg+xml`, which §4.6's image group
// already answers.
//
// WHAT THE PREVIOUS NOTE HERE SAID, AND WHY IT STOPPED BEING TRUE. It said the table survived because "its only
// caller is JS and there is no host→engine COMPUTE edge for a JS caller to reach a moved callee through". That
// was a fact about the ENGINE — the renderer is the driver, the bridge relays, and inverting that is the
// orchestration layer this architecture deletes — and it was correct while §7 lived only there. It is not a
// fact about the BROWSER PROCESS: that program is a dedicated Worker the trusted zone CALLS (0f4643f7), the
// edge into it is a postMessage the offscreen already owns, and classifying a response is the question a
// network service exists to answer. So the callee moved and the caller awaits it —
// `self.browserProcessClassify` in extension/browser-process-host.js.
//
// The judgement itself was not just a copy of a standard, and that half went with it rather than being left
// behind: asset-vs-API is a product decision §7 does not make, so it is its own component beside mime_sniff.c
// (network/resource_kind.c) stated over §7's COMPUTED type and §4.6's groups — which is also how it became
// correct about a mislabelled body, the case a declared-type test can never see.

// ─── React Server Components — GONE TO engine/host/solver/reply_decode.c ────────────────────────────────
//
// `isRSC` / `looksLikeRSC` / `parseRSC` parsed a React Flight stream (`text/x-component`, line-framed
// `<hex row id>:<payload>`) so lib/learn.js could register each client reference's chunk as an endpoint and
// merge the json rows into a response schema. The ENDPOINT half is the @H surface and is now learned in the
// engine, at `engine_provide` — the one point every fetched reply crosses exactly once — keyed on the type the
// server STATED (Fetch §4's extract a MIME type), because §7's COMPUTED type is the network service's answer
// and no renderer-side caller may reach it (browser_process/network/mime_sniff.c, a different program).
// `looksLikeRSC` did not move at all: it
// guessed the protocol from a body whose first two lines matched `^[0-9a-f]+:`, which is also the shape of a
// JSON object keyed by digits, and CLAUDE.md §RUN, DON'T MATCH names that ("no regex/name/identifier
// matching, scoring, heuristics"). The schema half went with learn.js's branch, into the moat the engine is
// taking over at jsaudit step 4 rather than being re-hosted twice.
