# API Security Researcher

A Chrome extension (MV3) that reverse-engineers a site's API surface and finds client-side XSS by **running the page's own JavaScript bundle for real** — under forced, multi-path exploration on a forked QuickJS-ng engine with a real Lexbor spec DOM. No debugger, no proxy, no `webRequest`.

The one sentence: **a browser with a BFS time-travel solver.** The browser half (Lexbor DOM + patched quickjs-ng, in C) is spec-faithful and boring by design; the solver half — forced multi-path concolic execution over copy-on-write snapshots — is the novel part, and it does one thing no scanner does: compute the **logged-in API surface while logged out**, and construct **replay-verified XSS PoCs**, from code that never ran.

## How It Works

Browse any site. In the background:

1. **The engine runs the bundle.** The unmodified JavaScript executes on the forked QuickJS-ng over a real Lexbor DOM/CSS/`URL`. Closures, prototypes, `Function.prototype.call/apply/bind`, `Proxy`, async, and framework state machines all evaluate with exact ECMAScript semantics because it *is* a real engine, not a re-implementation.
2. **Forced multi-path exploration** drives the branches real input could gate — login/flag/route-gated code, lazy-loaded chunks, never-called functions — so the engine reaches the auth/admin/billing endpoints an SPA ships to logged-out visitors but never fires. Every arm is a BFS flow, value-ordered by one work-fair queue, snapshotted (copy-on-write heap + DOM), preemptible, and resumable across browser sessions.
3. **External input is concolic**, not a placeholder: each attacker-controlled source (`location.hash/search`, `document.cookie`, `postMessage` data/origin, storage, response bodies) carries a source identity, the per-flow path constraints it accumulated, and a concrete example when the code pins or computes one. Control flow forks on it (reaching gated code) while real values propagate forward.
4. **The result is one structured record** the engine emits and the trusted zone consumes: endpoints with computed URLs/methods/headers/body fields and example values (`@H`), and security sinks with **replay-verified** PoCs (`@S`).
5. **Passive traffic still teaches it too.** Observed fetch/XHR/WebSocket/SSE/postMessage traffic is decoded through a protocol chain (JSON, JSPB/protobuf, gRPC-Web, batchexecute, multipart, SSE/NDJSON, GraphQL) and kept **beside** the derived surface rather than merged into it. Each record carries the provenance of the values it was built from, so a passively-seen endpoint may propose *what to explore* and never arrives graded as something the run derived. Endpoints learned *only* from runtime hooks are a signal that forced execution can be driven harder — the engine is the primary source, and the diff between the two is a diagnostic rather than a target, which a merge would destroy: pooling them yields one list in which the tool's own contribution is unmeasurable.

Open the popup to inspect, test, and export everything it found.

## Features

### API discovery & schema learning
- **Forced-execution discovery.** Fully-computed URLs/methods/headers/body fields for endpoints that haven't been called yet — including login-gated code in lazy chunks.
- **Value-constraint extraction.** Valid parameter values are whatever the executed code computes (`switch`/`case`, `.includes()`, equality chains evaluate naturally), surfaced as dropdowns in the Send panel. A value is concrete only where the code *pins* it (an equality gate) or *computes* it; a range/prefix/regex gate stays a domain-annotated shape — never a fabricated in-range guess.
- **Passive learning.** Every observed request/response teaches URL patterns, query/body params, field types, content types, and auth — held in its own provenance pool per service, never folded into the derived surface, so what execution contributed stays measurable.
- **Protocol reverse-engineering.** JSON/JSONP, Protobuf/JSPB, gRPC-Web, Google batchexecute, async-chunked, SSE/NDJSON, multipart, GraphQL, WebSocket, postMessage/MessageChannel.
- **Documentation discovery & source maps.** Probes OpenAPI/Google-Discovery at well-known paths; recovers source maps for TypeScript interfaces/enums; error-based schema probing learns field requirements from malformed-request responses.
- **Key extraction.** Recursive base64 scan of URLs/headers/bodies/DOM for known key/token shapes; cookie values are never stored, only presence.

### Client-side security review
Taint is observed during the *same real execution* used for API learning — no separate traversal, no regex, no name matching. Works on minified code because names are irrelevant: a sink is what an executed call reaches at the host edge.

