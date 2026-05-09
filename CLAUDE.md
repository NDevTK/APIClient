# API Security Researcher — Development Guide

Work autonomously and continuously across all pillars. No mocking, no placeholders, no heuristics, no summaries, no workarounds.
No context limits even when you get to 1M context it continues automatically.

## Project Overview

A Chrome Extension (MV3) for API discovery, protocol reverse-engineering (Protobuf/JSPB/JSON/gRPC-Web/GraphQL/SSE/NDJSON), JavaScript security code review, and security testing across all websites.

## Ideas for what to do during continuous work

Every change belongs to one pillar. After a landed change, the next change must be in a different pillar until all pillars have moved once; then rotate back. Don't stay in one pillar because it's easier.

1. **Schema learning** — map request/response schemas and URL params from observed traffic + AST call sites. Every field gets a real example value, never a default.
2. **Usable example values** — pull real captured values for every param/field; no synthesized defaults.
3. **Security issue detection** — AST-traced taint from known sources to known sinks. No name matching, no regex — only scope-resolved analysis.
4. **Service grouping quality** — group methods under the right host/path prefix. No cross-origin misattribution from concat tricks.
5. **API vs static asset classification** — magic-byte sniffing + content-type. No URL-suffix heuristics.
6. **Exploit probe** — prove execution end-to-end: NOT REPRODUCED / TAINT REACH ONLY / REAL EXPLOIT. Active payloads (img onerror) set a per-marker flag the probe checks via chrome.scripting.
7. **Real server interaction** — send learned requests through the active page (credentialed) via `schema.verify`, diff response against learned schema.
8. **Website JS review** — follow each finding back to the original JS (sourcemap-resolved). Every per-hop code snippet is captured at analysis time.
9. **Verifying & reviewing output quality** — findings are re-read against real JS; schema.review shows every leaf's example + provenance.
10. **UI review** — the popup and viewer render the same taint-path and tree-shake the reviewer would want; both inspected end-to-end with the harness before claims are made.
11. **Per-finding drill-down** — `finding`, `finding.func`, `finding.callers`, `finding.view`, `finding.exploit` together produce enough context that no guesses are needed.
12. **Harness reviewer context** — every harness command gives ground-truth AST data, not summaries: source snippets per hop, dim transitions, sanitizer report, preconditions, receiver type.
13. **Performance on real sites** — measure on a real 5 MB+ bundle. No change ships with regression > 10 % vs the previous real-site baseline.
14. **Facts not guesses** — every value the analyzer emits is traced structurally. No hardcoded strings, no depth caps, no magic numbers that paper over resolver gaps.
15. **Tooling context improvement** — when a review needed a manual step, the harness learns that step (hop snippets, receiverType, finding.view, etc.).

**Computed values are fully supported via static analysis — no runtime emulation, no sandbox, no eval.** Every URL part, header, body field, and call-site argument resolves through native ECMAScript semantics modeled in `lib/ast.js`: scope bindings, MemberExpression chains, `+` concat, template literals, `Object.assign`/spread, `Object.defineProperty` getters, `Function.prototype.call`/`apply`/`bind`, `for..in` over literal-keyed objects, WHATWG `URL` instance properties, inter-procedural param substitution, return-value unification. If a computed value doesn't resolve, the missing native-JS rule is the bug — add it to the resolver. Don't reach for runtime/eval/sandbox; don't fall back to placeholders; don't call the value "dynamic" or "computed at runtime" as if that excuses the gap.

**Always read real website JavaScript — never guess.** Before claiming a fix or a finding is correct, open the actual minified bundle in `globalStore.scriptCache` (or via `finding.view` / `finding.func` / `finding.callers` / `ast.probe.finding`) and confirm the AST decision against the real source. No assumed shapes, no "probably the framework does X", no test-only validation. If a real-site bundle is needed and not yet captured, capture it via the harness; don't reason without evidence.

**Explore real-site APIs end-to-end through the extension UI.** Static AST analysis tells you *what could happen*; the extension's job is to learn *how the API actually works in the wild* by combining (a) reading real JS via the AST pipeline to discover endpoints + schemas + auth shape + value constraints, (b) capturing real network traffic that flows when the user (or harness-driven popup) interacts with the page, (c) calling those learned methods back through the active page (credentialed) via `schema.verify` / popup `Send` and diffing real responses against the learned schema. The popup is the integration surface: when behaviour is unclear, drive the popup with the harness (`popup.select`, `popup.fill`, `popup.send`, `popup.response`) against the real site and read the JS hops that produced any surprise. Synthetic test inputs never substitute for the real-site loop; a learned method is "real" only after it has been replayed credentialed through the page and the response shape confirmed.

