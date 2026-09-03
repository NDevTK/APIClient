# Security Model

The analyzer runs **untrusted web bundles** to learn API surface. So the design assumes the
page, its content script, and the JS engine running the bundle are all hostile, and confines
them. This file is the threat model.

**A CLAIM HERE NAMES A MECHANISM AND THE FILE THAT CARRIES IT, NEVER A LINE NUMBER.** A `file.js:NNN`
citation is unverifiable the moment anyone edits above it, and it fails SILENTLY — it still resolves, to
whatever now sits on that line, so it reads as checked. This file carried 26 of them and 20 had drifted; the
drift was then measured a SECOND time within one session, as concurrent work moved `_browserFacts`,
`_senderOrigin`, `_recordProbeHit` and the router again while this paragraph was being written. Every
citation below is therefore a SYMBOL — `grep` it and the answer is either the mechanism or its absence, and
an absence is a finding rather than a stale pointer.

## Trust zones

| Zone | Trust | Why |
|------|-------|-----|
| Website + its JS bundle | **UNTRUSTED** | arbitrary attacker code |
| `content.js` / `intercept.js` (web renderer) | **UNTRUSTED** | a compromised renderer controls them; runs in every page (`<all_urls>`) |
| QuickJS/WASM engine (runs the bundle) | **UNTRUSTED** | it *executes* the attacker bundle. Confined by an opaque origin, Site Isolation and the WASM sandbox; it reaches the trusted zone only through the declared `content.mojom.Renderer` methods, which `mojo.js` validates in both directions |
| **`renderer.html`** — the document that HOSTS the engine | **UNTRUSTED** | an `<iframe sandbox="allow-scripts">` with no `allow-same-origin`, so its origin is UNIQUE and OPAQUE — cross-origin to the extension origin, which is what lets Site Isolation give it its own renderer process. `manifest.json` `sandbox.pages` lists it |
| **Offscreen document** (`ast-worker.html`) | **TRUSTED** | the only fully-trusted zone — owns state, the network chokepoint, the renderer registry, and the routing between instances |
| Service worker (`background.js`) | **STATELESS** | extension-page-only; never relays page data |

**THERE IS NO "ANALYSIS WORKER", AND THE ROW THAT SAID SO NAMED A FILE THAT DOES NOT EXIST.** This table
carried a row for `ast-thread.js` — "hosts the WASM", trusted code with hostile inputs — and `git grep
ast-thread` returns no such file. A threat model that names the wrong host is worse than none, because every
rule below inherits the wrong boundary: that row asserted the WASM's host was TRUSTED, which is the exact
claim `renderer-host.js` and `bridge.js` were rewritten to stop being true. `bridge.js` says so in its own
first paragraph — **"NO ENGINE LIVES IN THIS REALM"**. The offscreen used to `import("./lib/qjs/qjs.mjs")`
and build every instance with `createQJS()` in its own realm, which made "confined to the WASM sandbox" a
CONVENTION rather than a boundary: the trusted zone held the `Module` handle, `M.HEAPU8` was an exported view
of an untrusted instance's whole linear memory, and two instances in one realm were a NAMESPACE. That path is
DELETED, not kept beside the new one. `renderer-host.js` (Chromium's `RenderFrameHost`) materializes each
instance as a sandboxed frame and is the only thing that speaks to it; `render-process-host.js` (Chromium's
`RenderProcessHost`) decides that a renderer may exist at all and refuses a SECOND one for a cluster that
already has one, with a **`CHECK`** — fatal in dev AND release, because a violation means two heaps behind
one principal. **The boundary around the engine is now the browser's, not an import list somebody remembered
not to write.**

### What the one trusted zone actually holds — and why that is a named cost

The offscreen is not "the network chokepoint plus some state" any more. It is the ONLY zone in the extension
that is trusted at all, and everything that needs trust has accumulated in it: the sole
`chrome.runtime.onMessage` router (in `offscreen-brain.js` — the only one in THIS document; the extension's
other three belong to the SW, the content script and the popup, each a different document, which
`git grep -n onMessage.addListener extension/` settles in one command), the browser-fact mint that every
principal in the system is read out of (`_browserFacts`), the privileged-`chrome.*` RPC **client** (`swRpc` —
the SW performs `tabs.*` / `webNavigation.*` / `scripting.*` on its word alone), the network chokepoint
(`lib/safe-fetch.js`), the page-context relay's trusted end (`lib/schema.js`), IndexedDB persistence, the
engine pool and its level-1 WFQ (`bridge.js`), **the renderer registry and its one-per-cluster refusal
(`render-process-host.js`) plus the frames it materializes (`renderer-host.js`)**, the whole popup command
surface (`lib/popup-handlers.js`), the exploit-probe sessions, and live-traffic moat aggregation.

**This is stated, not defended.** Concentration is the correct SHAPE — a second trusted zone means two
places to get the principal right and a channel between them to get wrong — but it means that one router is
the single gate behind which all of it sits, and every rule below is a property
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
  both `facts.url` = the browser's `sender.url` for THAT document (`_browserFacts` sets `url: sender.url`; it
  travels on the `AST_ANALYZE` message as `sourceUrl` and reaches `safeFetch` as `opts.pageUrl` —
  `git grep -n pageUrl extension/bridge.js` is every call site). A sub-frame analyses as ITSELF, never as its
  embedder.
- **`sender.tab.url` is still carried, for a different question**: it is HTML §8.1.3.1's TOP-LEVEL CREATION
  URL, which §8.1.3.5 decides secure-context from, and it travels as `facts.topLevelUrl`
  (the field beside `url` in `_browserFacts`). Authorization keys on it — a message's authority is the tab it
  came from — but
  it is not a fetch principal. Two facts, two names, never one field doing both.
- A content-script-supplied URL (`msg.url`) is *never* the principal — that was an actual hole (a
  `scripts[0].url` fallback) and was removed.

### The principal is MINTED in one place, and the untrusted side sends no field shaped like one

Two mechanisms, and they are the reason "never URL-derived" is structural rather than a convention anyone
has to remember:

- **ONE MINT.** `_browserFacts(sender)` (`offscreen-brain.js`) takes a `MessageSender` **and nothing
  else** and is the only producer of `{documentId, origin, url, topLevelUrl, tabId, frameId}`. A caller
  cannot state one of these without holding the object the browser filled in, so the failure this exists to
  prevent — deriving a principal from an address — has no argument to travel in. Every field is DCHECKed at
  the mint; the record is frozen and branded `stated: _BROWSER_STATED` (`"MessageSender"`), and every consumer
  reads it back through `_statedFacts`, which is a **`CHECK`** — fatal in release too, because continuing with
  a principal this zone did not mint is worse than aborting. `_senderOrigin` is the origin half: a
  valid tuple origin as-is, an opaque one as a per-`documentId` `null:<uuid>` token so two opaque documents
  never compare same-origin, and no `documentId` → an unstorable fresh token → fails closed.
- **NO PRINCIPAL-SHAPED FIELD ON THE WIRE.** An untrusted zone does not get to state an origin or an
  address, so it does not get to SEND a field shaped like one. `content.js` used to put `origin:
  location.origin` and `pageUrl: docUrl` on its page-source message and on `CONTENT_FORM_SUBMIT`; nothing ever
  read them, and that is exactly what made them dangerous — they sat on the wire under the precise names the
  removed hole was spelled in, waiting for a consumer. They are deleted. The rule is the general one: a
  message from an untrusted zone carries MATERIAL (response bytes, the address the page is posting
  TO) and never identity.
