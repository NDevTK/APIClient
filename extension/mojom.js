/* mojom.js — THE INTERFACE DEFINITIONS. This is the `.mojom` file: the one description of what may cross a
 * process boundary here, loaded by EVERY realm that speaks the transport (the offscreen document, the browser
 * process's Worker), because an interface only exists if both ends agree on it. Chromium splits exactly here —
 * `mojo/public/js/bindings.js` is the machinery (extension/mojo.js) and a generated `*.mojom.js` is the
 * description — and the split is what stops a capability from being a pair of hand-written switch arms that
 * drift.
 *
 * WHY EVERY PARAMETER CARRIES A `why`. It is not a comment: mojo.js's validator PRINTS it when the value is
 * wrong. Both superseded transports held a `checkHeaderFacts` whose real content was these sentences — that an
 * absent `Content-Type` is §5.1's undefined supplied type and says so with null, never with "" — written once
 * per side and free to drift. Here the sentence lives with the DECLARATION, so both ends print the same one and
 * neither can hold a different rule.
 *
 * THE PROCESS TOPOLOGY THESE TWO DESCRIBE, and it is the one the platform forces rather than one chosen:
 *
 *   • The OFFSCREEN DOCUMENT is the BROWSER PROCESS. It owns what a browser process owns — the network
 *     chokepoint (`lib/safe-fetch.js`), the routing between instances, and the renderer REGISTRY that decides
 *     which renderers exist and mints their routing ids (`extension/render-process-host.js`, Chromium's
 *     `RenderProcessHost`). It is the one fully-trusted zone (SECURITY.md), it is the only zone that can
 *     create a frame, and it is JAVASCRIPT deliberately: it arbitrates between renderers of DIFFERENT
 *     ORIGINS, so a memory-corruption bug in it would be a cross-origin boundary failure.
 *   • A RENDERER is `extension/renderer.html` in an `<iframe sandbox="allow-scripts">` with no
 *     `allow-same-origin`, so its origin is unique and opaque and Site Isolation may put it in its own OS
 *     process. It runs the untrusted bundle, and `content.mojom.Renderer` is the whole of what it may be asked.
 *
 * THERE IS EXACTLY ONE PROCESS BOUNDARY HERE, WHICH IS WHY THERE ARE EXACTLY TWO INTERFACES. Three more stood
 * in this file — `network.mojom.ContentSniffer`, `content.mojom.RendererHost` and `content.mojom.Zygote` —
 * describing a dedicated Worker that held §7/CORB and the renderer registry in WASM and ordered the offscreen
 * back to materialize each frame. The offscreen WROTE that Worker's program, so it was never a trust boundary;
 * type sniffing is `safeFetch`'s again (CLAUDE.md §Architecture) and the registry is a `Map` in this realm, and
 * a mojom that declared a boundary between a component and its only caller is a description of a topology that
 * does not exist. A pipe is a PROCESS boundary or it is ceremony.
 *
 * AND `content.mojom.ChildProcess` IS THE RENDERER'S, ASKED OF IT AS A PROCESS RATHER THAN AS AN ABI. "How
 * much IPC does that process have open" is a question about a PROCESS, so it does not belong on an interface
 * that happens to carry the engine entries — which is also why it survived the deletion of the second child
 * that used to answer it identically.
 */
