// lib/learn.js — VDD passive learning: build the API method model from AST fetch call-sites (engine results),
// from observed requests (URL/query/header/body params + path templating + per-field stats), and from observed
// responses (JSON/JSPB/gRPC/batchexecute decoded into schemas). Extracted from the offscreen-brain.js monolith
// (one problem per file); loaded before it, resolves its callers (generateSchemaFromJson from lib/schema.js,
// extractInterfaceName/calculateMethodMetadata from lib/grouping.js, protobuf/discovery libs) at call-time.
// The Passive Learning feature, just relocated out of the brain.

function learnFromAstCallSite(docData, interfaceName, callSite, scriptUrl) {
  // Takes the DocData object directly (not documentId) so a TRANSIENT view
  // (_emptyDocView, documentId=null) can carry a globalStore-only merge for a
  // GONE document without getDoc(null) creating a phantom state.docs entry.
  const tab = docData;

  // Structural @T candidates carry url:null (a host-edge site in
  // unreached code whose value never resolved). They are surfaced as
  // structural candidates / focusedView review items, never as a
  // learnable endpoint — resolving null through new URL() fabricates a
  // bogus "/null" path (origin + String(null)). Skip cleanly.
  if (callSite.url == null || callSite.url === "") return null;

  // Resolve URL. Dynamic / unresolvable → register service-level only,
  // no synthetic method entry (would confuse the reviewer with made-up
  // paths like `dynamic_0`).
  //
  // Inline-content schemes (data:/blob:/about:/javascript:) aren't API
  // endpoints — they're content embedded in the bundle. Skip them so
  // the service list doesn't accumulate empty-host records with
  // garbled paths.
  if (/^(data|blob|about|javascript):/i.test(callSite.url)) return null;
  // Relative URLs resolve against the DOCUMENT's own url at runtime, NOT the
  // script's host. Cross-origin-hosted scripts (e.g. Reddit serves its
  // shreddit bundle from www.redditstatic.com but it fetches against
  // www.reddit.com when the bundle executes on a reddit.com page) would
  // otherwise be misattributed to the script's host. Using THIS document's url
  // (not the tab's top-page url) keeps an iframe's relative fetches on its own
  // origin. Fall back to scriptUrl only when the document url isn't available.
  const isDynamic = /^\$\{|^\(dynamic\)|^\{[a-zA-Z]/.test(callSite.url);
  let csUrl = null;
  if (!isDynamic) {
    try {
      const _baseForRel = (tab && tab.url) ? tab.url : scriptUrl;
      csUrl = /^https?:\/\//i.test(callSite.url)
        ? new URL(callSite.url)
        : new URL(callSite.url, _baseForRel);
    } catch (_) { return null; }
  }

  // Classification at AST-time is an OPEN question — we don't have a
  // response body to magic-byte-sniff, and request shape alone (GET
  // with no query / body) can mean either "static asset fetch" or
  // "plain API endpoint." We register the method regardless and defer
  // real API-vs-asset classification to the moment real traffic flows
  // (handleResponseBody stamps _responseKind="asset" via magic bytes —
  // classifyResponseAsset in lib/discovery.js).


  // Get-or-create docEntry — same prologue as learnFromRequest.
  // When the classifier falls back to hostname, also check for observed-
  // prefix clustering against siblings on the same host. If a shared
  // path prefix of >=2 segments exists, promote to a prefix-based bucket
  // AND migrate matching siblings so the service groups as one.
  let grouping = csUrl ? classifyInterface(csUrl) : { rule: "ast-dynamic", matched: "dynamic URL" };
  if (csUrl && grouping.rule === "hostname-fallback") {
    const refined = refineByObservedPrefix(tab, csUrl, grouping.name);
    if (refined) {
      migrateToCommonPrefixBucket(tab, grouping.name || interfaceName, refined, csUrl);
      grouping = refined;
      interfaceName = refined.name;
    }
  }
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry || !docEntry.doc) {
    docEntry = {
      status: "found",
      isVirtual: true,
      grouping: { rule: grouping.rule, matched: grouping.matched, firstUrl: csUrl ? csUrl.href : callSite.url },
      doc: {
        kind: "discovery#restDescription",
        name: interfaceName,
        title: `${interfaceName} (Learned)`,
        rootUrl: csUrl ? csUrl.origin + "/" : "https://" + interfaceName + "/",
        baseUrl: csUrl ? csUrl.origin + "/" : "https://" + interfaceName + "/",
        resources: { learned: { methods: {} } },
        schemas: {},
      },
    };
    tab.discoveryDocs.set(interfaceName, docEntry);
  }
  const doc = docEntry.doc;
  if (!doc.resources.learned) doc.resources.learned = { methods: {} };

  if (!csUrl) return docEntry;   // dynamic-URL case: docEntry exists, no method

  // Method name + collision handling — mirrors learnFromRequest.
  const { methodName: baseMethodName } = calculateMethodMetadata(csUrl, interfaceName);
  const qualifiedName = callSite.method.toLowerCase() + "_" + baseMethodName;
  // Verb-matched probed lookup (see learnFromRequest for the same rule):
  // a probed POST entry must not absorb GET traffic.
  const probedBase = doc.resources.probed?.methods?.[baseMethodName];
  const probedMethod = (probedBase && probedBase.httpMethod === callSite.method) ? probedBase : null;

  let methodName;
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];
  if (existingQualified) {
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== callSite.method) {
    const existQualName = existingBase.httpMethod.toLowerCase() + "_" + baseMethodName;
    if (!doc.resources.learned.methods[existQualName]) {
      existingBase.id = `${interfaceName.replace(/\//g, ".")}.${existQualName}`;
      doc.resources.learned.methods[existQualName] = existingBase;
    }
    delete doc.resources.learned.methods[baseMethodName];
    methodName = qualifiedName;
  } else {
    methodName = baseMethodName;
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;
  if (!doc.resources.learned.methods[methodName] && !probedMethod) {
    doc.resources.learned.methods[methodName] = {
      id: methodId,
      path: _decHoles(csUrl.pathname.substring(1)),
      httpMethod: callSite.method,
      parameters: {},
      request: null,
      origin: csUrl.origin,
      _astSourceScript: scriptUrl || null,
      _astInferred: true,
      _astCallSites: [],
    };
  }
  const m = probedMethod || doc.resources.learned.methods[methodName];
  if (m && !m.origin) m.origin = csUrl.origin;
  // Record every AST call site that registered this method. Dedup by
  // script + line so the same call doesn't accumulate on repeat scans.
  // Reviewer uses these to click through to the JS location each
  // endpoint was discovered in — facts about where the code lives.
  if (m) {
    if (!Array.isArray(m._astCallSites)) m._astCallSites = [];
    const cs = {
      script: scriptUrl || null,
      line: callSite.loc ? callSite.loc.line : null,
      column: callSite.loc ? callSite.loc.column : null,
      enclosingFunction: callSite.enclosingFunction || null,
    };
    const key = `${cs.script}:${cs.line}:${cs.column}`;
    const alreadySeen = m._astCallSites.some(x => `${x.script}:${x.line}:${x.column}` === key);
    if (!alreadySeen) m._astCallSites.push(cs);
  }

  // Type inference helper: pick from the first valid value's runtime type.
  // Not a guess — this is the literal AST observed. Default "string" when
  // no observations (neutral; no false precision).
  const _inferType = (validValues, defaultValue) => {
    const sample = Array.isArray(validValues) && validValues.length ? validValues[0]
      : (defaultValue !== undefined ? defaultValue : null);
    if (sample == null) return "string";
    if (typeof sample === "number") return "number";
    if (typeof sample === "boolean") return "boolean";
    return "string";
  };

  // Merge AST-observed valid values onto a target (param or schema prop).
  // Promotes to `enum` when distinct count >= 2 (matches prior behavior).
  // Re-picks the example value when new AST values land — without this,
  // a form-scan-created param whose initial pickExampleValue ran BEFORE
  // any AST values were added would stay frozen at "type-default" even
  // though tier-3 (ast-constraint) is now satisfied.
  const _mergeAstValues = (target, validValues, defaultValue) => {
    let merged = false;
    if (Array.isArray(validValues) && validValues.length) {
      const prev = Array.isArray(target._astValidValues) ? target._astValidValues.slice() : [];
      const before = prev.length;
      for (const vv of validValues) {
        const s = String(vv);
        if (prev.indexOf(s) < 0) prev.push(s);
      }
      if (prev.length > before) merged = true;
      target._astValidValues = prev;
      if (prev.length >= 2 && !target.customEnum && !target.enum) {
        target.enum = prev.slice();
        target._detectedEnum = true;
      }
    }
    if (defaultValue !== undefined) {
      if (target._astDefault !== defaultValue) merged = true;
      target._astDefault = defaultValue;
    }
    if (merged) {
      const ex = pickExampleValue(target, null);
      if (ex) {
        target._exampleValue = ex.value;
        target._exampleValueSource = ex.source;
        if (ex.confidence != null) target._exampleConfidence = ex.confidence;
      } else {
        // No real value was traceable — leave the field without an
        // example so callers can surface the gap rather than acting on
        // a synthesised type-default that nothing in the bundle
        // produced.
        delete target._exampleValue;
        delete target._exampleValueSource;
        delete target._exampleConfidence;
      }
    }
  };

  // Query params — direct registration.
  if (callSite.params) {
    for (const p of callSite.params) {
      if ((p.location || "query") !== "query") continue;
      if (!m.parameters[p.name]) {
        m.parameters[p.name] = {
          type: _inferType(p.validValues, p.defaultValue),
          location: "query",
          description: "Learned from AST fetch call site",
          _astInferred: true,
        };
      }
      _mergeAstValues(m.parameters[p.name], p.validValues, p.defaultValue);
    }
  }

  // Path params — the {name} segments __feUrlShape recovered from an
  // opaque path interpolation (e.g. /settings/avatars/{id}, where id is
  // real attacker/server input the bundle interpolated). Registered as
  // location:"path" (required — it IS the path) so the reviewer sees the
  // templated segment as a real parameter; value stays opaque (no
  // example) until real traffic or replay fills it.
  if (callSite.params) {
    for (const p of callSite.params) {
      if ((p.location || "query") !== "path") continue;
      if (!m.parameters[p.name]) {
        m.parameters[p.name] = {
          type: _inferType(p.validValues, p.defaultValue),
          location: "path",
          required: true,
          description: "Learned from AST fetch call site (path template)",
          _astInferred: true,
        };
      }
      // Real declared name from the page's source map (e.g. minified `e` →
      // `owner`), for display; the param key stays the minified name so URL
      // substitution still matches the `{e}` hole.
      if (p._sourceMapName && !m.parameters[p.name]._sourceMapName) {
        m.parameters[p.name]._sourceMapName = p._sourceMapName;
      }
      _mergeAstValues(m.parameters[p.name], p.validValues, p.defaultValue);
    }
  }

  // Reverse cross-doc reconcile: if THIS method is templated ({hole} path
  // segments), fold any existing CONCRETE same-host live records that match
  // the template into it, then drop the duplicate. Closes the dominant
  // first-load split where live requests (en-us/...) created concrete records
  // BEFORE the deep grind learned the template ({userLocale}/...) — the
  // forward `_matchTemplatedMethodAcrossHost` only catches the other order.
  // Each concrete segment aligned with a {hole} becomes that path-param's
  // example value (goal #2); the concrete method's query params / response /
  // request / stats fold in where the template lacks them. Precise match:
  // same segment count, literal segments equal, same HTTP method, matched
  // method's origin host == this host, and the dup must be concrete at >=1
  // hole (else it IS this template, not a distinct concrete record) — so
  // distinct endpoints are never merged.
  if (m && typeof m.path === "string" && m.path.indexOf("{") >= 0) {
    const _tSegs = m.path.split("/").filter(Boolean);
    const _hostname = csUrl.hostname;
    for (const [, _de] of tab.discoveryDocs) {
      if (!_de || !_de.doc || !_de.doc.resources) continue;
      for (const _bucket of Object.values(_de.doc.resources)) {
        if (!_bucket || !_bucket.methods) continue;
        for (const _key of Object.keys(_bucket.methods)) {
          const _cm = _bucket.methods[_key];
          if (!_cm || _cm === m || _cm.httpMethod !== m.httpMethod || typeof _cm.path !== "string") continue;
          const _cSegs = _cm.path.split("/").filter(Boolean);
          if (_cSegs.length !== _tSegs.length) continue;
          const _oh = (_cm.origin && URL.canParse(_cm.origin)) ? new URL(_cm.origin).hostname : null;
          if (_oh !== _hostname) continue;
          let _ok = true, _concreteAtHole = false;
          for (let _i = 0; _i < _tSegs.length; _i++) {
            const _isHole = _tSegs[_i].charAt(0) === "{" && _tSegs[_i].slice(-1) === "}";
            const _cHole = _cSegs[_i].charAt(0) === "{" && _cSegs[_i].slice(-1) === "}";
            if (_isHole) { if (!_cHole) _concreteAtHole = true; continue; }
            if (_tSegs[_i] !== _cSegs[_i]) { _ok = false; break; }
          }
          if (!_ok || !_concreteAtHole) continue;
          for (let _i = 0; _i < _tSegs.length; _i++) {
            if (!(_tSegs[_i].charAt(0) === "{" && _tSegs[_i].slice(-1) === "}")) continue;
            const _val = _cSegs[_i];
            if (_val.charAt(0) === "{") continue;
            const _hole = _tSegs[_i].slice(1, -1);
            if (!_hole) continue;   // generic {} hole -> the ENGINE's shape/concrete collapse owns its path-param
                                    // example (arg{i}); creating m.parameters[""] here made a duplicate empty-name @path param.
            if (!m.parameters[_hole]) m.parameters[_hole] = { type: "string", location: "path", required: true, description: "Learned (concrete value from live traffic)" };
            _mergeAstValues(m.parameters[_hole], [_val], _val);
          }
          if (_cm.parameters) for (const _pn in _cm.parameters) { if (!m.parameters[_pn]) m.parameters[_pn] = _cm.parameters[_pn]; }
          if (_cm.response && !m.response) m.response = _cm.response;
          if (_cm.request && !m.request) m.request = _cm.request;
          if (_cm._stats && !m._stats) m._stats = _cm._stats;
          delete _bucket.methods[_key];
        }
      }
    }
  }

  // Body params — build a direct AST schema (no synthetic JSON round-trip).
  const bodyParams = (callSite.params || []).filter(p => (p.location || "query") === "body");
  if (bodyParams.length) {
    const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
    if (!doc.schemas[schemaName]) {
      doc.schemas[schemaName] = { id: schemaName, type: "object", properties: {}, _astInferred: true };
    }
    const schema = doc.schemas[schemaName];
    if (!schema.properties) schema.properties = {};
    for (const bp of bodyParams) {
      if (!schema.properties[bp.name]) {
        schema.properties[bp.name] = {
          type: _inferType(bp.validValues, bp.defaultValue),
          _astInferred: true,
        };
      }
      _mergeAstValues(schema.properties[bp.name], bp.validValues, bp.defaultValue);
    }
    if (!m.request) m.request = { $ref: schemaName };
  }

  // Record content-type when AST captured it and the method hasn't seen
  // a real request-time content type yet. Real traffic overrides.
  if (callSite.headers && typeof callSite.headers === "object") {
    // AST-captured required headers: the SET the bundle actually attached at
    // the host edge (fetch init.headers / XHR setRequestHeader), each entry
    // {kind:"literal",value}|{kind:"opaque"} (older format: bare string).
    const ctEntry = callSite.headers["content-type"] || callSite.headers["Content-Type"];
    const ct = ctEntry && (typeof ctEntry === "string" ? ctEntry : ctEntry.value);
    if (ct && (!m.contentTypes || m.contentTypes.length === 0)) {
      m.contentTypes = [ct];
    }
    // Store the full set per-endpoint as transport metadata (NOT body params),
    // so the Send panel can show "this endpoint needs header X". A literal
    // supersedes an earlier opaque for the same header; real traffic refines.
    if (!m.requiredHeaders) m.requiredHeaders = {};
    for (const hk in callSite.headers) {
      const hv = callSite.headers[hk];
      // A value with a {hole} is a SHAPE (from a symbolic flow); a hole-free value is CONCRETE (a concolic
      // flow computed the real token/key). Classify holes as "opaque" so the CONCRETE value supersedes the
      // shape on merge (consistent with param example values) instead of first-write-wins.
      const norm = (typeof hv === "string") ? { kind: /\{[a-z]*\}/.test(hv) ? "opaque" : "literal", value: hv } : hv;
      if (!norm || !norm.kind) continue;
      const prev = m.requiredHeaders[hk];
      if (!prev || (prev.kind === "opaque" && norm.kind === "literal")) m.requiredHeaders[hk] = norm;
    }
  }

  // (Request body @BODY -> params[location:body] feeds the doc.schemas[…Request] builder above — the SINGLE
  //  request-body surface the Send panel + OpenAPI export read. No endpoint-entry copy; that was dead.)

  // Binary body: hostedge.bodyShape captured the full byte sequence + the
  // worker's magic-byte sniffer classified the wire format (protobuf,
  // grpc-web, gzip, zlib, json, bytes). For protobuf/grpc-web, decode via
  // lib/protobuf.js (pbDecodeRaw) so each field becomes a body param named
  // f<num>:<wire> with its concrete value as the example. Real wire-format
  // bytes, real values — not a name guess. (#7 protocol classification)
  if (callSite.bodyBinary && typeof callSite.bodyBinary.hex === "string" && typeof self.pbDecodeRaw === "function") {
    const bb = callSite.bodyBinary;
    m.bodyBinary = { byteLength: bb.byteLength | 0, protocol: bb.protocol || "bytes" };
    let pbBytes = null;
    if (bb.protocol === "protobuf") {
      pbBytes = _hexToBytes(bb.hex);
    } else if (bb.protocol === "grpc-web" && bb.hex.length >= 10) {
      // gRPC-Web frame = 1-byte flag + 4-byte BE length + payload. The
      // payload is the protobuf message; strip the 5-byte header.
      pbBytes = _hexToBytes(bb.hex.slice(10));
    }
    if (pbBytes && pbBytes.length > 0) {
      try {
        const fields = self.pbDecodeRaw(pbBytes);
        for (const f of fields) {
          // Field name as `f<num>` (wire format has no names; the .proto
          // descriptor would map it but we don't have one at AST time).
          // Wire-type tag suffixes the param name so the reviewer sees
          // what kind of value lives there (`varint` vs `len` vs `i32`).
          const wireTag = f.wire === 0 ? "varint" :
                          f.wire === 1 ? "i64" :
                          f.wire === 2 ? "len" :
                          f.wire === 5 ? "i32" : ("w" + f.wire);
          const pname = "f" + f.field + ":" + wireTag;
          let example;
          if (f.wire === 0) {
            example = String(f.data);
          } else if (f.wire === 2 && f.data instanceof Uint8Array) {
            // LEN: could be a string or a nested message. Try UTF-8 decode;
            // if the bytes look like a printable string, that's the value.
            // Otherwise emit hex so the bytes are still visible.
            let asString = "";
            try { asString = new TextDecoder("utf-8", { fatal: true }).decode(f.data); }
            catch (_) { asString = ""; }
            example = asString || ("0x" + Array.from(f.data).map(b => (b < 16 ? "0" : "") + b.toString(16)).join(""));
          } else if (f.data instanceof Uint8Array) {
            example = "0x" + Array.from(f.data).map(b => (b < 16 ? "0" : "") + b.toString(16)).join("");
          } else {
            example = String(f.data);
          }
          // Reuse the same param map the JSON body path uses so the popup
          // renders binary fields alongside textual ones uniformly.
          const existing = m.parameters && m.parameters[pname];
          if (!m.parameters) m.parameters = {};
          if (!existing) {
            m.parameters[pname] = { location: "body", _astValidValues: new Set([example]), _astInferred: true };
          } else if (existing._astValidValues) {
            existing._astValidValues.add(example);
          }
        }
      } catch (e) {
        /* pbDecodeRaw rejected — surface so a malformed protobuf body is
           visible (not silently dropped). The bytes stay on m.bodyBinary
           for the popup to inspect raw. */
        console.warn("[brain] protobuf decode failed:", e && e.message || e, "url=" + callSite.url);
      }
    }
  }

  // Apply example-value picker so the Send form has prefills even
  // before any real traffic hits — pickExampleValue's `ast-constraint`
  // tier uses the _astValidValues we just attached. applyStatsToMethod
  // also walks any body-schema props we created with _astInferred:true.
  applyStatsToMethod(m, doc);

  return docEntry;
}

// Find an existing method whose TEMPLATED path matches this concrete request
// path, so network traffic merges into the QuickJS-learned (or earlier
// network-templated) endpoint instead of forking a new per-value method. This
// is what lets QuickJS supply the editable URL structure (/{e}/{a}/…) while the
// network supplies the real example values for those path params: a match routes
// the request to that method, and the path-param value capture below records the
// concrete segment (NDevTK, APIClient) into its stats. Match = same verb, same
// segment count, every non-{…} segment equal, at least one {…} segment. No
// scoring — a literal segment-by-segment structural match.
function _matchTemplatedMethod(learnedMethods, httpMethod, pathname) {
  const segs = pathname.split("/").filter(Boolean);
  if (!segs.length) return null;
  for (const key in learnedMethods) {
    const mm = learnedMethods[key];
    if (!mm || mm.httpMethod !== httpMethod || !mm.path) continue;
    const tps = mm.path.split("/").filter(Boolean);
    if (tps.length !== segs.length) continue;
    let hasTemplate = false, ok = true;
    for (let i = 0; i < tps.length; i++) {
      if (tps[i].startsWith("{") && tps[i].endsWith("}")) { hasTemplate = true; continue; }
      if (tps[i] !== segs[i]) { ok = false; break; }
    }
    if (ok && hasTemplate) return key;
  }
  return null;
}

// Cross-doc template reconcile. Forced-exec may learn an endpoint as a
// TEMPLATED method (e.g. {userLocale}/content-nav/site-header.json) under
// one service grouping, while a concrete live request (en-us/content-nav/
// site-header.json) refines (refineByObservedPrefix) to a DIFFERENT
// same-host service — leaving the same logical endpoint split into a
// templated [ast] record and a concrete [live] one (observed live on
// learn.microsoft.com: /{userLocale}/content-nav vs /en-us/content-nav).
// `_matchTemplatedMethod` only searches one doc, so it can't bridge the
// split. This searches EVERY same-host doc and returns the doc name whose
// templated method matches this concrete path+method, so learnFromRequest
// can redirect the request into that doc — the concrete segment then lands
// as the param's example value (goal #2) instead of duplicating the
// endpoint. Safe: precise segment match (same count; each segment equal or
// a {hole}), same HTTP method only, and the matched method's origin host
// must equal this host — so distinct services aren't mis-merged.
function _matchTemplatedMethodAcrossHost(tab, hostname, httpMethod, pathname) {
  if (!tab || !tab.discoveryDocs) return null;
  for (const [docName, docEntry] of tab.discoveryDocs) {
    if (!docEntry || !docEntry.doc || !docEntry.doc.resources) continue;
    for (const bucket of Object.values(docEntry.doc.resources)) {
      if (!bucket || !bucket.methods) continue;
      const mk = _matchTemplatedMethod(bucket.methods, httpMethod, pathname);
      if (!mk) continue;
      const mm = bucket.methods[mk];
      const oh = (mm && mm.origin && URL.canParse(mm.origin)) ? new URL(mm.origin).hostname : null;
      if (oh === hostname) return docName;
    }
  }
  return null;
}

function learnFromRequest(documentId, interfaceName, entry, headers) {
  const tab = _docForLearning(documentId);
  const url = new URL(entry.url);
  const method = entry.method;

  // Record WHICH grouping rule fired when this service was first
  // created. Grouping decisions must be traceable to the rule that
  // produced them so reviewers can judge. When classifyInterface falls
  // back to hostname-only, also check for shared path prefixes with
  // siblings on the same host — observed-prefix clustering catches
  // cases like `/svc/shreddit/*` that no keyword rule covers.
  let grouping = classifyInterface(url);
  if (grouping.rule === "hostname-fallback") {
    const refined = refineByObservedPrefix(tab, url, grouping.name);
    if (refined) {
      migrateToCommonPrefixBucket(tab, grouping.name, refined, url);
      grouping = refined;
      interfaceName = refined.name;
    }
  }
  // Cross-doc template reconcile: if forced-exec already learned this endpoint
  // as a templated method under a DIFFERENT same-host service grouping, route
  // this concrete request into THAT doc so the same logical endpoint isn't
  // split into a templated [ast] record + a concrete [live] one. The
  // subsequent _matchTemplatedMethod (below) then finds the template within
  // the redirected doc and merges, recording the concrete segment as the
  // param example. Only redirects on a precise same-host templated match.
  const _crossHostDoc = _matchTemplatedMethodAcrossHost(tab, url.hostname, method, url.pathname);
  if (_crossHostDoc && _crossHostDoc !== interfaceName) {
    interfaceName = _crossHostDoc;
  }
  // Stamp the resolved name onto the entry so callers (handleResponseBody)
  // can use it for downstream lookups instead of their pre-migration
  // `service` variable — the bucket they came in with may have been
  // emptied and deleted during migration.
  entry.interfaceName = interfaceName;
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry || !docEntry.doc) {
    docEntry = {
      status: "found",
      isVirtual: true,
      grouping: { rule: grouping.rule, matched: grouping.matched, firstUrl: entry.url },
      doc: {
        kind: "discovery#restDescription",
        name: interfaceName,
        title: `${interfaceName} (Learned)`,
        rootUrl: url.origin + "/",
        baseUrl: url.origin + "/",
        resources: {
          learned: { methods: {} },
        },
        schemas: {},
      },
    };
    tab.discoveryDocs.set(interfaceName, docEntry);
  }

  const doc = docEntry.doc;
  if (!doc.resources.learned) doc.resources.learned = { methods: {} };

  // GraphQL: method name = operationName (or first root field). Every op on
  // a /graphql endpoint is its own method entry. Detect by parsing the
  // request body — we don't rely on URL containing "graphql" alone.
  let _nameHint = null;
  if (entry.rawBodyB64) {
    try {
      const _bodyText = new TextDecoder().decode(base64ToUint8(entry.rawBodyB64));
      const _gql = parseGraphQLRequest(_bodyText);
      if (_gql && _gql.operations && _gql.operations.length > 0) {
        // Batched: name from first op; other ops registered separately would
        // need splitting at the call site — left as one method for now.
        _nameHint = deriveGraphQLMethodName(_gql.operations[0]);
      }
    } catch (e) {
      /* GraphQL operation-name detection failed (body wasn't base64 /
         wasn't text / wasn't valid GraphQL syntax). The endpoint still
         registers under its fallback methodName; only the named
         disambiguation between `gql_GetUser` and `gql_GetRepo` on the
         same /graphql URL is missed. Surface so a GraphQL parser
         regression on a real bundle is visible. */
      console.debug("[brain] GraphQL name-hint derive failed:", e && e.message || e, "url=" + entry.url);
    }
  }

  const { methodName: baseMethodName } = calculateMethodMetadata(url, interfaceName, _nameHint);
  const qualifiedName = method.toLowerCase() + "_" + baseMethodName;

  // If this method was already probed with richer schema, update it there
  // instead — but ONLY when the probed entry's HTTP verb matches. A POST
  // probe entry must not absorb GET traffic (or vice versa); they're
  // distinct methods that happen to share a path. Without the verb match,
  // real GET stats land on the POST schema, corrupting the probed entry.
  const probedBase = doc.resources.probed?.methods?.[baseMethodName];
  const probedMethod = (probedBase && probedBase.httpMethod === method) ? probedBase : null;

  // Resolve method name — disambiguate when different HTTP methods hit the same path
  let methodName;
  // A concrete request that matches an existing TEMPLATED method (QuickJS gave
  // /{e}/{a}/… or earlier traffic templatized it) merges into that method, so
  // its real path-segment values become examples for the editable params.
  const _tplMatch = _matchTemplatedMethod(doc.resources.learned.methods, method, url.pathname);
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];

  if (_tplMatch) {
    methodName = _tplMatch;
  } else if (existingQualified) {
    // Already disambiguated from a prior collision — use qualified name
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== method) {
    // Collision: different HTTP method to same path — rename existing, qualify new
    const existQualName = existingBase.httpMethod.toLowerCase() + "_" + baseMethodName;
    if (!doc.resources.learned.methods[existQualName]) {
      existingBase.id = `${interfaceName.replace(/\//g, ".")}.${existQualName}`;
      doc.resources.learned.methods[existQualName] = existingBase;
    }
    delete doc.resources.learned.methods[baseMethodName];
    methodName = qualifiedName;
  } else {
    // No collision — use base name
    methodName = baseMethodName;
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;
  entry.methodId = methodId;

  if (!doc.resources.learned.methods[methodName] && !probedMethod) {
    doc.resources.learned.methods[methodName] = {
      id: methodId,
      path: _decHoles(url.pathname.substring(1)),
      httpMethod: method,
      parameters: {},
      request: null,
      origin: url.origin,
    };
  }

  const m = probedMethod || doc.resources.learned.methods[methodName];
  if (m && !m.origin) m.origin = url.origin;

  // Learn query parameters from URL
  if (!url.pathname.includes("batchexecute")) {
    url.searchParams.forEach((value, name) => {
      if (name === "key" || name === "api_key") return;
      // $httpHeaders is a gRPC-Web transport mechanism (CRLF-separated headers in URL),
      // not an API parameter. Putting it in a form input strips \r\n and corrupts the URL.
      if (name === "$httpHeaders") return;
      // $ct is a multipart batch Content-Type transport param, not an API parameter.
      if (name === "$ct") return;
      if (!m.parameters[name]) {
        m.parameters[name] = {
          type: isNaN(value) ? "string" : "number",
          location: "query",
          description: "Learned from request",
        };
      }
    });

    // Learn path parameters by comparing URL to stored template AND
    // by detecting ID-like segments on first observation.
    const segments = url.pathname.split("/").filter(Boolean);
    const templateParts = (m.path || "").split("/").filter(Boolean);
    if (templateParts.length === segments.length) {
      let changed = false;
      for (let i = 0; i < segments.length; i++) {
        if (templateParts[i].startsWith("{")) continue; // Already templated
        if (templateParts[i] !== segments[i]) {
          // Segment differs from template — definitely a parameter
          const paramName = `path_${templateParts[i] || "param" + i}`;
          templateParts[i] = `{${paramName}}`;
          if (!m.parameters[paramName]) {
            m.parameters[paramName] = {
              type: "string",
              location: "path",
              description: "Inferred path parameter",
            };
          }
          changed = true;
        }
        // (The `looksLikeDynamicSegment` regex-GUESS branch was deleted: a path segment becomes a {param}
        //  ONLY when it is OBSERVED to vary across requests (data-driven, above), never because a regex thinks
        //  it "looks like an ID". RUN, DON'T MATCH — the engine already marks genuinely-dynamic segments as
        //  {shape} holes from data-flow; a regex guess on a concrete segment merges distinct real endpoints.)
      }
      if (changed) m.path = templateParts.join("/");
    }
  }

  // Record the observed Content-Type on the method for replay fidelity
  if (headers["content-type"]) {
    const ct = headers["content-type"].split(";")[0].trim();
    if (!m.contentTypes) m.contentTypes = [];
    if (!m.contentTypes.includes(ct)) m.contentTypes.unshift(ct);
  }

  // Learn request body if present
  if (entry.rawBodyB64) {
    const bytes = base64ToUint8(entry.rawBodyB64);
    const text = new TextDecoder().decode(bytes);
    const isBatch = url.pathname.includes("batchexecute");

    if (isBatch) {
      const calls = parseBatchExecuteRequest(text);
      if (calls) {
        for (const call of calls) {
          const callMethodId = `${interfaceName.replace(/\//g, ".")}.${call.rpcId}`;
          if (!doc.resources.learned.methods[call.rpcId]) {
            doc.resources.learned.methods[call.rpcId] = {
              id: callMethodId,
              path: _decHoles(url.pathname.substring(1)),
              httpMethod: "POST",
              parameters: {},
              request: null,
            };
          }
          const callM = doc.resources.learned.methods[call.rpcId];
          const schemaName = `${call.rpcId}Request`;
          callM.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(
            call.data,
            schemaName,
            doc.schemas,
            true,
          );
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      }
    } else if (isMultipartBatch(headers["content-type"])) {
      // Multipart batch: each part is an individual HTTP sub-request with its own body
      const parts = parseMultipartBatchRequest(text, headers["content-type"]);
      if (parts) {
        for (const part of parts) {
          // Derive method name from part path
          const pathSegs = part.path.split("?")[0].split("/").filter(Boolean)
            .filter((s) => s.length <= 32);   // concrete segments as-is; the engine {shape}s dynamic ones
          const partMethodName = part.method.toLowerCase() + "_" +
            (pathSegs.join("_") || "batch_part");
          const partMethodId = `${interfaceName.replace(/\//g, ".")}.${partMethodName}`;
          if (!doc.resources.learned.methods[partMethodName]) {
            doc.resources.learned.methods[partMethodName] = {
              id: partMethodId,
              path: part.path,
              httpMethod: part.method,
              parameters: {},
              request: null,
              _batchPart: true,
            };
          }
          const partM = doc.resources.learned.methods[partMethodName];
          if (part.body) {
            try {
              const json = JSON.parse(part.body);
              const schemaName = `${partMethodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
              partM.request = { $ref: schemaName };
              const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
              mergeSchemaInto(doc, schemaName, newSchema);
            } catch (e) {
              /* Multipart-batch sub-request JSON parse failed for one
                 part — other parts still process. Surface so a malformed
                 sub-request body on an otherwise-valid batch is visible. */
              console.debug("[brain] multipart-batch request-part JSON parse failed:", e && e.message || e, "part=" + partMethodName, "url=" + entry.url);
            }
          }
        }
      }
    } else if (
      headers["content-type"]?.includes("grpc-web") ||
      headers["content-type"]?.includes("grpc+proto")
    ) {
      // gRPC-Web request body: 5-byte frame header + protobuf payload
      try {
        const parsed = parseGrpcWebFrames(bytes);
        if (parsed) {
          for (const frame of parsed.frames) {
            if (frame.type !== "data") continue;
            const tree = pbDecodeTree(frame.data, 8);
            if (tree && tree.length > 0) {
              const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
              m.request = { $ref: schemaName };
              const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
              mergeSchemaInto(doc, schemaName, newSchema);
            }
          }
        }
      } catch (e) {
        console.debug("[brain] grpc-web request-body decode failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (headers["content-type"]?.includes("json+protobuf")) {
      // JSPB body — positional array encoding
      try {
        const json = JSON.parse(text);
        if (Array.isArray(json)) {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas, true);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {
        console.debug("[brain] JSPB request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (
      headers["content-type"]?.includes("x-protobuf") ||
      headers["content-type"]?.includes("application/protobuf")
    ) {
      // Binary protobuf body
      try {
        const tree = pbDecodeTree(bytes, 8);
        if (tree && tree.length > 0) {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {
        console.debug("[brain] x-protobuf request-body decode failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (headers["content-type"]?.includes("json")) {
      try {
        const json = JSON.parse(text);
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
        m.request = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
        mergeSchemaInto(doc, schemaName, newSchema);
      } catch (e) {
        console.debug("[brain] JSON request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (headers["content-type"]?.includes("x-www-form-urlencoded")) {
      // Form-urlencoded with f.req JSPB (non-batchexecute, e.g. browserinfo)
      try {
        const params = new URLSearchParams(text);
        const fReq = params.get("f.req");
        if (fReq) {
          const json = JSON.parse(fReq);
          if (Array.isArray(json)) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
            m.request = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas, true);
            mergeSchemaInto(doc, schemaName, newSchema);
          }
        }
      } catch (e) {
        console.debug("[brain] form-urlencoded f.req parse failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (text && /^[\s﻿\x00-\x1f]*[{\[]/.test(text)) {
      // Structural JSON detection for request bodies whose content-type
      // is text/plain, missing, or anything other than the explicit
      // tagged shapes above. Many analytics + telemetry endpoints
      // (reddit /svc/shreddit/events, GitHub error reporters, snowplow
      // collectors) POST JSON with `Content-Type: text/plain` to dodge
      // CORS preflight. Body STRUCTURE is authoritative — same rule
      // already applied to responses at line ~2183. Without this,
      // observed traffic produces 1000+ orphan body-field stats with
      // 0 declared schema fields (verified on reddit.com events: 35
      // requests captured, 1141 orphan fields, 0 attached).
      try {
        const json = JSON.parse(text);
        if (json !== null && typeof json === "object") {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {
        /* Structural JSON sniff parse failure — text LOOKED like JSON
           (starts with `{`/`[` after whitespace) but JSON.parse rejected
           it. Most often: a partial body / malformed JSON / a JSON
           prefix followed by binary. Surface so the schema-not-learned
           symptom is traceable to the parse failure. */
        console.debug("[brain] structural-JSON request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    }
  }

  // ─── Statistics collection ───────────────────────────────────────────────
  // Only real network traffic reaches here — AST fetch call sites go
  // through learnFromAstCallSite directly (no synthetic entries).
  if (!m._stats) m._stats = { requestCount: 0, params: {}, bodyFields: {} };
  m._stats.requestCount++;

  // Track query param values
  if (!url.pathname.includes("batchexecute")) {
    url.searchParams.forEach((value, name) => {
      if (name === "key" || name === "api_key") return;
      if (name === "$httpHeaders" || name === "$ct") return;
      if (!m._stats.params[name]) m._stats.params[name] = createParamStats();
      updateParamStats(m._stats.params[name], value);
    });
  }

  // Track path param values. The path-param detection block above
  // updates m.path to a templated form like `/posts/{path_param1}` and
  // adds the parameter to m.parameters, but the runtime value of the
  // segment was not being recorded as an observation — leaving the
  // example resolver with only a type-default empty string. Walk the
  // current request's segments alongside the (now-templated) m.path and
  // record each {name} segment's value into the same stats bucket as
  // query params, so pickExampleValue picks the observed-top value.
  {
    const reqSegs = url.pathname.split("/").filter(Boolean);
    const tmplSegs = (m.path || "").split("/").filter(Boolean);
    if (tmplSegs.length === reqSegs.length) {
      for (let i = 0; i < tmplSegs.length; i++) {
        const t = tmplSegs[i];
        if (t.startsWith("{") && t.endsWith("}")) {
          const paramName = t.slice(1, -1);
          if (!paramName) continue;
          if (!m._stats.params[paramName]) m._stats.params[paramName] = createParamStats();
          updateParamStats(m._stats.params[paramName], reqSegs[i]);
        }
      }
    }
  }

  // Track body field values (JSON bodies only)
  if (entry.isJson && entry.decodedBody && typeof entry.decodedBody === "object") {
    const flat = flattenObjectValues(entry.decodedBody);
    for (const [fieldPath, value] of Object.entries(flat)) {
      if (typeof value === "string" || typeof value === "number") {
        if (!m._stats.bodyFields[fieldPath]) m._stats.bodyFields[fieldPath] = createParamStats();
        updateParamStats(m._stats.bodyFields[fieldPath], String(value));
      }
    }
  }

  // Apply stats-derived metadata back to parameters AND body-field schemas
  applyStatsToMethod(m, doc);

  // ─── Chain detection ────────────────────────────────────────────────────
  if (tab._valueIndex) {
    const requestParams = {};
    url.searchParams.forEach((v, k) => { requestParams[k] = v; });
    // Extract body values for chain matching: use JSON body if available
    let chainBody = {};
    if (entry.isJson && entry.decodedBody) {
      chainBody = entry.decodedBody;
    } else if (entry.rawBodyB64) {
      try {
        const _cbText = new TextDecoder().decode(base64ToUint8(entry.rawBodyB64));
        chainBody = JSON.parse(_cbText);
      } catch (e) {
        /* Chain-tracking body parse failed — request body isn't JSON
           (binary protobuf / form-encoded / etc.). chainBody stays
           empty so this request doesn't contribute body-value chain
           links; URL params still flow through findChainLinks above.
           Surface so a chain-detection gap on binary-body endpoints
           is visible. */
        console.debug("[brain] chain-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
    }
    const bodyValues = flattenObjectValues(chainBody);
    const links = findChainLinks(tab._valueIndex, requestParams, bodyValues, methodId);
    if (links.length) {
      m._chains = mergeChainLinks(m._chains, links);
      // Update outgoing chains on source methods
      for (var li = 0; li < links.length; li++) {
        var srcMethod = findMethodInDoc(doc, links[li].sourceMethodId);
        if (srcMethod) {
          if (!srcMethod._chains) srcMethod._chains = { incoming: [], outgoing: [] };
          var outLink = {
            targetMethodId: methodId,
            paramName: links[li].paramName,
            sourceFieldPath: links[li].sourceFieldPath,
            lastSeen: links[li].lastSeen,
          };
          var outDupe = false;
          for (var oi = 0; oi < srcMethod._chains.outgoing.length; oi++) {
            var o = srcMethod._chains.outgoing[oi];
            if (o.targetMethodId === methodId && o.paramName === links[li].paramName && o.sourceFieldPath === links[li].sourceFieldPath) {
              o.observedCount = (o.observedCount || 1) + 1;
              o.lastSeen = links[li].lastSeen;
              outDupe = true;
              break;
            }
          }
          if (!outDupe) {
            outLink.observedCount = 1;
            srcMethod._chains.outgoing.push(outLink);
          }
        }
      }
    }
  }
}

function _applyStatsToField(field, fieldStats, requestCount) {
  if (!field || !fieldStats) return;

  // Required detection
  const reqAnalysis = analyzeRequired(fieldStats, requestCount);
  if (!field.customRequired) {
    field.required = reqAnalysis.required;
    field._requiredConfidence = reqAnalysis.confidence;
  }

  // Enum detection
  const enumAnalysis = analyzeEnum(fieldStats);
  if (enumAnalysis.isEnum && !field.customEnum) {
    field.enum = enumAnalysis.values;
    field._detectedEnum = true;
  }

  // Default detection
  const defaultAnalysis = analyzeDefault(fieldStats);
  if (defaultAnalysis.hasDefault) {
    field._defaultValue = defaultAnalysis.value;
    field._defaultConfidence = defaultAnalysis.confidence;
  }

  // Type narrowing from format hints
  const narrowedFormat = analyzeFormat(fieldStats);
  if (narrowedFormat && field.type === "string") {
    field.format = narrowedFormat;
  }

  // Numeric range
  const range = analyzeRange(fieldStats);
  if (range) field._range = range;
}

// Aggregate stats across all array-index variants matching the schema's
// path pattern: schema uses `info[].source`, stats are keyed
// `info[0].source`, `info[1].source`, etc (per flattenObjectValues).
// Walking the schema with `[]` placeholders, we collect the per-index
// stats keys that match the pattern and merge their counters into a
// single virtual stats bucket so pickExampleValue + dominance reporting
// see the full population, not a single index slice.
function _aggregateStatsForSchemaPath(bodyFieldStats, schemaPath) {
  // Build a regex from the schema path: "info[].source" → /^info\[\d+\]\.source$/
  // First swap the `[]` placeholders for a sentinel that won't be touched
  // by regex escaping; escape the rest; then swap the sentinel back to
  // the index pattern. No regex from user input — schemaPath is built
  // by the walker from controlled property names + literal `[]` markers.
  const SENTINEL = "\0ARR\0";
  const escaped = schemaPath
    .split("[]").join(SENTINEL)
    .replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
    .split(SENTINEL).join("\\[\\d+\\]");
  const re = new RegExp("^" + escaped + "$");
  let merged = null;
  for (const key of Object.keys(bodyFieldStats)) {
    if (!re.test(key)) continue;
    const fs = bodyFieldStats[key];
    if (!fs) continue;
    if (!merged) {
      merged = createParamStats();
    }
    // Sum scalar counters
    merged.observed = (merged.observed || 0) + (fs.observed || 0);
    merged.empty = (merged.empty || 0) + (fs.empty || 0);
    merged.absent = (merged.absent || 0) + (fs.absent || 0);
    // Merge value frequency map
    if (fs.values) {
      if (!merged.values) merged.values = {};
      for (const v in fs.values) {
        merged.values[v] = (merged.values[v] || 0) + fs.values[v];
      }
    }
    // Track type observations
    if (fs.types) {
      if (!merged.types) merged.types = {};
      for (const t in fs.types) {
        merged.types[t] = (merged.types[t] || 0) + fs.types[t];
      }
    }
  }
  return merged;
}

// Walk a request schema's property tree, resolving $refs, and attach
// body-field stats at each dot-path that matches a stats.bodyFields
// entry. Stops when the schema ref has already been visited to avoid
// cycles on self-referential message types.
function _applyBodyFieldStats(m, doc, bodyFieldStats, requestCount) {
  if (!m.request || !m.request.$ref || !doc || !doc.schemas) return;
  function walk(sch, prefix, visited) {
    if (!sch || !sch.properties) return;
    for (const [k, def] of Object.entries(sch.properties)) {
      const dotPath = prefix ? prefix + "." + k : k;
      // Direct match: stats keyed exactly by dotPath (no array indices).
      // Aggregated match: stats keyed by index variants of an array-path
      // (info[0].x, info[1].x, ...) — merge into one virtual bucket.
      const fs = bodyFieldStats[dotPath] ||
        (dotPath.indexOf("[]") >= 0 ? _aggregateStatsForSchemaPath(bodyFieldStats, dotPath) : null);
      if (fs) {
        _applyStatsToField(def, fs, requestCount);
        // Example value + provenance on the field def so the form
        // renderer can prefill without a second pass.
        const ex = pickExampleValue(def, fs);
        if (ex) {
          def._exampleValue = ex.value;
          def._exampleValueSource = ex.source;
          if (ex.confidence != null) def._exampleConfidence = ex.confidence;
        } else {
          delete def._exampleValue;
          delete def._exampleValueSource;
          delete def._exampleConfidence;
        }
      } else {
        const ex = pickExampleValue(def, null);
        if (ex) {
          def._exampleValue = ex.value;
          def._exampleValueSource = ex.source;
        } else {
          delete def._exampleValue;
          delete def._exampleValueSource;
        }
      }
      // Recurse into sub-schemas. Track visited refs per-walk to prevent
      // infinite loops on recursive schemas (e.g. tree-shaped messages).
      if (def.$ref && !visited.has(def.$ref)) {
        visited.add(def.$ref);
        walk(doc.schemas[def.$ref], dotPath, visited);
        visited.delete(def.$ref);
      } else if (def.type === "array" && def.items && def.items.$ref && !visited.has(def.items.$ref)) {
        visited.add(def.items.$ref);
        walk(doc.schemas[def.items.$ref], dotPath + "[]", visited);
        visited.delete(def.items.$ref);
      } else if (def.type === "array" && def.items && def.items.properties) {
        // Inline array items (no $ref) — recurse into items.properties
        // directly. Without this, observed paths like `info[0].source`
        // never reach the schema's array-item field declarations and
        // every nested field stays orphaned. Verified on reddit
        // /svc/shreddit/events: 1141 orphan paths under info[0].* .
        walk(def.items, dotPath + "[]", visited);
      } else if (def.properties) {
        walk(def, dotPath, visited);
      }
    }
  }
  const rootSchema = doc.schemas[m.request.$ref];
  if (!rootSchema) return;
  walk(rootSchema, "", new Set([m.request.$ref]));
}

function applyStatsToMethod(m, doc) {
  // Runs for:
  //   - Real-traffic methods (m._stats populated by learnFromRequest).
  //   - AST-only methods (m._stats may be empty; m.parameters populated
  //     by learnFromAstCallSite with _astValidValues / _astInferred).
  // pickExampleValue naturally handles both: if paramStats is null/empty
  // and the field has _astValidValues, the `ast-constraint` tier wins;
  // otherwise it falls through to enum/format/type-default.
  const stats = m._stats || { requestCount: 0, params: {}, bodyFields: {} };

  // Observed-stats pass: fires analyzer metadata (required/enum/default/
  // format/range) for params we have counts on.
  for (const [name, paramStats] of Object.entries(stats.params || {})) {
    if (!m.parameters[name]) continue;
    _applyStatsToField(m.parameters[name], paramStats, stats.requestCount);
  }

  // Example value pass: cover EVERY declared parameter, real-observed
  // or AST-only. Previously AST-only params got no _exampleValue
  // because we iterated stats.params (which didn't include them) and
  // the Send form showed empty inputs forever until real traffic hit.
  for (const [name, param] of Object.entries(m.parameters || {})) {
    const paramStats = (stats.params || {})[name] || null;
    const ex = pickExampleValue(param, paramStats);
    if (ex) {
      param._exampleValue = ex.value;
      param._exampleValueSource = ex.source;
      if (ex.confidence != null) param._exampleConfidence = ex.confidence;
      else delete param._exampleConfidence;
    } else {
      delete param._exampleValue;
      delete param._exampleValueSource;
      delete param._exampleConfidence;
    }
  }

  // Body fields: the schema tree lives in doc.schemas — walk it and
  // attach stats + example values by dot-path. Without this, popup
  // rendering knew which FIELDS existed but not what values to prefill.
  if (doc && stats.bodyFields) {
    _applyBodyFieldStats(m, doc, stats.bodyFields, stats.requestCount);
  }

  // Correlations
  stats.correlations = detectCorrelations(stats);
}

function findMethodInDoc(doc, methodId) {
  if (!doc || !doc.resources) return null;
  for (const rKey of Object.keys(doc.resources)) {
    var methods = doc.resources[rKey]?.methods;
    if (methods) {
      for (var mKey in methods) {
        if (methods[mKey].id === methodId) return methods[mKey];
      }
    }
  }
  return null;
}

function learnFromResponse(documentId, interfaceName, entry) {
  if (!entry.responseBody) return;

  const tab = _docForLearning(documentId);
  const url = new URL(entry.url);
  const { methodName } = calculateMethodMetadata(url, interfaceName);
  // Check tab-level first, then fall back to globalStore (survives SW restarts)
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry?.doc) {
    const globalEntry = globalStore.discoveryDocs.get(interfaceName);
    if (globalEntry?.doc) {
      docEntry = globalEntry;
      // Also set on tab so subsequent lookups are fast
      tab.discoveryDocs.set(interfaceName, docEntry);
    }
  }
  if (!docEntry || !docEntry.doc) return;
  const doc = docEntry.doc;
  // Find method — try base name first, then HTTP-qualified name (from disambiguation)
  const qualifiedName = entry.method ? entry.method.toLowerCase() + "_" + methodName : null;
  const learned = doc.resources.learned?.methods;
  const m = learned
    ? (learned[methodName] || (qualifiedName ? learned[qualifiedName] : null))
    : null;
  // Also check probed methods
  const proM = doc.resources.probed
    ? doc.resources.probed.methods[methodName]
    : null;
  const targetM = m || proM;
  if (!targetM) return;

  // Decode base64 to text for JSON/Batch parsing
  let textBody = entry.responseBody;
  if (entry.responseBase64) {
    try {
      const bytes = base64ToUint8(entry.responseBody);
      textBody = new TextDecoder().decode(bytes);
    } catch (e) {
      textBody = null;
    }
  }
  if (!textBody) return;

  /* Magic-byte mimeType fallback. Servers commonly omit/genericize the
     Content-Type on binary responses (application/octet-stream, or no
     header at all); the existing isGrpcWeb/isSSE/isNDJSON dispatch
     below is string-on-mimeType, so an unmarked gRPC-Web frame or
     protobuf body falls through to the generic JSON path and the
     schema/example-value extraction never runs. Per CLAUDE.md #7
     (magic-byte sniff + content-type, never URL-suffix), peek at the
     leading bytes and synthesize a mimeType when the server didn't
     supply one. Real wire-format signatures (gRPC frame, protobuf
     varint tag, gzip/zlib magic) come straight from the spec.  */
  let mimeType = entry.mimeType || "";
  if (!mimeType || /^application\/octet-stream(?:$|;)/i.test(mimeType)) {
    let _sniffBytes = null;
    if (entry.responseBase64) {
      try { _sniffBytes = base64ToUint8(entry.responseBody); }
      catch (e) { console.warn("[brain] mime-sniff base64 decode failed:", e && e.message || e, entry.url); }
    } else if (typeof entry.responseBody === "string") {
      try { _sniffBytes = new TextEncoder().encode(entry.responseBody); }
      catch (e) { console.warn("[brain] mime-sniff encode failed:", e && e.message || e, entry.url); }
    }
    if (_sniffBytes && _sniffBytes.length >= 1) {
      const b0 = _sniffBytes[0];
      const n = _sniffBytes.length;
      // gRPC-Web frame: flag byte (0x00 uncompressed / 0x01 compressed)
      // + 4-byte BE payload length matching the remaining bytes
      if (n >= 5 && (b0 === 0x00 || b0 === 0x01)) {
        const declared = (_sniffBytes[1] << 24) | (_sniffBytes[2] << 16) | (_sniffBytes[3] << 8) | _sniffBytes[4];
        if (declared === n - 5) mimeType = "application/grpc-web+proto";
      }
      // gzip magic 1f 8b — the brain doesn't decompress here, but tagging
      // the mimeType means downstream sees a real content-encoding rather
      // than treating gzip bytes as text and corrupting the schema.
      if (!mimeType && n >= 2 && b0 === 0x1f && _sniffBytes[1] === 0x8b) mimeType = "application/gzip";
      // zlib magic 78 da | 78 9c | 78 01
      if (!mimeType && n >= 2 && b0 === 0x78 && (_sniffBytes[1] === 0xda || _sniffBytes[1] === 0x9c || _sniffBytes[1] === 0x01)) mimeType = "application/zlib";
      // Protobuf varint tag — first byte's low 3 bits ∈ {0,1,2,5}
      // (valid wire types) and field number > 0. Apply only when the
      // body decidedly isn't JSON (first byte not whitespace / { / [ /
      // " / digit / true|false|null start), since JSON is the
      // overwhelming default.
      if (!mimeType && (b0 !== 0x7b && b0 !== 0x5b && b0 !== 0x22 &&
                        !(b0 >= 0x30 && b0 <= 0x39) &&
                        b0 !== 0x74 && b0 !== 0x66 && b0 !== 0x6e &&
                        b0 !== 0x20 && b0 !== 0x09 && b0 !== 0x0a && b0 !== 0x0d)) {
        const wireType = b0 & 0x07;
        const fieldNum = (b0 & 0x78) >> 3;
        if ((wireType === 0 || wireType === 1 || wireType === 2 || wireType === 5) && fieldNum > 0) {
          mimeType = "application/x-protobuf";
        }
      }
    }
  }
  if (isAsyncChunkedResponse(textBody)) {
    const chunks = parseAsyncChunkedResponse(textBody);
    if (chunks) {
      if (!doc.resources.learned) doc.resources.learned = { methods: {} };
      // Use endpoint path as the method key (e.g. "hpba" from /async/hpba)
      const asyncPath = url.pathname.split("/").filter(Boolean).pop() || methodName;
      for (let i = 0; i < chunks.length; i++) {
        const chunk = chunks[i];
        if (chunk.type !== "jspb" || !Array.isArray(chunk.data)) continue;

        const chunkKey = `${asyncPath}_chunk${i}`;
        let callM =
          doc.resources.learned.methods[chunkKey] ||
          doc.resources.probed?.methods[chunkKey];
        if (!callM) {
          doc.resources.learned.methods[chunkKey] = {
            id: `${interfaceName.replace(/\//g, ".")}.${chunkKey}`,
            path: _decHoles(url.pathname.substring(1)),
            httpMethod: entry.method || "GET",
            parameters: {},
            request: null,
            response: null,
          };
          callM = doc.resources.learned.methods[chunkKey];
        }

        const schemaName = `${chunkKey}Response`;
        callM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          chunk.data,
          schemaName,
          doc.schemas,
          true,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    }
  } else if (isBatchExecuteResponse(textBody)) {
    const results = parseBatchExecuteResponse(textBody);
    if (results) {
      if (!doc.resources.learned) doc.resources.learned = { methods: {} };
      for (const res of results) {
        let callM =
          doc.resources.learned.methods[res.rpcId] ||
          doc.resources.probed?.methods[res.rpcId];
        // Create method entry if response arrived before request was learned
        if (!callM) {
          doc.resources.learned.methods[res.rpcId] = {
            id: `${interfaceName.replace(/\//g, ".")}.${res.rpcId}`,
            path: _decHoles(url.pathname.substring(1)),
            httpMethod: "POST",
            parameters: {},
            request: null,
            response: null,
          };
          callM = doc.resources.learned.methods[res.rpcId];
        }

        const schemaName = `${res.rpcId}Response`;
        callM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          res.data,
          schemaName,
          doc.schemas,
          true,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    }
  } else if (isGrpcWeb(mimeType)) {
    // gRPC-Web: unwrap frames, decode protobuf payload
    try {
      let bytes;
      if (isGrpcWebText(mimeType)) {
        // grpc-web-text uses base64 encoding
        bytes = base64ToUint8(
          entry.responseBase64 ? entry.responseBody : btoa(entry.responseBody),
        );
      } else {
        bytes = entry.responseBase64
          ? base64ToUint8(entry.responseBody)
          : new TextEncoder().encode(entry.responseBody);
      }
      const parsed = parseGrpcWebFrames(bytes);
      if (parsed) {
        for (const frame of parsed.frames) {
          if (frame.type !== "data") continue;
          const tree = pbDecodeTree(frame.data, 8, (val) => {
            if (typeof val === "string") {
              extractKeysFromText(documentId, val, entry.url, "response_grpc");
            }
          });
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
          targetM.response = { $ref: schemaName };
          const newSchema = generateSchemaFromPbTree(
            tree,
            schemaName,
            doc.schemas,
          );
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      }
    } catch (e) {
      /* gRPC-Web frame decode failed — the schema for this endpoint
         won't be learned from THIS response, the next captured response
         from the same URL may decode correctly. Surface so a real
         malformed-frame symptom (server-side bug or wrong protocol
         classification) is visible instead of disappearing into an
         empty schema. */
      console.debug("[brain] grpc-web frame decode failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (isSSE(mimeType)) {
    // Server-Sent Events: learn schema from JSON data payloads
    try {
      const events = parseSSE(textBody);
      if (events) {
        for (const evt of events) {
          if (typeof evt.data === "object" && evt.data !== null) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Event`;
            targetM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              evt.data,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
            break; // Schema from first JSON event is representative
          }
        }
      }
    } catch (e) {
      /* SSE parse failed — malformed event stream (server sent
         data without `data: ` prefix, missing terminator, etc.).
         Surface so the schema-learning skip is observable. */
      console.debug("[brain] SSE parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (isNDJSON(mimeType)) {
    // NDJSON: learn schema from first object
    try {
      const objects = parseNDJSON(textBody);
      if (objects && objects.length > 0) {
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
        targetM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          objects[0],
          schemaName,
          doc.schemas,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    } catch (e) {
      console.debug("[brain] NDJSON parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (isMultipartBatch(mimeType)) {
    // Multipart batch: learn schema from each part's body
    try {
      const parts = parseMultipartBatch(textBody, mimeType);
      if (parts) {
        for (let i = 0; i < parts.length; i++) {
          const part = parts[i];
          if (!part.body) continue;
          try {
            const json = JSON.parse(part.body);
            const partKey = `${methodName}_part${i}`;
            if (!doc.resources.learned) doc.resources.learned = { methods: {} };
            let partM = doc.resources.learned.methods[partKey];
            if (!partM) {
              doc.resources.learned.methods[partKey] = {
                id: `${interfaceName.replace(/\//g, ".")}.${partKey}`,
                path: _decHoles(url.pathname.substring(1)),
                httpMethod: entry.method || "POST",
                parameters: {},
                request: null,
                response: null,
              };
              partM = doc.resources.learned.methods[partKey];
            }
            const schemaName = `${partKey}Response`;
            partM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              json,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
          } catch (e) {
            /* One part's JSON parse failed — rest of the batch still
               processes. Surface so a malformed part on an otherwise-
               valid batch response is visible. */
            console.debug("[brain] multipart part JSON parse failed:", e && e.message || e, "partIdx=" + i, "url=" + entry.url);
          }
        }
      }
    } catch (e) {
      console.debug("[brain] multipart batch parse failed:", e && e.message || e, "url=" + entry.url);
    }
  /* THE `text/x-component` BRANCH IS GONE TO engine/host/solver/reply_decode.c. It parsed a React Flight
     stream here and did two things with it: registered every client reference's chunk as an endpoint, and
     merged each json row's shape into a synthesized response schema. The FIRST is the @H surface, and the
     engine now learns it at engine_provide — the one point every reply crosses once — from the reply's
     COMPUTED MIME type rather than from `looksLikeRSC`, a two-line regex over the body that fired whenever the
     Content-Type was empty and the payload looked like a JSON object keyed by digits (§RUN, DON'T MATCH). The
     SECOND is this file's own moat aggregation, and it STAYS: it reads bodies intercept.js captured off the
     LIVE page, which the engine never fetched and holds no reply record for, so it is a different input and
     not a duplicated algorithm. (This cited "jsaudit step 4" — a position in a derived queue, in a gate that
     is now deleted. A number nothing can be checked against is the stale-`DFAIL` shape exactly, which is why
     the rule was that nothing outside that file could cite one; three comments cited one anyway.) */
  } else if (isGraphQLUrl(url.href) && mimeType.includes("json")) {
    // GraphQL response: extract data/errors structure
    try {
      const gqlResp = parseGraphQLResponse(textBody);
      if (gqlResp) {
        for (const r of gqlResp.results) {
          if (r.data) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
            targetM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              r.data,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
          }
        }
      }
    } catch (e) {
      console.debug("[brain] GraphQL response parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (mimeType.includes("json") || mimeType.includes("javascript") ||
             /^[\s﻿\x00-\x1f]*[{\[]/.test(textBody)) {
    // JSON or JSONP (callback-wrapped JSON returned as text/javascript) OR
    // body whose first non-whitespace byte is `{` or `[`. Many APIs return
    // JSON under text/plain or no content-type (analytics endpoints,
    // Cloudflare-fronted services, etc.); gating on mimetype alone misses
    // them. Body STRUCTURE is authoritative — if it parses as a JSON
    // object/array, learn the schema; otherwise the catch silently drops.
    try {
      var _lrText = textBody;
      if (!mimeType.includes("json") && (mimeType.includes("javascript") || mimeType === "")) {
        var _lrJsonp = stripJsonp(textBody);
        if (_lrJsonp) _lrText = _lrJsonp;
      }
      // Strip Google XSSI prefix if present. Many Google (and now GitLab
      // snowplow, others) endpoints prepend `)]}'\n` to prevent <script>
      // JSON hijacking. JSON.parse would fail without this.
      if (_lrText.startsWith(")]}'")) _lrText = _lrText.replace(/^\)\]\}'[\r\n]*/, "");
      // Plain "ok" / "OK" confirmations aren't JSON — avoid learning a
      // schema from them.
      if (/^(ok|OK|true|false|null)\s*$/.test(_lrText)) throw new Error("non-object response");
      const json = JSON.parse(_lrText);
      // Only learn from object/array roots — bare strings/numbers/bool
      // don't carry a useful schema and would clutter the doc.
      if (json === null || (typeof json !== "object")) throw new Error("non-object root");
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {
      /* JSON/JSONP response parse failed — common when the body is
         truncated, has a JSONP callback we couldn't strip, isn't valid
         JSON, or is just `ok`/`true`/etc. (the explicit throw above).
         Surface so a schema-not-learned symptom traces to the parse
         step rather than disappearing. */
      console.debug("[brain] JSON/JSONP response parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (
    mimeType.includes("protobuf") ||
    entry.contentType?.includes("protobuf") ||
    mimeType.includes("octet-stream") ||
    entry.contentType?.includes("octet-stream")
  ) {
    // Decode response protobuf heuristically
    try {
      const bytes = entry.responseBase64
        ? base64ToUint8(entry.responseBody)
        : new TextEncoder().encode(entry.responseBody);
      const tree = pbDecodeTree(bytes, 8, (val) => {
        if (typeof val === "string") {
          extractKeysFromText(documentId, val, entry.url, "response_protobuf");
        }
      });
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {
      /* Protobuf response decode failed — body bytes didn't decode as
         valid wire format (might be a mis-classified text body, a
         compressed payload the brain didn't decompress, or a truncated
         response). Surface so the schema-not-learned symptom is
         traceable. */
      console.debug("[brain] protobuf response decode failed:", e && e.message || e, "url=" + entry.url);
    }
  }

  // ─── Chain value indexing ─────────────────────────────────────────────────
  // Index response values so subsequent requests can detect chains
  if (tab._valueIndex && textBody) {
    const methodId = targetM.id || `${interfaceName.replace(/\//g, ".")}.${methodName}`;
    try {
      var _ciText = stripJsonp(textBody) || textBody;
      const parsed = JSON.parse(_ciText);
      indexResponseValues(tab._valueIndex, parsed, methodId);
    } catch (_) {
      // Not JSON/JSONP — index the raw text body if it looks like a useful value
      if (textBody.length >= 4 && textBody.length <= 500) {
        indexResponseValues(tab._valueIndex, textBody, methodId);
      }
    }
  }
}

// (Schema inference -- generateSchemaFromPbTree/Json, inferJsonType, inferRepeatedItemType,
//  mergeSchemaInto -- extracted to lib/schema.js, loaded first. One problem per file.)

/**
 * Create a fetchFn bound to a specific tab.
 */
