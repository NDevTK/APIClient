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
- **This `sender.tab.url` is the analysis PRINCIPAL** for every downstream security decision (the SSRF
  origin and `window.location`). It is plumbed per page; a content-script-supplied URL (`msg.url`)
  is *never* used as the principal — that was an actual hole this session (a `scripts[0].url`
  fallback) and was removed.

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

- **cookies omitted** (`credentials:"omit"`) — no credentialed exfiltration.
- **GET only** — forced execution explores many paths; it never replays a state-changing method.
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

## The QuickJS/WASM sandbox is attacker-controlled

The bundle runs inside QuickJS (WASM linear memory), reaching the host only through fixed edges.

- It **cannot** read or set the principal (it lives in worker JS, outside the WASM, set per page) and
  **cannot** do raw network — its own `fetch()` calls are *recorded*, not issued; its `import()`
  targets become **untrusted URLs** that `safeFetch` gates (origin-relative SSRF + CORB).
- **One WASM instance per page** (`freshInstance`, isolated memory) — the sandbox is never reused
  across host pages, so a fetch is always governed by a single, correct origin.
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
| Bundle escapes the WASM sandbox into the worker | **Out of model** — assumes the WASM boundary holds; a true escape has raw network regardless, so the defense is the browser sandbox + CSP `connect-src` |
