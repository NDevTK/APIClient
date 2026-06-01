# API Security Researcher

A Chrome Extension (MV3) that passively reverse-engineers APIs, learns their schemas from live traffic, analyzes JavaScript bundles for vulnerabilities, and provides a complete testing workbench — all without a debugger or proxy.

## How It Works

Browse any website. The extension works in the background:

1. **Intercepts** every fetch/XHR/WebSocket/EventSource call via main-world wrappers, capturing request headers, request bodies, response headers, response bodies, and status — no `webRequest` permission, no Chrome debugger bar.
2. **Captures** cross-frame postMessage and MessageChannel messages via isolated-world listeners.
3. **Decodes** traffic through a protocol chain: async chunked, batchexecute, gRPC-Web, SSE, NDJSON, multipart, GraphQL, JSON, and Protobuf.
4. **Learns** API structure (VDD — Value-Driven Discovery) by merging schemas from every observed request and response into a unified service map.
5. **Analyzes** JavaScript bundles by *executing them for real* under forced multi-path exploration on a forked QuickJS-ng engine (WebAssembly) with a Lexbor spec DOM — extracting API call sites, computed URL/header/body values, and security taint, before any network call is made.
6. **Probes** for official documentation (OpenAPI, Google Discovery) at well-known paths and merges it with learned data.

Open the popup to inspect, test, and export everything it found.

## Features

### API Discovery & Schema Learning

- **Passive Learning**: Every request and response teaches the extension about URL patterns, query parameters, body fields, field types, content types, and authentication.
- **Forced-Execution Analysis**: The unmodified bundle is *run* on a forked QuickJS-ng (compiled to WebAssembly) inside an offscreen Web Worker. Only the host boundary is modelled — a real Lexbor spec DOM/CSS/`URL`, and record-only fetch/XHR/`location`/storage stubs. Closures, prototypes, `Function.prototype.call/apply/bind`, async, framework state machines (jQuery `$.ajax`, axios, react-query) all evaluate with exact ECMA semantics because it is a real engine, not a re-implementation. Discovers APIs that haven't been called yet, with fully-computed URLs/methods/headers/body fields.
- **Value Constraint Extraction**: Valid parameter values come from whatever the executed code computes (`switch`/`case`, `.includes()`, equality chains evaluate naturally). These appear as dropdowns in the Send panel for both URL params and request body fields.
- **Autonomous Documentation Discovery**: Probes well-known paths (`/.well-known/openapi.json`, `/swagger.json`, `/$discovery/rest`, etc.) with version, visibility, and auth variants.
- **Proto Field Maps & Enums**: Detects protobuf field number-to-name mappings and enum definitions from JavaScript bundles.
- **Source Map Recovery**: Fetches source maps and extracts TypeScript interfaces, enums, and type aliases. Enriches VDD parameters with original type names. Runs security analysis on unminified original sources.
- **Error-Based Schema Probing**: Sends intentionally malformed requests and learns field requirements and types from error responses.
- **Smart Key Extraction**: Scans URLs, headers, response bodies, and DOM for API keys and tokens (Google, Firebase, JWT, Stripe, Bearer, Mapbox, GitHub, etc.) with recursive base64 decoding.
- **Interface Grouping**: Groups endpoints into logical services (e.g., `api.example.com/v1`) based on host and path structure.
- **Cross-Script Analysis**: Buffers and concatenates inline and external scripts per tab for combined analysis, matching the browser's shared global scope.

### Protocol Reverse-Engineering

| Protocol | Capabilities |
|----------|-------------|
| **Google batchexecute** | Decode/encode batch RPC — nested RPC IDs, double-JSON payloads |
| **Protobuf / JSPB** | Wire-format codec, JSPB tree decoder, recursive base64 scanning |
| **gRPC-Web** | Decode binary/text frames, extract protobuf payloads and trailers, encode for replay |
| **Google Async Chunked** | Hex-length-prefixed streaming with JSPB extraction |
| **GraphQL** | Parse query/variables/operationName, render data/errors/extensions |
| **SSE / NDJSON / Multipart** | Server-Sent Events, newline-delimited JSON, multipart batch |
| **WebSocket** | Intercepts send/receive on live connections, with an interactive console for sending messages through captured sockets |
| **postMessage** | Captures cross-frame messages, grouped by source origin, with reply capability via stored `event.source` references |
| **MessageChannel** | Captures transferred ports from postMessage, instruments for bidirectional message logging, with send capability via stored port references |
| **EventSource** | Captures SSE streams |

