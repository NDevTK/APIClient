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
| Attacker tab (`ATTACKER_PAGE_URL` in `background.js`, used by live verify) | untrusted | a third-party document |
| Offscreen document (`ast-worker.html`: `offscreen-brain.js`, `bridge.js`, `lib/*`) | trusted | the only zone that holds state, principals and network policy |
| Service worker (`background.js`) | trusted, stateless | owns the offscreen's lifecycle, forwards `TAB_REMOVED`, and performs privileged `chrome.*` calls for the offscreen only |
| Popup (`popup.html`, `popup.js`, `lib/popup-*.js`) | trusted | an extension page that renders untrusted data |

The offscreen concentrates everything that needs trust: the one `chrome.runtime.onMessage` router of that
document (in `offscreen-brain.js`), the principal mint (`_browserFacts`), the privileged-RPC client (`swRpc`),
the network chokepoint and its operator tier (`lib/safe-fetch.js`), the speak-as identity (`lib/request-identity.js`),
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
- `content.js`'s `onMessage` accepts only a sender the browser stamps with this extension's origin and no
  tab. Chrome delivers to a content script only `chrome.tabs.sendMessage` from a privileged extension
  context, and `manifest.json` declares no `externally_connectable`, so no web page and no other extension
  has a door. Every payload-carrying send is routed with `{documentId}` and refused without one (the
  `WS_SEND_MSG`, `PM_SEND_MSG` and `MC_SEND_MSG` cases in `lib/popup-handlers.js`). `RESHIP`, which carries no data, goes to
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
  engine's `window.location`, travelling as `sourceUrl`; its response's IP address space is the
  private-network principal, carried on the zone's record of the document and read by `safeFetch` off
  `opts.doc`. `origin` is `MessageSender.origin`: the credentialed-read principal; it reaches `safeFetch` as
  `opts.pageOrigin`. Neither is parsed out of an address, so a sub-frame analyses as
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

A document load of a `data:` URL is not a request. Its bytes are in the URL and it reaches no other party, so
`navigationLoad` and trusted.mjs's `navigate` answer it with `safeFetchSchemeData` (Fetch §4.3 "Scheme fetch"'s
"data" arm, through the realm's own fetch) and ask no egress question. Its document's origin is opaque, so
`principalOf` gives it its own `null:` principal and cluster and it carries no session.

Which extension context can open a connection at all is enforced by CSP in all but two:
- `popup.html` and `dev.html` add `connect-src 'none'` to the manifest's extension-page policy, so the browser
  refuses a `fetch`, XHR, WebSocket, EventSource or beacon from either. The popup reaches the network only by
  messaging the offscreen.
- `renderer.html`'s own policy is `default-src 'none'`, which refuses every connection from the engine frame.
- The offscreen document (`ast-worker.html`: `connect-src 'self' https: http: data:`) can reach any http(s)
  host, so its CSP does not keep it on `safeFetch`. The realm does instead: `lib/safe-fetch.js` takes the
  platform's `fetch` at load and replaces `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`,
  `WebTransport` and `navigator.sendBeacon` with non-writable, non-configurable functions that `CHECK`-fail.
  The one exception is a `fetch` of this extension's own package, which `renderer-host.js` reads to build
  each renderer. That holds against the scripts loaded after it. The ones `ast-worker.html` loads before it
  hold no network constructor in code, which is a reading of their source, not an enforcement.
