# Security Model

The analyzer runs **untrusted web bundles** to learn API surface. So the design assumes the
page, its content script, and the JS engine running the bundle are all hostile, and confines
them. This file is the threat model; per-function contracts are inline (`grep @security-contract`).

## Trust zones

| Zone | Trust | Why |
|------|-------|-----|
| Website + its JS bundle | **UNTRUSTED** | arbitrary attacker code |
| `content.js` / `intercept.js` (web renderer) | **UNTRUSTED** | a compromised renderer controls them; runs in every page (`<all_urls>`) |
| QuickJS/WASM engine (runs the bundle) | **UNTRUSTED** | it *executes* the attacker bundle. Confined to the WASM sandbox + a fixed set of host edges |
| Analysis worker (`ast-thread.js`) | trusted code, **hostile inputs** | hosts the WASM; treats every value the engine produces (URLs, source-map pragmas) as attacker-shaped |
| **Offscreen document** | **TRUSTED** | the only fully-trusted zone — owns state and the network chokepoint |
| Service worker (`background.js`) | **STATELESS** | extension-page-only; never relays page data |

## The trust boundary — `sender.url`, never `sender.id`

A message's authority comes from **`sender.tab.url`** (set by the browser process, unforgeable),
**never** `sender.id` and **never** the message payload.

- `sender.id` only rejects *other* extensions — our own content script runs in every renderer, so a
  compromised renderer already holds our `sender.id`.
- A content script's `sender.url` is always the web origin (`https://…`), never `chrome-extension://`.
  The router drops a content message lacking `sender.tab`, routes web origins to the untrusted
  `handleContentMessage`, and only extension origins to trusted handlers.
- **This `sender.tab.url` is the analysis PRINCIPAL** for the SSRF host classification and
  `window.location`. It is plumbed per page; a content-script-supplied URL (`msg.url`)
  is *never* used as the principal — that was an actual hole this session (a `scripts[0].url`
  fallback) and was removed.
- **Same-origin principal for a CREDENTIALED read is a stricter value: the *requesting frame's*
  `MessageSender.origin`** (documentId-keyed, opaque-unique via `_senderOrigin`), **never** the top
  frame and **never** parsed from a URL. A page can sandbox its own iframe, giving it an opaque
  (`"null"`) origin whose URL still looks normal — so URL-parsing, or using the tab/main-frame origin,
  would let a sandboxed sub-frame read the *embedder's* authenticated bytes. An opaque origin is unique
  per spec → never same-origin with anything (the `"null"==="null"` collision is closed). The per-tab
  combined buffer tracks this as `frameOrigin`; two real origins in one buffer collapse to a unique
  opaque token → the credentialed reply-seed **fails closed**.
- **Direction matters for the extension trust boundary.** SW→offscreen (`__evt`) is authenticated by
  `sender.url`: a service worker has no frame, so Chrome leaves its `MessageSender.origin` undefined
  (a browser quirk) and only `sender.url` carries the extension origin. Document→document hops
  (offscreen→SW in background.js, offscreen→popup) use `sender.origin === chrome-extension://<id>`
  (browser-set, and `"null"` for a sandboxed extension page → rejected).

## State / storage

- Learned data (endpoints, schemas, field values, API keys) lives in the **offscreen brain +
  IndexedDB** (`uasr_store`, origin `chrome-extension://<id>`) — unreachable from content scripts.
- **`chrome.storage.local` is banned**: a compromised renderer with the `storage` permission could
  read every key (cross-site leakage). IndexedDB in the extension origin has no content-script read path.
- The service worker holds no learned state.

## Network — one chokepoint

Every analyzer-driven request goes through **`safeFetch`** (`lib/safe-fetch.js`); credentialed /
page-context requests go through **`pageContextFetch`** (as the actual page, browser-CORS/PNA-gated).
There is no raw `fetch` on any analyzer path. `safeFetch` guarantees, in one auditable place:

