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

### What the one trusted zone actually holds — and why that is a named cost

The offscreen is not "the network chokepoint plus some state" any more. It is the ONLY zone in the extension
that is trusted at all, and everything that needs trust has accumulated in it: the sole
`chrome.runtime.onMessage` router (`offscreen-brain.js:1360` — the only one in the document; `ast-worker.html`
loads 28 scripts and no other registers a listener), the browser-fact mint that every principal in the system
is read out of (`_browserFacts`), the privileged-`chrome.*` RPC **client** (`swRpc` — the SW performs
`tabs.*` / `webNavigation.*` / `scripting.*` on its word alone), the network chokepoint (`lib/safe-fetch.js`),
the page-context relay's trusted end (`lib/schema.js`), IndexedDB persistence, the engine pool and its
level-1 WFQ (`bridge.js`), the whole popup command surface (25 commands, `lib/popup-handlers.js`), the
exploit-probe sessions, and live-traffic moat aggregation.

**This is stated, not defended.** Concentration is the correct SHAPE — a second trusted zone means two
places to get the principal right and a channel between them to get wrong — but it means the router at
`offscreen-brain.js:1360` is the single gate behind which all of it sits, and every rule below is a property
of that one function or of what it hands a message to. The invariants that make it survivable are structural
and each is testable in isolation: the router branches on browser-set `sender` fields only; every principal
comes from ONE mint; the SW independently re-checks the caller of its privileged RPC rather than trusting
that the offscreen is the only one who can reach it. A rule here that cannot name the check backing it is a
residual, not an assertion.

## The trust boundary — `sender.url`, never `sender.id`

A message's authority comes from **`sender.tab.url`** (set by the browser process, unforgeable),
**never** `sender.id` and **never** the message payload.

- `sender.id` only rejects *other* extensions — our own content script runs in every renderer, so a
  compromised renderer already holds our `sender.id`.
- A content script's `sender.url` is always the web origin (`https://…`), never `chrome-extension://`.
  The router drops a content message lacking `sender.tab`, routes web origins to the untrusted
  `handleContentMessage`, and only extension origins to trusted handlers.
- **The analysis PRINCIPAL is the DOCUMENT'S OWN `sender.url`, not `sender.tab.url`** — a correction, and
  the looser rule this line used to state was an actual bug. `sender.tab.url` is the address of the TOP of
  the tab; using it as the origin-relative SSRF principal makes a frame inherit its embedder's
  classification, so a sub-frame in a tab whose top is `http://localhost/` was allowed to reach the user's
  intranet on the top page's behalf. The private-network principal and the engine's `window.location` are
  both `facts.url` = the browser's `sender.url` for THAT document (`offscreen-brain.js:219`, `:709`,
  reaching `safeFetch` as `opts.pageUrl` at `bridge.js:853/917/966`). A sub-frame analyses as ITSELF, never
  as its embedder.
- **`sender.tab.url` is still carried, for a different question**: it is HTML §8.1.3.1's TOP-LEVEL CREATION
  URL, which §8.1.3.5 decides secure-context from, and it travels as `facts.topLevelUrl`
  (`offscreen-brain.js:220`). Authorization keys on it — a message's authority is the tab it came from — but
  it is not a fetch principal. Two facts, two names, never one field doing both.
- A content-script-supplied URL (`msg.url`) is *never* the principal — that was an actual hole (a
  `scripts[0].url` fallback) and was removed.

### The principal is MINTED in one place, and the untrusted side sends no field shaped like one

Two mechanisms, and they are the reason "never URL-derived" is structural rather than a convention anyone
has to remember:

- **ONE MINT.** `_browserFacts(sender)` (`offscreen-brain.js:198`) takes a `MessageSender` **and nothing
  else** and is the only producer of `{documentId, origin, url, topLevelUrl, tabId, frameId}`. A caller
  cannot state one of these without holding the object the browser filled in, so the failure this exists to
  prevent — deriving a principal from an address — has no argument to travel in. Every field is DCHECKed at
  the mint; the record is frozen and branded `stated:"MessageSender"`, and every consumer reads it back
  through `_statedFacts` (`:238`), which is a **`CHECK`** — fatal in release too, because continuing with a
  principal this zone did not mint is worse than aborting. `_senderOrigin` (`:156`) is the origin half: a
  valid tuple origin as-is, an opaque one as a per-`documentId` `null:<uuid>` token so two opaque documents
  never compare same-origin, and no `documentId` → an unstorable fresh token → fails closed.