- The service worker (the manifest's `connect-src 'self' https: http: data:`) can reach any http(s) host and
  loads no seal. That it sends nothing is code discipline: `background.js` and `speak-as-rule.js` call no
  `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource` or `sendBeacon`.

| Gate | Enforcing symbol | Refusal reason |
|------|------------------|----------------|
| closed option set: an option `safeFetch` does not read (`method`, `body`, `as`, …) is refused, not dropped | `_refuseUnreadOptions` (DCHECK) | throws in dev; in release the option is dropped, which always lands on the GET this file sends |
| facts every caller must state | `_destinationOf`, `_provenanceOf`, `_docOf`, `_pinnedOf`, `_actorOf`, `_credentialedOf` (CHECK) | throws in dev and release |
| a URL the URL Standard §6.1 "URL class" constructor refuses is no request, as in a browser | `safeFetch`, `safeFetchOperator` | `bad-url`, graded `network` |
| GET only: `init.method` is the literal `"GET"` | `safeFetch`; callers ask `safeFetchMethodRefusal` first | `blocked-method:<M>` |
| http(s) only | `safeFetch` | `blocked-scheme:<scheme>` |
| no userinfo in the URL | `safeFetch` | `blocked-url-credentials` |
| a reply whose final URL does not parse (URL Standard §6.1 "URL class" throws) is not read | `safeFetch` | `blocked-final-url-unparseable`, graded `network` |
| origin-relative SSRF on the requested and the landed host: a target whose Local Network Access §2.1 "IP Address Space" is less public than the page's | `_ipAddressSpace`, `_lessPublicThan` against the asking document's record's space (`docRecordAddressSpace`) | `blocked-local-network:<target>-from-<page>`, `blocked-local-network-redirect:<target>-from-<page>` |
| per-origin egress policy, before the wire and on the landed URL | `_firingRefusal` | `blocked-signal:<signal>=<value>`, `blocked-signal-redirect:` |
| destructive-path deny list where the session pays and the request is not `observed`, before the wire and after a redirect | `_destructiveToken` | `blocked-destructive:<token>`, `blocked-destructive-redirect:` |
| own SOP/CORS on a credentialed reply, over the landed origin | `_resourceSameOrigin`, then exact-origin `Access-Control-Allow-Origin` and `Access-Control-Allow-Credentials: true` | `blocked-cors-credentialed:<landed origin>` |
| a page's `same-origin`-mode request to another origin, before the wire and on the landed URL | `safeFetch` | `blocked-mode-same-origin:<origin>` |
| a page's `cors`-mode reply from another origin passes Fetch §4.10 "CORS check" before its body is read, credentialed or not, and is CORS-filtered | `_corsCheck`, `_corsFilteredHeaders` | `blocked-cors:<failed step>:<landed origin>` |
| a page's cross-origin `cors` request whose use-CORS-preflight flag is set, or whose header list has a CORS-unsafe name, is preceded by Fetch §4.8 "CORS-preflight fetch" (uncredentialed `OPTIONS`, no redirect, no preflight cache) and is not sent unless the preflight passes; the preflight is CORS-checked against `opts.identity.origin`, which must equal the page origin | `_corsPreflight`, `_corsUnsafeRequestHeaderNames` | `blocked-cors-preflight:<failed step>:<origin>` |
| a page's `no-cors`-mode reply from another origin is opaque; a `fetch()` (destination "") gets no bytes at all | `safeFetch`, the record's `type: "opaque"` | (resolves, as in a browser) |
| CORB for a script-like destination (Fetch §2.2.5 "Requests"), whatever the status | `_isScriptLike`, `_corbDeniesScript` | `blocked-corb:<rule>:<type>` |
| a script-like body is never streamed, so CORB always reads the whole body | `_bodyGated` | `blocked-stream-body-gated:` |
| a request or provisioning notice is judged by a document of the instance that sent it (the zones, before `safeFetch`) | `docRecordOfAsker`, `docRecordAskerRefusal` | `blocked-asker:<role>-<reason>`, `blocked-asker:child-not-minted`, `blocked-asker:document-not-minted` |

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
- The principal is per call, never a global, because the zone interleaves many renderers. It is the asking
  document's record's IP address space, Local Network Access §3.5 "Integration with HTML"'s policy container
  field, set once when the zone provisions the document (`safeFetchDocumentAddressSpace`): a document created
  from an http(s) response has its response's (`_ipAddressSpace` on the response URL's host); a local-scheme
  document (`about:blank`, `data:`, `blob:`) has a clone of its initiator's (HTML §7.1.7 "Policy containers";
  for a nested navigable, its creator); any other document, and a local-scheme one no zone record initiated,
  has a new container's, initially public.
  `docRecord` CHECKs the word, and the meet of several askers' records takes the least public.
- A cross-origin script acts with the origin of the page it is loaded into, never its own host, which is what
  the per-call principal encodes.

## Operator sends

A request a person composes in the popup (the Send panel, and the Discovery panel's probe, service-info and
fetch-discovery buttons) leaves from the offscreen document through `safeFetchOperator` in
`lib/safe-fetch.js`. It speaks as the frame the request was observed in. The identity is that frame's, from
browser-stamped facts only (`frameIdentity`, `lib/request-identity.js`), and the request carries its headers
through a speak-as rule (below, "Sends that speak as a document"). There is no relay through the page's
renderer, so a compromised renderer can neither read nor forge an operator's reply.

- The tier CHECKs that the sender is an extension document, by its browser-set `MessageSender.origin`
  (`senderIsExtensionDocument`, the router's own gate for the popup command surface). Every extension page
  shares that origin and that trust; `sender.url` is not consulted, as it authorizes only the service
  worker's frameless hop. `lib/popup-handlers.js` hands the sender down to every send. A compromised
  extension page could therefore send as any observed frame, and the extension pages' Trusted Types policy
  and CSP are what bound that.
- The Send panel's identity preview (`SEND_IDENTITY_PREVIEW`, `operatorIdentityPreview` in `lib/send.js`)
  `CHECK`s the same sender predicate. It answers the popup the frame's Origin, Referer, partition key and
  the names of the cookies a send would carry, with the rule that excluded each other name, and never a
  cookie value. `SEND_JAR_DOMAIN` (`operatorJarDomain`) `CHECK`s it too and lists every open http(s) frame
  with its browser-stamped partition key. A send may speak with another open frame's jar, named by that
  frame's `documentId` and resolved through `chrome.cookies.getPartitionKey`, never by a typed key.
- Every caller also states whose act it is, and `pageContextRequireUserInitiated` `CHECK`s
  `PAGE_CONTEXT_USER_INITIATED`. The automatic discovery sweep (`handleResponseBody`) states
  `PAGE_CONTEXT_TOOL_INITIATED`, and `fetchDiscoveryForService` routes it to `safeFetch`.
- The request is the observed one. `intercept.js` records each fetch, XHR and EventSource request's
  `credentials`, `mode`, `referrer` and `referrerPolicy`, and an absent member is null rather than a default.
  The Send panel shows what it will send and lets the person choose the credentials mode. A request no
  observation describes states none, which is what the page's own `fetch(url)` sends.
- What a browser refuses still applies: a non-http(s) scheme, credentials in the URL, a same-origin-mode
  request to another origin, and Fetch §4.8's preflight. A forbidden request-header name (Fetch §2.2.2
  "Headers": `Cookie`, `Origin`, `Referer`, `Host`, any `Sec-` or `Proxy-` name, and the rest of the list)
  is refused with an error the Send panel shows, because the browser owns those headers.
- The destructive-path deny list does not apply. It is a floor under requests this tool composes on its
  own, where a wrong deny costs one unfired request. Applied here, it would veto the person's explicit act
  on a substring match.
- The reply is the trusted zone's own fetch, returned whole whether or not the frame's own script could
  have read it under CORS.