**Raw-JS context the analyzer leverages for learning.** The AST uses these context signals from the bundle (not just isolated fetch-call args):
- **Scope chains** — `path.scope.getBinding()` resolves identifiers through nested scopes; shadowed locals don't hijack globals.
- **Inter-procedural caller args** — `binding.referencePaths` walks every call site to substitute concrete values for params, including through wrapper functions (memoize, throttle, etc.).
- **Value constraints** — `switch/case`, `.includes(literal)`, equality chains (`x === "y" || x === "z"`) feed `_astValidValues` for params; pickExampleValue's `ast-constraint` tier surfaces them as example values.
- **Constructor chains for class methods** — `this.X` resolves through `this.X = paramRef`, `this.X = paramRef.Y`, AND `this.X = expr` (any RHS expression closed-over in the constructor's scope).
- **WHATWG URL semantics** — `new URL(input, base).href` / `.toString()` shape comes from the input arg; URL instance properties (`pathname`/`origin`/`search`/`hash`) extracted from a fully-constructed URL.
- **Native ECMAScript transforms** — `encodeURIComponent`/`encodeURI`/`decodeURI*`/`btoa`/`atob` are applied per spec when the arg resolves to a string (scope-checked globals).
- **Object.assign / spread / Object.defineProperty** — merged objects, getters resolve to underlying values.
- **Source-mapped TypeScript interfaces** — `lib/sourcemap.js` extracts interface fields and types from sourcemap'd `.ts` originals; `_tsType` / `_tsInterface` enrich learned params when names match.
- **Structural URL shape** — when full value resolution fails, `_resolveUrlStructuralShape` extracts path + query-param NAMES from concat / template literals so the schema gets the param names even with opaque values (no placeholder bytes in the URL string).

What's NOT used: JSDoc comments, runtime DOM inspection (meta tags, getAttribute values), HTTP headers from network probing. DOM-derived values genuinely don't exist statically — those bottom out in resolver gaps, which is the correct behaviour.

**Recursion is banned in our own code.** Every top-level function in scoped files (currently all of `extension/lib/`) must be iterative — public helpers, parsers, encoders, exporters alike. Use either an explicit-frame state machine on the shared `_runResolverStack` driver (for cycle-participating resolvers) or a worklist loop (for tree walkers / data flatteners). Self-recursion grows the JS call stack with input depth and crashes on adversarial inputs (deeply-nested AST bundles, protobuf payloads, JSON responses); a worklist keeps the stack at depth 1 regardless. The lint at `testing/audit-recursion.js` enforces this — it runs first in `test-ast.js` and fails the suite on any self-recursion in scoped files (no `_`-prefix discrimination — every function counts). Adding a file to the lint scope requires converting its existing recursion first; do not gate the lint behind test-only allowlists. Magic depth caps (`maxDepth = 8`, `Math.min(arr.length, 50)`) are NOT acceptable substitutes for iterative conversion — they discard data instead of handling it.

**Framework code is just JavaScript — trace through it, don't recognise it.** Every framework call (jQuery `.fail(cb)`, axios interceptors, react-query `useQuery`, redux thunks, …) eventually flows to a native browser API: `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, `postMessage`, `eval`, `innerHTML`, etc. The framework's implementation is bundled into the same source file the analyzer is reading; there is no opaque boundary. When a callback param like `responseText` traces to "no source", the right fix is NOT to add jQuery-specific knowledge ("`.fail` callbacks get XHR.responseText") — it's to make inter-procedural tracing follow the framework's OWN code into the native API call. The framework's `function fire(){ fn(jqXHR.responseText) }` is right there in the bundle; resolving the wrapper function's `referencePaths` reaches it. Framework recognition is always a workaround; bundle-deep tracing is always the proper fix.

Concretely: a callback-param trace on `errCb` in `function getXhr(url, errCb){ ... xhr.onerror = function(){ errCb(xhr); }; }` must follow `errCb`'s referencePaths to find the `errCb(xhr)` invocation, then resolve the arg `xhr` to a `new XMLHttpRequest()`, classifying it as the native taint source. This walk is mechanical (not pattern-matched) but currently the tracer doesn't run it for callback-param `responseText` reads — closing that gap is the proper fix for jQuery `.fail`, axios `.catch`, fetch `.then`, etc. simultaneously.

**Read the real JS yourself — don't rely on the analyzer to see what it's missing.** The analyzer can only report patterns it already understands; gaps it misses by definition won't show up in `audit.gaps` or `audit.schema`. To find new learnable shapes, fetch the actual bundle (via `script <url>` or direct HTTP), read the source around fetch call sites, and reason about how URLs / params / bodies are built. Ask: what string is the path? where does each query/body field's value come from? is it a literal, a switch case, a ternary on input, an object property, a closure variable, a class member set in constructor? For each value-source, check whether `_resolveAllValues` / `_resolveToObject` / `_resolveUrlStructuralShape` already handles that shape — if not, that's the next resolver path to add. Doing this manual review on a real bundle is the only way to surface missing logic; the analyzer's own output is a downstream view.

**Audit AST learning completeness on real source.** For every fetch call site the AST extracted, ask: did we learn EVERY URL parameter (query, path, fragment, body field) the source visibly assembles, with REAL example values (literal in source, `_astValidValues` from switch/includes/equality chains, observed value from real traffic)? If the source contains `searchParams.set("page", n)` / `?nwo=${encodeURIComponent(repo)}` / `body: JSON.stringify({title, content})` and the learned method is missing one of those params or has a type-default empty-string example, the AST resolver has a gap. Use `audit.gaps` to surface unresolved leaves, `method <svc> <id>` to read learned params + their `_exampleValueSource`, and `finding.func` / `finding.callers` to read the original code that should have produced them. Type-default fallbacks on a method derived from real source are a P1 bug — the value WAS in the bundle; the resolver missed it.

**Structural learning vs. value placeholders — they are different.** The "no placeholders" rule bans inventing fake VALUES (fake URL strings, fake body field values, fake header values). It does NOT ban STRUCTURAL learning when the structure is statically visible. A fetch like `fetch("/api/check?nwo=" + encodeURIComponent(repo))` has a fully-resolvable PATH (`/api/check`), a fully-resolvable PARAM NAME (`nwo`), and an opaque PARAM VALUE (`encodeURIComponent(repo)` where `repo` is a function param). The right output is: emit the method `GET /api/check` with param `{name: "nwo", location: "query", required: true, _astValueSource: "param-derived"}` and NO synthesized value. Refusing to emit anything because the value is opaque drops a learnable URL+param shape; emitting `?nwo=` as a literal value would be a placeholder. The line is: structure (paths, names, locations, types) is fair game even when values are opaque; values themselves must come from observation, AST literals, or scope-traced caller args.

**Missing-logic / missing-learning visibility.** Three commands together surface what the AST or schema learner *should* have caught but didn't:
- `audit.schema <svc> --min-obs 0` — every method (including AST-only) sorted worst-first by real-observation %; type-default-heavy methods are where param NAMES are learned but VALUES aren't.
- `audit.gaps [substr]` — every unique URL-resolver gap with the per-script line/col + source snippet; each gap is an identifier the resolver couldn't trace.
- `method <svc> <id>` — per-method drill-down with `_exampleValueSource` per param (observed-top, ast-constraint, param-derived, type-default, …); a real-source method whose params are all type-default is a P1 learning gap.

**Source-review tooling — what's available.** Reviewing real-site source goes through these commands (all give ground-truth, none summarise):
- `script <url|index>` — raw bundle dump under `testing/harness-dumps/`.
- `ast <url|index> [--full]` — every AST finding the analyzer produced for a script (enums, fetchSites, sinks, dangers, fieldMaps).
- `audit.sinks [substr]` — grep + AST cross-check; surfaces classifier-missed sinks.
- `audit.gaps [substr]` — every unique resolver gap with its per-script line/col + source snippet (added 2026-05).
- `finding <id>` — one finding's full taint path with per-hop dim transitions, code context, and enclosing function.
- `finding.func <id>` — source-mapped enclosing function body.
- `finding.callers <id>` — call sites of the enclosing function with arguments.
- `finding.view <id>` — the source viewer's tree-shaken slice (verifies the popup viewer's own focus).
- `ast.probe.finding <id>` — isolate the enclosing function and re-run the analyzer to confirm classifier polarity.
- `ast.probe --inline <code>` — run the live analyzer on an arbitrary code blob (synthetic minimal repros).