- **NO PRINCIPAL-SHAPED FIELD ON THE WIRE.** An untrusted zone does not get to state an origin or an
  address, so it does not get to SEND a field shaped like one. `content.js` used to put `origin:
  location.origin` and `pageUrl: docUrl` on `CONTENT_HTML` and `CONTENT_FORM_SUBMIT`; nothing ever read
  them, and that is exactly what made them dangerous — they sat on the wire under the precise names the
  removed hole was spelled in, waiting for a consumer. They are deleted. The rule is the general one: a
  message from an untrusted zone carries MATERIAL (html, response bytes, the address the page is posting
  TO) and never identity.
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
- **TWO PREDICATES, BECAUSE THOSE ARE TWO RULES — and the offscreen router keeps them separate**
  (`offscreen-brain.js:1375`). `fromExtUrl` (url prefix) authenticates only the SW's `__evt` hop;
  `fromExtDocument` (`sender.origin === EXTENSION_ORIGIN`) is what gates the trusted popup command surface.
  Collapsing them into one url-prefix test deletes the document→document rule: a **sandboxed** extension
  page's `sender.url` is still `chrome-extension://<id>/…` while its origin is the opaque `"null"`, so a
  prefix hands an opaque-origin document `GET_STATE` / `SEND_REQUEST` / `CLEAR_TAB`. `poc-sandbox.html` and
  `renderer.html` are both sandboxed (`manifest.json`), so this is a page that exists. Content types are
  dropped on the BROADER predicate (`:1397`) — dropping more can only ever refuse — and the popup gates the
  reverse direction on `sender.origin` too (`popup.js:762`), because a boundary checked on one side only is
  not a boundary.

## State / storage

- Learned data (endpoints, schemas, field values, API keys) lives in the **offscreen brain +
  IndexedDB** (`uasr_store`, origin `chrome-extension://<id>`) — unreachable from content scripts.
- **`chrome.storage.local` is banned, and the ban is ENFORCED BY THE MANIFEST**: a compromised renderer with
  the `storage` permission could read every key (cross-site leakage). IndexedDB in the extension origin has
  no content-script read path. The `storage` permission is not requested at all (`manifest.json`
  `permissions` = `offscreen`, `webNavigation`, `scripting`, `tabs`), so `chrome.storage` is not merely
  unused — it is **undefined in every context**, and a call would throw rather than leak. There are zero
  `chrome.storage.*` call sites in the extension; the four textual matches are the comments recording the
  ban and the removal of the old `chrome.storage.session` mirror (`lib/persistence.js:4/194`,
  `background.js:171`, `offscreen-brain.js:547`). The `session` mirror existed only because the brain used
  to live in the evictable SW; the offscreen document's stable lifetime replaced it.
- The service worker holds no learned state, and relays no page data: content scripts message the offscreen
  DIRECTLY (a `chrome.runtime.sendMessage` broadcast reaches every extension context), so the SW cannot
  launder a web renderer's message into a trusted extension-origin one. It forwards exactly one browser
  event the offscreen cannot observe (`__evt TAB_REMOVED`, `background.js:75`).

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
  **CREDENTIALED MODE IS DORMANT AND FAILS CLOSED — the rule above describes a capability nothing turns
  on.** No caller sets `opts.credentialed` (`bridge.js:854` reads `!!msg.credentialed` and nothing ever sets
  `msg.credentialed`), and no `pageOrigin` reaches the chokepoint from the engine host at all, so
  `_isRealOrigin("")` is false, no `ACAO` can ever match, and every credentialed read returns
  `blocked-cors-credentialed`. `bridge.js:840` carries a standing `@security-finding` naming the value to
  pass when it IS enabled — `msg.origin`, the browser's `MessageSender.origin` plumbed by
  `_dispatchDocument` — and naming the obvious wrong fix (`originOf(msg.sourceUrl)`), which is the exact
  URL-derivation this principal exists to forbid. **The credentialed reads that actually happen today go
  through the page-context relay instead** (see below), where the browser enforces SOP/CORS rather than us.
