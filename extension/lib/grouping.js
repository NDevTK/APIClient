// lib/grouping.js — Endpoint identity: group endpoints into services (by real origin + observed shared
// path prefixes) and derive a method name/id per endpoint. Extracted from the offscreen-brain.js monolith
// (one problem per file); loaded before it, functions resolve their callers at call-time. The name-regex
// GUESSING was already removed (classifyInterface -> real origin; no looksLikeDynamicSegment); grouping is
// now deterministic (origin + observed prefix) and the engine's {shape} path holes carry dynamic segments.

// Extract interface name from URL with better granularity
// Service grouping is inherently heuristic — there's no server-side
// fact that tells us "this URL path is service X". The function below
// applies a set of URL-structure rules, each with a named reason and
// the exact fragment it matched, so a reviewer can see WHY a request
// was grouped into a given bucket and judge whether the classification
// is right. `extractInterfaceName` remains a string-returning wrapper
// for back-compat with existing callers.
function classifyInterface(urlObj) {
  // Endpoint grouping is the REAL ORIGIN, never a name-regex / URL-pattern GUESS. Inferring an "interface
  // name" from path structure (batchexecute / googleapis short-names / $rpc / api-root keywords / version
  // segments) is the banned bundler-recognition heuristic: minified names are meaningless and the guess
  // silently drifts. The ENGINE owns endpoint identity from RUNNING the code; the popup groups endpoints by
  // the deterministic origin the browser actually saw. Finer grouping, if ever needed, comes from the engine,
  // never a JS regex.
  return { name: urlObj.hostname, rule: "origin", matched: urlObj.hostname };
}

function extractInterfaceName(urlObj) {
  return classifyInterface(urlObj).name;
}

// Refine a hostname-fallback classification by detecting shared path
// prefixes with URLs already registered on the same host. This is
// OBSERVATION-DRIVEN — when the tool sees multiple URLs under
// /svc/shreddit/… (or any other common path root) on a host with no
// /api/ or /v1/ keyword, it infers that prefix as a service boundary
// rather than dumping everything under one hostname bucket.
//
// Returns `null` if no shared prefix of >=2 segments is found with any
// sibling method. Otherwise returns a refined classification that
// replaces the hostname-fallback rule with `path-common-prefix`.
function refineByObservedPrefix(tab, urlObj, initialName) {
  if (!tab || !tab.discoveryDocs) return null;
  const hostname = urlObj.hostname;
  const newSegs = urlObj.pathname.split("/").filter(Boolean);
  if (newSegs.length < 2) return null;          // need at least 2 segs to form a prefix

  // Collect already-known method paths under the same hostname, by
  // checking each method's origin. Registered services don't always key
  // on hostname (a probe-discovered doc may have a distinct name), so
  // the origin check is the correct fact-based sibling test.
  const siblingPaths = [];
  for (const [, docEntry] of tab.discoveryDocs) {
    if (!docEntry || !docEntry.doc) continue;
    for (const bucket of Object.values(docEntry.doc.resources || {})) {
      for (const m of Object.values(bucket.methods || {})) {
        if (!m || typeof m.path !== "string" || !m.origin) continue;
        // canParse guard — same root-cause fix as the webnav origin handler.
        // A malformed stored m.origin yields null hostname which can't equal
        // hostname; the `continue` below is the correct semantic.
        const origHost = URL.canParse(m.origin) ? new URL(m.origin).hostname : null;
        if (origHost !== hostname) continue;
        siblingPaths.push(m.path);
      }
    }
  }
  if (!siblingPaths.length) return null;

  let bestPrefixLen = 0;
  for (const sp of siblingPaths) {
    const segs = sp.split("/").filter(Boolean);
    let i = 0;
    while (i < newSegs.length && i < segs.length && newSegs[i] === segs[i]) i++;
    // Require the match to be a STRICT prefix of both — otherwise
    // the two URLs are identical (same method, not a common service).
    if (i > bestPrefixLen && i < newSegs.length && i < segs.length + 1) {
      bestPrefixLen = i;
    }
  }
  if (bestPrefixLen < 2) return null;

  const prefixSegs = newSegs.slice(0, bestPrefixLen);
  // (No regex dynamic-ID trim: the engine marks genuinely-dynamic segments as {shape} from data-flow.)
  if (prefixSegs.length < 2) return null;

  const prefix = "/" + prefixSegs.join("/");
  return {
    name: hostname + prefix,
    rule: "path-common-prefix",
    matched: prefix,
  };
}