If a review needed a manual step that none of these covered, the harness has a gap (Pillar 12) — add the command before the gap is forgotten.

**Continuous workflow across every task type.** Schema learning, security detection, service grouping, exploit probing, server interaction, JS review, UI review, performance, harness context — all pillars are first-class. Never stop, never ask for confirmation before substantial reversible changes, never surface a limitation as a question; fix it. No workarounds means don't accept gaps — close them without shortcuts. The loop ends only on an irreversible external block.

## Operational Rules

**Output rule.** Output text only when progress is blocked by something external (missing credential, ambiguous irreversible choice). Otherwise call tools. Never write "tests pass, the fix is in" style summaries. The diff is the record.

**Rotation cadence.** After any non-trivial landed change, the next non-trivial change must be in a different pillar. Don't cluster.

**Verification protocol.** A change is landed only after it has been:
1. Exercised on the real site where it applies — the finding/fix seen, diffed, or measured end-to-end.
2. Exercised on at least one site where it should NOT apply — confirming no regression.
3. Snapshot comparisons preferred: `finding.snapshot <name>` before, `finding.diff <name>` after — the diff is the proof.

Tests are for detector polarity (positive + negative, pinned to similar-shape inputs). Tests are never a substitute for real-site verification.

**Resolver-gap ownership.** Any `resolverError` a batch produces is a P1. The next task after the batch is to add the missing resolution path — inter-procedural, scope-aware, whatever's needed. `_resolveAllValues` throws when a concat term can't be traced; the caller catches and records the gap. Don't paper over with placeholders, don't raise an excuse, don't use the word "unresolvable". Close the gap.

