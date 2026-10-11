# API Security Researcher

A Chrome extension (MV3) that maps a site's API surface and finds client-side XSS by **running the page's own JavaScript bundle** under forced multi-path exploration, on a forked QuickJS-ng engine over a spec-built Lexbor DOM. It needs no proxy and no `debugger` permission: requests are captured inside the page, and the extension's own sends get their headers from `declarativeNetRequest` rules (see [Permissions](#permissions)).

In one sentence: **a browser with a BFS time-travel solver.** The browser half (Lexbor and QuickJS-ng, in C) follows the specs. The solver half — forced multi-path concolic execution over copy-on-write snapshots — exists to do two things a scanner cannot:

- compute the **logged-in API surface while logged out**, because a single-page app ships the same bundle to every visitor;
- construct **XSS proofs of concept that are proven by firing**, from code that never ran.

What the tool adds on a given page is measurable: the addresses a run emitted that no parse of the served bytes can state, because they exist only at run time. A total is not that number — most of a total is the markup counted back.

## How it works

Browse any site. In the background:

1. **The engine runs the bundle.** The unmodified JavaScript executes on QuickJS-ng over a real DOM, CSS and `URL`. Closures, prototypes, `call`/`apply`/`bind`, `Proxy`, async code and framework state machines behave exactly as ECMAScript says, because this is a real engine rather than a model of one.
2. **Forced multi-path exploration** runs the branches that input could gate — login, feature flags, routes, lazy chunks, never-called functions — so the engine reaches endpoints an app ships to logged-out visitors but never calls. Every branch is a flow on one value-ordered queue, snapshotted (copy-on-write heap and DOM), preemptible, and resumable across browser sessions.
3. **External input is concolic.** Each attacker-controlled source (`location`, `document.cookie`, `postMessage`, storage, response bodies) carries its identity, the path constraints a flow has placed on it, and a concrete example when the code pins or computes one. Control flow forks on it; real values propagate.
4. **One structured result.** The engine emits endpoints with computed URLs, methods, headers, body fields and example values, and security sinks with fire-verified proofs of concept.
5. **Observed traffic is kept beside the derived surface, never merged into it.** Fetch, XHR, WebSocket, SSE and `postMessage` traffic is decoded (JSON, JSPB/protobuf, gRPC-Web, batchexecute, multipart, SSE/NDJSON, GraphQL) into its own pool. Every value carries its provenance — observed, derived or forced — so what execution contributed stays measurable.

Open the popup to inspect, send and export what it found.

## Features

### API discovery

- **Forced-execution discovery.** Computed URLs, methods, headers and body fields for endpoints that have not been called, including login-gated code in lazy chunks.
- **Example values from the code.** Valid values are whatever the executed code computes (`switch`/`case`, `.includes()`, equality chains), offered in the Send panel. A value is concrete only where the code pins it (an equality gate) or computes it; a range, prefix or pattern gate stays a domain-annotated shape, never an invented guess.
- **Passive learning.** Observed requests and responses teach URL patterns, parameters, field types, content types and auth, per service, in their own provenance pool.
- **Documentation discovery.** OpenAPI and Google Discovery documents are fetched at well-known paths, and Google APIs are probed with deliberately malformed bodies whose `google.rpc.Status` replies name the expected fields (`lib/discovery-probe.js`, `lib/req2proto.js`).
- **Key extraction.** URLs, headers, bodies and the DOM are scanned (recursively through base64) for key and token shapes. Cookie values are never stored, only their presence.

### Client-side security review

Taint is observed in the same execution used for API learning. There is no separate traversal and no name matching, so minified code is no obstacle: a sink is whatever an executed call reaches at the host edge.

- **Attacker sources** carry their real browser constraints: `location.hash` and `search` are percent-encoded per the WHATWG per-component sets, and a forgeable `message.origin` check is solvable while an exact `===` is not.
- **Sinks** are recorded only when an attacker-derived value reaches them: HTML, script execution, navigation, script URLs and event-handler attributes. A constant write, such as a feature-detection `innerHTML`, is not a finding.
- **Proofs of concept are constructed and proven by firing.** For a reached sink the engine searches concrete inputs, as ordinary flows, through the page's real filters, encoders and gates until one breaks out of the sink's parse context. A finding requires the fire marker to execute, not merely appear. Constraints are discharged by re-executing the real code, with no external SMT solver, taint tracker or transform inversion.
- **Policy is part of the proof.** CSP is parsed for a concrete bypass, and Trusted Types is enforced by running the page's own `default` policy at the sink.
- **Live verification in real Chrome.** The engine's exact payload is evaluated in an ordinary tab at a real registrable origin, behind a Run button the person clicks (a click is what lets the payload open windows past the popup blocker). A hit is attributed only when the browser-stated origin, frame and document of the reporting page match the document the tool delivered to. A page can still fake a hit from inside the delivered document itself; [SECURITY.md](SECURITY.md) states that residual.

