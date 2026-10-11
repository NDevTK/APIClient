# Security model

The analyzer runs untrusted web bundles to learn their API surface, so the page, its content scripts and the
JS engine that executes the bundle are all assumed hostile. This file is the threat model: what is trusted,
each trust boundary and the code that enforces it, the residuals that are still open, and the attack scenarios
with their outcome.

Every claim names a mechanism by file and symbol, never by line number. `git grep` the symbol: the answer is
either the mechanism or its absence, and an absence is a finding about this file.

## Trust zones

| Zone | Trust | Why, and what confines it |
|------|-------|---------------------------|
| Web page and its bundle | untrusted | attacker code |
| `intercept.js` (`MAIN` world, `document_start`, all frames) | untrusted | runs inside the page and holds nothing the page lacks |
| `content.js` (isolated world, `document_idle`, all frames, `match_about_blank`, `match_origin_as_fallback`) | untrusted | a compromised renderer controls it; it carries material, never identity |
| Engine frame (`renderer.html`: QuickJS/WASM with Lexbor) | untrusted | executes the bundle; a sandboxed frame at an opaque origin that reaches the trusted zone only through `content.mojom.Renderer` |
| Attacker tab (`ATTACKER_PAGE_URL` in `offscreen-brain.js`, used by live verify) | untrusted | a third-party document |
| Offscreen document (`ast-worker.html`: `offscreen-brain.js`, `bridge.js`, `lib/*`) | trusted | the only zone that holds state, principals and network policy |
| Service worker (`background.js`) | trusted, stateless | owns the offscreen's lifecycle, forwards `TAB_REMOVED`, and performs privileged `chrome.*` calls for the offscreen only |
| Popup (`popup.html`, `popup.js`, `lib/popup-*.js`) | trusted | an extension page that renders untrusted data |

The offscreen concentrates everything that needs trust: the one `chrome.runtime.onMessage` router of that
document (in `offscreen-brain.js`), the principal mint (`_browserFacts`), the privileged-RPC client (`swRpc`),
the network chokepoint (`lib/safe-fetch.js`), the trusted end of the page-context relay (`lib/schema.js`),
IndexedDB, the engine pool and its level-1 WFQ (`bridge.js`), the renderer registry (`render-process-host.js`)
and the frames it materializes (`renderer-host.js`), the popup command surface (`lib/popup-handlers.js`) and the
live-verify sessions. One trusted zone means one place to get the principal right. The invariants that make it
survivable are each checkable alone: the router branches only on browser-set `sender` fields, every principal
comes from one mint, and the service worker re-checks the caller of its privileged RPC itself.

## The engine frame: sandbox and opaque origin

- `renderer-host.js` materializes each engine instance as `<iframe sandbox="allow-scripts">` without
  `allow-same-origin`, so its origin is unique and opaque, cross-origin to the extension, and eligible for its
  own renderer process. `manifest.json` `sandbox.pages` lists `renderer.html` and nothing else; the effective
  sandboxing flags are the union of the attribute and the manifest's `sandbox` policy.
- A sandboxed extension page has no `chrome.*` API, so the engine frame cannot message the router or the
  service worker.
- The frame has no network. The manifest's `sandbox` CSP grants `connect-src *`; the frame's own meta policy
  (`default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval' blob:` in `renderer.html`) narrows it, and
  policies combine so that every one must allow. The frame cannot load files by URL either (COEP
  `require-corp` and its opaque origin refuse them), so `renderer-host.js` fetches a fixed list of five files
  off the extension origin (`check.js`, `mojo.js`, `mojom.js`, `lib/qjs/qjs.mjs`, `lib/qjs/qjs.wasm`) and hands
  the bytes over in the invitation.
- The transport is `content.mojom.Renderer` (`mojom.js`) over a `MessagePort`. `mojo.js` validates every
  incoming record with `CHECK` in dev and release (`Remote.prototype._onmessage`,
  `Receiver.prototype._onmessage`, `checkValues`, `requireWireAgreement`); a malformed record kills the
  connection and is counted, it does not reach an implementation. Before the pipe exists, `renderer-host.js`
  accepts the frame's hello on the window channel only from `e.source === f.contentWindow`.
- `registerRenderer` in `render-process-host.js` refuses a second renderer for an agent cluster that already
  has one with a `CHECK`, because two heaps behind one principal is the state the cluster key exists to forbid.

## Message authority: the browser-set sender fields

The offscreen router (`chrome.runtime.onMessage` listener in `offscreen-brain.js`):

| Check | What it decides |
|-------|-----------------|
| `sender.id !== chrome.runtime.id` → drop | rejects other extensions only; every content script of ours carries our id, so it is not a trust signal |
| `fromExtUrl` (`sender.url` starts with `chrome-extension://<id>/`) | authenticates the service worker's `__evt` hop, because a worker has no frame and Chrome leaves its `sender.origin` undefined |
| `CONTENT_TYPES` and not `fromExtUrl` → `handleContentMessage` | the untrusted arm: `CONTENT_SEED`, `CONTENT_FORM_SUBMIT`, `RESPONSE_BODY`, `PROBE_HIT`, `POC_RAN`; dropped on the broader URL predicate, since dropping more can only refuse |
| `fromExtDocument` (`sender.origin === EXTENSION_ORIGIN`) → `handlePopupMessage` | the document→document rule; a sandboxed extension page has the extension URL prefix and the opaque origin `"null"`, so it gets none of the command surface |

`handleContentMessage` drops a message with no `sender.tab` or no `sender.documentId` (a `(tab, frame)` pair is
reused across navigations at another origin, so it is never a fallback key).

The other ends apply the same rules:

- `background.js` serves only `sender.origin === EXT_ORIGIN`, and serves `__rpc` only when
  `sender.url === OFFSCREEN_HREF`, an equality with `chrome.runtime.getURL("ast-worker.html")`. A prefix would
  hand every future page whose name begins with that string the largest privilege the extension has.
- `popup.js` accepts only `sender.origin === EXTENSION_ORIGIN` (`isExtensionPage`), so the reverse direction is
  gated on the same fact.