- **cookies omitted by default** (`credentials:"omit"`) — no credentialed exfiltration. In
  **credentialed mode** (`opts.credentialed` — replay a learned GET to read the real *authenticated*
  reply, the logged-in API surface) cookies ARE attached, but the REPLY is gated by safeFetch's **own
  SOP/CORS**: same-origin to the page principal (`opts.pageOrigin` = the requesting frame's
  `MessageSender.origin`, opaque-unique — an opaque/`"null"` principal is same-origin with *nothing*)
  is readable; a cross-origin credentialed read is allowed
  only if the server granted the page's *exact* origin a credentialed read (`Access-Control-Allow-Origin`
  == origin, never `*`, **and** `Access-Control-Allow-Credentials: true`). The browser's same-origin
  policy does **not** apply to a host-permission fetch (it can read any origin), so safeFetch
  re-implements the credentialed-CORS rule on the bytes before returning them.
- **GET only** — forced execution explores many paths; it never replays a state-changing method. A
  well-designed server does not mutate on GET, so even a credentialed GET replay is side-effect-free;
  POST/PUT/DELETE endpoints are RECORDED by forced exec, never issued.
- **http(s) only** — `file:`/`data:`/`blob:`/`chrome-extension:` are rejected.
- **origin-relative SSRF (Private Network Access):** a *private* target (loopback / link-local /
  RFC1918) is blocked **unless the page principal is itself private** — a public page cannot use the
  extension's host permissions to reach the user's localhost/intranet; a localhost page *may* reach
  localhost. Checked on the **initial URL and the post-redirect final URL**.
- **CORB by load type (`opts.as`):** `"script"` (a chunk/import that becomes executable code) must be
  JS-typed when cross-origin — never read a cross-origin HTML/JSON body as code; `"sourcemap"`/data
  are exempt (not executed). Centralised here so a new code-loader can't forget it.