- **THE ONE ADDRESS-SHAPED FIELD THAT DOES CROSS IS A SUGGESTION, AND IT IS CHECKED BEFORE IT IS ACTED ON.**
  `CONTENT_SEED` carries `seedUrl` — the address the browser actually navigated to, which
  `PerformanceNavigationTiming`'s `name` states (Navigation Timing Level 2 §5 "Creating a navigation timing
  entry" sets it from the DOCUMENT'S URL at entry creation) and which `history.pushState` cannot forge. It is
  the one fact an ambient observer holds that no solver can derive, and shipping it is what let the
  content-script document FETCH be deleted.
  It is address-SHAPED and it is not a principal, and the distinction is enforced rather than asserted. The
  brain admits a seed **only where its origin is the origin of `_browserFacts.url`** — the browser's own
  address for that document — which is sound because HTML §7.2.5 "The History interface" refuses a rewrite
  unless "a Document document can have its URL rewritten to a URL targetURL", and that algorithm returns false
  "if targetURL and documentURL differ in their scheme, username, password, host, or port components". So
  pushState can move the PATH and never the ORIGIN, which admits every SPA route and no new destination.
  A mismatch is DROPPED closed with a debug line, never asserted — a `DCHECK` on a hostile input hands any web
  renderer an abort of the only trusted zone in the extension. And the seed is never the PRINCIPAL: the
  private-network classification and `window.location` are still `_browserFacts.url`, and `bridge.js` passes
  that as `safeFetch`'s `pageUrl`, so a suggested address can never authorize itself. **Nor can it authorize
  the person's cookies:** the seeded load is credentialed (§Network), and the principal that decides that is
  `_browserFacts`' `origin` — `_senderOrigin`'s browser-minted, opaque-unique answer, passed as
  `safeFetch`'s `pageOrigin` — so the seed decides only WHICH address is loaded, never whose session loads
  it. The two facts are separate arguments to the same call for exactly this reason.
- **AND THE SECOND DOCUMENT-LOAD TRANSPORT IS GONE WITH IT.** `content.js` used to re-fetch the page's own
  document with `credentials:"same-origin"` and ship the bytes. Those bytes were fetched in the PAGE'S realm
  and reached `safeFetch` never, so every guarantee in §Network below — scheme allowlist, origin-relative
  SSRF/PNA on the initial AND post-redirect URL, CORB by expected type, the credentialed destructive-path deny
  list — applied to the engine's own navigations and to nothing arriving that way.
  **THE ARGUMENT THAT USED TO STAND HERE IS DELETED BECAUSE THE CURRENT DESIGN REFUTES IT.** This bullet also
  said the old transport "was a SECOND credentialed GET of a document the person had already loaded, which a
  server may answer differently under cache, nonce, personalised SSR or a rate limit, so it did not even buy
  the observed bytes it appeared to." Every word of that is now equally true of `navigationLoad`, which is a
  second credentialed GET by design — so keeping the sentence would have this file arguing against the thing
  it describes two sections down. The real and only objection to the old transport was the one above it:
  the bytes reached the chokepoint never. A second GET is a cost the popup states in the row it renders for
  a load that failed ("A single-use URL … cannot be fetched twice"), not a security property.
  `bridge.js`'s `navigationLoad` is now the ONE document-load path — a seed's HTML §7.4 navigation and a child
  navigable's §7.4.5 load are the same call — and the only raw `fetch` left in `content.js` is the
  page-context relay's far end.
- **WHAT A PAGE COULD NOT LOAD IS THE LOADER'S REPORT, NOT A MESSAGE TYPE.** `CONTENT_HTML_UNAVAILABLE` — a
  document stating that its own bundle could not be re-fetched — is deleted with the fetch that produced it.
  The `{state:"unavailable", kind, status|detail}` record the popup renders is now written by `bridge.js`
  through `onNavigationOutcome`, from `safeFetch`'s own reply — so the `detail` is the CHOKEPOINT'S refusal
  reason (`blocked-scheme:`, `blocked-private-from-public`, `blocked-corb:`) rather than a page realm's
  exception text, and its fields are DCHECKed rather than validated-and-dropped, because the producer is now
  this trusted document rather than a hostile renderer.
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
  (`fromExtUrl` / `fromExtDocument`, declared together at the head of the router in `offscreen-brain.js`).
  `fromExtUrl` (url prefix) authenticates only the SW's `__evt` hop;
  `fromExtDocument` (`sender.origin === EXTENSION_ORIGIN`) is what gates the trusted popup command surface.
  Collapsing them into one url-prefix test deletes the document→document rule: a **sandboxed** extension
  page's `sender.url` is still `chrome-extension://<id>/…` while its origin is the opaque `"null"`, so a
  prefix hands an opaque-origin document `GET_STATE` / `SEND_REQUEST` / `CLEAR_TAB`. `poc-sandbox.html` and
  `renderer.html` are both sandboxed (`manifest.json` `sandbox.pages` names exactly those two), so this is a
  page that exists. Content types are dropped on the BROADER predicate — dropping more can only ever refuse —
  and the popup gates the reverse direction on `sender.origin` too (`isExtensionPage` in `popup.js`), because
  a boundary checked on one side only is not a boundary.

## State / storage

- Learned data (endpoints, schemas, field values, API keys) lives in the **offscreen brain +
  IndexedDB** (`uasr_store`, origin `chrome-extension://<id>`) — unreachable from content scripts.
- **`chrome.storage.local` is banned, and the ban is ENFORCED BY THE MANIFEST**: a compromised renderer with
  the `storage` permission could read every key (cross-site leakage). IndexedDB in the extension origin has
  no content-script read path. **The `storage` permission is not requested at all** — `manifest.json`
  `permissions` is `offscreen`, `webNavigation`, `scripting`, `tabs`, `unlimitedStorage` — so `chrome.storage`
  is not merely unused, it is **undefined in every context**, and a call would throw rather than leak.
  `unlimitedStorage` is NOT that permission and grants no read API: it lifts the origin's quota only, which is
  what makes the cross-session frontier's growth a preference rather than a wall (CLAUDE.md §OOM / paging).
  An enumeration of four permissions stood here while the manifest listed five.
  **There are zero `chrome.storage.*` CALL SITES** — that is the security claim, and it is the one to check:
  `git grep -n 'chrome\.storage' extension/` returns only comments recording the ban and the removal of the
  old `chrome.storage.session` mirror. (A count of those COMMENTS stood here too and was wrong within a few
  commits; a census of what grep answers belongs in the grep.) The `session` mirror existed only because the
  brain used to live in the evictable SW; the offscreen document's stable lifetime replaced it.
- The service worker holds no learned state, and relays no page data: content scripts message the offscreen
  DIRECTLY (a `chrome.runtime.sendMessage` broadcast reaches every extension context), so the SW cannot
  launder a web renderer's message into a trusted extension-origin one. It forwards exactly one browser
  event the offscreen cannot observe (`__evt TAB_REMOVED`, the `chrome.tabs.onRemoved` listener in
  `background.js`).

## Network — one chokepoint