- `content.js`'s `onMessage` does not check its sender. Chrome delivers to a content script only
  `chrome.tabs.sendMessage` from a privileged extension context, and `manifest.json` declares no
  `externally_connectable`, so no web page and no other extension has a door. Every payload-carrying send is
  routed with `{documentId}` and refused without one (`_sendPageFetch` in `lib/schema.js`; the `WS_SEND_MSG`,
  `PM_SEND_MSG` and `MC_SEND_MSG` cases in `lib/popup-handlers.js`). `RESHIP`, which carries no data, goes to
  every frame of every http(s) tab.

`sender.tab.url` authorizes nothing. It is HTML §8.1.3.1 "Environments"' top-level creation URL, carried as
`topLevelUrl` for HTML §8.1.3.5 "Secure contexts".

### The principal is minted in one place

- `_browserFacts(sender)` in `offscreen-brain.js` takes a `MessageSender` and nothing else and is the only
  producer of `{documentId, origin, url, topLevelUrl, tabId, frameId}`. The record is frozen and branded
  `stated: "MessageSender"`; every consumer reads it through `_statedFacts`, a `CHECK`. The brand catches the
  accident of a record assembled elsewhere inside the trusted zone; it is not an anti-forgery device.
- `_senderOrigin` is the origin half: a tuple origin as-is, an opaque one as a per-`documentId` `null:<uuid>`
  token (so two opaque documents never compare same-origin), and a sender with no `documentId` as a fresh
  unstorable token. `_docOrigins` keeps the mapping after the document's buffer is gone.
- There are two principals and they are never one field. `url` is the document's own `sender.url`: the
  private-network principal and the engine's `window.location`; it travels as `sourceUrl` and reaches
  `safeFetch` as `opts.pageUrl`. `origin` is `MessageSender.origin`: the credentialed-read principal; it
  reaches `safeFetch` as `opts.pageOrigin`. Neither is parsed out of an address, so a sub-frame analyses as
  itself, and a page that sandboxes its own iframe cannot have that frame read as its embedder.
- A message from an untrusted zone carries material (response bytes, the address the page is posting to) and
  never identity. The one address-shaped field on the wire is `CONTENT_SEED.seedUrl` (`_sendPageSeed` in
  `content.js`: `PerformanceNavigationTiming`'s `name`, else `location.href`). The router admits it only where
  its origin equals the origin of `_browserFacts.url`, which is sound because HTML §7.2.5 "The History
  interface" lets `pushState` move the path and never the origin. A mismatch is dropped, not asserted. The seed
  decides which address is loaded, never whose principal loads it.

## Network: the chokepoint

Analyzer HTTP goes through `safeFetch` in `lib/safe-fetch.js`, which both hosts load verbatim (the offscreen
through `ast-worker.html`, the native host through `engine/trusted.mjs`). Its extension callers are
`navigationLoad` and `frontierRederive` (document loads), `fetched` (the page's parked `fetch()`, script and
module loads) and `fetchedXhr` (the page's `XMLHttpRequest`) in `bridge.js`, and `_chokepointGetFn` (the
automatic discovery sweep) in `lib/discovery-probe.js`. The engine itself opens no socket: its requests park,
and the bridge relays each to `safeFetch`.

| Gate | Enforcing symbol | Refusal reason |
|------|------------------|----------------|
| closed option set: an option `safeFetch` does not read (`method`, `body`, `as`, …) is refused, not dropped | `_refuseUnreadOptions` (DCHECK) | throws in dev; in release the option is dropped, which always lands on the GET this file sends |
| facts every caller must state | `_destinationOf`, `_provenanceOf`, `_docReachOf`, `_pinnedOf`, `_actorOf`, `_credentialedOf` (CHECK) | throws in dev and release |
| GET only: `init.method` is the literal `"GET"` | `safeFetch`; callers ask `safeFetchMethodRefusal` first | `blocked-method:<M>` |
| http(s) only | `safeFetch` | `blocked-scheme:<scheme>` |
| no userinfo in the URL | `safeFetch` | `blocked-url-credentials` |
| origin-relative SSRF on the requested and the landed host: a target whose Local Network Access §2.1 "IP Address Space" is less public than the page's | `_ipAddressSpace`, `_lessPublicThan` against `opts.pageUrl` | `blocked-local-network:<target>-from-<page>`, `blocked-local-network-redirect:<target>-from-<page>` |
| per-origin egress policy, before the wire and on the landed URL | `_firingRefusal` | `blocked-signal:<signal>=<value>`, `blocked-signal-redirect:` |
| destructive-path deny list where the session pays and the request is not `observed`, before the wire and after a redirect | `_destructiveToken` | `blocked-destructive:<token>`, `blocked-destructive-redirect:` |
| own SOP/CORS on a credentialed reply, over the landed origin | `_resourceSameOrigin`, then exact-origin `Access-Control-Allow-Origin` and `Access-Control-Allow-Credentials: true` | `blocked-cors-credentialed:<landed origin>` |
| a page's `same-origin`-mode request to another origin, before the wire and on the landed URL | `safeFetch` | `blocked-mode-same-origin:<origin>` |
| a page's `cors`-mode reply from another origin passes Fetch §4.10 "CORS check" before its body is read, credentialed or not, and is CORS-filtered | `_corsCheck`, `_corsFilteredHeaders` | `blocked-cors:<failed step>:<landed origin>` |
| a page's cross-origin `cors` request whose use-CORS-preflight flag is set, or whose header list has a CORS-unsafe name, is preceded by Fetch §4.8 "CORS-preflight fetch" (uncredentialed `OPTIONS`, no redirect, no preflight cache) and is not sent unless the preflight passes; the preflight is CORS-checked against `opts.identity.origin`, which must equal the page origin | `_corsPreflight`, `_corsUnsafeRequestHeaderNames` | `blocked-cors-preflight:<failed step>:<origin>` |
| a page's `no-cors`-mode reply from another origin is opaque; a `fetch()` (destination "") gets no bytes at all | `safeFetch`, the record's `type: "opaque"` | (resolves, as in a browser) |
| CORB for a script-like destination (Fetch §2.2.5 "Requests"), whatever the status | `_isScriptLike`, `_corbDeniesScript` | `blocked-corb:<rule>:<type>` |
| a script-like body is never streamed, so CORB always reads the whole body | `_bodyGated` | `blocked-stream-body-gated:` |

Each refusal is graded by `_refused`: `network` where a real browser would also fail the request, `decline`
where only this tool declines, so the flow parks instead of running its failure path.

What the page may read of a reply is decided here from the request's Fetch §2.2.5 mode, which the engine states
for every request (the pending line's `MODE` field, the XHR record's `mode`) and `_modeOf` CHECKs. A host-permission
fetch skips the browser's own CORS, so `safeFetch` runs Fetch §4.1 "Main fetch"' response tainting itself:

- The request's origin is the page's, from the host's own record: `eng.origin` in `bridge.js` (the agent-cluster
  origin this zone assigned the instance, from `_browserFacts.origin` or from the URL this zone loaded for a
  child) and `e.origin` in `engine/trusted.mjs`. The engine states no origin; nothing on the pending line could
  name one.