Format badges (PROTO, JSPB, BATCH, gRPC-WEB, SSE, NDJSON, GRAPHQL, MULTIPART, ASYNC, WEBSOCKET, POSTMESSAGE, MSGCHANNEL) appear on request log entries.

### JavaScript Security Code Review

Taint is observed during the *same real execution* used for API learning — no separate traversal, no regex, no name matching. Works on minified code because names are irrelevant: a sink is what an executed call reaches at the host edge.

- **Attacker-taint sources** are the host-edge data made *opaque* (infectious through member-get / call / `JSON.parse` / iteration): `location.hash/search`, `document.cookie`, `*Storage.getItem`, `window.name`, `XHR.responseText/response`, `fetch` `Response` bodies, `postMessage` `data`/`origin`. DOM *structure* stays concrete so frameworks boot.
- **Sinks** are recorded only when the value reaching them is tainted: code-exec (`eval`/`Function`/string `setTimeout`/`setInterval`), open-redirect (`location.href`/`assign`/`replace`), DOM-HTML (`innerHTML`/`outerHTML`/`insertAdjacentHTML`/`document.write`), script-bearing `setAttribute`. A concrete write (e.g. jQuery feature-detection `innerHTML`) is not a finding and costs nothing.
- **Z3 path-satisfiability filter.** A tainted sink by itself is only *reach*. Z3 is linked into the engine (C, no JS bridge); the source→sink path constraints (Φ) accumulated over the attacker-tainted value are asserted with an exploit-shaped query: **SAT ⇒ REAL_EXPLOIT** (the model is a concrete attacker witness), **SAT(Φ) ∧ UNSAT(exploit) ⇒ TAINT_REACH** (feasible but Φ pins a non-exploitable value), **UNSAT(Φ) ⇒ INFEASIBLE** (X-Force forced incompatible gates — suppressed, never shown). The same solver also prunes infeasible flipped schedules during exploration. Z3 errors surface as a distinct `Z3_ERROR` + `@E`, never silently collapsed.
- **Unified, Z3-built PoC probe.** The exact JavaScript shown in the UI is *built from Z3's solve over the real trace* (each attacker leaf → channel/order/fieldPath/solved-value/decoders), not a hardcoded template. It runs verbatim in a manifest **sandbox page** (opaque origin = real cross-origin attacker) on your click, correlating back via the finding's `crypto.randomUUID` inside the payload (never data in the URL): **NOT REPRODUCED** or **REAL EXPLOIT**. The static badge is "PoC" (untested) until the probe confirms it.
- **Source positions**: every finding carries the bundle call chain from the engine's real `Error().stack`, mapped to combined-bundle line space for click-through to the original JS, plus a structured taint trace (data flow, conditionals Φ, interprocedural call chain, Z3's leaves/witness).

### Replay & Export

- **Message Console**: Click any WebSocket, postMessage, or MessageChannel log entry to open an interactive console — shows connection status, message history, and a composer to send messages through the live socket, `event.source` reference, or transferred port.
- **Session-Aware Replay**: Executes requests in the target page's context via `PAGE_FETCH` relay, automatically attaching cookies and session state.
- **Form Builder**: Auto-generated input fields from learned schemas — text inputs, enum dropdowns (from AST value constraints and observed traffic), nested message expansion, repeated field support.
- **Auto-Determined Encoding**: Content-Type and body mode (form/raw/GraphQL) set automatically from schema or replayed request headers.
- **Field Renaming**: Click the pencil icon to rename any field or parameter. Names persist in IndexedDB across sessions.
- **HAR Export**: Download the visible request log as a HAR 1.2 file for use with Burp Suite, ZAP, or browser DevTools.
- **Export Formats**: curl, fetch (JavaScript), Python (requests library).
- **OpenAPI Export**: Service-level export as OpenAPI 3.0.3 with learned schemas, field aliases, and `x-field-number` extensions for protobuf round-trip.
- **OpenAPI Import**: Import OpenAPI/Swagger specs to pre-populate schemas, merging with locally-learned data.

### Cross-Tab Request Log