- **Attacker sources** are host-edge data made *concolic* and infectious through member-get / call / `JSON.parse` / iteration. Each source carries its **browser constraints** — `location.hash`/`search` are percent-encoded per the WHATWG per-component sets, a forgeable `message.origin` check is solvable while an exact `===` is not.
- **Sinks** are recorded only when a *tainted* value reaches them: HTML, code-exec, navigation, script-URL, on\* handlers. A concrete write (e.g. jQuery feature-detection `innerHTML`) is not a finding and costs nothing.
- **PoCs are constructed, not templated — and proven by firing.** For a reached sink the engine *searches* concrete inputs through the **real** filters/encoders/gates as ordinary flows until one breaks out at the sink, then requires the fire-marker to actually execute (not merely appear). **No external Z3, no taint tracker, no transform-inversion — re-executing the real (interprocedural, shared-mutable-state) code discharges the constraints.**
- **Policy-relative.** A breakout in the model isn't a working exploit until it survives the page's real policy: CSP is parsed for the concrete bypass path, and Trusted Types is enforced at the sink by **running** the page's `default` policy `createHTML` to see if it actually sanitizes.
- **Live verification.** The engine's *exact* PoC is delivered, never a re-derived one. The attacker document is a document and this engine is a browser, so the delivery is the engine's own to implement — a same-origin `opener`, a `postMessage` arm and a named target are not the platform's to grant. **Real Chrome remains the oracle**: the same PoC delivered to the real page in a sandboxed attacker popup (opaque origin = a real cross-origin attacker) is ground truth. A hit there is a **REAL EXPLOIT**; an engine that fires where Chrome does not is a fidelity divergence to fix.

### Rendering
The browser half is a rendering engine, not a DOM shim: CSS parsing and cascade, computed and used values, block / inline / flex / table layout, CSS 2.1 §E.2 painting order into a display list, and a rasterizer that emits images. Because a document's layout and paint ride the same per-flow copy-on-write delta as everything else, **an explored world is a resumable state rather than a log line** — the admin surface and the logged-out surface are two live documents of one page, each continuable from its own snapshot at the exact point it was parked. Rendering one is a projection computed on demand, never an artifact anyone stored, so two such worlds can be rendered separately and compared.

Rendering is also where the engine's spec fidelity becomes measurable rather than asserted: a WPT *reftest* asks that one document's rendering equal another's, which is exactly a test of layout and paint order and is insensitive to font choice, because both sides go through the same rasterizer.

## Architecture

Two halves, never blurred: the **C engine is the browser and owns all analysis**; the JavaScript is the irreducible platform edge (relay network/storage/`chrome.*`, load the wasm, render the popup) — decomposed one-problem-per-file.

**Where the engine runs, which is a security claim and not a detail.** The engine does **not** run in the offscreen document. Every instance is a **renderer**: an `<iframe sandbox="allow-scripts" src="renderer.html">` whose unique **opaque origin** is the security boundary, reached over a mojom interface. The offscreen document is the only trusted zone, and it holds the network policy, the store and the popup — none of which the engine can touch. One WASM instance per origin-keyed agent cluster, `(browsing-context group, origin)`.

```
extension/
  intercept.js         Main-world fetch/XHR/WebSocket/EventSource wrapper (request + response capture)
  content.js           Isolated-world content script — the page-context relay + the postMessage/MessageChannel
                       listener. A seed is an ADDRESS, never bytes: this zone ships no document body and no
                       response headers, because a second document-load path would bypass the one chokepoint
  background.js        Thin, STATELESS service worker — owns the offscreen lifecycle, forwards browser events,
                       performs the privileged chrome.* the offscreen lacks. Holds NO state, does NO fetch.
  offscreen-brain.js   The offscreen CORE (stable lifetime + IndexedDB) — the TRUSTED ZONE. Wires lib/*.
  bridge.js            The platform edge the engine cannot be: network/IDB/renderer edges. NO analysis logic.
  renderer.html        The RENDERER DOCUMENT — one WASM engine instance, in a frame the browser isolates.
  renderer-host.js     The trusted side of one renderer; render-process-host.js owns the pool.
  mojo.js / mojom.js   The interface between the trusted zone and a renderer.
  popup.js             Popup controller (rendering, replay, form builder, security panel)
  check.js             The JS mirror of the engine's DCHECK/CHECK — a consumer never defaults a producer's field
  lib/                 The platform edge, one problem per file: schema, keys, persistence, grouping, learn,
                       response-decode, encode, discovery-probe, analyze, merge, send, serialize, sourcemap,
                       discovery, protobuf, req2proto, and safe-fetch.js — the ONE external-fetch chokepoint,
                       which alone decides scheme, SOP/CORS, credentials, provenance and destination.

engine/
  host/main.c            The native host's entry and the paint/ABI surface.
  host/solver/*.c        Time-travel primitives — the WFQ scheduler, per-flow COW heap+DOM delta, the concolic
                         value, value-domain constraints, HTML/JS breakout analysis, reply learning, the cold
                         tier, the @S candidate search.
  host/browser/core/*/   The browser, by component: css, layout, paint, dom, html, frame, events, fetch, loader,
                         indexeddb, streams, crypto, canvas, url, file, xml, timing, storage, image, graphics …
                         Each is built from its own spec section with a DCHECK asserting that section's invariant.
  host/check.h           CHECK (always-fatal, dev AND release) vs DCHECK (dev-only) — the offensive-programming
                         mechanism. A crash is the system doing its job: it names the capability to build.
  qjs/                   The forked quickjs-ng — ORDINARY TRACKED CONTENT of this repository, kept a minimal
                         delta over upstream. The host/scheduler lives OUTSIDE it, in engine/host/, which is a
                         statement about how much of upstream this project rewrites. The one gitlink left in
                         the tree is engine/qjs/test262, the conformance corpus.
  build.mjs              Builds the Chrome wasm target and the native target from the same sources.
```