- `basic` only where the requested and the landed URL are both same-origin with the page. Otherwise a `cors`
  reply passes §4.10 "CORS check" (`*` passes unless the credentials mode is `include`; the origin is serialized
  as `null` where a redirect left an origin the request was not made from) and becomes a CORS filtered response,
  whose headers `_corsFilteredHeaders` narrows to the safelist and `Access-Control-Expose-Headers` before the
  engine sees them. A cross-origin `no-cors` reply is opaque: for a `fetch()` the body is not read and the engine
  builds Fetch §2.2.6's opaque filtered response (`response_new_opaque`, `fetch_reply_type`); for an element (a
  classic script, an image, a stylesheet) the bytes are delivered for that element to consume.
- The mode is part of the request's identity: the engine keys its pending register on (method, mode, url)
  (`pending_index_key`, `engine_provide`), so a `<script src=u>` and a `fetch(u)` are two requests with two
  answers. The pending channel carries only `same-origin`, `cors` and `no-cors` (`pendingRequest` in `bridge.js`
  CHECKs it), because its reply returns to the asking page.
- Page-script reads of a no-cors element's bytes are refused inside the engine where the platform defines a
  read: a canvas tainted by a cross-origin image (`html_canvas_element.c`, origin-clean) and a cross-origin
  stylesheet's rules (`css_style_sheet.c`, origin-clean).

- Cookies are omitted unless the caller states `credentialed: true` and a Fetch §2.2.5 credentials mode;
  `_credentialedOf` composes the two and can only narrow. The willing callers are the document loads, and
  `navigationCarriesSession` in `bridge.js` makes them willing only where the address is same-origin with the
  browser-stated principal; an opaque principal loads uncredentialed. `fetched`, `fetchedXhr` and the discovery
  sweep state `credentialed: false`. The document load carries the session because a session-less tab models
  nothing: it is neither the person's browser nor a clean client, and its findings would not reproduce for the
  person. A seeded document whose response lands at another origin is not seated
  (`cross-origin-redirect:` in `bridge.js`), which covers the uncredentialed case the credentialed SOP cannot.
- Every document the person visits is seeded by `content.js`, so the extension repeats each same-origin page
  load with the person's cookies. That is a second GET of a page the person already loaded (a single-use URL
  cannot be loaded twice); the deny list does not apply to it because its provenance is `observed`.
