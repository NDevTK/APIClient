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
 * THE PROCESS TOPOLOGY THESE FIVE DESCRIBE, and it is the one the platform forces rather than one chosen:
 *
 *   • The BROWSER PROCESS is `extension/browser-process.js`, a dedicated Worker of the offscreen document. It
 *     owns what a browser process owns — the renderer REGISTRY, the routing ids, and (in-process, which is a
 *     real Chromium configuration and not a shortcut) the network service's own §7/CORB algorithms.
 *   • It CANNOT create a renderer itself, and that is a fact about the platform: a dedicated Worker's global is
 *     `DedicatedWorkerGlobalScope`, which has no `document` and no DOM, so there is no `createElement` in it.
 *     Chromium's browser process cannot fork a renderer by itself either — on Linux it asks the ZYGOTE, a
 *     helper process holding the state a renderer starts from, to do the fork on its behalf.
 *   • So the OFFSCREEN DOCUMENT is the zygote: `content.mojom.Zygote` is implemented in renderer-host.js and
 *     called BY the browser process. It holds no admission rule, no agent-cluster key and no ranking — it
 *     materializes the frame it is ORDERED to materialize and hands back the pipe. Where the analogy stops is
 *     worth stating: Chromium's zygote is a forked helper and ours is the trusted document that owns the DOM,
 *     which is exactly why it can create a frame when the browser process cannot.
 *   • A RENDERER is `extension/renderer.html` in an `<iframe sandbox="allow-scripts">` with no
 *     `allow-same-origin`, so its origin is unique and opaque and Site Isolation may put it in its own OS
 *     process. It runs the untrusted bundle, and `content.mojom.Renderer` is the whole of what it may be asked.
 *
 * AND `content.mojom.ChildProcess` IS IMPLEMENTED BY BOTH CHILDREN, which is what makes it the interface its
 * Chromium name says it is: "how much IPC does that process have open" is a question about a PROCESS, so it is
 * answered identically by the browser process's Worker and by a renderer's frame, and neither answer belongs on
 * an interface either of them happens to serve.
 */