## Storage & security model

All learned state lives in **IndexedDB in the offscreen document** (its lifetime is stable, so it survives MV3 service-worker eviction). `chrome.storage.local` is **banned** — a compromised renderer could read cross-site structural metadata.

Trust is decided by **`sender.tab.url`, never `sender.id`** (every sender carries the extension id, so it is not a discriminator), and the principal for a credentialed read is **`MessageSender.origin`**, never derived from a URL. The content script, the website and the QuickJS/WASM bundle are all **untrusted**; the offscreen document is the only trusted zone.

All network goes through one auditable chokepoint, `extension/lib/safe-fetch.js`, or the page-context relay. The untrusted engine holds **no network or security policy by construction** — the chokepoint alone decides scheme, SOP/CORS, credentials, and whether a derived request may be sent at all. Whether a request is fired is a **per-origin choice**, not a global switch: by default an origin is loaded the way a browser loads it — any subresource the page's own markup names or its own running code computes, scripts and styles alike, same-origin and credentialed — and nothing else. The discriminator is **whose act the request is**, not what the reply becomes; data fetches, probes and discovery are requests the page never makes, and need that origin deliberately widened.

**Nothing is refused at every setting, so the control surface *is* the safety.** There is no combination the code forbids outright. What the chokepoint does instead is compute every signal it can about a request — method, credential state, provenance (`observed` / `derived` / `forced`), URL-carried authority, destination, intended invalidity — and present them as separate per-origin rows, each with its **reliability** beside it (`certain`, `stated`, `partial`, `undetermined`, `intent`). A signal it cannot determine renders as a stated **unknown** rather than being left out, because an absent row reads as a question that was answered. A single collapsed risk score is deliberately *not* offered: a wrong weighting inside one is invisible, while a named signal a person allowed is a decision they can revisit.

One consequence is **named rather than mitigated**, and it is the honest limit of the model: an address the bundle computed out of a *credentialed* reply, fetched uncredentialed, still names that person's account. The chokepoint can read the credential state of the request in front of it and nothing about where the address came from, so the `lineage` row reads `unknown` for every request at every setting. The engine is the only thing that could compute it — it already grades every value's provenance — and that is unbuilt.

The page-context relay is the other edge, and it is exempt from the credentialed destructive-path deny list **by who acted, not by which transport carried it**: a request an operator composed at a surface that showed them the address, the verb and the body is authorized by a human, and a substring deny list standing there would be the tool vetoing its operator on a pattern they never saw. That exemption is a value the initiating surface *states* and the router `CHECK`s — an absent or unknown grade takes the refusing arm, so forgetting to state one is not a way to be exempted.

**Authorized security research only.** No `webRequest`/debugger permission; no visible browser-UI impact. Popup content is escaped to prevent self-XSS.

## Build

```
node engine/build.mjs            # the Chrome wasm target -> extension/lib/qjs/
node engine/build.mjs native     # the native host (the fixture, and --abi over the shipped ABI)
```

A build reads its inputs from the working tree, and this tree is edited continuously — so a number that is going to be quoted comes from a **frozen snapshot** at a named revision, never from the working copy:

```
engine/frozen_snapshot.sh <revision> <lane-name> node engine/build.mjs
```

A result quoted without the revision it came from is not a measurement.

## Gates

The engine asserts its own invariants at their origin, so a change is verified by whether the asserts stay **silent**. The corpora below are how code gets *exercised* so the asserts get a chance to fire — never the thing that certifies a change, and a passing suite proves nothing it did not exercise.

```
node engine/test262.mjs [subdir]    # ECMAScript conformance — the JS half's oracle, self-validating
node engine/wpt.mjs [subdir]        # web-platform-tests — the browser half's oracle, written by the spec's authors
node engine/solvergate.mjs [doc]    # the solver differential: one document under several schedules must emit
                                    # the SAME findings. Stores no expected output, so it fails only when a
                                    # build disagrees with ITSELF.
node engine/citegen.mjs [path]      # every spec citation resolved against a committed per-standard corpus:
                                    # wrong section, wrong title, fabricated or diverging quotation, bad step
node engine/idlgen.mjs              # the real .idl diffed against what each component installs — a GAP AUDITOR
```

Driving a document directly, for a real page or a served fixture:

```
node engine/trusted.mjs <url> <binary> --paint <dir>    # run one document; write one image per world
node testing/harness.js restart                         # live Chrome: kill + clear IndexedDB, pick up a rebuild
node testing/harness.js goto <url>                      # the ground truth — real Chrome, real page
```

Real Chrome is **confirmation**, never the source of truth — the spec is. A browser-fidelity gap is always "implement the spec at the root".
