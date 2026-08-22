# API Security Researcher

A Chrome extension (MV3) that reverse-engineers a site's API surface and finds client-side XSS by **running the page's own JavaScript bundle for real** — under forced, multi-path exploration on a forked QuickJS-ng engine (compiled to WebAssembly) with a real Lexbor spec DOM. No debugger, no proxy, no `webRequest`.

The one sentence: **a browser with a BFS time-travel solver.** The browser half (Lexbor DOM + patched quickjs-ng, in C) is spec-faithful and boring by design; the solver half — forced multi-path concolic execution over record/replay snapshots — is the novel part, and it does one thing no scanner does: compute the **logged-in API surface while logged out**, and construct **replay-verified XSS PoCs**, from code that never ran.

## How It Works

Browse any site. In the background:

1. **The engine runs the bundle.** The unmodified JavaScript executes on the forked QuickJS-ng (WASM) inside an offscreen Web Worker, over a real Lexbor DOM/CSS/`URL`. Closures, prototypes, `Function.prototype.call/apply/bind`, `Proxy`, async, and framework state machines (jQuery `$.ajax`, axios, react-query) all evaluate with exact ECMAScript semantics because it *is* a real engine, not a re-implementation.
2. **Forced multi-path exploration** drives the branches real input could gate — login/flag/route-gated code, lazy-loaded chunks, never-called functions — so the engine reaches the auth/admin/billing endpoints an SPA ships to logged-out visitors but never fires. Every arm is a BFS flow, value-ordered by one work-fair queue, snapshotted (copy-on-write heap + DOM), preemptible, and resumable across browser sessions.
3. **External input is concolic**, not a placeholder: each attacker-controlled source (`location.hash/search`, `document.cookie`, `postMessage` data/origin, storage, response bodies) carries a source identity, the per-flow path constraints it accumulated, and a concrete example when the code pins or computes one. Control flow forks on it (reaching gated code) while real values propagate forward.
4. **The result is one structured record** the engine emits and the offscreen bridge consumes: endpoints with computed URLs/methods/headers/body fields and example values (`@H`), and security sinks with **replay-verified** PoCs (`@S`).
5. **Passive traffic still teaches it too.** Observed fetch/XHR/WebSocket/SSE/postMessage traffic is decoded through a protocol chain (JSON, JSPB/protobuf, gRPC-Web, batchexecute, multipart, SSE/NDJSON, GraphQL) and merged into the same service map; the extension also probes well-known documentation paths (OpenAPI, Google Discovery). Endpoints learned *only* from runtime hooks are a signal that forced execution can be driven harder — the engine is the primary source.

Open the popup to inspect, test, and export everything it found.

## Features

### API discovery & schema learning
- **Forced-execution discovery.** Fully-computed URLs/methods/headers/body fields for endpoints that haven't been called yet — including login-gated code in lazy chunks.
- **Value-constraint extraction.** Valid parameter values are whatever the executed code computes (`switch`/`case`, `.includes()`, equality chains evaluate naturally), surfaced as dropdowns in the Send panel. A value is concrete only where the code *pins* it (an equality gate) or *computes* it; a range/prefix/regex gate stays a domain-annotated shape — never a fabricated in-range guess.
- **Passive learning (VDD).** Every observed request/response teaches URL patterns, query/body params, field types, content types, and auth — merged into a unified per-service map.
- **Protocol reverse-engineering.** JSON/JSONP, Protobuf/JSPB (wire codec + tree decoder + recursive base64), gRPC-Web, Google batchexecute, async-chunked, SSE/NDJSON, multipart, GraphQL, WebSocket, postMessage/MessageChannel.
- **Documentation discovery & source maps.** Probes OpenAPI/Google-Discovery at well-known paths (with version/visibility/auth variants); recovers source maps for TypeScript interfaces/enums and unminified security review; error-based schema probing learns field requirements from malformed-request responses.
- **Key extraction.** Recursive base64 scan of URLs/headers/bodies/DOM for known key/token shapes (Google, Firebase, JWT, Bearer, Stripe, Mapbox, GitHub, …); cookie values are never stored, only presence.

### Client-side security review
Taint is observed during the *same real execution* used for API learning — no separate traversal, no regex, no name matching. Works on minified code because names are irrelevant: a sink is what an executed call reaches at the host edge.