- `credentialed` here means cookies and nothing wider. It does not strip an `Authorization` header (on the XHR
  path `opts.headers` is the bundle's own list), authority carried in the URL, or the lineage of an address
  computed from a credentialed reply. Those are separate egress signals.
- A credentialed request may not state a header list (`_credentialedOf`, a `CHECK`), because a header such as
  `X-Http-Method-Override` is a route to a verb other than GET.
- The principal is per call, never a global, because the zone interleaves many renderers. `opts.pageUrl` comes
  from the mint and is DCHECKed non-empty and parseable; with the DCHECKs compiled out an unparseable principal
  classifies public, which still refuses every private target. A principal that names no host (`about:blank`,
  `data:`, `blob:`) is not a private-network principal.
- A cross-origin script acts with the origin of the page it is loaded into, never its own host, which is what
  the per-call principal encodes.

## The page-context relay

The trusted zone decides every request, and the document's isolated world sends it. `pageContextGet`,
`pageContextSend` and `pageContextFetch` in `lib/schema.js` send `PAGE_FETCH` through
`swRpc("tabs.sendMessage", …, {documentId})`. The `documentId` is the one the browser stamped on the
observation's sender, so it names the exact document, and a navigation cannot retarget it the way it can a
`frameId`. `content.js` accepts an order only from a sender the browser stamps with this extension's origin
and no tab. `handlePageFetch` runs `fetch(url, init)` in the isolated world. `init` carries the request's
own `credentials`, `mode`, `referrer` and `referrerPolicy`: `intercept.js` records them on every observed
fetch, XHR and EventSource request, and leaves out any member the page did not supply, so fetch applies its
default. A request no observation describes states none (`PAGE_CONTEXT_COMPOSED_INIT`), which is what the
page's own `fetch(url)` sends. The Send panel shows the mode, referrer and policy it will send, and lets the
operator choose the credentials mode.

Measured with `testing/page_identity_probe.cjs` (Chrome 148, a page on `http://localhost:18431` with
`Referrer-Policy: unsafe-url`, a `connect-src` naming one cross-site origin, and a service worker). For the
same credentials mode, the relay's raw header list matches the page's own fetch, order included: `Origin`,
`Referer` under the page's policy, `Sec-Fetch-Site`/`-Mode`/`-Dest`, `Sec-Fetch-Storage-Access`, and the
cookies, partitioned ones included. A cross-site POST gets the same CORS preflight. A recorded same-origin
referrer arrives as that path. A cross-origin one arrives as the page's URL, because Fetch §5.4 "Request
class" turns it into "client". The browser's SOP and CORS govern the request, not this extension.

`pageContextFetch` refuses a forbidden request-header name (Fetch §2.2.2 "Headers": `Cookie`, `Origin`,
`Referer`, `Host`, any `Sec-` or `Proxy-` name, and the rest of the list) with an error the Send panel shows.
The browser sets these itself and ignores a script's value. Two residuals, each a difference from the page
and not a privilege:
- The page's service worker does not see the send. A request it would have answered, rewritten or served from
  cache goes to the network instead.
- The page's CSP does not bind the isolated world. A send can reach an address the page's `connect-src`
  forbids, where the page's own fetch fails. The next member is the trusted zone applying the page's
  `connect-src` to engine and tool sends.

The alternatives were measured and rejected. A main-world send matches the page on every axis, but the
page's service worker can see it and forge the reply. An offscreen (`safeFetch`) fetch sends
`Sec-Fetch-Site: none`, no `Referer`, an `Origin` of `chrome-extension://…` on a POST and no preflight. It
also sends the unpartitioned first-party jar, so a cross-site request carries Lax and Strict cookies but not
partitioned ones. A `declarativeNetRequest` rule can rewrite those headers on the extension's own fetch, and
it is confined by `initiatorDomains`, `tabIds` and `requestMethods`. Still, the cookie order and partition
cannot be reproduced from `chrome.cookies` (no creation time, and the partition is keyed on the request's
cross-siteness), the preflight is absent, a reply's `Set-Cookie` lands in the person's unpartitioned jar, and
the rule set the `Origin` to an arbitrary site. That is an origin-forging capability this zone does not hold.

- Trusted to untrusted (the request) confers no privilege. A content script can already fetch anything its page
  can fetch with its own cookies, and the relay asks for exactly a request of that page. `pageContextFetch`
  refuses non-http(s) URLs, and the verb is decided at the trusted call site, because the far end takes
  `msg.method` verbatim.
- The credentials mode is the observed request's, or the operator's choice. A relayed cross-origin request
  spends the person's cookies at that host only when the page's own request did, or when the operator chose
  `include`.
- Every caller states whose act it is, and `pageContextRequireUserInitiated` `CHECK`s that it is
  `PAGE_CONTEXT_USER_INITIATED`. The callers that state it are the popup command cases (`PROBE_ENDPOINT`,
  `DISCOVER_SERVICE`, `FETCH_DISCOVERY` in `lib/popup-handlers.js`) and the Send panel's replay (`lib/send.js`).
  The automatic discovery sweep fired from `handleResponseBody` (`lib/response-decode.js`) states
  `PAGE_CONTEXT_TOOL_INITIATED`, and `fetchDiscoveryForService` then routes it to `safeFetch` instead. The grade
  separates an operator's act from an automatic caller inside the trusted zone; it is not proof that a person
  clicked.
- The destructive-path deny list does not apply here. It is a floor under requests this tool composes on its
  own, where a wrong deny costs one unfired request; applied here it would veto the operator's explicit act on
  a substring match.
- Untrusted to trusted (the reply) is attacker data. Nothing binds the returned `{status, headers, body}` to
  the request, and the offscreen files it under the URL it asked for (a residual, below).

## Every way bytes leave

An egress is any path by which bytes this extension composed reach a peer it did not author. Three questions
place one: who chose the destination, whose credentials pay, and who composed the bytes. A path whose
destination the analysed bundle chose is what the chokepoint exists for. A path whose destination and
credentials are the page's own confers no privilege: it is a downgrade, and a check there could only refuse
something the page can already do. A path whose bytes the operator composed is authorized by a human at a
surface that shows them the bytes, and what it owes is a destination this zone can name exactly.

| Channel | Destination chosen by | Credentials | Enforcement |
|---------|----------------------|-------------|-------------|
| Analyzer HTTP (`safeFetch`) | what the bundle computed, or a seed the browser stated | cookies only on a same-origin document load | the chokepoint, above |
| Requests as the page (page-context relay) | the trusted call site, for an operator's act | the page's own, under the observed request's credentials mode or the operator's choice | the browser's SOP/CORS; the initiator grade; forbidden request-headers refused |
| Renderer program assembly (`renderer-host.js`) | a constant list off the extension origin | none | no bundle influences it; a bundle-influenced name would make it analyzer HTTP |
| The page's own traffic through `intercept.js`'s `fetch`, `XMLHttpRequest`, `WebSocket` and `EventSource` wrappers | the page | the page's | none needed: the wrapper calls the original and confers nothing |
| Operator writes into a channel the page opened (`WS_SEND_MSG`, `PM_SEND_MSG`, `MC_SEND_MSG`) | the page opened the socket, port or window | the page's | the router's document→document rule; `documentId` routing; a postMessage target must be one real origin (`_isRealTargetOrigin` in `lib/popup-console.js`, DCHECKed in `lib/popup-handlers.js`) |
| Live-verify attacker tab (`tabs.create` of `ATTACKER_PAGE_URL`) | a constant | the person's profile | one GET of a fixed documentation page |
| Live-verify navigation (`window.open` of the delivery address) | the page the sink was observed on, plus a fragment or query the engine declared | the person's profile | the address (see Live verify); the person's click |

Not egress, though each greps like one: `renderer.html`'s `import()` of a `blob:` URL built from bytes the
offscreen already fetched; every `indexedDB.open`; the popup's request formatters, which build a `fetch(…)`
string for a person to copy; and `content.js`'s `submit` listener, which observes a submission the page made.

## Egress policy and its signals

Whether a request fires is a per-origin decision in `lib/safe-fetch.js`. The table `_EXPLORED` maps origin →
signal → permitted values; `_firingRefusal` fires a request when a default arm matches or when every gating
signal's value is permitted at the request URL's own `.origin`, and otherwise names the first signal that is
not. Nothing is refused at every setting.

| Signal | Values | Reliability |
|--------|--------|-------------|
| `destination` | `program`, `subresource`, `navigation`, `value` | stated (engine) |
| `actor` | `page`, `tool` | certain: a literal in trusted-zone source at every caller, narrowed to `tool` by `provenance=driven` |
| `provenance` | `observed`, `derived`, `forced`, `driven` | stated (engine) |
| `doc-reach` | `observed`, `derived`, `forced`, `driven` | stated (the zone that loaded the document) |
| `cookies` | `yes`, `no` | certain |
| `header-authority` | `none`, `unknown` | certain |
| `witness` | `pinned`, `unpinned`, `unstated` | stated (engine) |
| `url-authority` | `present`, `unknown` | partial: a miss is `unknown`, never "none" |
| `lineage` | `unknown` | undetermined |
| `method` (does not gate) | `GET` | certain |
| `invalidity` (does not gate) | `unstated` | intent |

`actor` is the one gating row the untrusted zone cannot reach; the `stated` rows are words the
attacker-controlled engine composes and the chokepoint `CHECK`s only for vocabulary. A signal the chokepoint
cannot determine is shown as `unknown` rather than left out, because an absent row reads as a question that was
answered. `_signalRegistryCheck` refuses a duplicate signal, an unknown reliability grade, a non-gating signal
with more than one value, and a default arm naming a value the registry does not have.

The five default arms (`_DEFAULT_ARMS`, shown to the person by `safeFetchDefaultArms`) are the page loading
itself as a browser would: a program load; any other subresource; a request with `provenance` and `doc-reach`
both `observed` (this is also what fires the seeded document load); the analysed page's own data request whose
address no fork pinned (`destination=value`, `witness=unpinned`, `actor=page`); and a child navigable the page
named (`destination=navigation`, `actor=page`, `provenance=derived`). Probes, discovery and tool-composed data
requests fire only at an origin a person widened.

`driven` is the engine's word for a request minted inside its own drive of a function the page never called
(`flow_path_driven`, asserted at `endpoint_record`). The page's code composed it, but it exists because this
tool called that code, so it is this tool's act: the `actor` row reads `tool` for it, and neither the data-request
arm nor the navigation arm admits it. The engine's word can only narrow `actor`, never widen it. A program or
subresource load a drive reaches still fires by its destination arm, which is the app's own public code.

- A person widens an origin through the popup's `EGRESS_POLICY` command (`renderEgressPolicy` in `popup.js`,
  which states `initiator: "user-initiated"`). The router admits it only on the document→document rule,
  `handlePopupMessage` re-asserts that, and `astDispatch`'s `AST_EGRESS_POLICY` arm in `bridge.js` `CHECK`s the
  initiator grade, asks `safeFetchWidenable` and `safeFetchSignalUsable` for a refusal reason, and only then
  calls `safeFetchWiden`, `safeFetchUnwiden` or `safeFetchPermit`. The subject is the browser-stated origin of
  the pinned document (`currentPrincipalOrigin` in `popup.js`), never one parsed from a URL.