// Migrate method entries whose path starts with the given prefix out of
// a hostname-fallback bucket and into a new common-prefix bucket. Used
// when refineByObservedPrefix detects a shared prefix — without this
// the original URL stays orphaned in the hostname bucket while the new
// URL lands in the refined bucket, producing two buckets for the same
// service. Schemas referenced via $ref by migrating methods are copied
// along to preserve lookups.
function migrateToCommonPrefixBucket(tab, oldName, refinement, urlObj) {
  const oldDoc = tab.discoveryDocs.get(oldName);
  if (!oldDoc || !oldDoc.doc) return;
  const newName = refinement.name;
  if (newName === oldName) return;
  const prefixSegs = refinement.matched.replace(/^\//, "").split("/").filter(Boolean);

  // Create or fetch the new docEntry.
  let newDoc = tab.discoveryDocs.get(newName);
  if (!newDoc) {
    newDoc = {
      status: "found",
      isVirtual: true,
      grouping: {
        rule: refinement.rule,
        matched: refinement.matched,
        firstUrl: urlObj ? urlObj.href : null,
      },
      doc: {
        kind: "discovery#restDescription",
        name: newName,
        title: `${newName} (Learned)`,
        rootUrl: oldDoc.doc.rootUrl,
        baseUrl: oldDoc.doc.baseUrl,
        resources: {},
        schemas: {},
      },
    };
    tab.discoveryDocs.set(newName, newDoc);
  }

  const oldSchemas = oldDoc.doc.schemas || {};
  const newSchemas = newDoc.doc.schemas;

  for (const [bucketKey, bucket] of Object.entries(oldDoc.doc.resources || {})) {
    const methods = bucket.methods || {};
    for (const methodKey of Object.keys(methods)) {
      const m = methods[methodKey];
      const mSegs = (m.path || "").split("/").filter(Boolean);
      let matches = mSegs.length >= prefixSegs.length;
      for (let i = 0; matches && i < prefixSegs.length; i++) {
        if (mSegs[i] !== prefixSegs[i]) matches = false;
      }
      if (!matches) continue;
      // Re-id to match new interface.
      m.id = `${newName.replace(/\//g, ".")}.${methodKey}`;
      if (!newDoc.doc.resources[bucketKey]) newDoc.doc.resources[bucketKey] = { methods: {} };
      newDoc.doc.resources[bucketKey].methods[methodKey] = m;
      delete methods[methodKey];
      // Copy the schema this method references so $ref lookups still work.
      if (m.request && m.request.$ref && oldSchemas[m.request.$ref] && !newSchemas[m.request.$ref]) {
        newSchemas[m.request.$ref] = oldSchemas[m.request.$ref];
      }
      if (m.response && m.response.$ref && oldSchemas[m.response.$ref] && !newSchemas[m.response.$ref]) {
        newSchemas[m.response.$ref] = oldSchemas[m.response.$ref];
      }
    }
  }

  // If the hostname bucket is now empty, remove it so it doesn't clutter.
  let remainingMethods = 0;
  for (const b of Object.values(oldDoc.doc.resources || {})) {
    remainingMethods += Object.keys(b.methods || {}).length;
  }
  if (remainingMethods === 0) tab.discoveryDocs.delete(oldName);
}

// (parseRpcPath + RPC_PATH_RE were DELETED with classifyInterface: they parsed a gRPC $rpc/ path into a guessed
//  service name — the same banned name-regex bundler-recognition heuristic. Endpoints group by real origin now.)


/** Detect path segments that look like dynamic IDs rather than resource names. */
// (looksLikeDynamicSegment DELETED: it regex-GUESSED whether a path segment was a dynamic ID — the banned
//  name-matching. The engine marks genuinely-dynamic segments as {shape} holes from real data-flow; a concrete
//  segment stays concrete. RUN, DON'T MATCH.)

function calculateMethodMetadata(urlObj, interfaceName, hint) {
  // Explicit hint (e.g. GraphQL operationName) takes precedence over URL.
  // A GraphQL endpoint at /svc/shreddit/graphql serves dozens of distinct
  // operations (GetUser, CreatePost, …). Without the hint every op would
  // collapse to one URL-derived name; with the hint each gets its own
  // method entry keyed by operationName.
  if (hint && typeof hint === "string" && hint.length > 0) {
    return {
      methodName: hint,
      methodId: `${interfaceName.replace(/\//g, ".")}.${hint}`,
    };
  }
  // batchexecute: use first rpcid from URL param (individual calls registered by learnFromRequest)
  if (urlObj.pathname.includes("batchexecute")) {
    const rpcids = urlObj.searchParams.get("rpcids") || "batch";
    const primaryRpcId = rpcids.split(",")[0].trim();
    return {
      methodName: primaryRpcId,
      methodId: `${interfaceName.replace(/\//g, ".")}.${primaryRpcId}`,
    };
  }

  const segments = urlObj.pathname.split("/").filter(Boolean).map(_decHoles);
  const interfaceParts = interfaceName.split("/");

  // Method segments are everything after the interface prefix
  // If interface is "example.com/api/v1" and path is "/api/v1/users/get"
  // startIdx should skip "api" and "v1".

  const hostname = urlObj.hostname;
  let startIdx = 0;
  if (interfaceName.startsWith(hostname)) {
    startIdx = interfaceParts.length - 1;
  }

  let methodSegments = segments.slice(startIdx);

  // Strip segments that look like hashes, long ID lists, or path-style params
  methodSegments = methodSegments.filter((s) => {
    if (s.length > 32) return false;
    if (s.includes("=")) return false; // path-style parameter (e.g. name=foo)
    return true;
  });

  // No regex ID-normalization: a concrete path segment stays concrete. The engine already marks
  // genuinely-dynamic segments as {shape} holes from real data-flow (they flow through here unchanged);
  // guessing that a concrete segment "looks like an ID" and collapsing it to _id MERGES distinct real
  // endpoints — the banned name-matching. RUN, DON'T MATCH.
  let methodName = methodSegments.join("_") || "root";

  // If it's a gRPC-style path, use the actual method name
  if (urlObj.pathname.includes("$rpc")) {
    methodName = segments[segments.length - 1];
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;

  return { methodName, methodId };
}

// ─── Smart Learning ──────────────────────────────────────────────────────────

// Register an AST-observed fetch call site as a method on its service
// WITHOUT fabricating data. Unlike the previous design (which laundered
// AST values through a fake URL + fake JSON body into learnFromRequest
// so generateSchemaFromJson could extract field names), this function
// attaches AST facts DIRECTLY:
//   - method.parameters[name]._astValidValues    query params
//   - doc.schemas[…].properties[name]._astValidValues  body fields
//   - method._astSourceScript                     where in the JS bundle
// All AST-origin data is tagged `_astInferred: true` so pickExampleValue
// reports it under `ast-constraint` provenance — never as "observed-top"
// (which implies the SERVER received this value; the server never did).
// Stats observation counters are never bumped.
/* Hex string → Uint8Array. Pair-by-pair scan; odd-length input is treated
   as ending one nibble early (a malformed body emission). No allocation
   bloat — single typed-array allocated at exact size. */