**Timeout root-cause.** A command that hangs past 30 s means the service worker or offscreen worker is stuck. Inspect `_scriptBuffers`, `_tabMeta`, the offscreen document via harness. Don't poll-retry. Don't raise the timeout. Find what's blocked.

**Decision log.** When a choice is ambiguous but reversible, pick the biggest-impact path, record the reason in the code comment or commit message, and proceed. Only pause on irreversible choices (destructive ops, shipping).

**Phase discipline.** Each task has three phases: investigate → change → verify on real site. Finish phase N before starting N+1. Don't interleave; don't claim done mid-phase.

**Comments policy.** No preachy comments, no self-congratulation, no "carefully removed the heuristic" narration. A comment explains WHY a non-obvious decision was made, not what the code does or what rule it follows.

**Balance, not breadth.** Touching all 15 pillars isn't the goal — the goal is that no one pillar grows while others rot. If a fix in pillar 3 creates a harness-context gap, pillar 12 is next.

## Never Do

- **Heuristics after being told no heuristics.** No magic numbers (`bfsDepth: 2`, `MAX_DEPTH = 10`), no name-based matching (`if (callee.name === "fetch")` without scope check), no scoring (A-F grades, confidence percentiles).
- **Placeholders / opaque fallbacks.** No `{foo}`, no `{param0}`, no `"unresolvable"`, no `_describeNode()` string lookups, no returning a partial concat as if complete. Computed values get fully resolved via the existing machinery — or the gap is reported and closed.
- **"unresolvable" / "can't" in code or tests.** The word is a lie; every value has a resolution path the analyzer either has or needs.
- **Ceremonial end-of-turn summaries.** No "tests 748/748 pass, the fix is landed". The diff and the tool output speak for themselves.
- **Stopping because the current micro-task is done.** Rotate pillars; pick up a resolver gap; verify on a new real site. Only stop on an irreversible external block.
- **Ignoring timeouts.** See operational rule above.
- **Claim a change works before real-site verification.** Tree-shake line counts, finding reviews, perf numbers — one run is not a claim.
- **Staying in one pillar because it's easy.** Security FP cleanup gets addictive; schema / service / UI work gets skipped. Rotate.
- **Synthetic tests standing in for real data.** A polarity test never substitutes for running the change against a real bundle.
- **Preachy or self-congratulating comments.** See comments policy.
- **Shipping mocked or placeholder responses.** Every fetched value, every schema example, every fetch-call-site URL is from an observed or fully-resolved source.

## Core Architecture

- **Request/Response Capture**: `intercept.js` runs in the page's main world (`world: "MAIN"`, `document_start`), wrapping `fetch()`, `XMLHttpRequest`, `WebSocket`, and `EventSource` to capture both request data (headers, body) and response data (status, headers, body). Single capture point — no `webRequest` permission needed. Data flows: `intercept.js` → `CustomEvent(__uasr_resp)` → `content.js` relay → `chrome.runtime.sendMessage(RESPONSE_BODY)` → `background.js` `handleResponseBody()` which creates log entries, extracts keys, learns schemas, triggers discovery/probing.
- **Protocol Handlers** (in `lib/discovery.js`):
  - `parseBatchExecuteRequest/Response` — Google batchexecute RPC
  - `parseAsyncChunkedResponse` — Google hex-length-prefixed async chunks
  - `parseGrpcWebFrames` / `encodeGrpcWebFrame` — gRPC-Web frame codec
  - `parseGraphQLRequest/Response` — GraphQL query/variables/operationName
  - `parseSSE`, `parseNDJSON`, `parseMultipartBatch` — streaming/batch formats
  - `convertOpenApiToDiscovery` / `convertDiscoveryToOpenApi` — OpenAPI 3.0 bidirectional conversion
  - `lib/protobuf.js`: Wire-format codec, `jspbToTree`, recursive base64 scanning
  - `lib/req2proto.js`: Universal error-based schema probing (Google-specific + generic)