Every analyzer-driven request goes through **`safeFetch`** (`lib/safe-fetch.js`); credentialed /
page-context requests go through **`pageContextFetch`** (as the actual page, browser-CORS/PNA-gated).
**There is no raw `fetch` on any ANALYZER path** — no path whose URL the analysed bundle influenced. The
qualifier is load-bearing, because the grep is not empty: `renderer-host.js` raw-`fetch`es a FIXED five-name
list (`check.js`, `mojo.js`, `mojom.js`, `lib/qjs/qjs.mjs`, `lib/qjs/qjs.wasm`) off our own extension origin
to assemble a renderer's program, since an opaque-origin frame cannot load them by URL itself; and
`content.js` raw-`fetch`es inside the untrusted renderer, which is the page-context relay's far end and is
covered below. Neither takes an address from a bundle. **THIS PARAGRAPH IS ABOUT `fetch`, AND `fetch` IS NOT
ALL EGRESS** — a socket frame, a port message and a top-level navigation are none of them a request this zone
issues, and the complete enumeration with what places each is *Every way bytes leave*, below. `safeFetch`
guarantees, in one auditable place:

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
  **CREDENTIALED MODE HAS EXACTLY ONE CALLER, AND IT IS THE DOCUMENT LOAD.** This paragraph used to say the
  mode was dormant with no caller; that stopped being true when the custom browser's tabs started sending
  cookies, which is what CLAUDE.md §A REAL NAVIGABLE requires of every one of them ("`safeFetch` supports
  credentialed loads, and a SAME-ORIGIN navigation carries them, exactly as a browser's does"). A
  session-less tab is a third thing that models nothing — not the person's browser and not a clean client —
  so it is served a bundle the person is never served and its findings do not reproduce for them.
  **THE CALLER IS `navigationLoad` (and `frontierRederive` beside it), AND WHAT BOUNDS IT IS ONE PREDICATE:**
  `navigationCarriesSession` in `bridge.js` — the session travels **only** where the address being loaded is
  same-origin with the **browser-stated** principal of the document being loaded (`msg.origin` /
  `e.origin`, `_senderOrigin`-minted, opaque-unique), never with an origin parsed out of an address. Three
  properties make that safe and each is a check rather than a claim: the seed is same-origin **by
  admission** (the brain refuses a `CONTENT_SEED` whose origin is not the origin of `_browserFacts.url`,
  which HTML §7.2.5 "The History interface" makes sound because `pushState` moves the path and never the
  origin); the chokepoint's own credentialed SOP therefore passes trivially and needs no `ACAO`; and the
  destructive-path deny list (`_destructiveToken`) is scoped to exactly the credentialed case and runs
  **pre-request and post-redirect**, so it is on this path in both positions. An **opaque-origin** document
  (a sandboxed frame) is same-origin with nothing, so its tab loads **uncredentialed** — the mode still
  fails closed everywhere a principal is not a real tuple origin.
  **AND ARMING THE DENY LIST ON A NAVIGATION IS OVER-SCOPED, WHICH IS STATED RATHER THAN QUIETLY LOOSENED.**
  A seeded document whose own address carries a listed token (`/settings/delete-account`) is now refused
  `blocked-destructive:<token>` and reported unanalysed. The list exists because forced execution builds
  requests no real client makes, and a seeded navigation is one the person's own browser made seconds ago in
  this profile — the harm condition is *credentialed AND not-observed*, and `safe-fetch.js` sees only the
  first half. **THE REASON IS NO LONGER THAT THE VOCABULARY IS MISSING, AND THIS SENTENCE SAID SO FOR LONGER
  THAN IT WAS TRUE.** It read "nothing in this system yet DECLARES a request's provenance … that vocabulary is
  the next subproblem", and live code cited this passage by name as its authority for leaving a gate unbuilt
  while the vocabulary existed — the stale standing claim CLAUDE.md ranks above a missing mechanism, because a
  missing mechanism is discovered by the next reader and a stale one is BELIEVED. OBSERVED / DERIVED / FORCED
  is composed in the engine at the PARK, out of the two facts that own it — the park's own kind (HTML §4.12.1
  "The script element"'s parser-inserted flag) and whether the parking flow's path had stood on an arm its own
  concrete example contradicts — and it rides the pending line beside the method and the destination, where
  both hosts' doors already refuse a token outside the three. **THE READER EXISTS, AND THIS PARAGRAPH SAID IT
  DID NOT FOR LONGER THAN THAT WAS TRUE** — the same failure it records one sentence up, made a second time
  about the same field. It read "What is missing is the READER: `bridge.js` validates the field and does not
  pass it to `safeFetch`, so the chokepoint that owns every risk decision is the one zone that cannot see it",
  and every `self.safeFetch` call site in `bridge.js` now states a `provenance`: `_provenanceOf` is a fatal
  `CHECK` on it, `_firingRefusal` decides from it, and `safeFetchWiden` is the per-origin authorisation this
  paragraph named as the diff the field would arrive with. So the list is no longer armed over the whole
  credentialed population: `safe-fetch.js` scopes it to `credentialed && provenance !== "observed"`, which
  exempts exactly the caller whose harm argument is false by construction — the ambient seed, whose address
  the person's own browser navigated to seconds ago in this profile. What is unchanged is the DIRECTION:
  over-broad is this list's cheap direction — one unfired request, reported with its token — and loosening a
  gate as a side effect of turning cookies on is its expensive one.
  **THE LEARNED-GET REPLAY IS UNCREDENTIALED, AND THAT IS PROVENANCE AND NOT PLUMBING.** `bridge.js`'s
  `fetched` passes no `pageOrigin` and states `credentialed: false`. A navigation is OBSERVED — the
  person went there — while an address the engine learned may be DERIVED or FORCED, and a credentialed reply
  to a FORCED request is evidence about what a server says to a request no client makes (CLAUDE.md §A
  REQUEST CARRIES THE PROVENANCE OF ITS VALUES). Turning that on is a per-origin configuration that does not
  exist yet; the standing `@security-finding` there names the value to pass (`msg.origin`) and the obvious
  wrong fix (`originOf(msg.sourceUrl)`), which is the exact URL-derivation this principal exists to forbid.
  **THIS BULLET SAID "nothing writes `msg.credentialed`", AND `git grep -n 'credentialed:' extension/bridge.js`
  REFUTED IT.** The seed's `AST_ANALYZE` writes that field from `navigationCarriesSession`, and the
  child-navigable, swap and cold-rehydration messages inherit it — so for the ordinary same-origin seeded page
  it is TRUE, and `fetched` read it. That is one field answering two questions: on the analyze message it
  states whether the DOCUMENT LOAD carried the session (what the frontier record remembers); read at the
  replay it was answering whether the REPLAY should. The looser question paid, silently — with no `pageOrigin`
  beside the flag, safeFetch's credentialed SOP has no real origin to match and no `ACAO` can equal it, so the
  person's cookies were spent and every reply was then refused `blocked-cors-credentialed:` unread. Four sites
  carried the claim (this bullet, `safe-fetch.js`'s `@security-contract` header, and two paragraphs in
  `bridge.js`); all four are corrected, and the flag is a literal so the two questions cannot re-converge.
  **AND EVERY POST-FETCH GATE NOW JUDGES THE POST-REDIRECT URL, WHICH IT DID NOT.** `safe-fetch.js` had two
  gates reading `resp.url` (the private-host re-check, the destructive-path re-check) and two reading the
  *requested* address (CORB's same-origin exemption, the credentialed SOP). Fetch §2.2.5 "Requests" makes a
  request's CURRENT URL "a pointer to the last URL in request's URL list" and §4.1 "Main fetch" gives a
  response the readable "basic" tainting on *that* origin matching the request's — so a **same-origin
  request that 302'd cross-origin** was CORB-exempt as same-origin and passed the credentialed SOP as
  same-origin, handing back another host's cookie-bearing reply. Harmless while the mode had no caller;
  live the moment it did. All four gates read the URL the bytes came from, and
  `blocked-cors-credentialed:<landed origin>` now names its ground like every other refusal in that file.
  **Credentialed reads also happen through the page-context relay** (see below), where the browser enforces
  SOP/CORS rather than us.
- **GET only, enforced by ABSENCE — AND, SINCE ABSENCE CANNOT SPEAK, BY A REFUSAL BESIDE IT.**
  `safe-fetch.js` hardcodes `method:"GET"` and never reads `opts.method` or `opts.body`. Forced execution
  explores many paths; it never replays a state-changing method. A well-designed server does not mutate on
  GET, so even a credentialed GET replay is side-effect-free; POST/PUT/DELETE endpoints are RECORDED by
  forced exec, never issued.
  **THIS BULLET USED TO OFFER A GREP AS THE ENFORCEMENT ("grep: no occurrence"), AND A GREP HOLDS ONLY ONE
  OF THE TWO HALVES.** Absence stops a verb being SENT and says nothing whatever to a call site that
  believes it sent one — so a new caller passing `method`/`body` was not refused, it was IGNORED, and the
  reply to a GET came back attributed to its POST. That is the exact substitution the paragraph below
  records paying for on the XHR path, arriving through the chokepoint's own front door instead. It is now
  structural in both directions: `_refuseUnreadOptions` makes the option set `safeFetch` reads a CLOSED one
  and refuses any field it will not read, so an option cannot be dropped in silence and a request carrying
  a verb cannot be written. It is the general form of the `as` DCHECK, which is deleted rather than kept
  beside it — a rule re-written per option has a hole for every option nobody thought of. It is a `DCHECK`
  because release can still PROCEED (an unread option drops exactly as before, and every one of them lands
  on the safe side: a dropped `method` fires the GET this file was always going to fire), and it may assert
  at all because the KEYS are composed in trusted-zone source at every call site while the untrusted engine
  supplies only VALUES. This is what makes the middle conjunct of CLAUDE.md's never-a-setting triple —
  credentialed AND state-mutating AND forced — false by construction rather than by convention.
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
  "analyzer probe headers only" comment was no longer the whole truth, and both halves of that correction are
  now landed rather than noted. `fetchedXhr` forwards the page's header list. It is within the model (an
  uncredentialed GET to a public host, with forbidden header names stripped by the browser) — a header on a
  request the page can already make for itself confers nothing.
  **AND THE SCOPE IS NOW A CHECK RATHER THAN A SENTENCE ABOUT WHO THE CALLERS ARE.** This bullet said the
  header path "stays within it only because credentialed mode is off", and that premise stopped being true
  the moment the document load started sending cookies — which THIS FILE records under the heading
  "CREDENTIALED MODE HAS EXACTLY ONE CALLER, AND IT IS THE DOCUMENT LOAD". What
  actually kept the combination from arising was that the callers stating a header list and the callers
  asking for cookies were disjoint sets of FUNCTIONS: a fact no site can see, that no diff has to preserve,
  and that CLAUDE.md names as the durable way to get an invariant wrong (an exemption scoped by a fact the
  exempting site cannot see). It matters because RFC 9110 §9.2.1 "Safe Methods" is enforced at the chokepoint
  STRUCTURALLY — the verb is a literal and `_refuseUnreadOptions` makes a caller-stated one unwritable — and
  a header list is the OTHER route to a verb: `X-Http-Method-Override` is a convention this project's own
  code sends (`lib/discovery.js` calls it "the documented trick"), so a bundle-chosen header list on a
  cookie-bearing request is CLAUDE.md's never-a-setting triple rebuilt one layer above the place that literal
  closed it. `_credentialedOf` in `safe-fetch.js` now derives the credential flag at the entry and **CHECKs**
  that a credentialed request states no header list — fatal in release, on the same discriminator
  `_destinationOf` and `_provenanceOf` carry, because the arm a compiled-out assert leaves is the one that
  sends them. It may assert at all for `_refuseUnreadOptions`' reason: the KEYS are trusted-zone literals at
  every call site, so no bundle can reach the abort. What the combination still needs before it can exist is
  a statement of WHOSE header list it is, carried from the site that knows exactly as `provenance` and the
  page-context relay's initiator grade already are; its absence shows as that abort and nowhere else.
- **http(s) only** — `file:`/`data:`/`blob:`/`chrome-extension:` are rejected.
- **origin-relative SSRF (Private Network Access):** a *private* target (loopback / link-local /
  RFC1918) is blocked **unless the page principal is itself private** — a public page cannot use the
  extension's host permissions to reach the user's localhost/intranet; a localhost page *may* reach
  localhost. Checked on the **initial URL and the post-redirect final URL**.
- **CORB by the request's DESTINATION, asked in Fetch §2.2.5 "Requests"' own words:** "A request's
  destination is script-like if it is `audioworklet`, `paintworklet`, `script`, `serviceworker`,
  `sharedworker`, or `worker`." That predicate (`_isScriptLike`) IS the CORB question, because script-like is
  exactly "these bytes will RUN as code" — so a cross-origin script-like body must be JS-typed and a
  cross-origin HTML/JSON body is never read as code, while every other destination is data and is exempt.
  `xslt` is deliberately outside the predicate, and the spec's own note is the reason rather than an
  oversight: it says algorithms using script-like "should also consider `xslt`", and considering it HERE
  yields exclusion, since this rule demands a JavaScript MIME and an XSLT stylesheet is XML. The day the
  engine loads one it needs its own rule, not this one.
  **THIS BULLET DESCRIBED A KEYWORD THAT HAS BEEN DELETED, AND THE KEYWORD WAS THE HOLE.** It said the
  enforcement was "an EQUALITY on one value — `opts.as === "script"`", that `"script"` was the only token any
  caller passes, and it offered a grep for `as:` as the check. `opts.as` is gone; that grep now answers only
  the comments recording its removal; and `safeFetch` DCHECKs `!("as" in opts)` at its entry, so a caller
  still passing one ABORTS rather than being silently classified as data. The keyword's defect was that a
  caller had to KNOW a load was code and remember to say so, which is how a document's own `<script src>`
  reached the rule unclassified and took the exempt arm by SILENCE. A destination is a property of every
  request and the engine states one at every park, so there is no longer a caller that can forget — and
  `_destinationOf` refuses anything outside §2.2.5's enumeration rather than defaulting it, because a caller
  who never thought about it must never read as one who answered "data". Centralised so a new code-loader
  can't forget it.
  **AND THAT REFUSAL IS A `CHECK`, WHICH IS THE HALF THIS BULLET GOT WRONG WHEN IT CLOSED THE KEYWORD'S HOLE.**
  It was a DCHECK on presence alone, and both of those are the fail-open direction. `_isScriptLike` answers
  false for every value it does not recognise, so an ABSENT destination and an INVENTED one take one arm and it
  is the exempt one — which means the dev-only assert was the entire enforcement and release had the keyword's
  hole back under a new spelling. The discriminator is the one stated for `opts.pageUrl` in the bullet below:
  a DCHECK is right where release can still PROCEED correctly with the check compiled out, and this field fails
  that test in the opposite direction. The value also crosses from the UNTRUSTED engine, whose own join and
  splitter assert §2.2.5's enumeration with DCHECKs of their own — so "the producer will not emit a bad token"
  was never a check this zone held. `bridge.js` CHECKs that line's INITIATOR and PROVENANCE, the two fields no
  ingestion is decided from; the field it IS decided from is asserted at the chokepoint instead of in either
  host's splitter, which is what makes it one check covering the extension and `engine/trusted.mjs` both.
- **per-call principal** (`opts.pageUrl` = the requesting DOCUMENT's own `sender.url`, never the tab's) —
  **not a shared global**: the trusted zone drives many renderers concurrently and interleaves their rounds,
  so a global principal would let one page's origin contaminate another's fetch.
  **AND "UNKNOWN PRINCIPAL → PUBLIC" WAS ONE SENTENCE OVER THREE DIFFERENT STATES, which is why the
  classification is unchanged and the sentence is not.** A principal that is ABSENT, one that will not PARSE,
  and one that parses and names NO HOST (`about:blank`, `data:`, `blob:` — reachable because
  `manifest.json` sets `match_about_blank` and `match_origin_as_fallback`) all reached `_isPrivateHost` as
  the same empty string and all classified public. Only the third is an input: a principal that names no
  server is not a private-network principal, and `safe-fetch.js` now reads that as the positive statement it
  is. The other two are **this zone's contract broken** — `opts.pageUrl` is never on the wire (§The principal
  is MINTED in one place), so it is always `_browserFacts`' `url`, already DCHECKed non-empty at the mint,
  and all four `safeFetch` call sites in `bridge.js` take it from there — so they are **DCHECKed** at the
  chokepoint. DCHECK and not CHECK because release must still be able to PROCEED, and it can: with the
  checks compiled out an unparseable principal still classifies as public and private targets are still
  blocked, so **the release behaviour named by the old sentence is exactly what it was**. This is not the
  hostile-input abort this file refuses elsewhere — that is `CONTENT_SEED`'s page-suggested address, which
  is dropped closed and is a different value.