- **Multi-Tab Filtering**: View requests from the active tab, all tabs, or a specific closed tab.
- **Session Persistence**: Logs live **in memory in the offscreen document** (its lifetime is stable, so they survive MV3 service-worker eviction); they are cleared on browser close or the 🗑️ bin button. (The former `chrome.storage.session` layer was removed once the offscreen document's lifetime became stable.)
- **Closed Tab Retention**: Logs from closed tabs remain accessible.
- **Search & Filter**: Text filter across URL, method, service, content type, and tab title.
- **Virtual Scroll**: Handles large request logs without DOM bloat.

## UI Panels

| Panel | Purpose |
|-------|---------|
| **Requests** | Live request log with service grouping, protocol badges, and cross-tab filtering |
| **Send** | Manual testing — service/method selector, form builder with dropdowns, headers editor, replay, message console (WebSocket/postMessage/MessageChannel), export (curl/fetch/Python/HAR/OpenAPI) |
| **Vulns** | All security findings sorted by severity with type badges, source classification, and code locations |
| **Keys** | Extracted API keys and tokens with origin, timestamps, and associated services |

## Installation

1. Clone this repository.
2. Open `chrome://extensions`.
3. Enable **Developer mode**.
4. Click **Load unpacked** and select the extension folder.

The analysis engine ships pre-built in `extension/lib/qjs/`. To rebuild it from the forked QuickJS-ng + Lexbor source (uses the bundled emsdk for the wasm target):
```
node engine/build.mjs worker     # build the extension's wasm worker (the ONE target)
node engine/build.mjs stage      # copy qjs_worker.js + hostedge.gen.js into extension/lib/qjs/
```
There is exactly **one** build target — the Chrome wasm worker (`SINGLE_FILE`, JSPI). The former native (`qjs.exe`), node wasm-CLI (`qjs_wasm.js`), and modular node (`qjs_mod.mjs`) targets are **banned and error out**: none has JSPI, host `fetch`/`safeFetch`, or real Chrome crypto/DOM, so they give false confidence (a bundle "converged" on the CLI while the live wasm spun on a fetched-chunk orphan the CLI never loaded). All verification is through the live Chrome harness — never node.

The engine source is pinned by the `engine/qjs` gitlink (a commit into the `apiclient-fork` branch of the [quickjs-ng fork](https://github.com/NDevTK/APIClient-quickjs)); the runtime artifact `extension/lib/qjs/qjs_worker.js` is built from it and committed alongside. Enable the repo hooks once so a gitlink bump can't be committed without re-staging the rebuilt worker (and vice-versa they stay consistent):
```
git config core.hooksPath hooks
```

## Architecture

```
intercept.js       Main-world fetch/XHR/WebSocket/EventSource wrapper (request + response capture)
content.js         Isolated-world content script (ships raw HTML/scripts/responses, PAGE_FETCH relay, postMessage/MessageChannel listener) — messages the offscreen brain DIRECTLY
background.js      Thin, STATELESS service worker — extension-page-only. Owns the offscreen lifecycle, forwards browser events the offscreen can't observe (webNavigation/tabs as __evt), and performs the privileged chrome.* the offscreen lacks on the brain's behalf (__rpc: tabs.*, scripting.exec). Holds NO state. Cross-origin fetch is NOT an SW job — COEP doesn't block fetch, so the offscreen + worker fetch directly via one safeFetch (GET, no cookies, http(s) only).
offscreen-brain.js The LEARNING BRAIN (runs in the offscreen document — stable lifetime + IndexedDB). All state + logic: globalStore, VDD schema learning, discovery, AST merge, request log, protocol classification, popup handlers, export, persistence. Privileged chrome.* APIs it lacks go through the SW via swRpc.
popup.js           Popup controller (rendering, replay, form builder, security panel) — messages the offscreen brain directly
popup.html/css     Popup markup and styles
ast-worker.html    Offscreen document — loads the brain (offscreen-brain.js) + the analysis Web Worker
ast-worker.js      In-document bridge: exposes self.astDispatch (the brain calls it directly) and merges streamed deep-grind results
ast-thread.js      Web Worker — schedule enumeration (Wizer-style snapshot/restore: boot the bundle ONCE, image linear memory, restore per schedule — no re-boot) + fast-by-default, WFQ-scheduled, preemptible, cross-session-resumable deep unused-feature grind

engine/qjs/        Forked QuickJS-ng + vendored Lexbor (Apache-2.0); ONE patched source → all targets:
  quickjs.c          Patched interpreter — opaque sentinel, selective branch forcing, infectious-opaque
  quickjs-forced.h   Forced-execution controller (schedule in, B/F trace out)
  qjsmain.c          Host main — installs the DOM binding, evals scripts, surfaces @E gaps
  qjs_dom.c          QuickJS↔Lexbor binding — spec DOM/CSS/customElements/WHATWG URL
  hostedge.js        Record-only Web-boundary shim (fetch/XHR/location/storage/MessageChannel)
  driver.js          Event-loop epilogue (pumps load/ready/message + XHR completion)
engine/build.mjs     Builds the ONE Chrome wasm worker target from the patched source; `stage`s it (native/cli/mod targets are banned and error out)
extension/lib/qjs/   Pre-built qjs_worker.js + generated hostedge.gen.js (shipped)

lib/
  sourcemap.js     Source map recovery — TypeScript interfaces, enums, type aliases
  discovery.js     Protocol parsers, schema resolution, bidirectional OpenAPI conversion
  protobuf.js      Wire-format codec, JSPB decoder, recursive base64 scanning
  req2proto.js     Error-based schema probing (Google-specific + generic)
  safe-fetch.js    The ONE external-fetch path (GET, cookies omitted, http(s) only) — shared by the brain + worker
  learnstate.js    DRY analysis-state classifier (complete / analyzing / stalled) — worker heartbeat, harness, popup
  priority.js      WFQ fiber comparator + weights (cross-page CPU fair-share; active-page focus tier)
```

### Data Flow

```
Page JS ──→ intercept.js (main world) ──→ CustomEvent ──→ content.js ──→ offscreen-brain.js
             (request headers/body +                      (broadcast reaches     │   (the brain;
              response headers/body)                       the offscreen doc      │    real sender —
                                                           directly)              │    tab/frame/url)
Cross-frame postMessage / MessageChannel ──→ content.js (message listener) ──────→│
                                                                              │
  background.js (SW): offscreen lifecycle + __evt (webNavigation/tabs) +      │
                      __rpc (tabs/scripting only; NO fetch — safeFetch direct)┤
                                              ┌───────────────────────────────┤
                                              ▼                               ▼
                                     VDD Schema Learning              Forced-Execution Analysis
                                     (learnFromRequest/Response)      (offscreen wasm worker)
                                              │                               │
                                              ▼                               ▼
                                     Discovery Docs (IndexedDB)      Security Findings
                                     Endpoints, Keys, Schemas        Fetch Sites, Constraints
                                              │                       Proto Maps, Enums
                                              └───────────┬───────────────────┘
                                                          ▼
                                                    popup.js (UI)
```

### Storage

All learned state lives in **IndexedDB in the offscreen document** (the brain's stable home). `chrome.storage.local` is **banned** (a compromised renderer could read cross-site structural metadata);

| Store | Backend | Scope | Lifetime |
|-------|---------|-------|----------|
| **GlobalStore** | IndexedDB `uasr_store` / `gapiStore` (offscreen document) | Cross-tab | Persistent (debounced save; restored on offscreen recreate) |
| **Request Logs** | In-memory (offscreen document) | Per-tab | Offscreen lifetime (survives SW eviction; cleared on browser close / bin) |
| **Field Renames** | IndexedDB (via GlobalStore) | Cross-tab | Persistent |
| **Deep-grind progress** | IndexedDB `feDeepDB` (offscreen document) | Per-bundle | Until complete |

### Background Analysis & Scheduling

The deep analysis learns the *complete* API surface — including features that haven't run yet and login-gated code in lazily-loaded chunks — by force-driving the bundle's unreached functions. This is a security-research tool you actively opened and are waiting on, so it is **fast by default**, and CPU/parallelism are **your choice** (popup ⚙):

- **Fast by default — a free core isn't left idle.** The default throttle is 0 (no per-schedule sleep): the learner uses the core to surface the API surface sooner. Relevance ordering + active-page focus already decide *what* runs first, so full speed just means *more learned sooner*. If you want a cooler background, the ⚙ **Yield throttle** slider raises the CPU-duty cap (the focused page stays hot regardless), and **Analyzer workers** grows the worker pool.
- **Priority frontier, not a FIFO — pause and resume, never cap.** A code path is only terminated when continuing would be pointless (a fixpoint-detected non-terminating loop); otherwise it pauses and resumes later. The schedule + deep-grind run as a **resumable priority frontier**: the most-useful work runs now, yields at per-orphan / per-branch boundaries, and resumes the rest later — no depth/step/time cap.
- **Weighted-fair across concurrent pages; the page you're on preempts.** Multiple open tabs' analyses are suspended JSPI fibers scheduled by **weighted fair queueing** (weight = reaches-host-edge + marginal endpoint yield), so CPU is shared fairly; the page you're actively viewing wins a strict focus tier above WFQ and resumes ahead of background grinds at the next yield. Parallelism is *across pages* (a worker pool) — a single page's grind uses one core; extra workers only parallelise concurrent tabs.
- **Most-relevant first, by view recency.** The most-recently-viewed page leads; within a grind, functions that reach a network edge (and ones whose body already ran in real context, so they resolve to concrete values) are driven before cold/sink-only ones, cheapest first. Progress is tracked by **which functions are done** (a stable per-function id), not a position counter — so the order can change freely between runs without ever re-doing or skipping a function. Each page is analyzed in isolation (only its own JavaScript).
- **Resumable across sessions.** Long grinds checkpoint their progress (and the captured JavaScript) to IndexedDB. If the MV3 service worker is evicted, or you close and reopen the browser, the analysis **resumes from where it left off** with no re-fetching and no lost work — automatically on browser start, without revisiting the page.
- **Eventual consistency, no quality loss.** Every site's grind eventually completes; none is dropped, and no endpoint or example value is sacrificed for speed.
- **The 🗑️ bin button is a hard stop + full reset.** It deletes *all* learned data and stops *all* work outright: the background worker is terminated (killing any running analysis immediately, not just asked to stop), its resumable checkpoint is deleted, and the global store + request logs are wiped. Work that was already in flight can't sneak results back in afterward. This is separate from the automatic re-scan that happens when the analyzer's own code changes — that re-scan *keeps* past findings (they accumulate); only the bin button clears them.

### Security Model

| Context | Process | Trust Level |
|---------|---------|-------------|
| `intercept.js` (main world) | Renderer | Untrusted — same origin as page, no extension APIs |
| `content.js` (isolated world) | Renderer | Low trust — ships raw page material under a fixed message type set; treated as untrusted page data |
| `background.js` (service worker) | Extension | Trusted — full extension APIs; thin, holds no learned data |
| `offscreen-brain.js` (offscreen document) | Extension | Trusted — owns IndexedDB + all learned state |
| `popup.js` (extension page) | Extension | Trusted |

The brain uses IndexedDB (inaccessible to content scripts) instead of `chrome.storage.local` to keep a compromised renderer from reading cross-site structural metadata. **Trust is decided by `sender.url`, never `sender.id`** — every `onMessage` sender carries the extension id (external senders go to `onMessageExternal`), so the id is not a discriminator; an extension-origin URL is a trusted context (popup/offscreen), a web URL is a content script (untrusted). The SW honors privileged `__rpc` **only** from the offscreen URL and browser-event `__evt` only from an extension origin; it never relays content-script payloads (content scripts reach the brain directly with the browser-verified `sender`, so page data is never laundered into a trusted origin). External fetches (lazy chunks, source maps, discovery probes) go **directly** from the offscreen document and its worker through one auditable `safeFetch` (GET only, cookies omitted, http(s) only) — not via the SW; COEP `require-corp` does not block `fetch`.

## Security & Privacy

- This tool is for **authorized security research only**.
- Cookie values are redacted; only their presence is tracked.
- No `webRequest` or debugger permission required — no visible browser UI impact.
- All dynamic content in the popup is escaped via `esc()` to prevent self-XSS.

## Testing

Analysis correctness is proven by *running real code* in the **live Chrome harness** (the only valid host — it has JSPI, `safeFetch`, and real Chrome crypto/DOM), never node:

```
node testing/harness.js restart                      # kill+clear-IDB, pick up a rebuilt qjs_worker.js
node testing/harness.js gate engine/qjs/_gate.js --deep    # endpoint/value polarity (server gate)
node testing/harness.js gate engine/qjs/_xss.js  --deep    # XSS sink polarity (4× REAL_EXPLOIT, Z3 witnesses)
node testing/harness.js gate engine/qjs/_url.js  --deep    # WHATWG URL (Lexbor) polarity
node testing/harness.js goto https://…               # live real-site analysis (the ground truth)
node testing/harness.js netdiff                      # live requests vs AST-learned (coverage diff)
node test-lib.js                                     # protobuf / discovery / OpenAPI conversion
```

Each gate reports `converged`, `endpointCount`/`sinkCount`, and `spinCount` (0 = no non-terminating loop). Real minified jQuery 3.7.1 is the standing regression gate (boots with zero spurious forks). Polarity fixtures never substitute for verifying a change against a real site through the harness. (The former `drive.mjs`/`mdrive.mjs` node drivers were deleted with the banned node targets.)