- **Schema Learning**: VDD engine maps request/response schemas and URL parameters from observed traffic. `learnFromResponse()` processes captured response bodies through the format chain: async chunked → batchexecute → gRPC-Web → SSE → NDJSON → multipart → GraphQL → JSON → protobuf.
- **Autonomous Discovery**: `buildDiscoveryUrls` probes well-known paths for official API documentation on new interfaces.
- **Collaborative Mapping**: Field and parameter renaming persisted in IndexedDB (via globalStore).
- **OpenAPI Export/Import**: `EXPORT_OPENAPI` converts a service's VDD to OpenAPI 3.0.3 (with `x-field-number` extensions for protobuf round-trip). `IMPORT_OPENAPI` converts OpenAPI specs into the internal format, merging with existing learned data.
- **Session Persistence**: Request logs saved to `chrome.storage.session` with 1-second debounced writes per tab. Survives MV3 service worker restarts, clears on browser close.
- **Cross-Tab Logs**: Popup filter dropdown shows requests from active tab, all tabs, or a specific tab. Closed tab logs are retained.
- **Auto-Determined Encoding**: Content-Type (`currentContentType`) and body mode (`currentBodyMode`) are set automatically from the VDD schema or replayed request headers — no manual dropdowns.
- **UI Management**: State-aware rendering in `popup.js` with 100ms render throttling, scroll position preservation, and independent panel navigation.

## File Map

| File | Role |
|------|------|
| `extension/manifest.json` | MV3 manifest. Permissions: `storage`, `offscreen`. Host permissions: `<all_urls>`. |
| `extension/intercept.js` | Main-world content script. Wraps `fetch`/`XHR`/`WebSocket`/`EventSource`, captures request headers+body and response headers+body. Emits `__uasr_resp` CustomEvent. Single capture point for all network data. |
| `extension/content.js` | Isolated-world content script. DOM key/endpoint scanning, `PAGE_FETCH` relay for session-aware requests, `__uasr_resp` event relay to background. |
| `extension/background.js` | Service worker. Request interception, key extraction, schema learning, request export builder, OpenAPI export/import, session storage, message routing. |
| `extension/popup.js` | Popup controller. Tab rendering, service filter, cross-tab log filtering, replay, export (curl/fetch/Python/OpenAPI). |
| `extension/popup.html` | Popup markup. |
| `extension/popup.css` | Popup styles. |
| `extension/viewer.html` | Source viewer markup. |
| `extension/viewer.js` | Source viewer: tree-shaken focused view per finding, click-to-definition via AST-built defMap/propDefs/refMap. |
| `extension/ast-worker.html` | Offscreen document that hosts the AST Web Worker. |
| `extension/ast-thread.js` | Web Worker loading Babel + `lib/ast.js`; handles `AST_ANALYZE`, `AST_BUILD_DEFINITION_MAP`, `AST_ANALYZE_BATCH`, `AST_PARSE_SOURCEMAP`, `AST_EXTRACT_TYPES`. |
| `extension/lib/discovery.js` | Protocol parsers (batchexecute, async chunked, gRPC-Web, GraphQL, SSE, NDJSON, multipart), OpenAPI bidirectional conversion. |
| `extension/lib/protobuf.js` | Protobuf wire-format codec, JSPB decoder, recursive base64 key scanning. |
| `extension/lib/req2proto.js` | Error-based schema probing engine. |
| `extension/lib/ast.js` | AST-based JS bundle analysis. Babel parser + traverse with scope-aware inter-procedural tracing. Extracts fetch call sites, proto field maps, enums, value constraints. Security code review: DOM XSS sinks, dangerous patterns (eval, postMessage, prototype pollution, open redirect), taint source tracking. Structural-def collection (defMap / propDefs / funcMap / allFuncRanges) piggy-backs on the same pre-pass so the viewer doesn't need a second Babel pass. |
| `extension/lib/sourcemap.js` | Source map recovery. Babel parser with TypeScript plugin extracts interfaces, enums, type aliases from original sources. |
| `extension/lib/babel-bundle.js` | Bundled Babel runtime (parser, traverse, types). Built by `node build.js` via esbuild. |
| `build.js` | esbuild config for bundling Babel into an IIFE for the service worker. |
| `testing/harness.js` | Reviewer-facing CLI. `findings`/`finding`/`finding.func`/`finding.callers`/`finding.view`/`finding.exploit`/`ast.probe`/`services`/`method`/`schema.verify`/`audit.sinks`/`audit.schema` plus popup drive commands. |