- The table is keyed by the serialized tuple origin, http(s) only: `safeFetchWidenable` refuses an address, an
  explicit default port and an opaque `null`.
- Grants persist in the offscreen's IndexedDB (`apiclient-frontier`, store `prefs`, key `exploreOrigins`),
  written from `safeFetchEgressTable` by `egressPolicyPersist`, so a grant the chokepoint refused is never
  stored. `egressPolicyReady` restores them through `safeFetchEgressStated`, and its promise is registered with
  `safeFetchEgressStating` at load; `safeFetch` awaits it before the firing question, so no door can be
  answered from the empty table. `_firingRefusal` DCHECKs that the table was stated.
- A stored grant from the earlier single-switch control (an array of origins) is dropped, not read as
  "everything", and the popup names the dropped origins (`legacyDropped`). A signal a stored grant does not
  name is not permitted, so adding a signal narrows every existing grant.
- The native host states its table synchronously, and `--explore <origin>` in `engine/trusted.mjs` is
  `safeFetchWiden`.

## Storage

- Learned state lives in the offscreen document and in IndexedDB at the extension origin: `uasr_store`
  (`lib/persistence.js`) for the learned store, and `apiclient-frontier` (`idbOpen` in `bridge.js`) for the
  cross-session frontier and the `prefs` store. The request log is in memory for the offscreen's lifetime.
  Content scripts have no read path into either.
- `manifest.json` requests `offscreen`, `webNavigation`, `scripting`, `tabs` and `unlimitedStorage`, and not
  `storage`, so `chrome.storage` is undefined in every context and a compromised renderer cannot read keys
  through it. `unlimitedStorage` lifts the origin's quota and grants no read API. The security claim is that
  `git grep -n 'chrome\.storage' extension/` finds no call site, only comments.
- The service worker holds no learned state and relays no page data. Content scripts message the offscreen
  directly, so the service worker cannot launder a renderer's message into an extension-origin one.
- The engine frame has an opaque origin and keeps nothing between sessions; parked work persists through the
  offscreen's frontier.

## The engine is attacker-controlled

- It cannot read or set a principal. Principals live in the offscreen, a different document, and `bridge.js`
  passes them to `safeFetch` per call. It has no network and no `chrome.*` (see the engine frame, above): its
  `fetch()`, XHR and `import()` targets are untrusted URLs the chokepoint judges.
- One instance per origin-keyed agent cluster, `(browsing-context group, origin)`, never per tab or per page.
  `clusterKeyOf` in `bridge.js` builds the key from browser-stated halves: the group is `sender.tab.id` and the
  origin is `_senderOrigin`'s, so two sandboxed iframes of one page are two clusters. A same-origin child is a
  second realm in its creator's heap (`child_in_this_agent` in `navigable.c`); a second cross-origin document of
  one cluster, or a navigation replacing a group's top, joins the running instance (`engineJoin`, over the
  engine's `qjs_join`) instead of getting a second heap.
- The engine names the child documents it creates; the offscreen routes them. Identity may be minted by the
  untrusted side because it is only a name, but routing and the origin stamped on a delivered message are the
  trusted zone's alone: a cross-document message is an emission (`windowproxy.post`, carrying the target, the
  sender's world, the target origin and the serialized bytes, from `window_message.c`), and the offscreen
  stamps it with `stampOrigin(eng.origin)` after a `CHECK` that the sending instance has a recorded principal.
  A forgeable `event.origin` would defeat every origin check in every bundle the engine analyses.
- Cross-instance state crosses as serialized, typed text, never a live value, and a synchronous cross-instance
  read is a suspend point. The cross-origin `WindowProxy` member list is declared beside the `Window` member
  table in `engine/host/browser/core/frame/window_proxy.c`.
- What the engine states to the chokepoint (`destination`, `mode`, `provenance`, `pinned`, `credentials`) is
  checked for vocabulary, not for truth. The engine can never turn cookies on (`_credentialedOf` only narrows),
  never chooses `actor`, and never states the origin a reply is judged against. A false mode can only pick
  among the three page-readable modes (`pendingRequest`), and the weakest of them, `no-cors`, hands a `fetch()`
  no bytes; the bytes it does hand an element are the ORB residual below.

## Live verify

Live verify runs the engine's exact PoC in the person's real Chrome. Its signal is one-sided: a hit says the
payload's code ran in the delivered document, not that the sink produced the call.

- `startExploitProbe` in `offscreen-brain.js` mints the marker with `crypto.randomUUID`, and `buildLiveDelivery`
  rewrites the engine's fire marker into `apiclientsink('<marker>')`. Only an `address` delivery produces a PoC:
  the page the sink was observed on, re-parsed by the URL parser, with its fragment (and for a query delivery,
  its query) cleared and the payload appended at the component the engine declared. Every other mechanism
  (`plant`, `referring-address`, `user-file`, `cross-document-message`) answers a stated reason (`pocWhy`)
  instead of a PoC.