- A stated `Origin` or `Referer`, for testing a server's CORS or CSRF checks: the Send panel's Origin and
  Referer fields, visible beside the credentials mode and named in the panel's note. Empty sends the frame's
  own. `safeFetchOperator` refuses a value that is not a serialized http(s) origin (or `null`), or a
  canonical fragment-less http(s) URL, and puts the stated value on the request's rule and its preflight's.
  The preflight is CORS-checked against the stated Origin. Cookies and `Sec-Fetch-*` stay the frame's. An
  export carries the stated headers too.
  - Only the operator tier passes `stated` to the speak-as rule. `safeFetch` passes null, and its closed
    option set refuses a `stated` option, so neither an engine send nor the tool's own sends can reach it.
  - The service worker's CHECK admits an `Origin` or `Referer` outside the identity only when it equals
    `stated`, and its message names that field.

Measured with `testing/page_identity_probe.cjs`. The oracle is the page's own fetch from
`http://localhost:18431` (`Referrer-Policy: unsafe-url`, a `connect-src` naming one cross-site origin, a
service worker). The Send panel's request carries the same set of header names and values as the page's
request, for same-origin and cross-site targets and both credentials modes: Origin, Cookie (partitioned
cookie included), `Sec-Fetch-*`, and the CORS preflight. Each difference is a named residual of "Sends that
speak as a document":
- Referer. The frame's own referrer policy is not observable from this zone, so a request that states none
  uses the default, strict-origin-when-cross-origin.
- Header-name case and order.
- The page's service worker does not see the send.
- The page's `connect-src` does not bind it.

Rejected alternatives, measured:
- The isolated world's fetch matches the page byte for byte, but its reply is a renderer's claim, and the
  engine needs a sender when no frame is open.
- A main-world fetch is visible to the page's service worker, which can forge the reply.

## Sends that speak as a document

The trusted zone can make its own fetch carry the request headers a document of a given identity would
send. An identity is a document: its origin and URL, and its cookie partition key. A frame's identity
comes from browser-stamped facts (`webNavigation`'s record and `cookies.getPartitionKey`, by
`documentId`). An engine instance's identity is the cluster origin the zone assigned and the URL it loaded.
Three callers speak as a document:
- every page request `bridge.js` or `engine/trusted.mjs` hands `safeFetch` (`pageRequestIdentity`). These
  are uncredentialed under the egress default, so they carry the identity's Origin, Referer and
  `Sec-Fetch-*` and no cookies;
- every credentialed document load `bridge.js` makes (`navigationIdentity`), as the request's client: a live
  sub-frame's seed as its parent frame (`frameIdentity` by the browser's `parentDocumentId`), a live
  top-level seed as its own frame, a child navigable's load as the document that holds it, and a document only
  the zone holds as its `engineIdentity`. The document that reads the reply stays the load's principal;
  `safeFetch` lets the two differ only for a navigation;
- every operator send (above).
Neither a message nor the engine can state an identity. `safeFetch` `CHECK`s that a credentialed request
has one, and the zone's own fetch always omits credentials: the extension's jar, whose partition is the
extension's, is never sent, and a reply's `Set-Cookie` is stored nowhere.
- `lib/request-identity.js` and `lib/speak-as.js` compute the headers:
  - `Origin` by Fetch §3.2 "append a request `Origin` header";
  - `Referer` by Fetch §5.4's referrer rule, so a referrer not same-origin with the identity is "client",
    and then Referrer Policy §8.3;
  - the cookies the identity would send, by RFC 6265bis §5.2 and §5.8.3;
  - `Sec-Fetch-Site`/`-Mode`/`-Dest` by Fetch Metadata §2;
  - `Sec-Fetch-Storage-Access` by Storage Access Headers §4.1.
  A preflighted send's OPTIONS request gets Fetch §4.8's own set.
- `speak-as-rule.js`, in the service worker, sets them through one `declarativeNetRequest` session rule per
  method the send puts on the wire. The rule is scoped to `tabIds: [TAB_ID_NONE]`, `initiatorDomains` (this
  extension), the method, and the exact URL (an escaped, anchored `regexFilter` when the URL holds `*`, `^`
  or `|`). Measured, a tab's own request to the same URL during a rule arrives untouched, and rules scoped to
  another initiator, tab or method do not apply.
- Only the offscreen document reaches the install arm (`__rpc`, `sender.url === OFFSCREEN_HREF`), and it
  sends the computed headers with the identity's origin. The service worker CHECKs that `Origin` equals that
  origin (or is `null`), and that `Referer` is absent or same-origin with it. A send can therefore speak
  only as an origin a document has. It cannot name an arbitrary site, which a bare rule can: measured, a
  rule set `Origin: https://bank.example`. The one exception is an operator send's stated `Origin` or
  `Referer` (above), which the install names as `stated`.
- The zone runs one send per URL at a time. That send's rules are installed together, the zone CHECKs that
  they are exactly the rules installed for the URL, and they are removed in a `finally`. So a preflight and
  its request are one unit, and two identities never share a rule.
- The offscreen document removes every rule of the family before its first send, because session rules
  outlive it. The service worker does not do this at its own start: it can be evicted while a send is in
  flight.
- `engine/trusted.mjs` has no service worker and no `declarativeNetRequest`. It keeps the same rules in a
  table, checked by the same `speakAsCheckedRules`, and sets them on its own fetch, which sends any header.

Residuals, each a difference from the document's own request:
- The header names a rule adds arrive lower-case and after the browser's own headers.
- The order between unpartitioned and partitioned cookies of equal path length is not the browser's.
  `chrome.cookies` states no creation time.
- The storage access status is assumed `active`.
- A reply's cookies are not stored. The zone's fetch omits credentials on every send, so a `Set-Cookie`
  never lands in a jar, the document's included ("Known residuals").