## AST Analysis Policy

All AST analysis uses Babel's scope system (`path.scope.getBinding()`) for variable resolution and data flow tracing. Never match syntax patterns directly — always resolve through scope. No regex, no string pattern matching, no name-based lookups (code is minified — names are meaningless). No scoring or heuristics.

**Scope-aware traversal rule**: All sub-tree traversals use `path.traverse(visitor)` on Babel paths — never `_babelTraverse(rawNode, visitor)` on raw AST nodes. The only `_babelTraverse` calls are the two top-level Program traversals (pre-pass and main pass). Functions that resolve callees (`_resolveCalleeToFunction`, `_resolveHandlerFunc`, etc.) return Babel paths, not raw nodes, so downstream code can call `funcPath.traverse()` for scope-aware walking. No `noScope: true` anywhere — every traversal has full scope access.

**No caps or depth limits**: AST analysis must handle arbitrarily large scripts (1MB+ minified bundles) without losing data. No script size limits, no depth limits that discard data, no `MAX_DEPTH` constants. ALL recursive AST walkers must be iterative — explicit stacks, generator-driver loops, or iterative chain-walks. The JS call stack is finite (~10k frames); minified or adversarial code can produce arbitrary AST depth in any node type (concat chains, ternary chains, deeply nested object literals, deeply nested calls, recursive var-aliasing, etc.). Don't restrict iterative conversion to "chainable types only" — tree-recursive walkers stack-overflow on unbalanced trees too. Convert every recursive AST walker.

**Resolver completeness**: `_resolveAllValues` traces every URL-producing expression to literal values. BinaryExpression `+` concats throw when any term can't be traced — that's a resolver gap, not an "unresolvable value". `_extractFetchCall` catches the throw, records the gap on `result.resolverErrors`, and skips the fetch site (no false-relative URL attributed to the page origin). Closing resolver gaps — adding the missing resolution path for the identifier that threw — is a P1 follow-up, not optional.

**Global verification via scope**: When detecting global sinks (`fetch`, `XMLHttpRequest`) or global sources (`location`, `document`, `window`), always verify the identifier is unbound: `!path.scope.getBinding("fetch")`. A local variable shadowing a global name is not a sink/source.