- **one same-origin answer, read by both post-fetch gates.** "Is the landed resource same-origin with the
  page principal" is computed once beside the response's own origin and read by CORB's same-origin exemption
  and by the credentialed SOP. It used to be asked twice — CORB through a helper that re-parsed the landed
  address under a `catch` of its own, the credentialed gate inline — which is a second derivation of one
  security question and the same shape as the four private re-derivations of the landed URL that the
  post-redirect fix removed. The helper is deleted rather than commented, so a body CORB exempts as
  same-origin that the credentialed gate refuses as cross-origin is no longer a state anyone can write.

A cross-origin script uses the **page's** origin, never the asset's own host (gstatic.com
JS in google.com acts as google.com) — exactly what the principal encodes.

### Every way bytes leave — and the three questions that place one

**THE CHOKEPOINT RULE ABOVE IS ABOUT ANALYZER-DRIVEN HTTP, AND READ AS A STATEMENT ABOUT ALL EGRESS IT IS
WRONG.** This extension has channels that are not `fetch` at all — a frame on a socket, a message on a port,
a top-level navigation — and a reader auditing "every egress" against the paragraph above does not find them,
concludes the list is complete, and is wrong in the one direction a threat model must not be wrong in. So the
enumeration is stated here, and it is stated as CATEGORIES rather than call sites, because a census of call
sites rots on the next commit while a category does not.

**An egress is any path by which bytes this extension composed reach a peer it did not author**, and three
questions place any one of them: **WHO CHOSE THE DESTINATION**, **WHOSE CREDENTIALS PAY FOR IT**, and **WHO
COMPOSED THE BYTES**. A path answering "the analysed bundle" to the first is what the chokepoint exists for. A
path answering "the page" to the first two confers no privilege and needs no gate — it is a DOWNGRADE, and
the check that would stand there could only refuse something the page can already do for itself. A path
answering "the operator" to the third is authorized by a human at a surface that shows them the bytes, and
what it owes is that the destination be one this zone can name exactly.