- The partition bit is computed as the request's own cross-site status, with the frame's
  `hasCrossSiteAncestor`. The person's third-party-cookie setting is not read.
- A redirect leaves the rule behind. The rule matches the first URL only, so a redirected request goes out
  as this extension.
- An operator's frame policy is not observable from this zone, so an operator send that states none uses the
  default policy, strict-origin-when-cross-origin. An engine page request carries the referrer and policy the
  engine states on its request record, and a referrer not of the identity's origin is "client" (Fetch §5.4).
- An iframe srcdoc document's referrer source, its container's URL, is not held.
- The document's service worker does not see the send.
- The document's CSP binds an engine page request inside the engine, not here. `fetch_main_blocked`
  (Fetch §4.1 "Main fetch" step 7) runs for `fetch()`, XHR, images, links and scripts before a pending line
  exists, and `fetch_main_report_csp` (step 4, "report Content Security Policy violations for request")
  reports the list's report-only policies first. A request whose URL is concolic is not checked, since no
  policy matches a shape. An operator send is not checked against the frame's policy at all.

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
| Operator sends as a frame (`safeFetchOperator`) | the person, in the popup | the frame's own, by its partition and the request's credentials mode | the popup-sender CHECK; the initiator grade; Fetch's own refusals and §4.8 preflight; forbidden request-headers refused; a stated Origin or Referer only from the Send panel's fields, refused unless well-formed |
| Renderer program assembly (`renderer-host.js`) | a constant list off the extension origin | none | no bundle influences it; a bundle-influenced name would make it analyzer HTTP |
| The page's own traffic through `intercept.js`'s `fetch`, `XMLHttpRequest`, `WebSocket` and `EventSource` wrappers | the page | the page's | none needed: the wrapper calls the original and confers nothing |
| Operator writes into a channel the page opened (`WS_SEND_MSG`, `PM_SEND_MSG`, `MC_SEND_MSG`) | the page opened the socket, port or window | the page's | the router's document→document rule; `documentId` routing; a postMessage target must be one real origin (`_isRealTargetOrigin` in `lib/popup-console.js`, DCHECKed in `lib/popup-handlers.js`) |
| Live-verify attacker tab (`attackerPage.open`: the service worker's `tabs.create` of its own `ATTACKER_PAGE_URL`) | a constant | the person's profile | one GET of a fixed documentation page |
| Live-verify navigation (`window.open` of the delivery address) | the page the sink was observed on, plus a fragment or query the engine declared | the person's profile | the address (see Live verify); the person's click |

The operator's channel writes are the one send outside `safeFetch`, and the reason is the reply's path: a
frame, port message or postMessage written into a channel the page holds is answered into the page's own
receiving code, which a channel the extension opened would never reach. They name no new destination (the
page chose it) and carry the page's own credentials, so they change no signal the chokepoint computes.

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
| `destination` | `program`, `subresource`, `value` | stated (engine) |
| `actor` | `page`, `tool` | certain: a literal in trusted-zone source at every caller, narrowed to `tool` by the engine's drive mark or the document record's |
| `provenance` | `observed`, `derived`, `forced` | stated (engine) |
| `doc-reach` | `observed`, `derived`, `forced` | the zone's document record (`lib/document-record.js`) |
| `cookies` | `yes`, `no` | certain |
| `header-authority` | `none`, `unknown` | certain |
| `witness` | `pinned`, `unpinned`, `unstated` | stated (engine), floored at `pinned` by the document record |
| `url-authority` | `present`, `unknown` | partial: a miss is `unknown`, never "none" |
| `lineage` | `unknown` | undetermined |
| `method` (does not gate) | `GET` | certain |
| `invalidity` (does not gate) | `unstated` | intent |

`actor` is the one gating row the untrusted zone cannot reach; the `stated` rows are words the
attacker-controlled engine composes and the chokepoint `CHECK`s only for vocabulary.

Every request also carries the zone's record of the document it was made from (`lib/document-record.js`):
the navigation's reach grade, its drive mark, its witness mark, and HTML §7.1.5's creation sandboxing flag
set. The zone writes one per document when it provisions it, from the provisioning notice composed with the
creating document's record by HTML's own rules, so a document inherits every mark above it and every flag the
rules carry. A record is frozen, minted only by `docRecord`, and written once per document
(a second write is a `CHECK` in the zone). `docRecordJoin`, the one function that derives a record from
others, `CHECK`s that the result clears no mark and strengthens no reach, so nothing a document does relaxes
what its navigation carried. The chokepoint reads the record as a floor under the engine's per-request words:
`doc-reach` is its grade, a `page` request from a driven document reads `actor=tool`, and one from a pinned
document reads `witness=pinned`, whatever the engine's record states. The engine's words can narrow these rows
and never widen them, so an engine that states `undriven` or `unpinned` for a driven or pinned peer's requests
does not launder them into the page's own.

A request is judged under its asking document's record. The engine names the asking document on the pending
line (`ASKERS`, every document whose park the line folds) and on the host request line, and the zone answers a
name with the record it wrote when it provisioned that document; for a document the engine created inside its
own heap, which the engine names `<creator>.<n>`, it answers with the nearest provisioned ancestor's
(`docRecordOfAsker`). A name whose walk meets another instance's document, or no provisioned document at all,
is refused with `blocked-asker:<role>-<reason>` (`docRecordAskerRefusal`), which says which document failed and
why: the `asker` of a request, the `creator` of a create, the `issuer` of a provisioning notice or the
`navigable`'s current document, each `elsewhere` or `unprovisioned`; a create whose child name is not minted from
its creator is `blocked-asker:child-not-minted`, and a navigation whose new name has no creator component is
`blocked-asker:document-not-minted`. A provisioning notice is judged the same way by the document that issued
it: a create by its creator, a `navigable.swap` or `navigable.navigate` by HTML §7.4.2.2's sourceDocument (whose record the
new document's joins with that of the navigable's current document, the one its name is minted from), and a
`document.seed` by the declaring document. An instance has no record of its own. A line that folds several
parks is judged under the meet of their records (`docRecordMeet`), because one reply answers them all and the
engine folds the line's own marks toward the most permissive member in the same way.

