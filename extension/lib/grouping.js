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

/* refineByObservedPrefix + migrateToCommonPrefixBucket ARE DELETED, AND THE ONLY THING THAT MADE THEM LOOK
   ALIVE WAS A COMPARISON AGAINST A RULE NOBODY PRODUCES. Both were reached from exactly one condition, spelled
   twice in lib/learn.js: `grouping.rule === "hostname-fallback"`. `classifyInterface` above answers "origin"
   and has for as long as it has been the deterministic-origin classifier; nothing in this extension has ever
   written "hostname-fallback", so the condition was false at both sites on every request, and the clustering
   this file's own header advertised ("origin + observed prefix") ran zero times. That also made
   migrateToCommonPrefixBucket one of the five places `grouping` appeared to be produced from — a producer in a
   census and in no execution — which is why the field read as better-established than it was.
   THEY DO NOT COME BACK AS THEY WERE. Inferring a service boundary from a shared PATH PREFIX is a URL-pattern
   guess, which is the thing the classifier above refuses by name; if finer grouping is ever wanted it comes
   from the engine, out of what running the code determined, never from a regex over an address. */

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