- **Attacker sources** are host-edge data made *concolic* and infectious through member-get / call / `JSON.parse` / iteration: `location.hash/search`, `document.cookie`, storage, `window.name`, XHR/`fetch` response bodies, `postMessage` `data`/`origin`. DOM *structure* stays concrete so frameworks boot. Each source carries its **browser constraints** — `location.hash`/`search` are percent-encoded per the WHATWG per-component sets, a forgeable `message.origin` check is solvable while an exact `===` is not.
- **Sinks** are recorded only when a *tainted* value reaches them: HTML (`innerHTML`/`outerHTML`/`insertAdjacentHTML`/`document.write`/`srcdoc`), code-exec (`eval`/`Function`/string `setTimeout`), navigation (`location`/`.href`/`.assign`/`.replace`), script-URL (`<script>.src` to an attacker origin), on\* handlers. A concrete write (e.g. jQuery feature-detection `innerHTML`) is not a finding and costs nothing.
- **PoCs are constructed, not templated — and proven by firing.** For a reached sink the engine *searches* concrete inputs through the **real** filters/encoders/gates as ordinary flows until one breaks out at the sink, then requires the fire-marker to actually execute (not merely appear). The PoC reflects the code: its encoding, context-escape, firing vector, gate-prefix, and delivery are determined by the observed filter + parse context + path + source. **No external Z3, no taint tracker, no transform-inversion — re-executing the real (interprocedural, shared-mutable-state) code discharges the constraints.**
- **Policy-relative.** A breakout in the model isn't a working exploit until it survives the page's real policy: CSP is parsed for the concrete bypass path (an allowlisted JSONP gadget-host, a loaded script-gadget library, `strict-dynamic`, or `nonce-protected` — a per-request nonce is *not* injection-leakable), and Trusted Types is enforced at the sink by **running** the page's `default` policy `createHTML` to see if it actually sanitizes.
- **Live verification.** The engine's exact PoC is delivered to the real page in a sandboxed attacker popup (opaque origin = real cross-origin attacker); it correlates back via a `crypto.randomUUID` inside the payload. A hit is a ground-truth **REAL EXPLOIT**; no hit is an engine-fidelity divergence to fix — never a re-derived PoC.

### Replay & export
Message console for live WebSocket/postMessage/MessageChannel; session-aware replay via the page-context `PAGE_FETCH` relay; a form builder auto-generated from learned schemas (enum dropdowns from computed values + observed traffic); HAR / curl / fetch / Python / OpenAPI export and OpenAPI import; per-field renames persisted in IndexedDB.

## Architecture

Two halves, never blurred: the **C engine is the browser and owns all analysis**; the JavaScript is the irreducible platform edge (relay network/storage/`chrome.*`, load the wasm, render the popup) — decomposed one-problem-per-file.