- **per-call principal** (`opts.pageUrl` = the page's `sender.tab.url`) — **not a shared global**: two
  grinds run concurrently in one worker, so a global principal would let one page's origin
  contaminate another's fetch. Unknown principal → treated as public → private targets blocked.

A cross-origin script/source-map uses the **page's** origin, never the asset's own host (gstatic.com
JS in google.com acts as google.com) — exactly what the principal encodes.

**`pageContextFetch` carries the GET-only rule in its SHAPE, because a check could not live at the far end.**
The relay ends in `content.js handlePageFetch`, which takes `msg.method` verbatim — and content.js is an
UNTRUSTED zone, so a method check written there checks nothing. The trusted sender is the only place the rule
can hold, and a single entry taking `opts.method` cannot hold it either: the two callers are different
operations (discovery LEARNS and may never mutate; the popup's Send REPLAYS A REQUEST THE USER TYPED, any
method by definition), so one shared parameter degenerates into whatever each caller passes. It did: discovery
sent `method:"POST"` with `X-Http-Method-Override: GET` — the documented trick for a service that 405s a
GET — fired automatically by passive learning with no user action, against both this section's "GET only" and
CLAUDE.md's "a state-mutating request is NEVER fired to learn". So there are **two entries** (`lib/schema.js`):
`pageContextGet(tabId, url, headers, documentId)` for learning, which has **no method parameter at all**, and
`pageContextSend` for the popup's manual replay, where the human chose the method. A discovery candidate is
correspondingly `{url, headers}` with no method field. The rule is structural — there is no place to express a
POST — which is the same shape `req2proto.c` has (no entry that issues a request) rather than a check someone
has to remember.

## The QuickJS/WASM sandbox is attacker-controlled

The bundle runs inside QuickJS (WASM linear memory), reaching the host only through fixed edges.

- It **cannot** read or set the principal (it lives in worker JS, outside the WASM, set per page) and
  **cannot** do raw network — its own `fetch()` calls are *recorded*, not issued; its `import()`
  targets become **untrusted URLs** that `safeFetch` gates (origin-relative SSRF + CORB).
- **One WASM instance per ORIGIN-KEYED AGENT CLUSTER** — `(browsing-context group, origin)`, isolated memory
  — never per tab and never per page, because a tab holds documents
  that may be cross-origin to each other and an iframe or a popup gets its own instance the moment its
  origin differs. The instance IS the principal: a fetch is governed by a single, correct origin
  because there is exactly one origin in the instance that issued it, and that stays exactly true when
  same-origin documents share one — origin, not document, is what the invariant is keyed on. Reusing
  an instance across ORIGINS would put two principals behind one `pageOrigin`; splitting one across
  same-origin documents would instead break HTML's own single-heap agent, which same-origin DOM
  adoption and cross-frame closures rely on.
  **BOTH HALVES OF THE KEY ARE BROWSER-STATED** (`clusterKeyOf` in `bridge.js`, over what `analyze.js`
  carries off the browser's `MessageSender`), because the untrusted engine may state neither: the ORIGIN
  half is `_senderOrigin`'s — the browser's `MessageSender.origin`, opaque-unique per document, so two
  sandboxed iframes of one page are two clusters and two instances — and the GROUP half is
  `sender.tab.id`, a tab being exactly one top-level traversable with every nested navigable under it in
  that traversable's group. A document the engine itself creates never goes through that key: it
  inherits its CREATOR's group on the `navigable.create` notice. RESIDUAL, and it is a narrowing rather
  than a merge: an auxiliary opened by `window.open()` without `noopener` is in the same group as its
  opener but lands in another tab, so this key splits that pair; closing it needs `openerTabId`, which
  `MessageSender` does not carry. UNBUILT, and named because the alternative is a second instance: a
  document of a cluster that the cluster's instance is not already running — a same-origin frame the
  engine did not model, a second cross-origin child at one origin, a navigation replacing the group's
  top — has no way in yet. It needs a `qjs_join` beside `qjs_init` (a second Lexbor parse, a realm
  through `engine_child_realm`, its scripts seeded on the same frontier), and the host asserts at each
  such arrival rather than provisioning a second heap for one agent.
- **The engine NAMES its own child documents; the offscreen ROUTES them.** An `<iframe>` insertion or
  a `window.open()` mints the child's name inside the instance (`"<parent>.<n>"`, unique by induction)
  and announces it as a one-way notice; the offscreen is what provisions an instance for that name and
  what carries a cross-document read to it. That split is deliberate and it is the same rule as
  `sender.tab.url`: **identity may be minted by the untrusted side because it is only a name, but
  ROUTING and the ORIGIN STAMPED ON A DELIVERED MESSAGE are the trusted zone's alone.** A forgeable
  `event.origin` would defeat every origin check in every bundle the engine analyses — it would report
  exploits that are not real and miss ones that are — so the engine never supplies one.
- **A cross-document delivery carries serialized bytes, never a live value**, for the same reason a
  parked flow's snapshot does: a live value crosses neither an instance, nor a session, nor a park.
- **Source maps stay outside QuickJS**: fetched/parsed in worker/offscreen JS (`as:"sourcemap"`),
  used only to label findings shown in the UI — never fed back into the engine.

## Per-boundary contracts

Each loader / trust boundary carries an inline `@security-contract` block stating *what crosses*
(javascript / sourcemap / data) × *whether it reaches QuickJS* (`quickjs-control`) × the
`enforced-by` check that backs the claim, plus honest `RESIDUAL` notes. The `enforced-by` pointer is
the honesty mechanism — a label that can't cite a real check is marked as a residual, not asserted.

## Known residuals (not yet airtight)

- **Worker-zone source-map fallback:** the primary map fetch is in the trusted offscreen
  (`pageContextFetch`), but a fallback (`_smGetParsed`) runs in the QuickJS-*hosting* worker (outside
  the WASM, but not the offscreen). Defense-in-depth would move it to the offscreen (needs offscreen
  chunk-map pre-fetch). Sound under the assumption that the WASM sandbox confines the bundle.
- **Redirect-to-private:** `safeFetch` re-validates the final URL after redirects (so internal data is
  never *ingested*), but preventing the redirected request from *reaching* a private host relies on the
  browser's Private Network Access for extension fetches.

## Finding quality — gated by CONSTRAINTS + sink sensitivity, never reachability

"External input reaches a sink" is NOT a finding by itself — the value is opaque for control-flow
*because* it is attacker-influenced, so the question is what the path CONSTRAINTS allow and how
sensitive the sink is. That constraint set is the same data Z3 already computes for path
satisfiability: the sanitizer ops surfaced in the finding UI (`encodeURIComponent()`,
`replace(/[<>]/g,'')`) **are** those constraints, not a separate analysis.

- **Taint → `fetch`/request param:** report only when the constraints let the input change a
  SECURITY-SENSITIVE parameter. `location.search` deciding the allow-list for a `DELETE /account`
  call is a finding; `document.referrer` / `document.title` flowing into an analytics/logging POST is
  **WAI**, not a finding. The discriminator is the sink + the constraints, never the data-flow edge.
- **Open redirect is NOT a finding on its own** (Google VRP stance) unless it also leaks a
  credential / access token — and a navigation is reachable via `w = open(); w.location = …` anyway,
  so the redirect primitive alone is weak. Flag it only bundled with a token/secret exfil.
- **XSS verification = build + RUN the PoC for real**, never asserted from the taint path alone. The
  PoC is constructed from Z3's solve over the real traced flow (CLAUDE.md: BUILT from the solve, never
  templated) and executed; the verifiable OUTCOME (sink fired with the attacker value) is the triage
  signal — it proves exploitability even though it does not localize the bug. The UI's beautified
  (source-mapped) taint-path + constraint view is the human-readable explanation of that same run.

## Attack scenarios

| Scenario | Outcome |
|----------|---------|
| Compromised renderer reads stored cross-site data | **Mitigated** — state is IndexedDB in the extension origin, not `chrome.storage.local`; no content-script read path |
| Renderer sends a data-returning message type (`GET_STATE`, …) | **Mitigated** — `sender.url` gate routes content scripts to the data-input-only handler; data types dropped |
| Renderer forges `sender.id` | **Not a threat** — already has it; `sender.url` is the real gate |
| Renderer forges `sender.url`/principal as a localhost origin to defeat SSRF | **Mitigated** — principal is `sender.tab.url` (browser-set), never `msg.*` |
| Bundle `import()`s `http://127.0.0.1/…` or an intranet host (public page) | **Mitigated** — `safeFetch` origin-relative SSRF blocks public→private (initial + post-redirect) |
| Bundle imports a cross-origin HTML/JSON endpoint as a "chunk" | **Mitigated** — `safeFetch as:"script"` CORB ingests JS-typed only |
| Concurrent localhost-page grind lends its origin to a public page's fetch | **Mitigated** — principal is per-call, not a shared worker global |
| A page sandboxes its own iframe (opaque origin) to read the embedder's credentialed API via the shared analysis | **Mitigated** — credentialed-read principal is the *requesting frame's* `MessageSender.origin` (opaque-unique, documentId-keyed), never the top frame or URL-parsed; an opaque origin is same-origin with nothing; a mixed-origin buffer fails closed |
| Sandboxed *extension* page (origin `"null"`) impersonates a trusted extension document | **Mitigated** — document→document hops require `sender.origin === chrome-extension://<id>`; `"null"` ≠ that |
| Bundle escapes the WASM sandbox into the worker | **Out of model** — assumes the WASM boundary holds; a true escape has raw network regardless, so the defense is the browser sandbox + CSP `connect-src` |