- `EXPLOIT_PROBE_ARM` runs `armAttackerPage`: `tabs.create` of `ATTACKER_PAGE_URL` (`https://example.com/`),
  a wait for the tab to complete, then `scripting.exec` with the named injector `pocAttackerButton`
  (`background.js`), which renders a Run button in the attacker document's `MAIN` world. The offscreen selects
  the injector by name and passes only serializable arguments, so it cannot inject code of its own composing.
  The person's click is the user activation `window.open` needs.
- The delivery is a real top-level navigation in the person's profile, with cookies, outside `safeFetch`. It is
  inside the model because of the address: scheme, host and path are the ones the person's browser already
  loaded, and the analyzer supplies only a fragment or query. The deny list's harm condition (credentialed and
  not observed) is false by construction there.
- The delivery witness is the browser's: `_recordProbeDelivery` latches `delivered` when a top-level document
  whose browser-stated origin and frame match the session's expectation announces itself (`CONTENT_SEED`) at
  the delivered address, compared against the browser-stated URL or the page-claimed seed, and it records which
  answered. A document at the same address apart from the payload's component is recorded as a near miss.
- `intercept.js` installs `window.apiclientsink` in the page's `MAIN` world; a call is relayed by a
  `__uasr_probe_hit` event and a `data-uasr-hits` attribute mirror to `content.js`, which sends `PROBE_HIT`.
  `_recordProbeHit` records every hit and marks it `attributed: false`, with its reason, when the session
  delivered nothing, when the browser's `MessageSender.origin` is not the origin of the address this zone built,
  when the frame is not top-level, or when a different document of the target origin already fired the marker.
  A browser fact is compared against this zone's expectation, never the reverse. The record splits
  `browserStated` from `pageClaimed`, and `lib/popup-security.js` DCHECKs that every hit states `attributed`.
- `POC_RAN` is the attacker document's own account of its run; the offscreen records the browser-stated origin
  beside it and treats it as an explanation, never as evidence of a delivery.
- The marker rides inside the payload, which is delivered in the page's own address, so any script in the
  delivered document can read it and call the hook. A positive is fabricable by the document under test and a
  negative is suppressible by it. The verdicts `_pollVerify` renders say so: the fired verdict states that it is
  not proof the sink produced the call, and every non-firing verdict states that it is not a statement that the
  sink is safe.

## Popup rendering and extension CSP

- Extension pages run under `default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self';
  style-src 'self'; img-src 'self'; connect-src 'self' https: http: data:; frame-src 'self';
  frame-ancestors 'none'; form-action 'none'` (`manifest.json`), with COEP `require-corp` and COOP
  `same-origin`. No resource is web-accessible, and `frame-ancestors 'none'` stops any page framing the popup or
  the offscreen.
- The popup renders page-derived strings into `innerHTML` after escaping them with `esc`
  (`lib/popup-response.js`). Escaping is a convention at each site; the CSP bounds a miss. Injected markup
  cannot run script (no `'unsafe-inline'`), load an off-origin image or stylesheet, or submit a form; it can
  still show spoofed text or a link the person might click.
- Page-claimed fields are labelled as claims: `_claimedOriginPair` and `PAGE_CLAIMED_ORIGINS_TITLE`
  (`lib/popup-reqlog.js`) for postMessage and MessageChannel origins, and the `browserStated` / `pageClaimed`
  split for probe hits.

## Verdicts

