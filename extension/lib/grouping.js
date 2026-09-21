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

/* `pathNoUrl` IS THE PATH TO READ WHEN THERE IS NO URL OBJECT, AND IT IS THE ONLY WAY TO ASK THIS QUESTION
   ABOUT AN ADDRESS WHOSE ORIGIN THE CODE NEVER DETERMINED. lib/callsite-url.js answers `originKnown:false`
   with `url:null` for a call site whose SCHEME/HOST/PORT region holds a hole (`{cfg.apiBase}/v1/users/{id}`),
   which is a positive statement about the page and not a malformed address — and every read below is of a URL
   component, so without this the one caller that holds such an address cannot ask at all. That is what the
   early return in lib/learn.js used to do instead, and it took the whole parameter surface of those call
   sites with it.
   IT IS IGNORED WHERE `urlObj` IS PRESENT, WHICH IS DELIBERATE AND IS NOT THIS PARAMETER BEING HALF-ROUTED.
   The literal arm reads `urlObj.pathname`, which lib/callsite-url.js parses from a MASKED template, so a hole
   NAME comes back percent-encoded there — a real defect, with its own named residual at that caller, whose
   repair CHANGES EXISTING METHOD NAMES. A method name is a record's KEY, so re-spelling one is a behaviour
   change owed its own diff and its own reasoning about the records already stored under the old name; folding
   it in here would hide it inside a change about a different population. So this arm is byte-identical to
   what it answered before for every caller that has a URL.
   A HOSTNAME IS NOT ASKED FOR BESIDE IT, and the reason is that there is nothing for one to do: `startIdx`
   below strips an interface PREFIX from the path, and it fires only where `interfaceName` holds a `/`, which
   `classifyInterface` never writes — it answers the bare hostname, and the prefix clustering that once wrote
   more is deleted. On the no-URL arm `interfaceName` is the origin SHAPE and the path is the remainder beside
   it, so no part of the path is interface prefix and `startIdx` is 0, which is what the guarded read yields. */
function calculateMethodMetadata(urlObj, interfaceName, hint, pathNoUrl) {
  DCHECK(!!urlObj || typeof pathNoUrl === "string",
         "calculateMethodMetadata was given neither a URL object nor a path string — every answer below is " +
         "read off one or the other, so a caller holding an address whose origin is a shape must hand over " +
         "lib/callsite-url.js's resolved `path` or this returns a method name derived from nothing");
  const pathname = urlObj ? urlObj.pathname : pathNoUrl;
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
  if (pathname.includes("batchexecute")) {
    /* THE QUERY IS ASKED OF THE URL OBJECT OR OF NOBODY, and on the no-URL arm there is nobody BY
       CONSTRUCTION rather than by omission: lib/callsite-url.js's header records that endpoint.c builds a
       call site's address cut at the first `?` — "the query is not in this string, it is in `params[]` under
       location query" — so a template carries no query on EITHER arm and this read has always answered empty
       for a call site. `"batch"` is therefore the same answer the URL arm already gives for one, and it is
       reached here without asking a null object for a component it does not have. */
    const rpcids = (urlObj ? urlObj.searchParams.get("rpcids") : null) || "batch";
    const primaryRpcId = rpcids.split(",")[0].trim();
    return {
      methodName: primaryRpcId,
      methodId: `${interfaceName.replace(/\//g, ".")}.${primaryRpcId}`,
    };
  }

  const segments = pathname.split("/").filter(Boolean).map(_decHoles);
  const interfaceParts = interfaceName.split("/");

  // Method segments are everything after the interface prefix
  // If interface is "example.com/api/v1" and path is "/api/v1/users/get"
  // startIdx should skip "api" and "v1".

  const hostname = urlObj ? urlObj.hostname : null;
  let startIdx = 0;
  if (hostname !== null && interfaceName.startsWith(hostname)) {
    startIdx = interfaceParts.length - 1;
  }

  const methodSegments = segments.slice(startIdx);

  // No regex ID-normalization: a concrete path segment stays concrete. The engine already marks
  // genuinely-dynamic segments as {shape} holes from real data-flow (they flow through here unchanged);
  // guessing that a concrete segment "looks like an ID" and collapsing it to _id MERGES distinct real
  // endpoints — the banned name-matching. RUN, DON'T MATCH.
  //
  // A SEGMENT FILTER STOOD DIRECTLY ABOVE THAT PARAGRAPH AND WAS THE THING IT FORBIDS. It dropped any
  // segment longer than 32 characters, or containing "=", before the join — so two paths differing ONLY
  // in a dropped segment produced ONE methodName, and lib/learn.js resolves a same-name collision only
  // when the VERB differs: same verb, distinct path falls to its "no collision" arm and REUSES the
  // incumbent entry, so the second and later endpoints are never registered at all. Nine fingerprinted
  // asset paths under one directory collapsed to the directory's own name, the panel offered one row,
  // and `opt.dataset.path` sent that row to whichever of the nine happened to arrive first.
  //
  // LENGTH IS NOT A FACT ABOUT A SEGMENT. The threshold is wrong in BOTH directions at once, which is the
  // tell that it was a guess rather than a rule: a 36-char UUID is dropped while a 32-char MD5 hex is
  // kept, so it does not even achieve its own stated purpose consistently — and on the side where it does
  // fire it merges endpoints that are genuinely distinct. "=" is the same guess: /api/user=alice and
  // /api/user=bob are two addresses and became one name.
  //
  // NAMED RESIDUAL. WHAT IS NOT COVERED: an endpoint whose path carries a genuinely DYNAMIC segment that
  // no template names. `_matchTemplatedMethod` (lib/learn.js) merges a concrete request into a method
  // whose stored path holds a {…} hole — a literal segment-by-segment structural match, no scoring — but
  // only the ENGINE puts the hole there, out of real data flow. An endpoint seen only on the wire has no
  // template, so each observed value of its dynamic segment now mints its own method where a browser
  // would show one parameterised endpoint. That is the correct direction to be wrong in: an extra row is
  // a real address a reviewer can send to, and a merged row is a claim about an endpoint nobody observed.
  // WHAT THE NEXT DIFF BUILDS: the {…} hole for a wire-observed dynamic segment, emitted where the engine
  // already emits it from data flow, so `_matchTemplatedMethod` has something to match.
  // HOW ITS ABSENCE WOULD SHOW: one service's method dropdown listing many rows whose paths agree in
  // every segment but one, each row's `_stats.requestCount` standing at 1.
  let methodName = methodSegments.join("_") || "root";

  // If it's a gRPC-style path, use the actual method name
  if (pathname.includes("$rpc")) {
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
//   - …._astForcedValues                          the same, for a value EVERY sighting of which stood on a
//                                                 forced arm: a real observation and a request no client
//                                                 makes, so a pool of its own rather than an entry in the
//                                                 list the panel prefills from
//   - method._astSourceScript                     where in the JS bundle
// All AST-origin data is tagged `_astInferred: true` so pickExampleValue
// reports it under `ast-constraint` provenance — never as "observed-top"
// (which implies the SERVER received this value; the server never did).
// Stats observation counters are never bumped.
/* Hex string → Uint8Array. Pair-by-pair scan; odd-length input is treated
   as ending one nibble early (a malformed body emission). No allocation
   bloat — single typed-array allocated at exact size. */