- **Trace to network sinks**: The browser has two network primitives — `fetch()` and `XMLHttpRequest`. All JS network calls flow through them. AST identifies these global sinks and traces backward through scope bindings to extract URLs, methods, headers, body structure, and query params.
- **Inter-procedural tracing**: When a sink argument is a function parameter, use `binding.referencePaths` to find all call sites and resolve the concrete values passed by callers. Trace through wrapper function chains using cycle detection (visited-set), not depth limits.
- **Resolve values**: Use `path.scope.getBinding()` to follow variable references through proper scope chains. Collect value constraints from switch/case, `.includes()`, equality chains scoped by `scope.uid`.
- **Type tracking**: `_typeEnv` maps `scopeUid:varName` to deterministic types (e.g., `new XMLHttpRequest()` → `"XMLHttpRequest"`, `[...]` → `"Array"`, `new Map()` → `"Map"`). `_getTrackedType(path, node)` resolves an identifier's type through scope; falls back to `_inferTypeFromAssignments` which memoises the traversal of `binding.constantViolations` on the binding object. Used to disambiguate `.open()` (XHR vs other objects), iterator methods (`.forEach()` on Array vs non-iterable), and collection-lookup receivers (`.get()` on Map vs URLSearchParams). Types are scope-keyed so shadowed constructors don't inherit outer types.
- **Taint source classification**: `_matchTaintSource(path, node)` walks MemberExpression chains structurally, verifies the root identifier is unbound via scope, normalizes `window.`/`self.` prefixes, and matches against taint patterns. Replaces string-based `_describeNode()` lookup for security decisions.
- **Dimensional taint model**: Every taint hop carries `{origin, path, query, hash, content}` dims. `origin` is the sole sink-check signal for open-redirect / request-forgery. `location.href` has no origin dim (current-origin locked); free-form sources (`storage.getItem`, `event.data`, `cookie`, `window.name`) have origin. Transforms strip URL-part dims and upgrade origin when the structural marker is removed (`location.hash.slice(1)`).
- **Per-hop code snippets**: `_tvsSource` and `_tvsHop` capture a ±70-char window from the bundle at each hop's position (`_snippetAtLoc`). The popup taint-path accordion and `finding <id>` both render these inline; no separate `finding.map` call per hop.
- **CFG + sanitizer path analysis**: `_buildCFG(bodyPath)` builds a basic-block control flow graph from a function body path. `_hasSanitizerOnAllPaths()` checks whether all paths from a taint source's block to a sink's block pass through a sanitizer. Sanitizers are verified scope-aware (`_isSanitizerCall` checks `!path.scope.getBinding()` for globals like `DOMPurify`, `encodeURIComponent`). If sanitized on all paths, severity is downgraded to `"info"` rather than suppressed.
- **Feed into the learning pipeline**: AST call sites are converted to synthetic requests and passed through `learnFromRequest()` — the same function that learns from real network traffic. AST learns APIs before they happen; network traffic confirms and enriches them.
- **Page-origin attribution**: relative fetch URLs from AST call sites resolve against the PAGE's origin (the tab URL), not the script's host. A script served from `www.redditstatic.com` that calls `fetch('/svc/shreddit/graphql')` on a `www.reddit.com` page hits reddit at runtime, and the learned method is attributed to `www.reddit.com`. Same rule in `learnFromAstCallSite` and in the endpoint-registration block of `_analyzeCombinedScripts`.
- **No protocol classification**: AST does not classify calls as gRPC/REST/RPC. Protocol detection is the network monitoring code's job.
- **Security sink detection**: Detect DOM XSS sinks (`innerHTML`, `outerHTML`, `document.write`, `eval`, `new Function`, `insertAdjacentHTML`, `setTimeout`/`setInterval` with string arg, `setAttribute("on*")`) and open redirects (`location.href`/`location.assign`/`location.replace`). All sinks only flag when the value traces to a **user-controlled source** — no vulnerability exists without attacker-controlled data reaching the sink. Sink entries carry `receiverType` (e.g. `Element`, `FormData`, `URL`) so reviewers see at a glance whether the sink object is a real DOM receiver.
- **Taint source tracking**: `_traceValueSource()` classifies value origins by tracing through scope bindings, function params, string concatenation, and method calls. Known user-controlled sources: `location.*`, `document.referrer`, `document.URL`, `document.cookie`, `window.name`, `event.data`. Propagates taint through variable assignment chains and method calls on tainted objects. Collection-lookup methods (`.get`/`.has` on tracked `Map`/`Set`/`WeakMap`/`WeakSet` receivers) do NOT propagate key-taint to the return value — the stored value is independent of the key the attacker chose.
- **Dangerous pattern detection**: Detect `postMessage` listeners without `event.origin` checks, prototype pollution (`obj[userControlledKey] = value`), dynamic `RegExp` with user-controlled patterns. Prototype pollution and RegExp only flag when the key/pattern traces to a user-controlled source — dynamic keys are ubiquitous in minified code (iterators, object merges, polyfills).
- **All-dims-false suppression**: both `_pushSink` and `_pushDangerous` drop findings whose source's dims are all false (every URL-part dim stripped along the chain + no content dim). That's an attacker-reach label on a value the attacker can no longer influence — unactionable signal.
- **No sensitive data scanning**: API key and token extraction is handled by `KEY_PATTERNS` in `background.js` — AST does not duplicate this.

## Development Standards

- **Naming**: `camelCase` for logic, `UPPER_SNAKE_CASE` for constants. Unified `methodId` format: `interface.name.method`.
- **MV3 Compliance**: No `webRequest` or debugger API. All network capture via main-world prototype wrapping in `intercept.js`.
- **Message Routing**: `chrome.runtime.onMessage` routes by sender origin — extension pages go to `handlePopupMessage`, content scripts go to `handleContentMessage`. Allowed content script types: `CONTENT_KEYS`, `CONTENT_ENDPOINTS`, `RESPONSE_BODY`.
- **UI Security**: Strict origin checks in `onMessage` handlers. All dynamic content passed through `esc()` to prevent XSS.
- **Data Persistence**: `scheduleSave()` for global store writes (IndexedDB, inaccessible to content scripts), `scheduleSessionSave(tabId)` for per-tab request log writes (`chrome.storage.session`, 1s debounce). GlobalStore uses IndexedDB instead of `chrome.storage.local` to prevent compromised renderers from reading cross-site structural metadata.
- **Intercept Script Safety**: `intercept.js` runs in main world — never blocks the caller (async body reads). Uses IIFE to avoid global pollution. Filters internal requests (`_uasr_send`, `_internal_probe` hashes). Static asset and noise path filtering done in `background.js` `handleResponseBody()`.
- **Send Panel**: Content-Type and body mode are auto-determined (no manual dropdowns). `currentContentType` set from `schema.contentTypes[0]` or replayed request headers. `currentBodyMode` set to `form` (schema loaded), `graphql` (GraphQL URL), or `raw` (fallback). `setBodyMode()` toggles panel visibility.