- Only firing proves a PoC, and the PoC is derived from the run (the real sink parse context, byte provenance
  through the real filter, and the path's value domain), never from a payload table. Absence of a PoC is a
  parked search, never a "safe" verdict.
- An open redirect is not a finding class: a redirect executes nothing, so there is nothing for the one oracle
  to fire (`engine/host/browser/core/frame/location.c`).

## Known residuals

Each residual states what is not covered, what would close it, and how its absence shows.

| Residual | Not covered | What closes it | How its absence shows |
|----------|-------------|----------------|-----------------------|
| Private network by name (write side unmitigated) | `_ipAddressSpace` classifies IP literals by Local Network Access's "Non-public IP address blocks" table and the names `localhost`, `*.localhost` and `*.local`; any other name is `public`. A name that resolves to a local or loopback address (an intranet name, `.internal`, `.lan`, a single-label host, `127.0.0.1.nip.io`) is therefore sent from a public page, including by the default arm for the page's own unpinned `fetch()`. The reply is unreadable unless that host answers `Access-Control-Allow-Origin` for the page or `*` (`_corsCheck`). No stable MV3 surface resolves a name before the connection: `chrome.dns` is Chrome Dev only, `webRequest` reports the address only on events after the request was sent, and blocking `webRequest` is policy-installed only. Whether Chrome's own Local Network Access check applies to this extension's offscreen fetches is unmeasured | Chrome's local-network gate shown, in real Chrome, to apply to this extension's fetches, or a resolver this zone can ask before connecting | a page analysed from a public origin causing a request to a host on the person's network |
| Redirect hops between the first and last URL | `safeFetch` sees only the requested and the landed URL, so a redirect chain that leaves the origin and returns is tainted `basic` where Fetch would taint it, and an intermediate redirect response's own CORS check is not run | each hop recorded per request. `fetch`'s `redirect: "manual"` cannot supply it: it answers Fetch §2.2.6 "Responses"' opaque-redirect filtered response (status 0, empty header list, null body), so the `Location` is not readable. `webRequest.onBeforeRedirect`, non-blocking in MV3, reports each hop's `redirectUrl`, `responseHeaders` and `ip`, if it is shown this extension's offscreen fetches and an event can be tied to one `fetch` call, neither of which is measured | a `cors` reply delivered `basic` after a chain that crossed origins |
| The request carries no page `Origin` header | `Origin` is a forbidden request header, so the zone's request carries this extension's origin or none, and a server that reflects the request's `Origin` answers an `Access-Control-Allow-Origin` that is not the page's. The page then gets a network error where a browser's request would pass: a narrowing | a transport that can state the page's origin | a cross-origin API that works in a browser failing `blocked-cors:allow-origin-mismatch` here |
| No ORB for a no-cors element's bytes | CORB applies to script-like destinations only, so a cross-origin `no-cors` image, style or font body of any type is delivered to the engine; page script cannot read it, but a compromised engine could | Chromium's ORB rules for the other no-cors destinations | a JSON or HTML body delivered for an `image` destination |
| Redirect to a less public host | `safeFetch` re-judges the landed host and refuses to ingest, but the hop that reached it was already sent | a walk judging each `Location` before it is followed, which needs the 3xx's `Location` that `redirect: "manual"` hides (see the row above) | a public host's 3xx onto an intranet address appearing in that host's logs |
| Credential lineage | the chokepoint sees the credential state of the request in front of it and nothing about where the address came from | a `credentialed` bit on the response the engine ingests, carried into the concolic value's source identity and composed at the park beside the provenance word | every request at an origin reading `lineage=unknown`, including addresses the bundle built from a logged-in reply |
| Authority in a path segment | `url-authority` matches capability-URL parameter names and the JWS prefix only; an opaque token in a path reads `unknown` | read the signal off the engine, whose park knows which source each segment came from | `url-authority=unknown` doing all the work at a widened origin whose addresses carry capability tokens |
| Engine-stated signals | `destination`, `provenance` and `witness` are the engine's words, so an engine that states `observed` or `unpinned` falsely reaches the default arms. What fires is still an uncredentialed GET under every other gate | none in this design; `actor` is the row that does not rest on the engine | requests firing by default at an unwidened origin whose provenance the run's own log contradicts |
| The control is the safety | with nothing refused at every setting, a person who ticks a row without reading it permits its consequence | the surface stating each row's consequence and reliability (built); nothing removes the person's choice | a widened origin firing requests its owner did not expect |
| Same-document probe-hit fabrication | `window.apiclientsink` lives in the delivered document's `MAIN` world, and every fact the hook could report about its caller comes from that world | irreducible by this mechanism | a fired verdict for a sink that never fired, on a page that reads its own address |
| Hit attribution is not tied to the delivery witness | `_recordProbeHit` latches the first top-level document of the target origin to fire the marker; it does not require that document to be the one `_recordProbeDelivery` witnessed | compare `facts.documentId` against `ses.delivered.documentId` | an attributed hit whose `documentId` differs from the session's `delivered.documentId` |
| Fragment deliveries may not be witnessed | a witness needs the payload's component in `MessageSender.url` or in the seed address; if both drop the fragment, a `#` delivery latches nothing | `content.js` ships the document's `location.href` on `CONTENT_SEED` beside `seedUrl`, compared as a third source | a `#` delivery reporting not delivered with a nonzero `deliveredNearMiss` |
| A forgeable principal demand needs a chosen origin | `ATTACKER_PAGE_URL` is a constant, so a delivery whose victim checks `event.origin` against a shape is refused with the shape stated | a per-session attacker address derived from `principalGates`, plus the person's statement of which origins they control | a `forgeable` finding answering `pocWhy` where `none` findings beside it arm a Run button |
| Relayed-reply attribution | a `PAGE_FETCH` reply is vouched for only by the renderer that returned it and is filed under the requested URL, cross-origin included | none; the learned store is a research artifact, never evidence about a third party | a learned endpoint at another origin whose reply no server sent |
| Passive learning is page-claimed | `RESPONSE_BODY`, `CONTENT_FORM_SUBMIT` and the `sourceOrigin` / `targetOrigin` on `PM_RECV` / `MC_OPEN` records are built in the page's renderer, and any script there can dispatch the wrapper events, so a page can file records under any address or origin. They do not reach the engine as an `event.origin` | none at this layer; the popup labels them as claims (`_claimedOriginPair`) and refuses to reply to a non-origin (`_isRealTargetOrigin`) | records for an origin the person never visited, learned while on an unrelated page |
| Cross-origin child navigables load uncredentialed | `navigationCarriesSession` refuses the session cross-origin, because the chokepoint's credentialed gate asks whether the requesting principal may read the bytes, while a navigation's reader is another instance keyed on the response's origin | a document-load type in `safe-fetch.js` whose read principal is the response's own origin, not an `if` at the caller | cross-origin child documents loading in their logged-out form |
| Page requests are uncredentialed | `fetched` and `fetchedXhr` state `credentialed: false`, so the analysed page's own data requests see the logged-out reply. They already pass the page origin the credentialed SOP would need (`eng.origin`); the wrong origin would be `originOf(msg.sourceUrl)`, which gives a sandboxed iframe its embedder's | a per-origin credential decision over the request's own credentials mode | an app whose boot data differs by session analysed in its logged-out form |
| A credentialed request cannot carry a header list | `_credentialedOf` `CHECK`s the combination, because nothing states whose header list it is | an ownership statement carried from the site that knows, as `provenance` is | that `CHECK` firing |
| `same-origin` credentials mode where the zone is willing | `_credentialedOf` has no pre-request same-origin test, so it `CHECK_FAIL`s | the same-origin test on the request URL against `pageOrigin`, read before the wire | that `CHECK_FAIL` firing; every willing caller today states `include` |
| A joined document is judged under its root's reach grade | `fetched` reads `msg.provenance` from the root's message, so a document joined through `engineJoin` is judged as its root was reached | the park's document name on the pending line, so the lookup is per document | at an origin widened for one grade, identical requests firing from a joined document and refused from a top-level one reached the same way |
| An auxiliary opened without `noopener` | it shares its opener's group but lands in another tab, so `clusterKeyOf` splits the pair; `MessageSender` carries no opener | a browser-stated opener fact joined to the opener's group | an opener and its popup analysed in two instances |
| Unused privileged primitives | the `tabs.remove` arm of `_swRpc` and the `readExecFlag`, `postMessage`, `seedStorage` and `dispatchEvents` injectors in `_PROBE_INJECTORS` have no caller; `scripting.exec` accepts any `tabId` and `allFrames`. Today they are reachable only by the offscreen (`OFFSCREEN_HREF` equality), so they are the blast radius of a hole in that gate | delete them; the `scripting` permission stays for `pocAttackerButton` | `git grep` finding the names in `background.js` and nowhere else |
| Dev-build asserts on renderer-supplied input | `_checkPageFetchReply` (`lib/schema.js`) DCHECKs the shape of the content script's reply, and `renderer-host.js`'s window handshake DCHECKs the frame's record. In dev a compromised renderer can make those handlers throw; in release a malformed reply reaches its consumers unvalidated | refuse a malformed reply with the relay's own error instead of asserting on it | a dev build throwing at those sites for a page's malformed reply |
| Channels CSP does not govern, after a sandbox escape | an escape into `renderer.html`'s realm has no `chrome.*`, no principal and no `fetch`; whether it can reach the network through a channel CSP does not cover (such as WebRTC) or by navigating its own frame is not measured in this tree. The offscreen's `frame-src 'self'` is the policy that would answer the navigation half | a measured negative in real Chrome with a positive control beside it | traffic from the engine frame's process that no `safeFetch` call accounts for |

## Attack scenarios

| Scenario | Outcome |
|----------|---------|
| A compromised renderer reads stored cross-site data | mitigated: state is in IndexedDB at the extension origin, `chrome.storage` is undefined, and content scripts have no read path |
| A renderer sends a command type (`GET_STATE`, `SEND_REQUEST`, `CLEAR_TAB`, `EGRESS_POLICY`) | mitigated: the router reaches `handlePopupMessage` only on `sender.origin === EXTENSION_ORIGIN` |
| A renderer forges `sender.id` | not a threat: it already holds it; the router decides on `sender.origin` and `sender.url` |
| A renderer states a principal, or a localhost address, to defeat the SSRF check | mitigated: no principal-shaped field is on the wire; every principal comes from `_browserFacts(sender)` |
| A renderer seeds a cross-origin address to spend the person's session at a host it names | mitigated: `CONTENT_SEED` is admitted only at the origin of `_browserFacts.url`, and `navigationCarriesSession` attaches cookies only same-origin with the browser-stated principal |
| A bundle imports `http://127.0.0.1/…` or an intranet IP from a public page | mitigated: `_ipAddressSpace` on the requested and landed host, for every block in Local Network Access's table |
| A page on the person's network reaches the machine's loopback | mitigated: loopback is less public than local, so `_lessPublicThan` refuses it |
| A bundle fetches an intranet DNS name, or a public name resolving to loopback, from a public page | partially mitigated: the reply is unreadable unless that host grants the page's origin or `*` (`_corsCheck`); the request is still sent (see the private-network-by-name residual) |
| A bundle reads a cross-origin reply its browser would refuse it | mitigated: a `cors` reply needs Fetch §4.10's grant, a `no-cors` `fetch()` is opaque, and a `same-origin` request to another origin is a network error; the origin judged is the host's record of the page, never one the engine states |
| A bundle imports a cross-origin HTML or JSON endpoint as a chunk | mitigated: the import's destination is script-like, so CORB refuses a non-JavaScript body at any status |
| A sub-frame in a tab whose top is `http://localhost/` reaches the person's intranet | mitigated: the SSRF principal is the frame's own `sender.url`, never `sender.tab.url` |
| A localhost page's analysis lends its origin to a public page's fetch | mitigated: the principal is per call, never a global |
| A page sandboxes its own iframe to read the embedder's credentialed API | mitigated: the credentialed-read principal is the requesting frame's `MessageSender.origin`, opaque-unique, and an opaque origin is same-origin with nothing |
| A seeded page's server redirects the cookie-bearing document load to another origin | mitigated: the credentialed SOP reads the landed origin (`blocked-cors-credentialed:`), and `bridge.js` refuses to seat a cross-origin response (`cross-origin-redirect:`) |
| The analyzer's own credentialed GET ends the person's session or destroys a resource | mitigated as a floor: GET only, and `_destructiveToken` refuses a listed path token before the wire and after a redirect for any credentialed request that is not `observed`; an unmatched path is not thereby safe |
| A compromised renderer asks the relay for a fetch the offscreen never authorized | not a threat: it cannot address the relay, and the relay confers nothing the renderer lacks |
| A compromised renderer lies about a relayed reply | residual: the body is a claim filed under the requested URL; CORS still applies, and cross-origin cookies are spent only when the observed request or the operator chose `include` |
| The page's script reads or alters a relayed send | not a threat: the send runs in the isolated world, and measured, the page's service worker does not see it |
| A compromised renderer drives the socket, port or window send relays | not a threat: it cannot reach them, and the channel is the page's own |
| An automatic caller in the trusted zone uses the relay or widens an origin | mitigated: the relay `CHECK`s `PAGE_CONTEXT_USER_INITIATED` and `AST_EGRESS_POLICY` `CHECK`s the initiator; the automatic sweep is routed to `safeFetch` |
| A page widens its own origin for exploration | mitigated: `EGRESS_POLICY` is on the command surface the router gates on the extension origin |
| A grant for one origin fires requests at another | mitigated: the table is keyed by the serialized tuple origin and compared with the request URL's own `.origin` |
| A request arrives before the person's grants are restored | mitigated: `safeFetch` awaits the restore registered with `safeFetchEgressStating` |
| A grant from the single-switch control covers signals that did not exist when it was made | mitigated: the restore drops it and names it; an unnamed signal is not permitted |
| An uncredentialed request carries the person's authority in its address | partially mitigated: `url-authority=present` needs its own permission; a miss reads `unknown` |
| An address computed from a credentialed reply is fetched uncredentialed | not mitigated: `lineage` reads `unknown` for every request |
| Another document (other origin, other frame, second document of the target origin) calls `apiclientsink` with a marker it read | mitigated: `_recordProbeHit` records it `attributed: false` with its reason |
| The delivered page calls `apiclientsink` itself | residual, irreducible by this mechanism |
| A compromised renderer reaches `chrome.tabs.*` or `chrome.scripting` through the service worker | mitigated: the service worker serves only the extension origin and pins `__rpc` to `OFFSCREEN_HREF` by equality |
| A sandboxed extension page impersonates a trusted extension document | mitigated: its origin is `"null"`, which fails every document→document check |
| The bundle escapes the WASM sandbox into its host realm | out of model; the realm is `renderer.html` at a unique opaque origin with no `chrome.*`, no principal and no `connect-src`, reachable to the trusted zone only through validated `content.mojom.Renderer` records. See the residual on channels CSP does not govern |
| A second instance is provisioned for an agent cluster that already has one | mitigated: `registerRenderer` refuses it with a `CHECK` in dev and release |
| Injected markup in the popup runs script or exfiltrates | mitigated by CSP: no inline script, no off-origin images, styles or forms; spoofed text or a clickable link remains possible |