(function (g) {
  "use strict";

  DCHECK(!!g.mojo && typeof g.mojo.defineInterface === "function",
         "extension/mojo.js is not loaded in this realm — the IDL is validated as it is declared, so a mojom " +
         "file that loads first would install interfaces nothing ever checked");

  /* ── THREE INTERFACES STOOD HERE AND ALL THREE ARE DELETED, WHICH IS ONE RULING APPLIED TWICE.
     `network.mojom.ContentSniffer` declared `CheckCorb` and `ClassifyResource` over that program's MIME
     Sniffing, CORB, JSON-sniff, nosniff and resource-kind units; `content.mojom.RendererHost` declared the
     renderer REGISTRY over its registry unit; `content.mojom.Zygote` was the
     offscreen's half of the fork order those two needed, because the program holding them was a dedicated
     Worker and a dedicated Worker's global has no `document` to create a frame with.
     CLAUDE.md §Architecture settles the first: "TYPE SNIFFING STAYS IN JAVASCRIPT, in `safeFetch`, where
     SECURITY.md puts it" — the trusted zone READ the bytes, it answers once, and it STAMPS what it decided
     onto the reply record (`computedType`) so the renderer is told rather than left to derive a second answer.
     The same test settles the second: what belongs in the engine is what a FLOW needs mid-execution, whose
     answer must fork and park with the flow, and a `Map` from an agent cluster key to an integer forks with
     nothing and parks with nothing. It is `extension/render-process-host.js` — and the component whose whole
     job is to arbitrate between renderers of different ORIGINS is the one that most needs to be memory-safe.
     AND THE THIRD GOES BECAUSE THE OTHER TWO DID. A zygote exists to obey a process that cannot create a
     frame; with nothing left in that process, `Zygote` and `RendererHost` were a pipe between a component and
     its only caller, in one realm, on one thread — three thread hops per renderer, an admission decision that
     could suspend between its check and its write, and a MessagePort detached and re-adopted so that "the
     endpoint travelled through the deciding process" would be a true sentence. An interface exists only if
     both ends agree on it, and there are no longer two ends. */

  /* ────────────────────────────────────────────────────────────────────────────────────────────────────────
     THE RENDERER — the engine ABI, TYPED. This is the channel that carries nearly all of this extension's IPC
     and the only one whose peer SECURITY.md calls UNTRUSTED: the renderer EXECUTES the attacker's bundle. Until
     this interface existed it was also the only channel with no declared types at all — a generic envelope
     (`{fn, ret, args, bodies}`) relayed twenty ABI entries of varying shapes and every reply was parsed by hand
     in renderer-host.js, while the validator guarded the TRUSTED worker. That was backwards, and this is the
     correction: the validator now stands on the boundary the threat is on, and a record that does not match a
     declared type kills the CONNECTION (Mojo's ReportBadMessage) rather than being handed to a call.
     THE PARAMETER TYPES ARE FACTS, NOT CHOICES. Every one is read off `engine/host/main.c`'s own `QJS_EXPORT`
     body — `qjs_top_weight` returns a C `double`, `qjs_bundle_id` an `unsigned` the wasm hands back as an i32,
     `qjs_provide` a `(const char *body, unsigned body_len)` PAIR — and `engine/build.mjs`'s QJS_ABI is the
     authoritative list of which entries exist. The ORDINALS are that list's order.
     ONE INTERFACE FOR ALL TWENTY, WHICH IS AN ORDERING DECISION AND NOT A FILING ONE. Mojo orders messages
     within a pipe and across none; `Begin` may not overtake `Init` and `Step` may not overtake `Begin` — the C
     entries assert exactly that, on `g_ctx` and `g_begun` — so the sequence stays ordered by being one
     interface rather than by a rule somebody remembers.
     WHY THE DOCUMENT IS `array<uint8>` AND NOT THE TWO SHAPES IT REPLACES. The old envelope carried a document
     EITHER as a JS string (content.js ships a serialized DOM) OR as bytes (a child navigable's document is
     safeFetch's response body), and the untrusted frame ran a UTF-8 encode on the first. `qjs_init` takes ONE
     thing — a byte sequence and its LENGTH — so the wire says one thing and the encode happens once, in the
     zone that already holds the characters. The LENGTH is what makes `array<uint8>` the honest declaration
     rather than a spelling of a C string: a document may contain a 0x00 (HTML §13.2.3.5 "Preprocessing the
     input stream" defines its handling rather than forbidding it), and both zones used to assert that away —
     the wire carried the count and the C entry threw it out for a `strlen`.
     NO PARAMETER IS TRANSFERRED, and that is now a property of the TYPE rather than a caveat at a call site.
     mojo.js builds the transfer list from the declared types — that is what makes `handle<message_pipe>` a real
     pipe pass — and Mojo's `array<uint8>` is a COPIED byte sequence; the type for bytes that MOVE is
     `mojo_base.mojom.BigBuffer`, a different declaration. So "transfer this body" is not a flag on a call, it
     is a second byte type, and the ownership answer differs per parameter: `Init`'s document is ALSO retained
     by bridge.js as the cold recipe it writes to IndexedDB, so moving it would leave the cross-session frontier
     holding a page that parses to nothing, while `Provide`'s body is minted by safeFetch per call and has no
     second reference. One type declared for both would make one spelling mean two ownerships with nothing on
     the wire to tell them apart, which is the failure a validator exists to prevent.
     ──────────────────────────────────────────────────────────────────────────────────────────────────────── */

  /* THE ONE FACT EVERY REPLY CARRIES, declared ONCE and referenced by every method: a shared record cannot
     drift, twenty hand-copied ones can. It is a fact about the PROCESS rather
     than about any method — an ABI call is the only thing that can grow a wasm linear memory, so the moment a
     call answers is exactly the moment the number changed, and no extra round trip could be fresher. That is
     also why it is not a method of its own: a poll would report the memory as of the poll, which is a different
     instant from the one the caller is ranking against.
     IT IS A `double` AND NOT AN `int32`: a working set is a byte count that may pass 2^31 (a 4 GiB wasm memory
     is representable and an int32 would wrap it negative), and `uint64` would be a BigInt this transport does
     not carry. A double holds every integer to 2^53 exactly, and WASM memory is a whole number of 64 KiB pages,
     which bridge.js asserts where it reads it. */
  var WORKING_SET = { name: "workingSetBytes", type: "double",
    why: "HEAPU8.length — the view over this instance's ENTIRE linear memory, the one input to the trusted " +
         "zone's Level-1 RAM floor and the one fact it cannot read for itself: there is no Module in the " +
         "offscreen's realm any more, which is the whole point of the boundary. An absent one would read as " +
         "\"this instance occupies no memory\", which admits another engine against RAM already spent" };

  /* THE SEVEN ARGUMENTS THAT ROOT OR JOIN A DOCUMENT. `Init` and `Join` take the identical list because
     main.c's two entries have byte-identical C signatures, and they are a shared record for that reason rather
     than for brevity: it is ONE contract taken by two operations (root this agent at a document / add a
     document to the agent already running), so a change to what a document arrives with must reach both. */
  var DOCUMENT = { name: "document", type: "array<uint8>",
    why: "the document's BYTES, which is what qjs_init/qjs_join take (a pointer and a LENGTH — a document " +
         "may contain a 0x00 and the tokenizer has a rule for it per state) and what HTML §13.2.3.2's " +
         "encoding sniffing is defined over — the algorithm the engine owes, and this zone owes it the bytes " +
         "for. A string here is a zone that ran a decode it does not own" };
  var DOCUMENT_URL = { name: "url", type: "string",
    why: "§4.4's document ADDRESS and not its origin: the engine derives the origin from the address itself " +
         "(§4.7's serialization, its own url.c), so the principal and the address are one fact from one place. " +
         "Handing over a bare origin made every relative URL a bundle built resolve against the site root" };
  var DOCUMENT_ID = { name: "docId", type: "string",
    why: "the name this document is known by inside the agent — the key qjs_route and the notice stream name a " +
         "target document with. It is minted by the trusted zone for a root and by the ENGINE for a child it " +
         "creates (`<parent>.<n>`), which SECURITY.md permits precisely because it is only a name" };
  var DOCUMENT_HEADERS = { name: "headers", type: "string",
    why: "the response's HEADER FIELD LINES verbatim, which is what HTML §7.1.7 \"Policy containers\"' " +
         "create-a-policy-container-from-a-fetch-response is run over. The empty string is the positive " +
         "statement that this document had no response at all (an about:blank, a serialized DOM off " +
         "content.js), which differs from a response carrying no headers" };
  /* THE CREATOR'S POLICY CONTAINER, WHICH IS TWO PARAMETERS BECAUSE CSP §2.2 MAKES IT TWO THINGS. It is
     declared beside `headers` and never inside it: HTML §7.3.2.1 "Creating browsing contexts" sets a
     created Document's policy container to "a clone of creator's policy container", and a clone is not a
     response header. Relaying it as one is what this pair replaces, and the half that could not be relayed is
     the whole defect — CSP §2.2 "Policies" makes a CSP list "a struct consisting of policies (a list of
     policies) and a self-origin (an origin which is used when matching the 'self' keyword)", and §2.2.2
     "Parse response's Content Security Policies" fixes that self-origin to "response's URL's origin". So a
     policy delivered as a header is a policy whose `'self'` names the RECEIVING document, which for an
     inherited list is wrong in both directions at once. */
  var INHERITED_CSP = { name: "inheritedCsp", type: "string",
    why: "the serialized CSP list of HTML §7.1.7's CLONE of the creator's policy container, for a document " +
         "another instance's `navigable.create` announced. The empty string is a list with no policies, which " +
         "is NOT the same statement as 'no creator' — that one is made by the self-origin beside it" };
  var INHERITED_CSP_SELF_ORIGIN = { name: "inheritedCspSelfOrigin", type: "string",
    why: "CSP §2.2's SELF-ORIGIN of that inherited list — the CREATOR's origin, serialized, which §6.7.2.8 " +
         "\"Does url match expression in origin with redirect count?\" is what reads. It cannot be recovered " +
         "from the policy bytes (§2.2.2 states it from outside them) and the receiving instance cannot derive " +
         "it (its own address is the wrong answer by construction), so it crosses or `script-src 'self'` on " +
         "the creator permits the child's origin and refuses the creator's. AND IT IS THE FIELD THAT SAYS " +
         "WHETHER THERE IS A CREATOR AT ALL: §2.2 gives every CSP list a self-origin, so an empty one is a " +
         "Document with no creator (a root document, a rehydrated recipe, §7.3.2.3's swapped-to context) " +
         "while a non-empty one beside an empty policy is a real creator holding no policies. The engine " +
         "distinguishes them because a Document merges CSP §3.3's `<meta>` policies into that SAME list under " +
         "that SAME self-origin, so a `data:` child's `<meta>` policy resolves `'self'` differently in the " +
         "two cases" };
  /* HTML §7.1.4 "Cross-origin embedder policies"' EMBEDDER POLICY — the ITEM of that same §7.1.7 container,
     which travels as four fields because §7.1.4 makes a policy "a value, a reporting endpoint string, a report
     only value, and a report only reporting endpoint string" and §7.1.7's clone-a-policy-container moves every
     one of them ("set clone's embedder policy to a COPY of policyContainer's embedder policy").
     IT IS NOT A RESPONSE HEADER AND CANNOT BE RELAYED AS ONE, for the reason the CSP list beside it cannot: the
     item belongs to the CREATOR's response, and the child's own response is where a header would be read. A
     child provisioned without it is created claiming `unsafe-none` for a creator that opted into cross-origin
     isolation — an inheritance silently deleted, with no header anywhere on the child's side to disagree with.
     THE VALUES ARE §7.1.4's OWN TOKENS AND THIS ZONE DOES NOT INTERPRET THEM. The emitting engine writes
     `unsafe-none`/`require-corp`/`credentialless` onto its `navigable.create` notice and this relays the bytes;
     the receiving engine is the only party that turns one back into a value, and it CRASHES on a token naming
     none of the three rather than reading it as the default. */
  var INHERITED_COEP = { name: "inheritedCoep", type: "string",
    why: "§7.1.4's embedder policy VALUE of that inherited container, as one of the section's three token " +
         "strings. There is no empty spelling: a container that exists has an embedder policy, initially a " +
         "new one, so the absence of a creator is still said by the self-origin above and this field always " +
         "names a policy" };
  var INHERITED_COEP_ENDPOINT = { name: "inheritedCoepEndpoint", type: "string",
    why: "§7.1.4's reporting endpoint of that policy — the `report-to` parameter of the creator's header, " +
         "whose absence §7.1.4 spells as the EMPTY STRING and never as null" };
  var INHERITED_COEP_REPORT_ONLY = { name: "inheritedCoepReportOnly", type: "string",
    why: "§7.1.4's REPORT ONLY value of that policy, from the creator's " +
         "`Cross-Origin-Embedder-Policy-Report-Only`. It is a separate item because §7.1.4.2's embedder " +
         "policy checks reads it separately — its report-only arm fires where the parent's report-only value " +
         "is compatible with cross-origin isolation and the response's value is not" };
  var INHERITED_COEP_REPORT_ONLY_ENDPOINT = { name: "inheritedCoepReportOnlyEndpoint", type: "string",
    why: "§7.1.4's report only reporting endpoint of that policy. It is carried even though §7.1.4's own " +
         "obtain never writes it — BOTH of its branches set `policy's endpoint`, which is the section's own " +
         "text — because the item exists and a serialization that dropped an item is the defect this whole " +
         "record is shaped to prevent" };

  var TOP_LEVEL_URL = { name: "topLevelUrl", type: "string",
    why: "§8.1.3.1's top-level creation URL, which §8.1.3.5 reads to decide whether this realm is a SECURE " +
         "CONTEXT and therefore which of Web IDL §3.3.13's members exist in it. The engine refuses an empty " +
         "one, so a document reaching here without it is one whose API surface is silently smaller" };

  /* NO `version:` LINE, HERE OR BELOW, AND ITS DELETION IS A MECHANISM RATHER THAN A TIDY-UP. Both interfaces
     carried `version: 0`; mojo.js posted it on every bind and the peer asserted it — and it caught nothing,
     because nothing increments it and no change to a method list moves it. A mojom.js that gained a method,
     renumbered an ordinal or changed a parameter type shipped `0` on both sides, so the check compared 0
     against 0 and passed for exactly the skew it named itself as guarding against. Mojo's `version` is for
     interfaces that EVOLVE across independently-shipped components ([MinVersion], a peer that may legitimately
     be older); there is no such peer here — the renderer's mojom.js is the bytes the offscreen read off this
     same extension — so what has to be asserted is not a number somebody remembers to bump but that the two
     reads were one generation. mojo.js's `wireContract()` derives that from these declarations. */
  g.mojo.defineInterface({
    name: "content.mojom.Renderer",
    methods: [
      { ordinal: 0, name: "Init",
        params: [DOCUMENT, DOCUMENT_URL, DOCUMENT_ID, DOCUMENT_HEADERS, TOP_LEVEL_URL,
                 INHERITED_CSP, INHERITED_CSP_SELF_ORIGIN, INHERITED_COEP, INHERITED_COEP_ENDPOINT,
                 INHERITED_COEP_REPORT_ONLY, INHERITED_COEP_REPORT_ONLY_ENDPOINT],
        reply: [
          { name: "rc", type: "int32",
            why: "qjs_init's own return. Its C body is a wall of CHECKs whose failures abort the instance, so " +
                 "the only value it can produce is 0 — which is exactly why reading it costs nothing and why a " +
                 "non-zero is an entry that started reporting a failure this zone was not listening for" },
          WORKING_SET] },

      { ordinal: 1, name: "Join",
        params: [DOCUMENT, DOCUMENT_URL, DOCUMENT_ID, DOCUMENT_HEADERS, TOP_LEVEL_URL,
                 INHERITED_CSP, INHERITED_CSP_SELF_ORIGIN, INHERITED_COEP, INHERITED_COEP_ENDPOINT,
                 INHERITED_COEP_REPORT_ONLY, INHERITED_COEP_REPORT_ONLY_ENDPOINT],
        reply: [
          { name: "rc", type: "int32",
            why: "qjs_join's own return, on Init's rule — the entry CHECKs every precondition and aborts, so a " +
                 "non-zero return is a contract that changed" },
          WORKING_SET] },

      { ordinal: 2, name: "GetBundleId",
        params: [],
        reply: [
          { name: "bundleId", type: "int32",
            why: "qjs_bundle_id returns a C `unsigned` and the wasm hands it back as an i32, so what crosses is " +
                 "SIGNED and the reader takes `>>> 0` — declaring an int32 says that rather than hiding it " +
                 "behind a type the wire does not have. It is never 0: document_bundle_id folds an empty scan " +
                 "to 1 precisely so a 0 cannot key every unidentifiable document to one frontier entry" },
          WORKING_SET] },

      { ordinal: 3, name: "Begin",
        params: [
          { name: "recipes", type: "string",
            why: "the parked residue this frontier key last wrote to IndexedDB, replayed as decision vectors. " +
                 "The empty string is a FRESH frontier, which is a different thing from a resumed one that " +
                 "happened to hold nothing" }],
        reply: [WORKING_SET] },

      { ordinal: 4, name: "Step",
        params: [],
        reply: [
          { name: "code", type: "int32",
            why: "ENGINE_STEP_DONE (0), ENGINE_STEP_YIELD (2) or ENGINE_STEP_STALLED (3), and nothing else. " +
                 "The three are three different things to do next and the engine is the only zone that knows " +
                 "which: a yield asks to be OUTRANKED, a stall asks to be PAID, DONE ends the session. This " +
                 "declared two while the scheduler produced three, because the ABI folded the stall into the " +
                 "yield — a fold no host can undo, which cost the two-instance driver 10.8 million no-op " +
                 "steps against a peer that had already said what it was owed. The mirror defect is on the " +
                 "same field: a host that branched on a value the engine never produces (1, NEED_FETCH) left " +
                 "the whole qjs_pending->safeFetch->qjs_provide reply path unreachable in the shipped " +
                 "extension. Enumerate the codes; never default one" },
          WORKING_SET] },

      { ordinal: 5, name: "GetResult",
        params: [],
        reply: [
          { name: "result", type: "string",
            why: "the ONE result document, JSON, as result_json composes it — the host does exactly one " +
                 "JSON.parse of it and no consumer re-derives a field. It is never empty: result_json answers " +
                 "nothing only when the composition itself could not be allocated, which is this page's entire " +
                 "finding set being dropped" },
          WORKING_SET] },

      { ordinal: 6, name: "Teardown",
        params: [],
        reply: [WORKING_SET] },

      { ordinal: 7, name: "GetPending",
        params: [],
        reply: [
          { name: "requests", type: "string",
            why: "the REQUESTS flows are parked on — one `METHOD<TAB>URL` line each, newline-joined, \"\" for " +
                 "none, deduped by the PAIR. The field was `urls` and the list was addresses alone, which is a " +
                 "request named by half of itself: a page that issues a GET and a POST to one address parks " +
                 "two, and both settled with whichever body the zone fetched first. TAB can occur in neither " +
                 "half — URL Standard §4.4 URL parsing has the basic URL parser remove every ASCII tab or " +
                 "newline from its input before anything else, and Fetch §2.2.1 Methods makes a method a byte " +
                 "sequence matching the method token production, whose tchar (RFC 9110 §5.6.2 Tokens) is " +
                 "VCHAR-only and excludes HTAB. It is the grammar GetHostRequests already answers in, and an " +
                 "empty list is the empty string and never a NULL pointer turned into the four characters " +
                 "\"null\" and then into one bogus record" },
          WORKING_SET] },

      { ordinal: 8, name: "GetChunks",
        params: [],
        reply: [
          { name: "urls", type: "string",
            why: "the addresses the module loader is loading CODE from — a CLASSIFICATION of the GetPending " +
                 "list and not a second owed list, which is why it is still addresses and carries no method. " +
                 "Every one was recorded by module_load at the moment it PARKED the load, so each is already a " +
                 "GetPending line; fetching it a second time and providing it again answers a request that " +
                 "carries a reply, which is engine_provide's answered-twice DFAIL. What it decides is the CORB " +
                 "class — a body that becomes executable code is fetched `as:\"script\"` (SECURITY.md " +
                 "§Network), and a cross-origin HTML/JSON body must never be read as code. A record dropped " +
                 "here is a lazy chunk fetched as data, and CLAUDE.md calls that surface the headline moat" },
          WORKING_SET] },

      { ordinal: 9, name: "Provide",
        params: [
          { name: "method", type: "string",
            why: "the METHOD half of the request this answers, which GetPending named first on the line. The " +
                 "engine's pending register is keyed on the PAIR, so a reply carrying the address alone lands " +
                 "in `method` at the C entry with every later operand shifted — which is why qjs_provide " +
                 "DCHECKs the method's presence and engine_provide asserts the token production rather than " +
                 "matching nothing in silence. It is the request's " +
                 "identity and not a hint: the answer to a GET is not the answer to a POST of one address" },
          { name: "url", type: "string",
            why: "the TARGET half of the same request, matched with the method against what a flow parked on " +
                 "— the engine's own qjs_provide DFAILs on a pair nothing is parked on, which is what an " +
                 "invented record produces" },
          { name: "reply", type: "string",
            why: "the reply's METADATA as JSON, so it carries its type: a bare string could not say `null` for " +
                 "Fetch §5.6's network error without it being the four characters \"null\", and could not " +
                 "carry the URL list, the status or the headers at all" },
          { name: "body", type: "array<uint8>?",
            why: "Fetch §2.2.5's body is a BYTE SEQUENCE, and the decode this zone used to run (§5.2's text()) " +
                 "destroyed exactly the evidence HTML §8.1.4.2's classic-script decode exists to read. `null` " +
                 "is the network error — no body at all — which a zero-length body is not" }],
        reply: [WORKING_SET] },

      { ordinal: 10, name: "GetTopWeight",
        params: [],
        reply: [
          { name: "weight", type: "double",
            why: "the Level-1 WFQ's one input, and -Infinity is an ANSWER: engine_top_weight is " +
                 "`flow_next_to_run(NULL) ? flow_weight(b) : -1.0/0.0`, the engine's positive statement that " +
                 "its frontier holds no RUNNABLE flow — which this record has always claimed and which the C " +
                 "side only now answers, since it used to ask flow_best over every member including the ones " +
                 "waiting on the host — and the right statement, since that value ranks below every real " +
                 "weight by arithmetic rather than by a branch. Refusing it destroyed and re-provisioned every " +
                 "drained engine, 43279 times in one page load. NaN and +Infinity are still broken and the " +
                 "READER says so, because which non-finite values are meaningful is the consumer's vocabulary" },
          WORKING_SET] },

      { ordinal: 11, name: "SetYieldFloor",
        params: [
          { name: "floor", type: "double",
            why: "the runner-up engine's weight: this engine yields the instant its top flow is outranked, " +
                 "which is what makes the Level-1 interleave a VALUE yield and not a fixed slice. -Infinity is " +
                 "the floor for a pool with nobody else in it; a NaN would make every comparison false so the " +
                 "top flow never yields at all, which the caller asserts against before it gets here" }],
        reply: [WORKING_SET] },

      { ordinal: 12, name: "RequestPark",
        params: [],
        reply: [WORKING_SET] },

      { ordinal: 13, name: "EmitPartial",
        params: [],
        reply: [WORKING_SET] },

      { ordinal: 14, name: "GetHostRequests",
        params: [],
        reply: [
          { name: "requests", type: "string",
            why: "`id<TAB>op` per line — the flows suspended on something only the host can do. It deliberately " +
                 "does NOT dedupe (two identical questions from two flows are two questions), so a record " +
                 "dropped here is a flow parked forever on an answer nobody will send" },
          WORKING_SET] },

      { ordinal: 15, name: "HostAnswer",
        params: [
          { name: "request", type: "int32",
            why: "the id engine_host_answer walks every flow's register for. The engine's counter starts at 1, " +
                 "so a 0 answers a call site that does not exist and the answer is dropped on the far side" },
          { name: "answer", type: "string",
            why: "the answer as JSON, carrying its type across the seam exactly as a reply's metadata does" },
          { name: "completion", type: "int32",
            why: "ECMA-262 6.2.4's completion TYPE — 0 NORMAL, 1 THROW. An answer is a completion record and " +
                 "not a value: this zone fetched bytes rather than running another instance's program, so it " +
                 "has nothing to have thrown in, while a relayed cross-agent operation answers with 1 and the " +
                 "thrown value, which is what lets the asking page's try/catch run" },
          { name: "body", type: "array<uint8>?",
            why: "the fetched bytes beside the record, for Provide's reason — a Document is parsed from a byte " +
                 "sequence and XHR §3.6.6 decodes the RECEIVED bytes with the final encoding. `null` is the " +
                 "positive statement that this answer has none, which is what every request kind whose answer " +
                 "is a number or a document NAME is" }],
        reply: [WORKING_SET] },

      { ordinal: 16, name: "GetHostNotices",
        params: [],
        reply: [
          { name: "notices", type: "string",
            why: "the engine's one-way notices, newline-joined — a navigable created, a message posted, a " +
                 "cross-agent operation answered or HANDED BACK, a world of this instance ended, a PEER's " +
                 "world segment carried into this park's residue. This zone OWES each of them an action: a notice read and " +
                 "discarded is a document nothing runs and a message nothing delivers. The last two are a " +
                 "PAIR and the second is why the first has a deadline: `world.gone` is broadcast to the LIVE " +
                 "pool, and `world.parked` names the worlds a COLD document is still holding a segment for, " +
                 "so a death announced between them has nowhere to be delivered unless this zone holds it" },
          WORKING_SET] },

      { ordinal: 17, name: "Route",
        params: [
          { name: "record", type: "string",
            why: "the delivery record verbatim, in the engine's own grammar — this zone routes TEXT, because " +
                 "only an engine knows what a name in another agent's namespace means" },
          { name: "senderOrigin", type: "string",
            why: "the SENDER'S ORIGIN, stamped by the TRUSTED zone and never by the untrusted engine. " +
                 "SECURITY.md: a forgeable event.origin defeats every origin check in every bundle the engine " +
                 "analyses — it would report exploits that are not real and miss ones that are — so the engine " +
                 "never supplies one, and this parameter is where the trusted statement crosses" }],
        reply: [WORKING_SET] },

      { ordinal: 18, name: "Perform",
        params: [
          { name: "token", type: "string",
            why: "the rendezvous token this zone minted for the asking flow. The peer answers BY RUNNING A " +
                 "PROGRAM on its own frontier, so the completion does not exist when this call returns and " +
                 "arrives later as a notice echoing this token verbatim" },
          { name: "record", type: "string",
            why: "the cross-agent operation, in remote_object.c's grammar — a member whose value is an OBJECT " +
                 "crosses as a NAME in the answering agent's namespace, which JSON could only either " +
                 "serialize (returning something that is not the thing) or drop" }],
        reply: [WORKING_SET] },

      { ordinal: 19, name: "HostAnswerRemote",
        params: [
          { name: "request", type: "int32",
            why: "the host-request id of the flow that asked, on HostAnswer's rule" },
          { name: "world", type: "string",
            why: "which of the answering document's TIMELINES computed this completion, in world_serialize's " +
                 "grammar and relayed verbatim off the notice that carried it. A peer's document state IS its " +
                 "flows, so one question has N true answers — and this field is the only thing that tells a " +
                 "SECOND timeline (a fork the asking flow owes) from ONE timeline's answer delivered TWICE (a " +
                 "relay defect). Without it both look identical and the receiving engine cannot assert on " +
                 "either" },
          { name: "completion", type: "string",
            why: "the peer's completion record, RELAYED WHOLE and unread for Perform's reason. An empty one is " +
                 "not `undefined`, it is a relay that lost the peer's answer, and the engine's own decoder " +
                 "says so at the other end" }],
        reply: [WORKING_SET] },

      { ordinal: 20, name: "WorldGone",
        params: [
          { name: "world", type: "string",
            why: "the name of a world ANOTHER instance has finished with — its flow left that instance's " +
                 "frontier, or its whole session parked into a generation that will never mint again. This " +
                 "instance holds a COW segment for every foreign world that ever reached it (solver/world.h), " +
                 "materialized on arrival and released only when told, so without this record an instance that " +
                 "answered anything holds a foreign flow's state for the rest of its process and can never " +
                 "park. It carries no target document and no sender origin, and both absences are the design: " +
                 "the sending engine does not track which peers a flow reached — releasing a world with no " +
                 "segment is a no-op, so tracking it would be state kept only to avoid one — which is why this " +
                 "zone BROADCASTS it, and nothing on the receiving side runs page code, so there is no " +
                 "`event.origin` for a stamp to be the truth of" }],
        reply: [WORKING_SET] },

      /* THE OTHER HALF OF `Join`, AND THE TWO ARE ONE NAVIGATION. `Join` adds the Document the browser
         navigated TO; this names the one it navigated AWAY FROM, so the instance stops running two tops for
         one traversable. It takes `DOCUMENT_ID` and nothing else because everything else about the outgoing
         document is already inside the agent — it was rooted or joined there, with its address, its origin,
         its response and its policy container — and the one fact only this zone holds is WHICH of the several
         documents an origin-keyed agent cluster contains the browser replaced.
         IT IS HTML §7.4.6.1 "Updating the traversable"'s DEACTIVATE A DOCUMENT FOR A CROSS-DOCUMENT NAVIGATION
         and NOT §7.3.1.6 "Navigable destruction": a navigation destroys no navigable, it replaces the Document
         active in one. Both reach §7.5.10 "Destroying documents" one step down, which is why the engine serves
         them from one machine.
         NO `rc`. Every precondition qjs_unload has is a CHECK or a DCHECK that aborts the instance — the
         document is one this agent holds, or it is not — so there is no value for a reply field to carry, and
         the acknowledgement is the working set alone (a void entry still answers, so a WASM abort inside it
         reaches this zone as a rejection rather than as silence). */
      { ordinal: 21, name: "Unload",
        params: [DOCUMENT_ID,
                 { name: "incomingDocId", type: "string",
                   why: "the name of the Document the navigation LOADED — HTML §7.4.6.1 is written over " +
                        "`targetEntry` and hands its Document to the unload, and HTML §7.5.9 \"Unloading " +
                        "documents\" takes it as the optional `newDocument`. It travels because it is what " +
                        "makes the ORDER assertable at the engine (the incoming Document is joined first, so " +
                        "a zone that called Join and Unload the other way round is caught at the entry), and " +
                        "because the engine queues the operation's own tasks in that document's realm — " +
                        "§7.5.10 step 7 removes every queued task of the DESTROYED document, so the outgoing " +
                        "realm would make the first timeline's destruction drop every other timeline's" }],
        reply: [WORKING_SET] },
    ],
  });

  /* ── THE BROWSER'S CONTROL INTERFACE TO A CHILD PROCESS. Chromium's `content.mojom.ChildProcess` is the
     interface the browser holds to every child; this is the one method of it we need, and it is here rather
     than folded into another interface because "how much IPC does that process have open" is a fact about the
     PROCESS and not about anything it serves. */
  g.mojo.defineInterface({
    name: "content.mojom.ChildProcess",
    methods: [
      { ordinal: 0, name: "GetMojoStats",
        params: [],
        reply: [
          { name: "remotes", type: "string", why: "interfaces this process calls OUT on, comma-joined and sorted" },
          { name: "receivers", type: "string", why: "interfaces this process IMPLEMENTS, comma-joined and sorted" },
          { name: "endpoints", type: "int32",
            why: "open pipe endpoints in this process — a Remote and a Receiver each hold one END, so this is " +
                 "the process's half of every pipe it is on" }] },
    ],
  });
})(self);