### Rendering

The browser half is a rendering engine: CSS parsing and cascade, computed and used values, block, inline, flex and table layout, CSS 2.1 Appendix E painting order, and a rasterizer. Layout and paint ride the same per-flow copy-on-write state as everything else, so an explored world is a resumable document rather than a log line. The logged-in and logged-out surfaces are two live documents of one page, each renderable and continuable from its own snapshot. WPT reftests measure layout and paint fidelity directly.

## Architecture

Two halves: the **C engine is the browser and owns all analysis**; the JavaScript is the platform edge (network, storage, `chrome.*`, loading the WASM, the popup), one problem per file.

Every engine instance runs in a **renderer**: an `<iframe sandbox="allow-scripts" src="renderer.html">` whose opaque origin is the security boundary, reached over a mojom-style interface. The offscreen document is the only trusted zone and holds the network policy, the store and the popup's data, none of which the engine can reach. There is one WASM instance per origin-keyed agent cluster.

```
extension/
  intercept.js         Main-world fetch/XHR/WebSocket/EventSource capture
  content.js           Isolated-world relay; a seed it forwards is an address, never document bytes
  background.js        Stateless service worker: offscreen lifecycle, browser events, privileged chrome.* calls
  speak-as-rule.js     The service worker's rule that makes a zone send carry a page's headers
  ast-worker.html      The offscreen document: the trusted zone's page
  offscreen-brain.js   The trusted zone: wires lib/*, owns IndexedDB
  bridge.js            Network, IndexedDB and renderer edges for the engine
  renderer.html        One sandboxed engine instance
  renderer-host.js     The trusted side of one renderer; render-process-host.js owns the pool
  mojo.js, mojom.js    The interface between the trusted zone and a renderer
  popup.js             Popup controller; lib/popup-*.js are its panels
  dev.html, dev.js     Diagnostics page: engine readings a developer reads, over the popup's messages
  check.js             JS mirror of the engine's DCHECK/CHECK
  lib/safe-fetch.js    The one external-fetch chokepoint: scheme, SOP/CORS, credentials, provenance, destination
  lib/                 Learning, schema, keys, persistence, grouping, merge, send, encode, serialize,
                       protocol decoders, discovery, OpenAPI import/export

engine/
  host/main.c            Native host entry and the shipped ABI
  host/solver/           Scheduler (one weighted fair queue), per-flow copy-on-write heap and DOM, concolic values,
                         constraint domains, HTML/JS breakout analysis, reply learning, the cold tier, @S search
  host/browser/core/     The browser by component: css, layout, paint, dom, html, frame, events, fetch, loader,
                         indexeddb, streams, crypto, canvas, url, xml, storage, image, ...
  host/check.h           CHECK (always fatal) and DCHECK (dev builds): every invariant asserted at its origin
  qjs/                   The forked QuickJS-ng, tracked in this repository; engine/qjs/test262 is a submodule
  lexbor/                The vendored Lexbor (DOM, HTML and CSS parsers)
  build.mjs              Builds the Chrome WASM target and the native host from the same sources
```

## Permissions

Each permission in `extension/manifest.json` and what it is for:

| Permission | Used for |
|---|---|
| `offscreen` | The offscreen document (`ast-worker.html`) that holds the trusted zone, IndexedDB and every engine renderer. |
| `declarativeNetRequestWithHostAccess` | One session rule per in-flight send that speaks as a document, which sets the `Origin`, `Referer`, `Sec-Fetch-*` and `Cookie` headers the zone computed on exactly that request (`speak-as-rule.js`). |
| `cookies` | Reading a document's cookies and its partition key, so a credentialed send carries that document's jar (`lib/request-identity.js`). |
| `webNavigation` | Frame identities (`getAllFrames`) for the document a send speaks as, and navigation events the popup follows. |
| `tabs` | The active tab, the live-verify attacker tab, and messages to content scripts. |
| `scripting` | Arming the live-verify attacker tab: a predefined injector run in that tab's main world, refused for any tab the service worker did not open. |
| `unlimitedStorage` | The IndexedDB store, which holds the cross-session frontier and cold snapshots. |
| `<all_urls>` host access | The content scripts on every page, and `safeFetch`'s requests. |