## Security Model

**Threat**: A compromised renderer process can execute code in the content script's isolated world, gaining access to any `chrome.storage.local`/`.sync` APIs the extension has permission for. This leaks cross-site data — a renderer compromised on site A could read structural metadata (API schemas, endpoint URLs, field names) learned from sites B, C, D.

**Trust boundaries**:

| Context | Process | Capabilities | Trust level |
|---------|---------|-------------|-------------|
| `intercept.js` (main world) | Web page renderer | Same-site fetch/XHR only. No extension APIs. | Untrusted — same origin as page |
| `content.js` (isolated world) | Web page renderer | `chrome.runtime.sendMessage` (type-restricted), DOM access. No storage APIs used. | Low trust — restricted message whitelist |
| `background.js` (service worker) | Extension process | Full extension APIs, IndexedDB, `chrome.storage.session` | Trusted — privileged boundary |
| `popup.js` (extension page) | Extension process | `chrome.runtime.sendMessage` to background | Trusted |

**Storage isolation**:

- **GlobalStore** (schemas, endpoints, API keys, probe results): IndexedDB in the service worker origin. Content scripts cannot access IndexedDB for `chrome-extension://` origins, so a compromised renderer has no direct read path.
- **Request logs** (URLs, headers, bodies): `chrome.storage.session` with default `TRUSTED_CONTEXTS` access level. Content scripts excluded. Auto-clears on browser close — no persistent URL leakage.

**Message gating**: `chrome.runtime.onMessage` routes by `sender.url` origin (set by the browser process, unforgeable by renderers). `sender.id` only rejects other extensions — since we inject into every page, compromised renderers already have our `sender.id`. Content scripts can only send `CONTENT_KEYS`, `CONTENT_ENDPOINTS`, `RESPONSE_BODY`. All other message types (including data-returning ones like `GET_STATE`, `GET_ALL_LOGS`, `GET_TAB_LIST`) are rejected for non-extension-page senders. See `SECURITY.md` for full threat model.

## Common Tasks

- **Extend Key Patterns**: Update `KEY_PATTERNS` in `background.js`.
- **Adjust Method Heuristics**: Modify `calculateMethodMetadata` in `background.js`.
- **Add Export Format**: Add a `formatXxx()` function in `popup.js` and a button in `popup.html`. The `BUILD_REQUEST` message handler in `background.js` returns the fully-encoded `{ url, method, headers, body }`.
- **Add Intercepted Content Types**: Modify `isBinary()` in `intercept.js` for binary encoding detection. Add static asset extensions to the filter regex in `handleResponseBody()` in `background.js`.
- **Add Protocol Parser**: Add parser + detector in `lib/discovery.js`, add `learnFromResponse()` branch in `background.js`, add renderer in `popup.js` (wire into `renderResultBody()`), add format badge.
- **UI Changes**: Ensure new components maintain scroll position in their respective panels. The Send panel includes export buttons (curl, fetch, Python).
- **Cross-Tab Features**: Tab metadata tracked in `_tabMeta` Map. Filter logic in popup controlled by `logFilter` state variable. New message types: `GET_TAB_LIST`, `GET_ALL_LOGS`.
- **OpenAPI Export/Import**: Service-level via `EXPORT_OPENAPI`/`IMPORT_OPENAPI` messages. `convertDiscoveryToOpenApi()` / `convertOpenApiToDiscovery()` in `lib/discovery.js`. Service selector in popup filters methods and controls export scope.
- **Add Security Pattern**: For new DOM XSS sinks, add detection to `_processSecurityCallSink` (call-based) or `_processSecurityAssignSink` (assignment-based) in `lib/ast.js`. For new dangerous patterns, add to `_processDangerousPattern` (call-based) or `_processDangerousAssignment` (assignment-based). Use `_traceValueSource()` to classify severity. New taint sources go in `_TAINT_SOURCES`. Polarity tests in `test-ast.js` (positive + negative). Real-site verification required — one site where it fires, one where it must not.
- **Close a resolver gap**: `_resolveAllValues` throws an error naming the identifier. Add the resolution path (new ImportDeclaration handling, new MemberExpression case, new function-return path, etc.). Polarity test + real-site re-run.