- **GET only, enforced by ABSENCE** — `safe-fetch.js` hardcodes `method:"GET"` and never reads `opts.method`
  or `opts.body` (grep: no occurrence). Forced execution explores many paths; it never replays a
  state-changing method. A well-designed server does not mutate on GET, so even a credentialed GET replay is
  side-effect-free; POST/PUT/DELETE endpoints are RECORDED by forced exec, never issued.
  **THE REFUSAL IS NO LONGER SILENT ON THE XHR PATH, and it is the CALLER's, not the chokepoint's.**
  `bridge.js` used to hand the chokepoint the page's real `method`/`body`/`credentials` and they were dropped
  without a word, so `xhr.open("POST", u)` was answered with the reply to a *GET* of `u` and the engine
  modelled that reply as the POST's — a wrong answer, which is worse than an absent one, and every @H example
  value and @S verdict downstream of it was derived from a response the server never gave for that request.
  `fetchedXhr` now refuses a non-GET before the call — `blocked-method:<M>`, the same shape as
  `blocked-scheme:` — with no bytes beside it, which is the network error §3.5.6's "handle errors" turns into
  the page's `error` event. It passes ONLY what the chokepoint reads, so there is nothing left for it to drop
  in silence; `q.credentials` is deliberately not mapped onto `opts.credentialed` (that flag is this zone's
  decision, never the analysed bundle's). **STILL OPEN on the `fetch()` path**, and it is the same defect one
  seam over: the pending register carries the method (`PEND_METHOD`) but `engine_pending_urls` joins URLs
  ALONE and `engine_provide` fills every entry whose URL matches, so a page that issues a GET and a POST to
  one address has both promises settled with the GET's body. The fix is in the engine — the pending record
  and the provide key are `(method, url)`, not `url`.
- **`opts.headers` COMES FROM THE UNTRUSTED BUNDLE on the XHR path** — a correction; `safe-fetch.js`'s own
  "analyzer probe headers only" comment is no longer the whole truth. `fetchedXhr` forwards the page's
  header list. It is within the model (an uncredentialed GET to a public host, with forbidden header names
  stripped by the browser), and it stays within it only because credentialed mode is off: a bundle-chosen
  header list on a cookie-bearing request is a different question and must be re-decided when that lands.
- **http(s) only** — `file:`/`data:`/`blob:`/`chrome-extension:` are rejected.
- **origin-relative SSRF (Private Network Access):** a *private* target (loopback / link-local /
  RFC1918) is blocked **unless the page principal is itself private** — a public page cannot use the
  extension's host permissions to reach the user's localhost/intranet; a localhost page *may* reach
  localhost. Checked on the **initial URL and the post-redirect final URL**.
- **CORB by load type (`opts.as`):** `"script"` (a chunk/import that becomes executable code) must be
  JS-typed when cross-origin — never read a cross-origin HTML/JSON body as code; `"sourcemap"`/data
  are exempt (not executed). Centralised here so a new code-loader can't forget it.
- **per-call principal** (`opts.pageUrl` = the requesting DOCUMENT's own `sender.url`, never the tab's) —
  **not a shared global**: two grinds run concurrently in one worker, so a global principal would let one
  page's origin contaminate another's fetch. Unknown principal → treated as public → private targets blocked.

A cross-origin script/source-map uses the **page's** origin, never the asset's own host (gstatic.com
JS in google.com acts as google.com) — exactly what the principal encodes.

### The page-context relay — the trusted zone borrows a privilege it does not have

`pageContextFetch` (`lib/schema.js:412`) issues the request AS THE PAGE: the offscreen sends `PAGE_FETCH`
over `swRpc("tabs.sendMessage", …, {documentId})` and `content.js handlePageFetch` (`content.js:319`) runs
`fetch(url, {credentials:"same-origin", …})` in the renderer. This is the only credentialed read that
actually happens, and it is the one edge where the trusted zone instructs an UNTRUSTED one. Both directions
have to be stated, because they fail differently and only one of them is a privilege question.

- **Trusted → untrusted (the request): no privilege is conferred, so nothing needs to stop it.** The content
  script can already fetch anything its own page can fetch with its own cookies; the relay asks for a subset
  of what that renderer holds. It is a DOWNGRADE from the extension's `<all_urls>` reach, not a grant. This
  is why "what stops a compromised content script asking for a fetch the offscreen never authorised" has no
  check behind it and needs none — a compromised renderer does not need our relay to make that request.
  What the relay does need, and has, is that the far end holds NO policy: `handlePageFetch` takes
  `msg.method` verbatim, so the VERB is decided entirely at the trusted call site. That is why the three
  entries are named for their OPERATION rather than for a method (`pageContextGet` GET-only for learning a
  published document, `pageContextSend` any method because the human chose it in the Send panel,
  `pageContextFetch` POST for `req2proto`'s malformed-body error probe) and each states its verb at the call
  site rather than inheriting one. Routing is `documentId`-only — no `frameId` fallback (reused across
  navigations at a different origin) and no target-less `tabs.sendMessage` (which broadcasts to every frame).
  No `documentId` → refuse.
- **Untrusted → trusted (the reply): the reply is ATTACKER DATA, and it is attributed to the URL we asked
  for.** Nothing binds the returned `{status, headers, body}` to the request; a compromised renderer can
  return anything. Two consequences, and they are not the same size. Same-origin: the page fabricates data
  about its own endpoints, which it could equally do by serving them — not a finding. **Cross-origin: the
  relay's reply is attributed to a URL on ANOTHER origin.** The browser is what makes this small — a
  cross-origin `fetch` with `credentials:"same-origin"` sends no cookies and is CORS-gated, so the honest
  answer is usually an error — but the renderer is free to answer that error with a plausible body, and the
  offscreen files it under the other origin's endpoint in the learned store and the popup. **The learned
  store therefore holds page-CLAIMED data, and a finding derived from a relayed reply is only as good as
  the renderer that returned it.** Learned data is a research artifact, never evidence about a third party.
- **`credentials:"same-origin"` is load-bearing and was previously unstated**: it is what keeps a relayed
  cross-origin request from spending the user's cookies at a host the page merely named.

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

## The live PoC verify is ONE-SIDED — a hit is a page's CLAIM, not a browser's answer

`intercept.js` runs in the **MAIN world** (`manifest.json`), i.e. inside the page, i.e. fully
attacker-controlled. It installs `window.apiclientsink(id)` (`intercept.js:509`) and relays each call to
`content.js` (CustomEvent + a `data-uasr-hits` attribute mirror), which forwards it as `PROBE_HIT`
(`content.js:73`); the offscreen correlates it to a probe session by the id (`offscreen-brain.js:949`) and
the popup reports **"REAL EXPLOIT — apiclientsink fired in real Chrome"** (`lib/popup-security.js:480`).
Everything on that path before the offscreen is the page. So:

- **The marker is not a secret from the target.** It is a fresh `crypto.randomUUID`
  (`offscreen-brain.js:1155`), correctly kept out of the URL as a *token*, but it rides INSIDE the delivered
  payload as `apiclientsink('<uuid>')` — and the payload is delivered TO the page, in its own fragment or
  query string (`:1101`). Any script on the page can read `location.hash`, lift the uuid and call the hook
  without any sink having fired.
- **Therefore a POSITIVE verdict is FABRICABLE and a NEGATIVE is SUPPRESSIBLE.** The false positive is the
  one that matters: a hostile page can make the tool assert a real, Chrome-confirmed exploit against itself.
  Suppression is unavoidable in any design (the page owns the renderer) and is already reported honestly —
  "NOT REPRODUCED … Not a statement that the sink is safe" (`lib/popup-security.js:505`).
- **This does not weaken the rule; it bounds the claim.** A hit means *the payload's code ran in the page* —
  which is what a fired sink produces and is why the signal is worth having. It does not mean the SINK
  produced it. "Engine agreement" is agreement with a witness the page can coach.
- **MISSING CHECK, and it is buildable now:** `PROBE_HIT` is accepted from ANY document in ANY tab that
  knows the marker — the offscreen stamps `tabId`/`frameId` onto the record (`offscreen-brain.js:957`) but
  never *compares* them, and never compares the reporting document's `MessageSender.origin` against
  `origin(session.pageUrl)`. The probe is delivered to a known page, so a hit from anywhere else is not a
  weaker hit, it is a different document's claim about someone else's exploit and must be REFUSED. That
  binding closes cross-document fabrication and leaves only the irreducible same-page case, which is then
  the honest residual rather than an unmeasured one.

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
- **Fabricable PoC-fire hit** — the section above. A page that reads the marker out of the payload
  delivered to it can assert a REAL EXPLOIT verdict. The origin binding on `PROBE_HIT` is not yet built.
- **Relayed-reply attribution** — a `PAGE_FETCH` reply is attributed to the URL the offscreen asked for and
  is vouched for by nothing but the renderer that returned it.
- **Page-claimed origins in the request log** — `sourceOrigin` / `targetOrigin` on a `PM_RECV` / `MC_OPEN`
  record (`content.js:159`, `:189`) are read off a real `MessageEvent` in the isolated world, so they are
  browser-stated *in a renderer we do not trust*. They do NOT reach the engine (no `sourceOrigin` consumer in
  `bridge.js`), so the "the engine never gets a forgeable `event.origin`" rule is intact. The UI half is
  CLOSED: both renderers print them as `page-claimed: A → B` with the reason on the element
  (`lib/popup-reqlog.js` `_claimedOriginPair` / `PAGE_CLAIMED_ORIGINS_TITLE`, used by `lib/popup-console.js`),
  and an empty half prints as `(none stated)` rather than `?`. The send panel no longer *acts* on one it
  cannot address either: a PM reply whose page-claimed source origin is empty or `"null"` is REFUSED instead
  of being sent with `targetOrigin: "*"`, which would have delivered an operator-typed payload to whatever
  document held that window. What remains is that the store still files these records under a claimed origin
  — a research artifact, like every relayed reply.
- **`scripting` + MAIN-world injection is an unused privilege held open.** `_PROBE_INJECTORS` and the
  `scripting.exec` RPC arm (`background.js:102`, `:184`) grant arbitrary MAIN-world execution in any tab
  (`world:"MAIN"`, `target.allFrames`) — full same-origin control of every site the user has open — and
  **nothing calls it** (`background.js:88` carries the standing finding; `tabs.create` / `tabs.remove` /
  `tabs.get` likewise have no caller). The gate is sound and is an *equality*, not a prefix
  (`sender.url === chrome.runtime.getURL("ast-worker.html")`, `background.js:219/23`), so it is unreachable
  today. It is the blast radius of any future hole in that one gate, and the `scripting` permission exists
  only for it. Either the probe orchestration lands and uses it, or the arm, the injectors, the three unused
  `tabs.*` arms and the permission are DELETED together.

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
| A sub-frame in a tab whose TOP is `http://localhost/` reaches the user's intranet | **Mitigated** — the SSRF principal is the frame's own `sender.url`, never `sender.tab.url` (this was a real bug; `sender.tab.url` survives only as `topLevelUrl` for secure-context) |
| Content script states its own `origin` / `pageUrl` and a future consumer reads one as the principal | **Mitigated** — no principal-shaped field is on the wire at all; every fact comes from `_browserFacts(sender)` and is re-asserted by `_statedFacts` (a release-fatal `CHECK`) |
| Compromised renderer asks the relay for a fetch the offscreen never authorised | **Not a threat** — the relay confers no privilege the renderer lacks; it is a downgrade from `<all_urls>`, and the trusted call site is the only place a verb is chosen |
| Compromised renderer LIES about a relayed reply | **Residual** — the reply is attributed to the requested URL and vouched for by nobody; cross-origin cookies are withheld (`credentials:"same-origin"`) and CORS still applies, but the body is a claim |
| Page fakes an exploit by calling `apiclientsink` with the marker it read out of the payload | **Residual, unmitigated** — a REAL EXPLOIT verdict is fabricable; the origin binding on `PROBE_HIT` is unbuilt |
| Compromised renderer reaches `chrome.tabs.*` / MAIN-world `scripting` through the SW RPC | **Mitigated** — the SW serves `sender.origin === chrome-extension://<id>` only, and pins `__rpc` to `sender.url === getURL("ast-worker.html")` by equality (a prefix would hand every future page whose name merely begins with it the largest privilege the extension has) |
| A page sandboxes its own iframe (opaque origin) to read the embedder's credentialed API via the shared analysis | **Mitigated** — credentialed-read principal is the *requesting frame's* `MessageSender.origin` (opaque-unique, documentId-keyed), never the top frame or URL-parsed; an opaque origin is same-origin with nothing; a mixed-origin buffer fails closed |
| Sandboxed *extension* page (origin `"null"`) impersonates a trusted extension document | **Mitigated** — document→document hops require `sender.origin === chrome-extension://<id>`; `"null"` ≠ that |
| Bundle escapes the WASM sandbox into the worker | **Out of model** — assumes the WASM boundary holds; a true escape has raw network regardless, so the defense is the browser sandbox + CSP `connect-src` |