```
intercept.js       Main-world fetch/XHR/WebSocket/EventSource wrapper (request + response capture)
content.js         Isolated-world content script — ships raw HTML/scripts/responses + PAGE_FETCH relay +
                   postMessage/MessageChannel listener; messages the offscreen brain directly
background.js      Thin, STATELESS service worker — extension-page-only. Owns the offscreen lifecycle,
                   forwards browser events (__evt: webNavigation/tabs), performs the privileged chrome.*
                   the offscreen lacks (__rpc: tabs.*, scripting). Holds NO state, does NO fetch.
offscreen-brain.js The offscreen CORE (stable lifetime + IndexedDB): globalStore + content/frontier message
                   routing + doc/log lifecycle. WIRES the lib/* components below; holds no analysis logic.
popup.js           Popup controller (rendering, replay, form builder, security panel)
ast-worker.html    Offscreen document — loads the lib/* components + offscreen-brain.js + the analysis worker
ast-worker.js      In-document bridge: exposes self.astDispatch (drive the wasm) and streams results

lib/               The analysis, decomposed — one problem per file, loaded before offscreen-brain.js
                   (classic <script> global scope; each resolves its callers at call-time):
  schema.js          VDD schema inference — JSON + JSPB/protobuf trees → JSON-Schema shapes, merge/drift
  keys.js            API-key/token extraction (recursive base64 scan)
  persistence.js     IndexedDB store save/load (chrome.storage.local is banned)
  grouping.js        Endpoint identity — group by real origin + observed prefix, derive method name/id
  learn.js           VDD passive learning — from AST call-sites, observed requests, observed responses
  response-decode.js Response protocol chain (JSON/JSPB/gRPC-Web/batchexecute/multipart/SSE/…)
  encode.js          Request-body encoding for replay/export (GraphQL, form→JSON/JSPB/protobuf wire)
  discovery-probe.js Active doc discovery — OpenAPI/Google-Discovery fetch, req2proto probing, virtual docs
  analyze.js         Engine-worker orchestration — hash+cache scripts, drive astDispatch, source-map recovery
  merge.js           Engine @RESULT → doc model merge + cross-tab/global aggregation
  send.js            Send-panel replay — resolve schema, coerce values, execute in page context
  popup-handlers.js  Popup command dispatch (state/send/export/rename/clear/exploit-probe/console)
  serialize.js       Serialize merged state for the popup + persistence
  sourcemap.js       Source-map recovery (TS interfaces/enums/aliases)
  discovery.js       Protocol parsers, schema resolution, bidirectional OpenAPI conversion
  protobuf.js        Wire-format codec, JSPB decoder, recursive base64 scan
  req2proto.js       Error-based schema probing
  safe-fetch.js      The ONE external-fetch path (GET, cookies omitted, http(s) only)

engine/
  host/main.c        The scheduler: the ONE BFS work-fair queue, per-flow COW heap+DOM deltas, boot-as-flow,
                     branch forcing, @H/@S detection, the @S candidate solver. The forced-exec brain.
  host/browser/*.c   Spec-faithful Web APIs, each a real component built from its Web IDL (Blob, Response,
                     TrustedTypes, CSSOM, cookies, crypto, CSP analysis, …) over the Lexbor DOM.
  host/solver/*.c    Time-travel primitives — the concolic value, per-flow DOM COW delta, value-domain
                     constraints, HTML-breakout analysis, reply learning, the WFQ.
  host/check.h       CHECK (always-fatal) vs DCHECK (dev-only) — offensive-programming invariants.
  qjs/               Submodule: forked quickjs-ng (branch apiclient-v2), a minimal delta over upstream.
  build.mjs          Builds the ONE Chrome wasm target (SINGLE_FILE, JSPI). Native/CLI targets are banned —
                     none has JSPI, safeFetch, or real Chrome crypto/DOM, so they give false confidence.
```

## Storage & security model

All learned state lives in **IndexedDB in the offscreen document** (its lifetime is stable, so it survives MV3 service-worker eviction). `chrome.storage.local` is **banned** — a compromised renderer could read cross-site structural metadata.

Trust is decided by **`sender.url`, never `sender.id`** (every sender carries the extension id, so it is not a discriminator): an extension-origin URL is a trusted context (popup/offscreen), a web URL is an untrusted content script. The SW honors privileged `__rpc` only from the offscreen URL and `__evt` only from an extension origin, and never relays content-script payloads (content scripts reach the brain directly with the browser-verified `sender`). External fetches go directly from the offscreen document and its worker through one auditable `safeFetch` (GET, cookies omitted, http(s) only). The untrusted per-page wasm holds no network/security policy — `safeFetch` alone enforces SOP/CORS/method/credentials.

**Authorized security research only.** No `webRequest`/debugger permission; no visible browser-UI impact. Popup content is escaped to prevent self-XSS.

## Build & test

The engine ships pre-built in `extension/lib/qjs/`. To rebuild from the forked QuickJS-ng + Lexbor source (bundled emsdk):

```
node engine/build.mjs cow       # build the ONE Chrome wasm worker target -> extension/lib/qjs/
```

The submodule `engine/qjs` (branch `apiclient-v2`) is a minimal delta over upstream `quickjs-ng`; the host/scheduler lives outside it in `engine/host/`. All verification is through the **live Chrome harness** — the only valid host (it has JSPI, `safeFetch`, and real Chrome crypto/DOM); native/node give false confidence and are banned.

```
node testing/harness.js restart                 # kill + clear IndexedDB, pick up a rebuilt worker
node testing/harness.js goto <url>              # analyze a served fixture or a real site (the ground truth)
node testing/harness.js offscreen "<js>"        # inspect globalStore in the offscreen context
node testing/harness.js netdiff --unused        # learned-not-live surface (a diagnostic, not the target)
```
