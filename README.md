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
- **Severity** is category-fixed: `code-exec` → critical, else high (sanitizer-path downgrade is honest future work, not claimed).
- **Source positions**: every finding carries the bundle call chain from the engine's real `Error().stack`, mapped to combined-bundle line space for click-through to the original JS.

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
- **Session Persistence**: Logs stored in `chrome.storage.session` — survive MV3 service worker restarts, auto-clear on browser close.
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

The analysis engine ships pre-built in `extension/lib/qjs/`. To rebuild it from the forked QuickJS-ng + Lexbor source (requires gcc for the native iterate binary and the bundled emsdk for the wasm targets):
```
node engine/build.mjs            # native qjs.exe + cli/mod/worker wasm + stage
node engine/build.mjs worker     # just the extension's wasm worker
node engine/build.mjs stage      # copy qjs_worker.js + hostedge.gen.js into extension/lib/qjs/
```

## Architecture

```
intercept.js       Main-world fetch/XHR/WebSocket/EventSource wrapper (request + response capture)
content.js         Isolated-world content script (DOM scanning, PAGE_FETCH relay, intercept relay, postMessage/MessageChannel listener)
background.js      Service worker (request interception, VDD learning, analysis orchestration, protocol classification, export)
popup.js           Popup controller (rendering, replay, form builder, security panel)
popup.html/css     Popup markup and styles
ast-worker.html    Offscreen document — thin relay to the analysis Web Worker
ast-thread.js      Web Worker — schedule enumeration (one fresh wasm instance per schedule) + throttled, preemptible, cross-session-resumable deep unused-feature grind

engine/qjs/        Forked QuickJS-ng + vendored Lexbor (Apache-2.0); ONE patched source → all targets:
  quickjs.c          Patched interpreter — opaque sentinel, selective branch forcing, infectious-opaque
  quickjs-forced.h   Forced-execution controller (schedule in, B/F trace out)
  qjsmain.c          Host main — installs the DOM binding, evals scripts, surfaces @E gaps
  qjs_dom.c          QuickJS↔Lexbor binding — spec DOM/CSS/customElements/WHATWG URL
  hostedge.js        Record-only Web-boundary shim (fetch/XHR/location/storage/MessageChannel)
  driver.js          Event-loop epilogue (pumps load/ready/message + XHR completion)
engine/build.mjs     Builds native qjs.exe + 3 wasm targets from one source; `stage`s the worker
extension/lib/qjs/   Pre-built qjs_worker.js + generated hostedge.gen.js (shipped)

lib/
  sourcemap.js     Source map recovery — TypeScript interfaces, enums, type aliases
  discovery.js     Protocol parsers, schema resolution, bidirectional OpenAPI conversion
  protobuf.js      Wire-format codec, JSPB decoder, recursive base64 scanning
  req2proto.js     Error-based schema probing (Google-specific + generic)
```

### Data Flow

```
Page JS ──→ intercept.js (main world) ──→ CustomEvent ──→ content.js ──→ background.js
             (request headers/body +                                          │
              response headers/body)                                          │
                                                                              │
Cross-frame postMessage / MessageChannel ──→ content.js (message listener) ──→│
                                                                              │
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

| Store | Backend | Scope | Lifetime |
|-------|---------|-------|----------|
| **GlobalStore** | IndexedDB (service worker origin) | Cross-tab | Persistent |
| **Request Logs** | `chrome.storage.session` | Per-tab | Browser session |
| **Field Renames** | IndexedDB (via GlobalStore) | Cross-tab | Persistent |
| **Deep-grind progress** | IndexedDB (offscreen worker origin) | Per-bundle | Until complete |

### Background Analysis & Scheduling

The deep analysis learns the *complete* API surface — including features that haven't run yet and login-gated code in lazily-loaded chunks — by force-driving the bundle's unreached functions. That is a lot of work on a large site, so it is designed to be **thorough but gentle on the device**:

- **Low CPU, never pins a core.** Work runs in short bursts (one execution schedule, or a small batch of unreached functions) with a sleep of about the same length after each (~50% duty cycle). Analysis is serial and throttled, not parallel — parallelism would only add heat. Time is treated as free because it runs in the background; your machine stays cool and responsive.
- **The page you're on comes first.** A freshly visited page's quick review is high priority and **preempts** any in-progress deep dive of a previously visited site. The deep dive pauses at a safe checkpoint, the new page's findings appear right away, then the deep dive resumes — only ever one analysis running at a time.
- **Resumable across sessions.** Long deep dives checkpoint their progress (and the captured JavaScript) to IndexedDB. If the MV3 service worker is evicted, or you close and reopen the browser, the analysis **resumes from where it left off** with no re-fetching and no lost work — and it resumes automatically on browser start, without needing to revisit the page. The most recently relevant site is resumed first.
- **Eventual consistency, no quality loss.** Every site's deep dive eventually completes; none is dropped to save time, and no endpoint or example value is sacrificed for speed. Depth and example quality are never traded away — the cost is spread over time instead.

### Security Model

| Context | Process | Trust Level |
|---------|---------|-------------|
| `intercept.js` (main world) | Renderer | Untrusted — same origin as page, no extension APIs |
| `content.js` (isolated world) | Renderer | Low trust — message whitelist: `CONTENT_KEYS`, `CONTENT_ENDPOINTS`, `RESPONSE_BODY` |
| `background.js` (service worker) | Extension | Trusted — full extension APIs, IndexedDB |
| `popup.js` (extension page) | Extension | Trusted |

GlobalStore uses IndexedDB (inaccessible to content scripts) instead of `chrome.storage.local` to prevent compromised renderers from reading cross-site structural metadata. Message routing validates `sender.url` origin (unforgeable by the browser process).

## Security & Privacy

- This tool is for **authorized security research only**.
- Cookie values are redacted; only their presence is tracked.
- No `webRequest` or debugger permission required — no visible browser UI impact.
- All dynamic content in the popup is escaped via `esc()` to prevent self-XSS.

## Testing

Analysis correctness is proven by *running real code*, not unit assertions:

```
cd engine/qjs
node drive.mjs hostedge.js _gate.js driver.js     # endpoint/value polarity (server gate)
node drive.mjs hostedge.js _xss.js  driver.js     # XSS sink polarity (positive + negative)
node drive.mjs hostedge.js _url.js  driver.js     # WHATWG URL (Lexbor) polarity
QJS_BIN=qjs_wasm.js node drive.mjs ...            # same, on wasm — native ≡ wasm before any claim
node ../../testing/harness.js goto https://…      # live Chrome (CDP) — the only real-site ground truth
node ../../test-lib.js                            # protobuf / discovery / OpenAPI conversion
```

Real minified jQuery 3.7.1 is the standing regression gate (boots with zero spurious forks). Polarity fixtures never substitute for verifying a change against a real site through the harness.