- **Analyzer-driven HTTP — `safeFetch`, the chokepoint.** The document load (`navigationLoad`, and
  `frontierRederive` beside it), the learned-GET replay (`fetched`), and the page's own parked `fetch`/XHR
  (`fetchedXhr`). Destination chosen from what the bundle computed; credentials omitted except on the
  document load, where `navigationCarriesSession` bounds them. Every guarantee above is this row's.
- **Requests issued AS THE PAGE — the page-context relay.** `pageContextGet` / `pageContextSend` /
  `pageContextFetch` reach `content.js`'s `handlePageFetch`, whose raw `fetch` runs in the untrusted renderer.
  Destination is this zone's; credentials are the page's own (`credentials:"same-origin"`); the enforcement is
  the BROWSER's SOP/CORS rather than ours, and the credentialed destructive-path deny list DELIBERATELY does
  not reach here — it is a floor under requests this tool composes on its own, and this relay carries an
  operator's, which is a fact the relay REQUIRES ITS CALLER TO STATE rather than one that happens to be true of
  the call graph. Both directions are the subsection below, because they fail
  differently and only one of them is a privilege question.
- **Renderer program assembly — `renderer-host.js`.** A raw `fetch` of a FIXED name list off our own extension
  origin, because an opaque-origin frame cannot load those files by URL itself. The destination is a constant
  in this zone and no bundle influences it; adding a bundle-influenced name to that list would make it an
  analyzer path, and it would then belong in the row above.
- **The page's own traffic through a wrapper — `intercept.js`.** The `fetch`, `XMLHttpRequest`, `WebSocket`
  and `EventSource` wrappers CALL the originals they replaced. No request exists here that the page did not
  make: the wrapper is an observer and the egress is the page's. It runs in the MAIN world, so it holds no
  privilege the page lacks and can confer none.
- **Operator-composed writes into a channel the PAGE opened — the socket, port and window send relays.** A
  frame on a `WebSocket` the page's own realm opened, a message on a `MessagePort` the page entangled, a
  `postMessage` into a window the page holds. **This is a downgrade for a sharper reason than the fetch relay
  is**: the extension does not choose the destination AT ALL. The connection, the port and the window were
  opened by the page before this extension said anything, they are held in the page's own MAIN world, and
  what the trusted zone supplies is a channel id and a payload the OPERATOR typed into the Send panel. The one
  fact this zone does state is the window relay's `targetOrigin`, and it is asserted to be a single real
  origin — never empty and never `"*"`, which would deliver an operator-typed payload to whichever document
  holds that window by the time it lands.
  Authorization is the ordinary document→document rule and nothing bespoke: the offscreen router admits the
  command only on `sender.origin === chrome-extension://<id>` (so a sandboxed extension page, whose origin is
  the opaque `"null"`, gets none of it and in any case holds no `chrome.*` to try with), the SW pins its
  `__rpc` to the offscreen document by URL EQUALITY, and the `tabs.sendMessage` that carries it is routed
  `documentId`-only — no `frameId` fallback, no target-less broadcast, no `documentId` → refuse. The far end
  in `content.js` does not re-check its own sender, and that is sound by the PLATFORM's delivery rule rather
  than by oversight: a content script's `onMessage` is reachable only by `chrome.tabs.sendMessage` from a
  privileged extension context — a `chrome.runtime.sendMessage` broadcast is not delivered to content scripts
  — and `manifest.json` declares no `externally_connectable`, so no other extension and no web page has a
  door. A compromised renderer needs none of this: it already holds the socket.
- **The live-verify NAVIGATION — the one egress that is not a request this zone issues.** The offscreen builds
  the PoC's JavaScript as a `window.open` of the delivery address; the popup embeds `poc-sandbox.html`, an
  opaque-origin sandboxed page, and hands it that string; the OPERATOR'S CLICK is the user activation that
  lets it run. What results is a real top-level credentialed GET in the person's own profile, and it reaches
  `safeFetch` never — there is no interception layer and none is wanted, because the whole value of the
  signal is that CHROME answers rather than we do (§The live PoC verify).
  **It is inside the model because of what the address IS, and that is a property of how it is built rather
  than a promise.** `buildLiveDelivery` takes the page the sink was OBSERVED on, re-parses it through the URL
  parser, clears its fragment and query, and appends the payload at the component the ENGINE declared — so
  scheme, host and path are the ones the person's own browser already loaded, and the analyzer supplies only
  a fragment or a query string. The harm condition the credentialed destructive-path deny list exists for is
  *credentialed AND not-observed*, and the second half is false here by construction; that is why the list's
  absence on this path is a statement of its scope and not a gap in it. An address that does not parse yields
  no delivery at all, because a page with no parseable address has no origin a hit could be attributed to
  either.

**AND WHAT GREPS LIKE AN EGRESS AND IS NOT ONE**, because an enumeration is only worth having if it also
refuses: `renderer.html`'s `import()` of a `blob:` URL assembled from bytes `renderer-host.js` had already
fetched; every `indexedDB.open`; the popup's request formatters, which build a `fetch(…)` STRING for a human
to copy; the page's `submit` listener in `content.js`, which OBSERVES a submission the page performed, while
the driven `form.submit()` runs inside the ENGINE and leaves through the same host edges as the bundle's own
code; and the `scripting.exec` / `tabs.create` / `tabs.remove` / `tabs.get` RPC arms, which have no callers at
all (§Known residuals). None of these composes bytes for a peer.

### The page-context relay — the trusted zone borrows a privilege it does not have

`pageContextFetch` (`lib/schema.js`) issues the request AS THE PAGE: the offscreen sends `PAGE_FETCH`
over `swRpc("tabs.sendMessage", …, {documentId})` and `content.js`'s `handlePageFetch` runs
`fetch(url, {credentials:"same-origin", …})` in the renderer. This is the only credentialed read that
actually happens, and it is the one edge where the trusted zone instructs an UNTRUSTED one. Both directions
have to be stated, because they fail differently and only one of them is a privilege question.