The flag set follows §7.1.5 and §7.3.1.7 exactly. A nested navigable's set is the union of the notice's set
(the engine's computation of its `<iframe sandbox>` attribute and its embedder's active set) and the creating
document's record, so no frame regains a permission any sandbox above it removed. An auxiliary navigable (a
notice with no parent) is refused outright when the creator's record carries the sandboxed auxiliary
navigation flag, and otherwise starts from the notice's set, plus the creator's whole set only when the
creator's carries the sandbox-propagates flag. So `allow-popups-to-escape-sandbox` lets a popup out only when
every sandbox above the creator allowed it. The one branch that is not the spec's: the risk marks always
propagate, popups included, because they describe this tool's act and no attribute the page writes can opt
out of it. The zone hands the engine the record's flag set, never the notice's, so a notice that drops a flag
its creator's sandbox carries is not believed. A signal the chokepoint
cannot determine is shown as `unknown` rather than left out, because an absent row reads as a question that was
answered. `_signalRegistryCheck` refuses a duplicate signal, an unknown reliability grade, a non-gating signal
with more than one value, and a default arm naming a value the registry does not have.

The four default arms (`_DEFAULT_ARMS`, shown to the person by `safeFetchDefaultArms`) are the page loading
itself as a browser would: a program load; any other subresource; a request with `provenance` and `doc-reach`
both `observed` (this is also what fires the seeded document load); and the analysed page's own request whose
address no fork pinned (`destination=value`, `witness=unpinned`, `actor=page`). A navigation reads
`destination=value`: a new navigable, like a postMessage, is judged by the same rows as a `fetch()`. Probes,
discovery and tool-composed requests fire only at an origin a person widened. The witness mark is independent
of the provenance: a forced path that pinned nothing fires, and a `derived` path that pinned a value does not.

Beside the provenance, every engine record of a request states a drive mark, `driven` or `undriven`
(`PENDING_DRIVEN_*`, read off `flow_path_driven` and asserted at `endpoint_record`): `driven` means the request
was minted inside a flow driven by this tool: a drive of a function the page never called, or a candidate run
that re-executes the page with a payload the engine chose (an @S breakout, or a witness value under
verification). It is a separate fact from the provenance, because a drive that forced no branch computes its
values from real inputs and stays `derived`; a candidate's path is marked forced and pinned at the substitution. The
`actor` row reads it alone: a `page` request that is `driven` reads `tool`, so the page-request arm does not
admit it, while a program or subresource load a drive reaches still fires by its destination
arm. The mark can only narrow `actor`, never widen it, and a `page` caller must relay it (`_drivenOf`).

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
- `declarativeNetRequestWithHostAccess` exists so a zone send can carry a document's headers (below, "Sends
  that speak as a document"). It is the variant that acts only where host permission exists, and that adds
  no install-time warning. `speak-as-rule.js` is its only caller, and every rule it installs passes the
  identity CHECKs there.
- `manifest.json` requests `offscreen`, `cookies`, `declarativeNetRequestWithHostAccess`, `webNavigation`,
  `scripting`, `tabs` and `unlimitedStorage`, and not `storage`, so `chrome.storage` is undefined in every context and a compromised renderer cannot read
  keys through it. `unlimitedStorage` lifts the origin's quota and grants no read API. The security claim is that
  `git grep -n 'chrome\.storage' extension/` finds no call site, only comments.
- `cookies` exists so a send that speaks as a document can carry exactly that document's cookies
  (`lib/request-identity.js`). It reads the jar with `cookies.getAll`, partitioned cookies through a
  `partitionKey`, and asks `cookies.getPartitionKey` for a frame's key. Only the service worker's `__rpc`
  arms call it, and those serve only the offscreen document (`OFFSCREEN_HREF`). No arm writes or removes a
  cookie. The grant is read access to every cookie under `<all_urls>`, so a hole in the `__rpc` sender gate
  exposes the person's whole jar.
- The service worker holds no learned state and relays no page data. Content scripts message the offscreen
  directly, so the service worker cannot launder a renderer's message into an extension-origin one.
- The engine frame has an opaque origin and keeps nothing between sessions; parked work persists through the
  offscreen's frontier.

## The engine is attacker-controlled

- It cannot read or set a principal. Principals live in the offscreen, a different document, and `bridge.js`
  passes them to `safeFetch` per call. It has no network and no `chrome.*` (see the engine frame, above): its
  `fetch()`, XHR and `import()` targets are untrusted URLs the chokepoint judges.