**Not used:** `webRequest`, `debugger` and a proxy. Traffic is captured in the page itself (`intercept.js`, which wraps `fetch`, XHR, WebSocket and EventSource in the main world). `webRequest` reports none of the offscreen document's own requests, so it cannot observe the zone's sends or their redirect hops. The `debugger` permission's `Fetch` domain could pause a redirect hop before it is sent, and this extension does not take it. [SECURITY.md](SECURITY.md) states what each of these leaves uncovered.

## Storage and security

All learned state lives in **IndexedDB in the offscreen document**, whose lifetime survives service-worker eviction. `chrome.storage.local` is not used.

Trust is decided by browser-stamped sender fields, never `sender.id`: `MessageSender.origin` for a message from an extension document, and `sender.url` only for the service worker, which has no frame. The principal for a credentialed read is `MessageSender.origin`. The content script, the website and the WASM engine are untrusted.

All network leaves from the offscreen document through `lib/safe-fetch.js`. A request that speaks as a page, which covers the engine's page requests and an operator's Send-panel request, carries that document's headers: its `Origin`, its `Referer` under Referrer Policy, its `Sec-Fetch-*`, and, where credentialed, its cookies, partitioned ones included. The headers are computed in the trusted zone from browser-stamped identity (`lib/request-identity.js`) and set on that one request by a `declarativeNetRequest` rule the service worker installs and removes (`speak-as-rule.js`). The observed request's own credentials mode, mode, referrer and referrer policy are recorded by `intercept.js` and replayed. Fetch's forbidden request-headers (`Cookie`, `Origin`, `Referer`, …) cannot be typed into a request, because the browser owns them. The page's service worker does not see these sends, and the page's CSP does not bind them. The engine holds no network policy. Whether a request fires is a **per-origin setting**. By default an origin is loaded the way a browser loads it: what the page's markup names and its own code requests, and nothing else. Probes, discovery and requests built on forked values need that origin deliberately widened. The chokepoint shows each request's signals (method, credential state, provenance, URL-carried authority, destination) as separate rows with their reliability, and an undetermined signal reads `unknown` rather than being left out.

Extension pages run under a CSP of `script-src 'self' 'wasm-unsafe-eval'` with no inline script, so page-derived text shown in the popup cannot run script.

The full threat model, every boundary and its known residuals are in [SECURITY.md](SECURITY.md). **Authorized security research only.**

## Build

```
node engine/build.mjs            # both targets: WASM into extension/lib/qjs/, native host into engine/host/out/
node engine/build.mjs native     # the native host only
```

A build reads the working tree. A number that will be quoted comes from a frozen snapshot at a named revision:

```
engine/frozen_snapshot.sh <revision> <lane-name> node engine/build.mjs
```

## Testing

The engine asserts its invariants where they originate, so a change is verified by the asserts staying silent. The corpora exercise code so the asserts can fire; a passing suite proves nothing about what it did not exercise.

```
node engine/test262.mjs [subdir]    # ECMAScript conformance
node engine/wpt.mjs [subdir]        # web-platform-tests
node engine/solvergate.mjs [doc]    # one document under several schedules must emit the same findings
node engine/citegen.mjs [path]      # every spec citation checked against the standard's text
node engine/idlgen.mjs              # Web IDL diffed against what each component installs
```

Driving a document directly:

```
node engine/trusted.mjs <url> <binary> [--paint <dir>]   # run one document natively; optionally write images
engine/host/out/qjs-native-none --doc <name>             # one fixture document (--list-docs lists them)
node testing/harness.js restart                          # live Chrome: clear storage, load the rebuilt extension
node testing/harness.js goto <url>                       # real Chrome on a real page
```

The spec is the source of truth and real Chrome confirms it. A fidelity gap is fixed by implementing the spec at the root.