- **Trusted → untrusted (the request): no privilege is conferred, so nothing needs to stop it.** The content
  script can already fetch anything its own page can fetch with its own cookies; the relay asks for a subset
  of what that renderer holds. It is a DOWNGRADE from the extension's `<all_urls>` reach, not a grant. This
  is why "what stops a compromised content script asking for a fetch the offscreen never authorised" has no
  check behind it and needs none — a compromised renderer does not need our relay to make that request.
  **AND THE CREDENTIALED DESTRUCTIVE-PATH DENY LIST DOES NOT APPLY ON THIS EDGE, WHICH IS A RULE ABOUT WHO
  ACTED RATHER THAN ABOUT WHICH TRANSPORT CARRIED IT.** A gate was built at `pageContextFetch` and removed.
  The facts it rested on are all TRUE and are worth keeping so they are not re-derived into the same wrong
  conclusion: `safe-fetch.js` scopes the list to `credentialed && provenance !== "observed"`, so a request
  that never reaches that file cannot be inside the scope; every request this relay sends IS credentialed by
  construction (`handlePageFetch` sets `credentials: "same-origin"` with no parameter in which a caller could
  say otherwise); and two of its three entries name POST, which RFC 9110 §9.2.1 "Safe Methods"' safe set does
  not contain — "Of the request methods defined by this specification, the GET, HEAD, OPTIONS, and TRACE
  methods are defined to be safe." Every one of those is about the REQUEST, and none of them about who
  decided to send it. That is the whole of the error.
  The list is sound because it is ALLOWED TO BE WRONG: a wrong deny costs one unfired request that forced
  execution still derives and reports in full. That arithmetic inverts the moment a human composed the
  request. A token list standing there is not a
  floor under our autonomy, it is the tool VETOING ITS OPERATOR on a substring match, and the cost is a person
  told their own explicit act was blocked by a pattern they never saw. The egress taxonomy above already draws
  this line: a path answering the operator is authorized by a human at a surface that shows them the bytes.
  That is the strongest grade in this document; a token list is the weakest, and the weak one does not
  overrule the strong one. The list stays scoped inside `safeFetch`, for `safeFetch`'s own autonomous
  requests, and a reader who finds this transport ungated has found a decision rather than a gap.
  **AND THE EXEMPTION IS SCOPED BY A CARRIED FACT, BECAUSE THE SENTENCE IT USED TO BE SCOPED BY WAS FALSE.**
  This paragraph said the relay's entries are "operator-typed — the Send panel … — or an operator-initiated
  probe", and that was a claim about WHICH FUNCTIONS HAPPENED TO CALL WHICH, made in a file where nobody could
  check it. Three callers reached this transport with no human anywhere: `lib/response-decode.js`'s automatic
  discovery sweep, its automatic req2proto error probe, and its automatic service-info probe — all three fired
  from `handleResponseBody` the instant a captured response body arrived, all three credentialed by
  construction, two of them POSTing a body the app never produced. The exemption covered every one of them,
  because the relay could see the request and never the act.
  The grade is therefore a VALUE now, stated at the door the act enters by and carried down every frame to the
  relay, which asserts it (`pageContextRequireUserInitiated`, `lib/schema.js`). It cannot be inferred here and
  no attempt is made to: a URL, a verb, a header map and a body look identical whether an operator typed them
  or a response handler composed them, so a relay that guessed would answer the question its own exemption
  rests on with a heuristic. An unstated grade takes the same arm as `tool-initiated`, which is the arm that
  aborts, so forgetting to state one is not a way to be exempted.
  The automatic callers were MOVED OR REMOVED rather than exempted. The discovery sweep now routes its
  candidate GETs through `safeFetch` when its grade is `tool-initiated` — uncredentialed, so it sees what a
  logged-out client sees, which is a real reduction stated rather than worked around. The two POST probes have
  no automatic form at all: `safeFetch` hardcodes `method:"GET"` and reads neither `opts.method` nor
  `opts.body`, which is how RFC 9110 §9.2.1 "Safe Methods" is enforced structurally, so there is no chokepoint
  that could carry them and teaching it one would delete that argument from the file whose safety rests on it.
  Both survive at the grade entitled to them — the Discovery panel's per-endpoint **probe** and **service
  info** buttons, and the Send panel — and what does not survive is the automatic service-info probe's reach
  over captured POSTs that mint no endpoint record (Google `batchexecute` / `$rpc`), which have no button
  because they have no record to hang one on.
  What the relay does need, and has, is that the far end holds NO policy: `handlePageFetch` takes
  `msg.method` verbatim, so the VERB is decided entirely at the trusted call site. That is why the three
  entries are named for their OPERATION rather than for a method (`pageContextGet` GET-only for learning a
  published document, `pageContextSend` any method because the human chose it in the Send panel,
  `pageContextFetch` POST for `req2proto`'s malformed-body error probe) and each states its verb at the call
  site rather than inheriting one — and, for the same reason, its INITIATOR GRADE, since the far end holds no
  more policy about who acted than it does about which verb to use. Routing is `documentId`-only — no `frameId` fallback (reused across
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

- It **cannot** read or set the principal — the principal lives in the TRUSTED OFFSCREEN, which is a
  different DOCUMENT from the one the engine runs in rather than merely a different scope, and `bridge.js`
  passes it to `safeFetch` as `opts.pageUrl` per call — and it **cannot** do raw network: its own `fetch()`
  calls are *recorded*, not issued, and its `import()` targets become **untrusted URLs** that `safeFetch`
  gates (origin-relative SSRF + CORB). It cannot reach `chrome.*` either, and that is the browser's answer
  rather than ours: an opaque origin is not the extension origin, so `chrome.runtime` is not exposed to it.
- **One WASM instance per ORIGIN-KEYED AGENT CLUSTER** — `(browsing-context group, origin)`, isolated memory
  — never per tab and never per page, because a tab holds documents
  that may be cross-origin to each other and an iframe or a popup gets its own instance the moment its
  origin differs. The instance IS the principal: a fetch is governed by a single, correct origin
  because there is exactly one origin in the instance that issued it, and that stays exactly true when
  same-origin documents share one — origin, not document, is what the invariant is keyed on. Reusing
  an instance across ORIGINS would put two principals behind one `pageOrigin`; splitting one across
  same-origin documents would instead break HTML's own single-heap agent, which same-origin DOM
  adoption and cross-frame closures rely on.
  **BOTH HALVES OF THE KEY ARE BROWSER-STATED** (`clusterKeyOf` in `bridge.js`, over the fields the
  `AST_ANALYZE` message carries off `_browserFacts`' reading of the browser's `MessageSender` — an
  `analyze.js` named here previously has never been a file in this tree), because the untrusted engine may
  state neither: the ORIGIN
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
- **THERE IS NO SOURCE-MAP PIPELINE, so it is not a boundary this file has anything to say about.** This
  section used to state that source maps were "fetched/parsed in worker/offscreen JS (`as:"sourcemap"`), used
  only to label findings shown in the UI"; the trust table called the engine's source-map pragmas an
  attacker-shaped input; §Known residuals named a `_smGetParsed` worker-zone fallback; and §Finding quality
  described a "beautified (source-mapped)" taint-path view. **Four sites, one subsystem, and it is gone** —
  `git grep -n 'sourceMappingURL\|_smGetParsed\|as: *"sourcemap"' extension/` finds nothing that fetches,
  parses or renders a map, and `bridge.js`, `lib/popup-form.js`, `lib/popup-send.js` and `lib/send.js` each
  record in their own comments that the `_sourceMapName` / `sourceMapsByUrl` fields were readers with no
  producer and were deleted. Nothing is asserted in its place: a threat model does not keep a boundary warm
  for a component that no longer exists, because the residual then reads as a live hazard and sends the next
  reader hunting code to harden.

## The live PoC verify is ONE-SIDED — a hit is a page's CLAIM, not a browser's answer

`intercept.js` runs in the **MAIN world** (`manifest.json` `content_scripts`), i.e. inside the page, i.e.
fully attacker-controlled. It installs `window.apiclientsink(id)` and relays each call to `content.js`
(CustomEvent + a `data-uasr-hits` attribute mirror), which forwards it as `PROBE_HIT`; the offscreen
correlates it to a probe session by the id and ATTRIBUTES it (`_recordProbeHit` in `offscreen-brain.js`), and
the popup reports **REAL EXPLOIT** / **NOT REPRODUCED** (`lib/popup-security.js`).
Everything on that path before the offscreen is the page. So:

- **The marker is not a secret from the target.** It is a fresh `crypto.randomUUID` minted by
  `startExploitProbe`, correctly kept out of the URL as a *token*, but it rides INSIDE the delivered
  payload as `apiclientsink('<uuid>')` — and the payload is delivered TO the page, in its own fragment or
  query string (`out.targetUrl = base.href + deliveryPrefix + payload`). Any script in that document can read
  `location.hash`, lift the uuid and call the hook without any sink having fired.
- **Therefore a SAME-DOCUMENT positive verdict is FABRICABLE and a NEGATIVE is SUPPRESSIBLE.** The false
  positive is the one that matters: a hostile page can make the tool assert a real, Chrome-confirmed exploit
  against itself. Suppression is unavoidable in any design (the page owns the renderer) and is already
  reported honestly — "NOT REPRODUCED … Not a statement that the sink is safe" (the verdict strings at the
  end of `lib/popup-security.js`).
- **This does not weaken the rule; it bounds the claim.** A hit means *the payload's code ran in the page* —
  which is what a fired sink produces and is why the signal is worth having. It does not mean the SINK
  produced it. "Engine agreement" is agreement with a witness the page can coach.
- **THE ORIGIN BINDING IS BUILT — this file ASKED for it, got it, and then kept asking.** A previous version
  of this section carried a "MISSING CHECK, and it is buildable now" bullet, a matching entry in §Known
  residuals, and a matching `Residual, unmitigated` row in the table below. The check was built; none of the
  three sites were updated. A threat model that keeps requesting a check it already has is the stale-`DFAIL`
  failure mode — it reads as authoritative and sends the next reader to build what is already there.
  `_recordProbeHit` (`offscreen-brain.js`) REFUSES a hit whose reporting document is not the delivered one,
  on three comparisons in order: the browser's `MessageSender.origin` against the origin of the address THIS
  zone built and handed to `window.open` (`ses.expect.origin`); `frameId` against the top-level `0`; and a
  latched `deliveredDocumentId`, so a SECOND document of the target origin cannot fire a marker the first
  already fired. **THE DIRECTION IS THE RULE:** a browser-stated FACT is compared against OUR expectation,
  and an expectation is never promoted to a fact. A session that delivered nothing attributes nothing, and an
  opaque target can never attribute a hit, because an opaque origin is same-origin with nothing.
  A mismatch is RECORDED, never dropped (§@S: absence of a PoC is never a "safe" verdict) — it lands on the
  same `hits` array carrying `attributed: false` and its reason, and `lib/popup-security.js` DCHECKs that
  every hit reaching the panel states an `attributed` boolean, rendering the refused ones separately. The
  record also splits `browserStated` from `pageClaimed`, so the untrusted half is never spliced onto the
  trusted one under unmarked names.
  **WHAT REMAINS IS IRREDUCIBLE BY THIS MECHANISM:** `window.apiclientsink` is installed in the delivered
  page's own main world, so any script in THAT document can still call it, and every fact the hook could
  report about its own caller is reported by that same world. Cross-document fabrication is CLOSED;
  same-document fabrication is the honest residual, and it is now a measured one.

## Per-boundary contracts — there is ONE, and the scheme described here was never built

**`@security-contract` is on exactly one site in the tree, and `enforced-by` is on none.** This section
opened "Each loader / trust boundary carries an inline `@security-contract` block", and the file's own first
paragraph told the reader to `grep @security-contract` for the per-function contracts. Two commands settle
it: `git grep -n '@security-contract'` returns `extension/lib/safe-fetch.js` and this file, and
`git grep -n 'enforced-by'` returns this file ALONE. The `enforced-by` pointer was described here as "the
honesty mechanism — a label that can't cite a real check is marked as a residual, not asserted"; a vocabulary
asserted only by the document describing it is that very failure, performed on itself.

**WHAT IS REAL is the one enforcement point and the four standing findings.** `lib/safe-fetch.js` carries
`@security-contract  ENFORCEMENT POINT (the single network chokepoint)`. The live per-site security prose is
spelled `@security-finding` — four of them, and each names a HAZARD rather than certifying a boundary:
`background.js` (nothing calls `scripting.exec`), `bridge.js` twice (no `pageOrigin` reaches the chokepoint
**from the learned-GET replay path** — the document-load path now passes one; `headers` comes from the
untrusted bundle), and `offscreen-brain.js` (against the URL-derived-principal "fix").
`git grep -n '@security-finding' extension/` is the whole list. **A boundary is documented by the
DCHECK/CHECK standing at it and by this file, not by a tag vocabulary with one member.** Reviving a
per-boundary tag scheme means tagging every boundary in the same diff; a scheme with one instance and a
paragraph claiming universality is worse than none, because it invites the reader to trust an audit that was
never run.

## Known residuals (not yet airtight)

- **Redirect-to-private:** `safeFetch` re-validates the final URL after redirects (so internal data is
  never *ingested*), but preventing the redirected request from *reaching* a private host relies on the
  browser's Private Network Access for extension fetches.
- **Fabricable PoC-fire hit, NARROWED to the delivered document** — the section above. The origin binding on
  `PROBE_HIT` **is built** (`_recordProbeHit`), so a hit from another origin, another frame, or a second
  document of the target origin is refused and recorded as `attributed: false`. What survives is that
  `window.apiclientsink` lives in the delivered page's own main world: a script in THAT document can still
  assert a REAL EXPLOIT verdict for a sink that never fired. Irreducible by this mechanism.
- **Relayed-reply attribution** — a `PAGE_FETCH` reply is attributed to the URL the offscreen asked for and
  is vouched for by nothing but the renderer that returned it.
- **A CROSS-ORIGIN child navigable loads UNCREDENTIALED, where a real browser would send cookies** — the one
  place the custom browser's tabs are *not* the person's browser, and it is a fidelity gap rather than a
  hazard. `navigationCarriesSession` refuses the session there because `safeFetch`'s credentialed gate asks
  whether the REQUESTING principal may read the bytes, while a navigation's reader is a DIFFERENT instance
  keyed on the RESPONSE's own origin — the bytes never enter the initiator's heap. Asking with cookies today
  would spend the session at that host for a reply the chokepoint must then refuse, which is strictly worse
  than asking without. What it needs is a document load TYPE in `safe-fetch.js` whose read principal is the
  response's own origin; it must not be an `if` at the caller, which would put a network policy where this
  file gives one only to the chokepoint.
- **Page-claimed origins in the request log** — `sourceOrigin` / `targetOrigin` on a `PM_RECV` / `MC_OPEN`
  record (the two sites in `content.js` that build them) are read off a real `MessageEvent` in the isolated
  world, so they are
  browser-stated *in a renderer we do not trust*. They do NOT reach the engine (no `sourceOrigin` consumer in
  `bridge.js`), so the "the engine never gets a forgeable `event.origin`" rule is intact. The UI half is
  CLOSED: both renderers print them as `page-claimed: A → B` with the reason on the element
  (`lib/popup-reqlog.js` `_claimedOriginPair` / `PAGE_CLAIMED_ORIGINS_TITLE`, used by `lib/popup-console.js`),
  and an empty half prints as `(none stated)` rather than `?`. The send panel no longer *acts* on one it
  cannot address either: a PM reply whose page-claimed source origin is empty or `"null"` is REFUSED instead
  of being sent with `targetOrigin: "*"`, which would have delivered an operator-typed payload to whatever
  document held that window. What remains is that the store still files these records under a claimed origin
  — a research artifact, like every relayed reply.
- **`scripting` + MAIN-world injection is an unused privilege held open — FOUR RPC arms with ZERO callers.**
  `_PROBE_INJECTORS` and the `scripting.exec` arm of `_swRpc` (`background.js`) grant arbitrary MAIN-world
  execution in any tab (`world:"MAIN"`, `target.allFrames`) — full same-origin control of every site the user
  has open. The count is measured rather than estimated: of the six arms `_swRpc` serves, `tabs.sendMessage`
  and `tabs.query` have callers, and **`scripting.exec`, `tabs.create`, `tabs.remove` and `tabs.get` have
  none** — `git grep -n '"scripting.exec"\|"tabs.create"\|"tabs.remove"\|"tabs.get"' extension/` hits
  `background.js` and nothing else. (`tabs.remove` briefly acquired one caller and lost it again when the
  tab-ownership component was reverted; the arm is unused once more.) `background.js` carries the standing
  `@security-finding` beside them. The gate is sound and is an *equality*, not a prefix — `sender.url ===
  OFFSCREEN_HREF`, where `OFFSCREEN_HREF` is `chrome.runtime.getURL("ast-worker.html")` — so it is
  unreachable today. It is the blast radius of any future hole in that one gate, and the `scripting` permission exists
  only for it. Either the probe orchestration lands and uses it, or the arm, the injectors, the three unused
  `tabs.*` arms and the permission are DELETED together.

## Finding quality — gated by CONSTRAINTS + sink sensitivity, never reachability

"External input reaches a sink" is NOT a finding by itself — the value is opaque for control-flow
*because* it is attacker-influenced, so the question is what the path CONSTRAINTS allow and how
sensitive the sink is. That constraint set is what the flow's own RE-EXECUTION narrowed the value to: the
sanitizer ops surfaced in the finding UI (`encodeURIComponent()`, `replace(/[<>]/g,'')`) **are** those
constraints, not a separate analysis.

**THERE IS NO Z3, AND THIS SECTION CITED CLAUDE.md FOR IT WHILE CLAUDE.md FORBIDS IT.** Two sentences here
attributed the constraint set and the PoC construction to "Z3's solve", one of them in a parenthesis reading
"(CLAUDE.md: BUILT from the solve, never templated)". CLAUDE.md §Solver half says the opposite in bold —
"**Re-execution discharges the constraints — NO external Z3, NO taint tracker, NO recorded
transform-expression, NO chain-inversion**" — and README.md says it again. The tree agrees:
`git grep -in '\bz3\b' engine/host engine/qjs` finds nothing but a punycode string in a public-suffix table.
A citation to an authority that says the reverse is worse than no citation, because a reader who checks the
authority concludes the document is right and the code is behind. **The mechanism is re-execution:** the
engine runs the real filter on real operands and observes which bytes survive, which is what lets it see a
config-loaded allowlist that no transform-expression or SMT encoding can.

- **Taint → `fetch`/request param:** report only when the constraints let the input change a
  SECURITY-SENSITIVE parameter. `location.search` deciding the allow-list for a `DELETE /account`
  call is a finding; `document.referrer` / `document.title` flowing into an analytics/logging POST is
  **WAI**, not a finding. The discriminator is the sink + the constraints, never the data-flow edge.
- **Open redirect is NOT a finding on its own** (Google VRP stance) unless it also leaks a
  credential / access token — and a navigation is reachable via `w = open(); w.location = …` anyway,
  so the redirect primitive alone is weak. Flag it only bundled with a token/secret exfil.
- **XSS verification = build + RUN the PoC for real**, never asserted from the taint path alone. The
  PoC is DERIVED from the run — the sink's real parse context, the per-flow byte provenance through the real
  filter, and the path's value domain, solved jointly by re-executing candidates (CLAUDE.md §@S: CONSTRUCTED
  from the running code, never templated) — and then executed; the verifiable OUTCOME (sink fired with the
  attacker value) is the triage signal, and it proves exploitability even though it does not localize the
  bug. The UI's taint-path + constraint view is the human-readable explanation of that same run. It is not
  source-mapped: there is no source-map pipeline (§The QuickJS/WASM sandbox), and the "beautified
  (source-mapped)" view named here previously describes a component that no longer exists.

## Attack scenarios

| Scenario | Outcome |
|----------|---------|
| Compromised renderer reads stored cross-site data | **Mitigated** — state is IndexedDB in the extension origin, not `chrome.storage.local`; no content-script read path |
| Renderer sends a data-returning message type (`GET_STATE`, …) | **Mitigated** — `sender.url` gate routes content scripts to the data-input-only handler; data types dropped |
| Renderer forges `sender.id` | **Not a threat** — already has it; `sender.url` is the real gate |
| Renderer forges `sender.url`/principal as a localhost origin to defeat SSRF | **Mitigated** — the principal is the browser-set `MessageSender`, never `msg.*`. (This row used to say the principal is `sender.tab.url`, which contradicts the corrected rule two sections above and restates the very bug that rule records: the SSRF principal is the frame's OWN `sender.url`.) |
| Bundle `import()`s `http://127.0.0.1/…` or an intranet host (public page) | **Mitigated** — `safeFetch` origin-relative SSRF blocks public→private (initial + post-redirect) |
| Bundle imports a cross-origin HTML/JSON endpoint as a "chunk" | **Mitigated** — the import's Fetch §2.2.5 destination is script-like, which is the question `safeFetch`'s CORB rule asks, so a cross-origin body is ingested JS-typed only. (This row named `as:"script"`, the deleted load-type keyword; a caller that still passes one now aborts at the chokepoint's entry) |
| Concurrent localhost-page grind lends its origin to a public page's fetch | **Mitigated** — principal is per-call, not a global shared across the concurrently-driven renderers |
| A sub-frame in a tab whose TOP is `http://localhost/` reaches the user's intranet | **Mitigated** — the SSRF principal is the frame's own `sender.url`, never `sender.tab.url` (this was a real bug; `sender.tab.url` survives only as `topLevelUrl` for secure-context) |
| Content script states its own `origin` / `pageUrl` and a future consumer reads one as the principal | **Mitigated** — no principal-shaped field is on the wire at all; every fact comes from `_browserFacts(sender)` and is re-asserted by `_statedFacts` (a release-fatal `CHECK`) |
| Compromised renderer asks the relay for a fetch the offscreen never authorised | **Not a threat** — the relay confers no privilege the renderer lacks; it is a downgrade from `<all_urls>`, and the trusted call site is the only place a verb is chosen |
| Compromised renderer drives the socket / port / window send relays to reach a peer it could not otherwise | **Not a threat** — it cannot reach them (a content script's `onMessage` is delivered only from a privileged extension context, and no `externally_connectable` entry exists), and if it could it would gain nothing: the connection, the port and the window are the page's own, opened in its own MAIN world, so the relay chooses no destination and confers no capability |
| The live-verify PoC navigates the person's own profile, with cookies, outside the chokepoint | **In model, and it is the address that puts it there** — the delivery URL is the page the sink was OBSERVED on, re-parsed, with only a fragment or query the engine declared appended; the navigation needs the operator's click for its user activation; and the deny list's harm condition (*credentialed AND not-observed*) is false by construction rather than by a check |
| Compromised renderer LIES about a relayed reply | **Residual** — the reply is attributed to the requested URL and vouched for by nobody; cross-origin cookies are withheld (`credentials:"same-origin"`) and CORS still applies, but the body is a claim |
| A DIFFERENT document (other origin, other frame, second document of the target origin) calls `apiclientsink` with a marker it read | **Mitigated** — `_recordProbeHit` compares the browser's `MessageSender.origin`, the `frameId` and a latched `deliveredDocumentId` against what this zone actually delivered, and records the hit `attributed: false` with its reason rather than dropping it |
| The DELIVERED page itself calls `apiclientsink` with the marker out of its own payload | **Residual, irreducible by this mechanism** — the hook is installed in that page's own main world, so a REAL EXPLOIT verdict is fabricable by the document under test |
| Compromised renderer reaches `chrome.tabs.*` / MAIN-world `scripting` through the SW RPC | **Mitigated** — the SW serves `sender.origin === chrome-extension://<id>` only, and pins `__rpc` to `sender.url === getURL("ast-worker.html")` by equality (a prefix would hand every future page whose name merely begins with it the largest privilege the extension has) |
| A seeded page's server 302s the analyzer's cookie-bearing document load to another origin | **Mitigated** — `safeFetch`'s credentialed SOP reads the POST-REDIRECT origin (Fetch §2.2.5 "Requests"' current URL), so the other host's authenticated bytes are refused *inside* the chokepoint as `blocked-cors-credentialed:<landed origin>`; `bridge.js` independently refuses to seat a document whose response URL is cross-origin to the browser-stated address (`cross-origin-redirect:<origin>`), which is what still answers for an uncredentialed (opaque-principal) load |
| A page seeds a cross-origin address to spend the user's session at a host it merely names | **Mitigated** — twice, on two different facts: the brain admits a `CONTENT_SEED` only where its origin is the origin of `_browserFacts.url` (HTML §7.2.5 lets `pushState` move the path, never the origin), and `navigationCarriesSession` attaches cookies only where the address is same-origin with the browser-minted `MessageSender.origin`. A seed that passed neither would be dropped before a request exists |
| The analyzer's own credentialed GET ends the user's session or destroys a resource | **Mitigated** — method is GET (RFC 9110 §9.2.1 Safe Methods), and `_destructiveToken`'s deny list refuses a session-ending or resource-destroying path token, pre-request *and* post-redirect, scoped to exactly the credentialed case. Not a claim that an unmatched path is safe — a floor under the policy, never a substitute for it |
| A page sandboxes its own iframe (opaque origin) to read the embedder's credentialed API via the shared analysis | **Mitigated** — credentialed-read principal is the *requesting frame's* `MessageSender.origin` (opaque-unique, documentId-keyed), never the top frame or URL-parsed; an opaque origin is same-origin with nothing; a mixed-origin buffer fails closed |
| Sandboxed *extension* page (origin `"null"`) impersonates a trusted extension document | **Mitigated** — document→document hops require `sender.origin === chrome-extension://<id>`; `"null"` ≠ that |
| Bundle escapes the WASM sandbox into its host realm | **Out of model, but the blast radius is bounded by the browser** — the host realm is `renderer.html`, a sandboxed frame at a UNIQUE OPAQUE origin, so an escape lands in a document that is cross-origin to the extension, holds no `chrome.*`, holds no principal, and can be given its own renderer process by Site Isolation; it reaches the trusted zone only through the declared `content.mojom.Renderer` methods, validated in both directions by `mojo.js`. This row named "the worker" while the engine still ran in the trusted offscreen realm, where an escape would have held the `Module` handle for every other instance |
| A second WASM instance is provisioned for an agent cluster that already has one (two principals, one heap) | **Mitigated** — `render-process-host.js` refuses it with a `CHECK`, fatal in dev AND release; the refusal was a dev-only `DCHECK` whose release path fell through to overwriting the map entry |