- One instance per origin-keyed agent cluster, `(browsing-context group, origin)`, never per tab or per page.
  `clusterKeyOf` in `bridge.js` builds the key from the message's `groupId` and `origin`. For a document the
  browser reported, both halves are browser-stated: the group is `_browserFacts`' `tabId` (`sender.tab.id`)
  and the origin is `_senderOrigin`'s, so two sandboxed iframes of one page are two clusters. A document the
  zone provisions itself takes its creator's group, or a group the zone mints for a `noopener` auxiliary, a
  swap, a seed or a cold resume, and never one the engine states. Its principal comes from its record
  (`principalOf`, HTML §7.3.2.1 "Creating browsing contexts"' determine the origin): a record carrying the
  sandboxed origin flag gets a fresh `null:` token, so it is its own cluster, never joins an instance of its
  URL's origin, loads and fetches with no session, and speaks to pages as `"null"`. The engine computes
  `"null"` from the same flag set (`qjs_init`). A same-origin child is a second realm in its creator's heap (`child_in_this_agent` in `navigable.c`); a second cross-origin document of
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
- `EXPLOIT_PROBE_ARM` runs `armAttackerPage`: the service worker's `attackerPage.open` opens its own
  `ATTACKER_PAGE_URL` (`https://example.com/`) and records the tab in `_attackerTabs`; after a wait for the tab
  to complete, `scripting.exec` with the named injector `pocAttackerButton` (`background.js`) renders a Run
  button in the attacker document's `MAIN` world. `scripting.exec` `CHECK`s that the tab is one
  `attackerPage.open` opened and is still open, and injects its top frame only. The offscreen selects the
  injector by name and passes only serializable arguments, so it cannot inject code of its own composing, nor
  name the address or the tab it runs in. The record is the worker's memory, so after an eviction an injection
  is refused rather than allowed. The person's click is the user activation `window.open` needs.
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
- `popup.html` and `dev.html` require Trusted Types with no policy (`require-trusted-types-for 'script';
  trusted-types 'none'`), so every HTML sink throws when handed a string. Page-derived text reaches them only
  as text nodes and allowlisted attribute values built by `lib/dom-builder.js`, which parses no markup;
  `domLink` is its one writer of `href`, and only for an http(s) URL. A page-derived string can still read as
  spoofed text, or be a link the person might click.
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
| A credentialed send's reply cookies | A send that speaks as a document carries the identity's cookies, with the zone's fetch omitting credentials, so the reply's `Set-Cookie` is stored nowhere, where the identity's own request would store it in its jar. This extension holds no `webRequest` permission, so it cannot read them (with it, `onHeadersReceived` reports the offscreen document's replies; `Set-Cookie` under `extraHeaders` is not measured); letting the browser store them applies the extension's own storage rules and writes the real jar first | the `webRequest` permission and its `onHeadersReceived` with `extraHeaders`, then RFC 6265bis §5.7's storage model run on the identity's site for cookies | a server that rotates a token on a tool-sent credentialed request leaving the person's next own request presenting the retired value |
| A speak-as send's redirect hops | A send that speaks as a document goes out under a declarativeNetRequest rule naming its URL, so each redirect hop goes out as this extension: uncredentialed, with the zone fetch's own `Sec-Fetch-*` and no Referer. The offscreen document cannot read a redirect's `Location` or headers (`redirect: "manual"` answers an opaque-redirect with none, measured in Chrome 148). This extension holds no `webRequest` permission. With it, Chrome 148 reports the offscreen document's fetches to the service worker (measured with a scratch extension): `onHeadersReceived` with `extraHeaders` gives a 3xx's `Location`, `onSendHeaders` gives the Cookie each hop sent, and a declarativeNetRequest `block` on the send's exact URL with `responseHeaders: [{header: "location"}]` stops the platform following, so the hop is never sent. Re-sending a hop the platform already followed is declined: it is a second request on someone else's server | each hop followed by the zone itself: the platform's follow blocked per send as above, the `Location` read from `webRequest`, every gate run on it, and the hop sent as its own request under its own speak-as rule. It needs the `webRequest` permission, which this extension does not take | a redirected document loaded logged out, its hop reaching the server with `Sec-Fetch-Mode: cors` and no Referer |
| Private network by name (write side unmitigated) | `_ipAddressSpace` classifies IP literals by Local Network Access's "Non-public IP address blocks" table and the names `localhost`, `*.localhost` and `*.local`; any other name is `public`. A name that resolves to a local or loopback address (an intranet name, `.internal`, `.lan`, a single-label host, `127.0.0.1.nip.io`) is therefore sent from a public page, including by the default arm for the page's own unpinned `fetch()`. The reply is unreadable unless that host answers `Access-Control-Allow-Origin` for the page or `*` (`_corsCheck`). No stable MV3 surface resolves a name before the connection: `chrome.dns` is Chrome Dev only, `webRequest` reports the address only on events after the request was sent, and blocking `webRequest` is policy-installed only. Whether Chrome's own Local Network Access check applies to this extension's offscreen fetches is unmeasured | Chrome's local-network gate shown, in real Chrome, to apply to this extension's fetches, or a resolver this zone can ask before connecting | a page analysed from a public origin causing a request to a host on the person's network |
| Redirect hops between the first and last URL | `safeFetch` sees only the requested and the landed URL, so a redirect chain that leaves the origin and returns is tainted `basic` where Fetch would taint it, and an intermediate redirect response's own CORS check is not run | each hop recorded per request. `fetch`'s `redirect: "manual"` cannot supply it: it answers Fetch §2.2.6 "Responses"' opaque-redirect filtered response (status 0, empty header list, null body), so the `Location` is not readable there; `webRequest` reads it, under a permission this extension does not take (see "A speak-as send's redirect hops") | a `cors` reply delivered `basic` after a chain that crossed origins |
| The request carries no page `Origin` header | `Origin` is a forbidden request header, so the zone's request carries this extension's origin or none, and a server that reflects the request's `Origin` answers an `Access-Control-Allow-Origin` that is not the page's. The page then gets a network error where a browser's request would pass: a narrowing | a transport that can state the page's origin | a cross-origin API that works in a browser failing `blocked-cors:allow-origin-mismatch` here |
| No ORB for a no-cors element's bytes | CORB applies to script-like destinations only, so a cross-origin `no-cors` image, style or font body of any type is delivered to the engine; page script cannot read it, but a compromised engine could | Chromium's ORB rules for the other no-cors destinations | a JSON or HTML body delivered for an `image` destination |
| A browser-reported local-scheme frame's address space | A local-scheme document the zone provisions takes its initiator's address space from the initiator's record. A local-scheme frame the person's browser loaded (`content.js` runs in a `data:`, `blob:` or `about:` frame under `match_origin_as_fallback`) reaches the zone as an ambient seed with no record of its initiator, so it gets a new container's space, public, where HTML §7.1.7 "Policy containers" and Local Network Access §3.5 give it a clone of its initiator's. Its requests to that initiator's private network are refused: a narrowing. No browser-stated fact names the initiator. `runtime.MessageSender` states the frame's own `url`, `origin`, `frameId` and `documentId`, and `webNavigation.getFrame`/`getAllFrames` add `parentFrameId` and `parentDocumentId`, read from Chromium's API schemas and not measured for such a frame. The parent is not the initiator when a sibling or an opener navigates the frame. The frame's origin does not fix the initiator's container either: FileAPI §8.3.2 "Access restrictions on blob URLs" exempts top-level navigations, so a public page can load a loopback origin's `blob:` URL and its `about:` children then carry that origin under the public page's container. Either fact would widen | a browser source that names a navigation's initiating document; the DevTools Protocol under the `debugger` permission is the unmeasured candidate, which this extension does not take | `blocked-local-network:<space>-from-public` on a request from a local-scheme frame of a loopback or local page |
| Redirect to a less public host | `safeFetch` re-judges the landed host and refuses to ingest, but the hop that reached it was already sent | a walk judging each `Location` before it is followed, the hop-by-hop follow "A speak-as send's redirect hops" names | a public host's 3xx onto an intranet address appearing in that host's logs |
| Credential lineage | the chokepoint sees the credential state of the request in front of it and nothing about where the address came from | a `credentialed` bit on the response the engine ingests, carried into the concolic value's source identity and composed at the park beside the provenance word | every request at an origin reading `lineage=unknown`, including addresses the bundle built from a logged-in reply |
| Authority in a path segment | `url-authority` matches capability-URL parameter names and the JWS prefix only; an opaque token in a path reads `unknown` | read the signal off the engine, whose park knows which source each segment came from | `url-authority=unknown` doing all the work at a widened origin whose addresses carry capability tokens |
| Engine-stated signals | `destination`, `provenance` and `witness` are the engine's words, so an engine that states `observed` or `unpinned` falsely for a request made on its own path reaches the default arms. A document's own drive and witness marks are not this case: the zone's record floors them. What fires is still an uncredentialed GET under every other gate | none in this design; `actor` and the document record are the parts that do not rest on the engine | requests firing by default at an unwidened origin whose provenance the run's own log contradicts |
| The control is the safety | with nothing refused at every setting, a person who ticks a row without reading it permits its consequence | the surface stating each row's consequence and reliability (built); nothing removes the person's choice | a widened origin firing requests its owner did not expect |
| Same-document probe-hit fabrication | `window.apiclientsink` lives in the delivered document's `MAIN` world, and every fact the hook could report about its caller comes from that world | irreducible by this mechanism | a fired verdict for a sink that never fired, on a page that reads its own address |
| Hit attribution is not tied to the delivery witness | `_recordProbeHit` latches the first top-level document of the target origin to fire the marker; it does not require that document to be the one `_recordProbeDelivery` witnessed | compare `facts.documentId` against `ses.delivered.documentId` | an attributed hit whose `documentId` differs from the session's `delivered.documentId` |
| Fragment deliveries may not be witnessed | a witness needs the payload's component in `MessageSender.url` or in the seed address; if both drop the fragment, a `#` delivery latches nothing | `content.js` ships the document's `location.href` on `CONTENT_SEED` beside `seedUrl`, compared as a third source | a `#` delivery reporting not delivered with a nonzero `deliveredNearMiss` |
| A forgeable principal demand needs a chosen origin | `ATTACKER_PAGE_URL` is a constant, so a delivery whose victim checks `event.origin` against a shape is refused with the shape stated | a per-session attacker address derived from `principalGates`, plus the person's statement of which origins they control | a `forgeable` finding answering `pocWhy` where `none` findings beside it arm a Run button |
| Passive learning is page-claimed | `RESPONSE_BODY`, `CONTENT_FORM_SUBMIT` and the `sourceOrigin` / `targetOrigin` on `PM_RECV` / `MC_OPEN` records are built in the page's renderer, and any script there can dispatch the wrapper events, so a page can file records under any address or origin. They do not reach the engine as an `event.origin` | none at this layer; the popup labels them as claims (`_claimedOriginPair`) and refuses to reply to a non-origin (`_isRealTargetOrigin`) | records for an origin the person never visited, learned while on an unrelated page |
| Cross-origin child navigables load uncredentialed | `navigationCarriesSession` refuses the session cross-origin, because the chokepoint's credentialed gate asks whether the requesting principal may read the bytes, while a navigation's reader is another instance keyed on the response's origin | a document-load type in `safe-fetch.js` whose read principal is the response's own origin, not an `if` at the caller | cross-origin child documents loading in their logged-out form |
| Page requests are uncredentialed | `fetched` and `fetchedXhr` state `credentialed: false`, so the analysed page's own data requests see the logged-out reply. They already pass the page origin the credentialed SOP would need (`eng.origin`); the wrong origin would be `originOf(msg.sourceUrl)`, which gives a sandboxed iframe its embedder's | a per-origin credential decision over the request's own credentials mode | an app whose boot data differs by session analysed in its logged-out form |
| A credentialed request cannot carry a header list | `_credentialedOf` `CHECK`s the combination, because nothing states whose header list it is | an ownership statement carried from the site that knows, as `provenance` is | that `CHECK` firing |
| `same-origin` credentials mode where the zone is willing | `_credentialedOf` has no pre-request same-origin test, so it `CHECK_FAIL`s | the same-origin test on the request URL against `pageOrigin`, read before the wire | that `CHECK_FAIL` firing; every willing caller today states `include` |
| Whether a navigable is nested or auxiliary is the engine's word | the zone reads the create notice's parent field to choose §7.1.5's union over §7.3.1.7's popup rule. Every fact it could read the kind from instead (the parent and opener fields, the `navigable.request` that precedes a child's create) is engine-stated too, because the analysed navigables exist only inside the engine. The creator's record bounds it: a frame stated as a popup sheds only what §7.3.1.7 lets a popup of that creator shed, which is nothing unless the creator's record has neither the sandboxed auxiliary navigation flag nor the sandbox-propagates flag, that is, unless every sandbox above the creator allowed `allow-popups-to-escape-sandbox`. The creator is itself the engine's word (the create notice's `creator`, checked only to be a document of the creating instance, per "Which document asked is the engine's word"), so the bound is the most permissive record among that instance's documents, and naming such a creator sheds its flags with no mislabel at all. The risk marks are not affected | a navigable kind and a creator the trusted zone observes, which needs the analysed frame tree to exist outside the engine | a frame's record without a flag the record of the document whose code created it carries |
| Which document asked is the engine's word | the asking document's name is engine-stated, so an engine can name any document of its own instance, and the floor for a request is the most permissive record among that instance's documents rather than their join. Documents of one agent cluster share one heap and call into each other's realms, so which of them made a request is a fact only that heap holds | none in this design; a name outside the instance is refused | a request firing under a record that the document whose code made it does not carry |
| A document the engine created in its own heap has its nearest provisioned ancestor's record | the zone never sees an in-heap creation's own words (a same-origin frame's sandbox attribute, a forced or driven navigation inside the instance), so such a document's requests are floored by its nearest provisioned ancestor's record and its own marks rest on the engine's per-request words | in-heap creations announced to the zone with their words, recorded as a provisioned document's are | an in-heap document's request firing at a default arm when the navigation that made its document was forced or driven |
| An auxiliary opened without `noopener` | it shares its opener's group but lands in another tab, so `clusterKeyOf` splits the pair; `MessageSender` carries no opener | a browser-stated opener fact joined to the opener's group | an opener and its popup analysed in two instances |
| Dev-build asserts on renderer-supplied input | `renderer-host.js`'s window handshake DCHECKs the frame's record. In dev a compromised renderer can make those handlers throw; in release a malformed reply reaches its consumers unvalidated | refuse a malformed record with the handshake's own error instead of asserting on it | a dev build throwing at those sites for a page's malformed reply |
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
| A compromised renderer forges an operator's reply | not a threat: an operator send is the offscreen document's own fetch and never passes through the renderer |
| A compromised extension page sends as an observed frame | residual: the operator tier trusts every extension document, which can also state any `Origin` and `Referer`; the pages' Trusted Types policy and CSP bound it |
| A compromised renderer drives the socket, port or window send relays | not a threat: it cannot reach them, and the channel is the page's own |
| An automatic caller in the trusted zone makes an operator send or widens an origin | mitigated: the operator tier `CHECK`s an extension-document sender and `PAGE_CONTEXT_USER_INITIATED`, and `AST_EGRESS_POLICY` `CHECK`s the initiator; the automatic sweep is routed to `safeFetch` |
| A page widens its own origin for exploration | mitigated: `EGRESS_POLICY` is on the command surface the router gates on the extension origin |
| A grant for one origin fires requests at another | mitigated: the table is keyed by the serialized tuple origin and compared with the request URL's own `.origin` |
| A request arrives before the person's grants are restored | mitigated: `safeFetch` awaits the restore registered with `safeFetchEgressStating` |
| A grant from the single-switch control covers signals that did not exist when it was made | mitigated: the restore drops it and names it; an unnamed signal is not permitted |
| An uncredentialed request carries the person's authority in its address | partially mitigated: `url-authority=present` needs its own permission; a miss reads `unknown` |
| An address computed from a credentialed reply is fetched uncredentialed | not mitigated: `lineage` reads `unknown` for every request |
| Another document (other origin, other frame, second document of the target origin) calls `apiclientsink` with a marker it read | mitigated: `_recordProbeHit` records it `attributed: false` with its reason |
| The delivered page calls `apiclientsink` itself | residual, irreducible by this mechanism |
| A compromised renderer reaches `chrome.tabs.*` or `chrome.scripting` through the service worker | mitigated: the service worker serves only the extension origin and pins `__rpc` to `OFFSCREEN_HREF` by equality |
| A hole in that `__rpc` gate arms the live-verify injector in one of the person's tabs | mitigated: `scripting.exec` injects only into a tab `attackerPage.open` opened at the worker's own `ATTACKER_PAGE_URL`, a `CHECK` on the worker's record |
| A sandboxed extension page impersonates a trusted extension document | mitigated: its origin is `"null"`, which fails every document→document check |
| The bundle escapes the WASM sandbox into its host realm | out of model; the realm is `renderer.html` at a unique opaque origin with no `chrome.*`, no principal and no `connect-src`, reachable to the trusted zone only through validated `content.mojom.Renderer` records. See the residual on channels CSP does not govern |
| A second instance is provisioned for an agent cluster that already has one | mitigated: `registerRenderer` refuses it with a `CHECK` in dev and release |
| Injected markup in the popup runs script or exfiltrates | mitigated: the popup parses no page-derived markup (Trusted Types with no policy throws at every HTML sink), and its CSP refuses inline script and off-origin images, styles and forms; spoofed text or an http(s) link remains possible |