(function (g) {
  "use strict";

  DCHECK(!!g.mojo && typeof g.mojo.defineInterface === "function",
         "extension/mojo.js is not loaded in this realm — the IDL is validated as it is declared, so a mojom " +
         "file that loads first would install interfaces nothing ever checked");

  /* Two facts every response question takes, and the reason they are stated per method rather than shared: a
     mojom method's parameter list IS its contract, and a shared record would let one method's list change
     under the other. The sentences repeat because the rules do. */
  var CONTENT_TYPE = { name: "contentType", type: "string?",
    why: "an ABSENT Content-Type is §5.1's \"the supplied MIME type is undefined\" and says so with null, " +
         "never with \"\" — an empty header is a value a server can really send and means something else" };
  var XCTO = { name: "xContentTypeOptions", type: "string?",
    why: "this boundary carries the HEADER VALUE and not the derived flag, so Fetch's determine-nosniff runs " +
         "once beside the algorithms that read it (network/nosniff.c) instead of as an `indexOf(\"nosniff\")` " +
         "in a zone the architecture leaves a bridge; an absent header is null exactly as an absent " +
         "Content-Type is, and `foo, nosniff` does NOT set the flag because the standard matches the FIRST value" };
  var HEADER = { name: "header", type: "array<uint8>",
    why: "§5.2's resource header is a BYTE SEQUENCE and the whole reason this decision is taken in another " +
         "process is that it reads the body — a string here is a zone that ran a decode it does not own, and " +
         "every non-ASCII signature in §6's tables is what the decode destroys" };

  /* ── THE NETWORK SERVICE'S CONTENT DECISIONS. In Chromium these run in the network service and never in a
     renderer: a renderer that classifies for itself can mine a cross-origin body it would otherwise have been
     handed empty. Here the network service is in-process with the browser process — one Worker — which is a
     configuration Chromium itself ships rather than a collapse invented here. */
  g.mojo.defineInterface({
    name: "network.mojom.ContentSniffer",
    version: 0,
    methods: [
      { ordinal: 0, name: "CheckCorb",
        params: [CONTENT_TYPE, XCTO,
          { name: "sameOrigin", type: "bool",
            why: "the principal comparison is a fact the TRUSTED zone MADE from the browser's " +
                 "MessageSender.origin (SECURITY.md forbids re-deriving it from a URL), so it crosses as a " +
                 "browser-stated boolean and this process has no URL to invent one from" },
          HEADER],
        reply: [
          { name: "allow", type: "bool",
            why: "the verdict, §7's computed essence and the rule that decided are written together by corb.c" },
          { name: "computed", type: "string", why: "§7's computed MIME type, written with the verdict by corb.c" },
          { name: "reason", type: "string", why: "the rule that decided, written with the verdict by corb.c" }] },

      { ordinal: 1, name: "ClassifyResource",
        params: [CONTENT_TYPE, XCTO,
          { name: "opaque", type: "bool",
            why: "Fetch §2.2.6: the response is an opaque filtered response, so its body is null and its " +
                 "header list is empty by construction — a fact only the zone HOLDING the Response can state, " +
                 "and one no amount of looking at bytes can tell apart from a body that was read and was empty" },
          HEADER],
        reply: [
          { name: "asset", type: "bool",
            why: "the verdict and the rule that decided are written together by resource_kind.c" },
          { name: "reason", type: "string", why: "the rule that decided, written with the verdict by resource_kind.c" }] },
    ],
  });

  /* ── THE BROWSER PROCESS'S RENDERER REGISTRY. The interface name is Chromium's own for the browser-side
     object that owns renderer processes. The registry lives behind it and nowhere else: which agent clusters
     have a renderer, what routing id each was given, and the refusal of a second one for a cluster that
     already has one — SECURITY.md's one-instance-per-`(browsing-context group, origin)` rule, held by the
     process whose job it is to hold it rather than by the zone that wants the renderer. */
  g.mojo.defineInterface({
    name: "content.mojom.RendererHost",
    version: 0,
    methods: [
      { ordinal: 0, name: "CreateRendererForCluster",
        params: [
          { name: "clusterKey", type: "string",
            why: "SECURITY.md's agent cluster — `(browsing-context group, origin)`, both halves BROWSER-STATED " +
                 "— which is the unit a renderer IS, so a renderer is asked for by cluster and by nothing else" }],
        reply: [
          { name: "routingId", type: "int32",
            why: "minted by this registry and by nothing else: it is the only name a renderer has, and one the " +
                 "asking zone could mint for itself would be a renderer this process never decided on" },
          { name: "pipe", type: "handle<message_pipe>?",
            why: "the renderer's own pipe, forked by the zygote and RELAYED through this process — null " +
                 "exactly when `error` is not, because a launch that failed has no pipe rather than a dead one" },
          { name: "error", type: "string?",
            why: "a launch that failed is an OUTCOME and not a broken message (a page whose engine aborted its " +
                 "boot is recorded as a crashed instance), so it is a declared nullable field and never a " +
                 "rejected pipe; exactly one of `pipe` and `error` is non-null and both ends assert it" }] },

      { ordinal: 1, name: "RendererTerminated",
        params: [
          { name: "routingId", type: "int32",
            why: "the renderer whose frame is gone. It is FIRE-AND-FORGET because a real renderer's death is " +
                 "OBSERVED rather than acknowledged, and it is a method of THIS interface — not a second one — " +
                 "so that one pipe orders it against the next CreateRendererForCluster for the same cluster, " +
                 "which would otherwise be refused as a duplicate of a renderer that no longer exists" }],
        reply: null },

      { ordinal: 2, name: "GetRegistry",
        params: [],
        reply: [
          { name: "clusters", type: "string",
            why: "the registered agent cluster keys, comma-joined and sorted, with the key's NUL separator " +
                 "rendered as `|` so a reader can see it — a diagnostic view of the authority, not a second copy" },
          { name: "routingIds", type: "string",
            why: "the live routing ids, comma-joined and sorted, so the asking zone can check the renderers it " +
                 "holds against the ones this process decided on rather than against its own count" },
          { name: "live", type: "int32", why: "registered renderers right now" },
          { name: "launched", type: "int32", why: "forks this registry ordered that produced a renderer" },
          { name: "terminated", type: "int32", why: "renderers this registry has been told are gone" },
          { name: "failed", type: "int32", why: "forks this registry ordered whose renderer did not boot" },
          { name: "nextRoutingId", type: "int32",
            why: "the next id this registry will mint — the counter is here, so a renderer's id is evidence of " +
                 "which process created it" }] },
    ],
  });

  /* ── THE ZYGOTE, implemented in the OFFSCREEN and called by the browser process. This is the inversion: the
     only code path in this extension that materializes a renderer frame is the implementation of this method,
     so a renderer exists if and only if the browser process ordered one. */
  g.mojo.defineInterface({
    name: "content.mojom.Zygote",
    version: 0,
    methods: [
      { ordinal: 0, name: "ForkRenderer",
        params: [
          { name: "routingId", type: "int32",
            why: "the id the browser process minted for this renderer — the zygote does not allocate it, " +
                 "because allocating it is deciding, and deciding is what this call is obeying" },
          { name: "clusterKey", type: "string",
            why: "the agent cluster this renderer IS, carried so the frame can be identified in the document " +
                 "(its title) and so the pool's own name for the instance can be checked against the browser's" }],
        reply: [
          { name: "pipe", type: "handle<message_pipe>?",
            why: "the renderer's pipe, TRANSFERRED — which is why this is a real pipe pass and not a clone: " +
                 "the zygote is detached from it here and gets it back only from the browser process, so the " +
                 "endpoint the pool ends up calling on provably travelled through the deciding process" },
          { name: "error", type: "string?",
            why: "the engine's own abort or a frame that never booted; null exactly when `pipe` is not" }] },
    ],
  });

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
     thing — a NUL-terminated byte sequence it `strlen`s — so the wire says one thing, the encode happens once
     in the zone that already holds the characters, and the NUL assertion bridge.js makes over those bytes now
     covers BOTH shapes: a string document containing U+0000 used to truncate the parse with nothing to say so.
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

  /* THE ONE FACT EVERY REPLY CARRIES, declared ONCE and referenced by every method for the reason CONTENT_TYPE
     above is: a shared record cannot drift, twenty hand-copied ones can. It is a fact about the PROCESS rather
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

  /* THE FIVE ARGUMENTS THAT ROOT OR JOIN A DOCUMENT. `Init` and `Join` take the identical list because main.c's
     two entries have byte-identical C signatures, and they are a shared record for that reason rather than for
     brevity: it is ONE contract taken by two operations (root this agent at a document / add a document to the
     agent already running), so a change to what a document arrives with must reach both. */
  var DOCUMENT = { name: "document", type: "array<uint8>",
    why: "the document's BYTES, which is what qjs_init/qjs_join take (a NUL-terminated pointer they strlen) " +
         "and what HTML §13.2.3.2's encoding sniffing is defined over — the algorithm the engine owes, and " +
         "this zone owes it the bytes for. A string here is a zone that ran a decode it does not own" };
  var DOCUMENT_URL = { name: "url", type: "string",
    why: "§4.4's document ADDRESS and not its origin: the engine derives the origin from the address itself " +
         "(§4.7's serialization, its own url.c), so the principal and the address are one fact from one place. " +
         "Handing over a bare origin made every relative URL a bundle built resolve against the site root" };
  var DOCUMENT_ID = { name: "docId", type: "string",
    why: "the name this document is known by inside the agent — the key qjs_route and the notice stream name a " +
         "target document with. It is minted by the trusted zone for a root and by the ENGINE for a child it " +
         "creates (`<parent>.<n>`), which SECURITY.md permits precisely because it is only a name" };
  var DOCUMENT_HEADERS = { name: "headers", type: "string",
    why: "the response's HEADER FIELD LINES verbatim, which is what §7.2.6's policy container is parsed from. " +
         "The empty string is the positive statement that this document had no response at all (an " +
         "about:blank, a serialized DOM off content.js), which differs from a response carrying no headers" };
  var TOP_LEVEL_URL = { name: "topLevelUrl", type: "string",
    why: "§8.1.3.1's top-level creation URL, which §8.1.3.5 reads to decide whether this realm is a SECURE " +
         "CONTEXT and therefore which of Web IDL §3.3.13's members exist in it. The engine refuses an empty " +
         "one, so a document reaching here without it is one whose API surface is silently smaller" };

  g.mojo.defineInterface({
    name: "content.mojom.Renderer",
    version: 0,
    methods: [
      { ordinal: 0, name: "Init",
        params: [DOCUMENT, DOCUMENT_URL, DOCUMENT_ID, DOCUMENT_HEADERS, TOP_LEVEL_URL],
        reply: [
          { name: "rc", type: "int32",
            why: "qjs_init's own return. Its C body is a wall of CHECKs whose failures abort the instance, so " +
                 "the only value it can produce is 0 — which is exactly why reading it costs nothing and why a " +
                 "non-zero is an entry that started reporting a failure this zone was not listening for" },
          WORKING_SET] },

      { ordinal: 1, name: "Join",
        params: [DOCUMENT, DOCUMENT_URL, DOCUMENT_ID, DOCUMENT_HEADERS, TOP_LEVEL_URL],
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
            why: "ENGINE_STEP_DONE (0) or ENGINE_STEP_YIELD (2) and nothing else — the engine folds STALLED " +
                 "into YIELD itself. A host that branched on a third value left the whole " +
                 "qjs_pending->safeFetch->qjs_provide reply path unreachable in the shipped extension" },
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
          { name: "urls", type: "string",
            why: "the addresses flows are parked on, newline-joined, \"\" for none — every owed list this ABI " +
                 "answers crosses that way, so an empty list is the empty string and never a NULL pointer " +
                 "turned into the four characters \"null\" and then into one bogus record" },
          WORKING_SET] },

      { ordinal: 8, name: "GetChunks",
        params: [],
        reply: [
          { name: "urls", type: "string",
            why: "the module loader's owed chunk addresses, on GetPending's rule. A fetch whose body is " +
                 "JAVASCRIPT is always fetched AND executed — the lazy-chunk surface CLAUDE.md calls the " +
                 "headline moat — so a record dropped here is an endpoint set nobody ever sees" },
          WORKING_SET] },

      { ordinal: 9, name: "Provide",
        params: [
          { name: "url", type: "string",
            why: "the address this reply answers, matched against what a flow parked on — the engine's own " +
                 "qjs_provide DFAILs on one nothing is parked on, which is what an invented record produces" },
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
                 "`flow_best() ? flow_weight(b) : -1.0/0.0`, the engine's positive statement that its frontier " +
                 "holds no runnable flow — and the right statement, since that value ranks below every real " +
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
                 "cross-agent operation answered. This zone OWES each of them an action: a notice read and " +
                 "discarded is a document nothing runs and a message nothing delivers" },
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
          { name: "completion", type: "string",
            why: "the peer's completion record, RELAYED WHOLE and unread for Perform's reason. An empty one is " +
                 "not `undefined`, it is a relay that lost the peer's answer, and the engine's own decoder " +
                 "says so at the other end" }],
        reply: [WORKING_SET] },
    ],
  });

  /* ── THE BROWSER'S CONTROL INTERFACE TO A CHILD PROCESS. Chromium's `content.mojom.ChildProcess` is the
     interface the browser holds to every child; this is the one method of it we need, and it is here rather
     than folded into another interface because "how much IPC does that process have open" is a fact about the
     PROCESS and not about anything it serves. */
  g.mojo.defineInterface({
    name: "content.mojom.ChildProcess",
    version: 0,
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
