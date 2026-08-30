/* mojo.js — THE IPC PRIMITIVE, and it is Mojo rather than a vocabulary coined here. CLAUDE.md §Architecture:
 * "Use browser / JS-engine developers' concepts and names — never coin a system when an established one
 * exists." Chromium's inter-process layer is Mojo, its concepts are message pipes / interfaces / Remote /
 * Receiver / an interface broker, and every one of them maps onto something this platform already has.
 *
 * WHAT THIS REPLACES. Two ad-hoc envelope vocabularies grew, one per boundary, each carrying its own `id`
 * routing table, its own `op` string, its own `ok`/`err` convention and its own reply drain — renderer-host's
 * `{v:1,id,op:"call",fn,ret,args,bodies}` and, on the boundary to the browser process's Worker, its own
 * `{v:1,id,op,…}`. Two transports for
 * one problem is the dual system CLAUDE.md forbids: every capability a boundary gains is a new `op` hand-written
 * on both sides, and every omission is silent. Mojo's answer is that a boundary carries no capability list at
 * all — it carries ONE brokered request, `GetInterface(name)`, and everything else rides a pipe of its own.
 * BOTH are gone now, and the second one is why the first was worth doing. The renderer envelope was the GENERIC
 * one — a `fn` string, a `ret` string, an `args` list of shape tags and a `bodies` list those tags indexed —
 * which relayed the whole twenty-entry qjs_* ABI without declaring a single parameter's type, so the peer whose
 * messages SECURITY.md says must be assumed hostile was the one peer nothing validated. `content.mojom.Renderer`
 * declares every one of those parameters, and the validator that already killed the connection on a malformed
 * record from the TRUSTED worker now stands where the threat is.
 *
 * THE MAPPING, CONCEPT BY CONCEPT, and every row is a thing this platform actually has:
 *
 *   Mojo message pipe          → `MessagePort`. Point-to-point, ordered, and TRANSFERABLE, which is the whole
 *                                property: a pipe endpoint can be sent inside a message, which is how a new
 *                                interface is handed over. `MessageChannel` is `mojo::CreateMessagePipe`.
 *   Mojo node / invitation     → the `Worker` handle. Creating the child gives the parent one primordial pipe
 *                                and nothing else, exactly as `mojo::OutgoingInvitation` does; the child's
 *                                first message on it is its acceptance.
 *   `mojo::Remote<T>`          → `Remote` below: the caller end, with one JS method per mojom method.
 *   `mojo::Receiver<T>`        → `Receiver` below: the implementation end.
 *   `mojo::PendingRemote<T>` /
 *   `mojo::PendingReceiver<T>` → an UNBOUND `MessagePort` sitting in a message's transfer list. A method whose
 *                                parameter type is `handle<message_pipe>` is passing one.
 *   `BrowserInterfaceBroker`   → `Connection.bindInterface(name)`: a NAME goes out on the primordial pipe with
 *                                one end of a fresh pipe, and the peer binds its implementation to that end.
 *                                Chromium's binder map is `mojo.exposeInterface` — process-wide, registered at
 *                                startup, consulted for every peer, which is what `content::BinderMap` is.
 *   mojom IDL                  → extension/mojom.js. Interfaces are DECLARED in one file both processes load,
 *                                because an interface only exists if both ends agree on it.
 *   message header             → `{w, i, o, r, f}`: wire version, interface, ORDINAL, request id, flags. The
 *                                flags are Mojo's own two — `kMessageExpectsResponse` and `kMessageIsResponse`
 *                                — with Mojo's values. Ordinals are what the wire carries (mojom's `@0`), so
 *                                reordering a method list is not a wire change and renaming one is not either.
 *   `ReportBadMessage`         → a validation failure is not an error return. It is the peer being broken, so
 *                                it kills the CONNECTION: in the child that is posted as an abort and every
 *                                outstanding call rejects, which is what a real bad-message kill looks like
 *                                from the browser's side.
 *
 * THE THREE PLACES THIS DELIBERATELY DIVERGES FROM MOJO, stated rather than left to be noticed:
 *
 *   (1) THE INTERFACE NAME IS ON THE WIRE. Mojo does not put it there — the pipe implies the interface. It is
 *       carried here because the failure it catches is otherwise silent-then-wrong: a broker that bound the
 *       wrong implementation to a pipe would answer the FIRST few ordinals plausibly (they exist on both
 *       interfaces) and only diverge later, and the crash would then name an ordinal instead of the two
 *       interfaces that got crossed. It is asserted and never dispatched on, exactly like the `v:1` both
 *       superseded transports carried.
 *   (2) EVERY RECORD A CHILD POSTS CARRIES THE CHILD'S STDIO. Mojo has no such field, because a real child
 *       process has a real stderr the browser reads off an fd. Here the only path out of a Worker is
 *       postMessage, and the `@WHY` a CHECK prints immediately before abort() is the one line that says what
 *       broke — so it rides the TRANSPORT (attached by `_envelope`, absorbed by `_absorb`), never an
 *       interface's parameters. A process's output is a fact about the PROCESS and not about any interface it
 *       happens to serve, and which pipe answers next is not knowable, so it cannot live on one of them.
 *   (3) THE WIRE CONTRACT ITSELF CROSSES THE HANDSHAKE. Mojo needs nothing of the kind: both ends are
 *       GENERATED from one `.mojom` by the build, so a skew is not representable and the compiler is the
 *       check. This platform cannot have that. mojom.js is READ TWICE — the offscreen loads it by
 *       `<script src>` when ast-worker.html parses, and renderer-host.js `fetch()`es the same URL later to
 *       build the invitation the frame boots from — so "one description loaded by both realms" is a claim
 *       about two reads of one URL at two instants, with nothing in the extension tying them to one
 *       generation. A hand-written `version: 0` stood here and DID NOT tie them: it was constant, nothing
 *       ever incremented it, and a mojom.js that gained a method, renumbered an ordinal or changed a
 *       parameter type shipped `0` on both sides — so the bind-time check compared 0 against 0 and passed
 *       for every skew it named itself as guarding against. It is deleted, and what crosses instead is the
 *       CONTRACT, DERIVED from the declarations rather than maintained beside them: a mismatch cannot be
 *       forgotten, because nothing has to be remembered. It is not a negotiation and there is no
 *       compatibility arm — a peer whose contract differs is REFUSED, at the acceptance and at every bind,
 *       exactly as a mismatched generated binding would fail to link.
 *
 * ORDERING, AND WHAT IS NOT BUILT. Mojo guarantees ordering WITHIN a pipe and nothing across pipes; the tool
 * for cross-interface ordering is an ASSOCIATED interface, which multiplexes several interfaces onto one pipe
 * so their messages keep one order. None is built here, and the reason is specific rather than "not needed
 * yet": the one pair of messages whose relative order is load-bearing — `RendererTerminated` for an agent
 * cluster and the next `CreateRendererForCluster` for that same cluster, which would otherwise be refused as a
 * duplicate — are two METHODS OF ONE INTERFACE, so one pipe already orders them. Nothing else in mojom.js has a
 * cross-interface ordering requirement, and the renderer's is the case that shows why: `Init`, `Begin`, `Step`,
 * `Provide` and `Teardown` are a STRICTLY ORDERED sequence — the engine's own entries assert it (`qjs_begin`
 * aborts if `qjs_init` did not build the context, `qjs_step` if the frontier was never seeded) — and they are
 * ordered for free because they are twenty methods of ONE interface on ONE pipe. Splitting the ABI into several
 * interfaces by theme would have been the readable choice and would have made that ordering an unstated
 * assumption across pipes Mojo explicitly does not order.
 * The day a real cross-interface requirement appears, it is an associated interface and not a sleep.
 * A HANDLER THAT SUSPENDS DOES NOT BLOCK THE PIPE, which is also Mojo's behaviour: `CreateRendererForCluster`
 * awaits a fork order to the other process, and a `GetRegistry` behind it is answered while that await is
 * outstanding. That is why the registry entry is taken BEFORE the first await, the same discipline 94c5998e
 * fixed the engine pool with.
 */
(function (g) {
  "use strict";

  var WIRE = 1;
  /* Mojo's own flag values (mojo/public/cpp/bindings/lib/message.h). */
  var F_EXPECTS_RESPONSE = 1;
  var F_IS_RESPONSE = 2;

  /* ── THE TYPE SYSTEM. mojom's spellings, and only the ones this transport actually carries. Structured
     clone reproduces each of them natively, so nothing is ever encoded in transit — a number stays a number
     (so a peer's `0` is still distinguishable from the string "0") and a byte sequence stays bytes. */
  function typeOk(t, v) {
    if (t === "string")  return typeof v === "string";
    if (t === "string?") return v === null || typeof v === "string";
    if (t === "bool")    return typeof v === "boolean";
    if (t === "int32")   return typeof v === "number" && (v | 0) === v;
    /* `double` IS THE IEEE-754 TYPE AND THEREFORE CARRIES THE NON-FINITE VALUES, which is the whole reason a
       flow weight is declared as one rather than as an int32 with a sentinel beside it. `engine_top_weight` is
       `flow_next_to_run(NULL) ? flow_weight(b) : -1.0/0.0`, so -Infinity is the engine's own vocabulary for a
       frontier holding no runnable flow — which is what this line has always said and what the C side only now
       answers: it used to ask flow_best, over EVERY member including the ones waiting on the host, so a fully
       blocked engine published a weight belonging to a flow that could not run. A transport that refused
       -Infinity is exactly the defect 355a03d2 fixed one layer
       up: every drained engine aborted its round, was destroyed and re-provisioned, 43279 times in one page
       load. WHICH non-finite values are meaningful is the consumer's rule and not this layer's — bridge.js
       narrows to {finite, -Infinity} at the read, where the vocabulary is known — so all a type check may say
       here is that a double is a double. */
    if (t === "double")  return typeof v === "number";
    if (t === "array<uint8>")  return v instanceof Uint8Array;
    /* AND ITS NULLABLE FORM IS A POSITIVE STATEMENT, never an omission: `null` is "this answer carries no body
       at all" — Fetch §5.6's network error, a load that did not load — which a zero-length byte sequence is
       not, because a 204 has a body of no bytes and a failure has no body. */
    if (t === "array<uint8>?") return v === null || v instanceof Uint8Array;
    if (t === "handle<message_pipe>")  return v instanceof MessagePort;
    if (t === "handle<message_pipe>?") return v === null || v instanceof MessagePort;
    DFAIL("a mojom declaration named a type this bindings layer does not carry: `" + t + "` — the type list is " +
          "the whole contract of what may cross a process boundary, and a new one is a serialization decision " +
          "(what structured clone reproduces, and whether it is a handle that must be transferred) rather than " +
          "a name to add");
    return false;   /* release path under the assert: an uncarryable type is not silently posted */
  }
  function isHandle(t) { return t === "handle<message_pipe>" || t === "handle<message_pipe>?"; }

  /* ── HOW A DECLARED PARAMETER BECOMES WASM OPERANDS, DERIVED, AND THE ONE PLACE THAT ANSWERS IT.
     `content.mojom.Renderer`'s peer is not JavaScript: it is a wasm module whose entries take C operands, so
     every caller of that interface has to turn one DECLARED PARAMETER into one or two of them. That fact was
     written down four times — `renderer.html` carried a hand-aligned `place: [...]` per method, `harness.js`
     re-derived the same counts by REGEXING that array out of the document's source text, `route.mjs` walked
     the types itself, and a fifth copy stood as a literal arity beside them — and the list went SHORT TWICE,
     one parameter apart, with nothing to say so: a short list hands the entry a call one operand short and
     every later argument is read one slot early, which is a wrong call rather than a missing one.
     IT IS DERIVED HERE FOR THE REASON `typeOk` AND `isHandle` ARE HERE. Those two already answer what a
     declared type MEANS operationally — what value satisfies it, and which one is a handle that must be
     transferred rather than copied. "How many operands, of what form" is that same question asked for the one
     peer that is not a JS realm, so it is one more property of the declared type and not a table beside it.
     TWO FACTS DECIDE A PLACEMENT AND ONLY ONE OF THEM IS THE TYPE. The type decides the FORM and the
     OPERAND COUNT — a byte sequence is a (pointer, length) PAIR because the entry takes both and because a
     length recovered with `strlen` would end at a 0x00 the sequence may legitimately contain. OWNERSHIP is
     the other, it is a fact about the C ENTRY rather than about the wire, and no wire type carries it: it is
     the difference between a live document and a use-after-free, so the DECLARATION states it (`retained`)
     and this reads it rather than inferring it from the type. Inferring it is what the superseded table did —
     it tied `array<uint8>` to "kept" and `array<uint8>?` to "freed", which is NULLABILITY standing in for
     ownership and holds only for as long as those happen to be the same two parameters.
     A TYPE WITH NO PLACEMENT CRASHES, and a `retained` this cannot honour crashes with it. A number is passed
     BY VALUE, so there is no pointer for an entry to keep and `retained: true` on one is a declaration that
     cannot be performed; a handle is a MessagePort, which does not cross into linear memory at all. Guessing
     an operand count for either is how a call goes one slot out. */
  function abiPlacement(d) {
    DCHECK(!!d && typeof d.type === "string" && typeof d.retained === "boolean",
           "an ABI parameter was placed from a declaration that does not state its OWNERSHIP — `retained` says " +
           "whether the C entry keeps the pointer past the call, which no wire type can carry and which is the " +
           "difference between a live document and a use-after-free, so it is declared beside the type and " +
           "never defaulted here");
    if (d.type === "string")
      return { form: "string", operands: 1, nullable: false, retained: d.retained };
    if (d.type === "array<uint8>" || d.type === "array<uint8>?")
      return { form: "bytes", operands: 2, nullable: d.type === "array<uint8>?", retained: d.retained };
    if (d.type === "int32" || d.type === "double") {
      DCHECK(!d.retained,
             "the parameter `" + d.name + "` is declared `" + d.type + "` and RETAINED — a number crosses BY " +
             "VALUE, so there is no pointer in linear memory for the entry to keep and no allocation for a " +
             "caller to withhold; a declaration that says otherwise describes a placement nobody can perform");
      return { form: "number", operands: 1, nullable: false, retained: false };
    }
    DFAIL("the parameter `" + (d && d.name) + "` is declared `" + (d && d.type) + "` and this bindings layer " +
          "performs no wasm placement for that type — a placement is how a declared type becomes operands of a " +
          "C entry, and an invented one is an operand COUNT nobody knows, which reads every later argument of " +
          "the call one slot early. Build the placement here, where every caller of the ABI reads it");
    /* RELEASE PATH UNDER THE ASSERT, AND IT REFUSES rather than answering. A guessed count is a wrong call;
       an absent one is a caller that stops. */
    throw new TypeError("no wasm ABI placement for the declared type `" + (d && d.type) + "`");
  }

  /* ── AND THE WHOLE PARAMETER LIST PLACED IN ONE WALK, KEYED BY THE DECLARATION'S OWN NAMES. `abiPlacement`
     above answers for ONE parameter; this is the loop over all of them, and the loop is where the going-short
     happened. It is the same shape `placeParams` below is for the MOJO transport and it exists for the
     identical sentence: a positional operand list is a second copy of the parameter list kept by hand at every
     call site, and the only thing a positional caller can be told is a COUNT.
     WHAT MAKES IT A COMPONENT RATHER THAN A HELPER: the raw-ccall drivers do not go through the transport at
     all, so `placeParams`' two refusals never ran for them, and each one carried its own array of values in
     declaration order. That list went short THREE times now — twice for the reasons this file's `abiPlacement`
     paragraph records, and once more when HTML §3.1.3's ancestor origins statement was added: every document
     of the solver's differential gate died in `navigable_root_ancestor_origins` because the fifteenth operand
     arrived as a zero-filled NULL, which emscripten's wrapper reports on the too-MANY side only.
     BOTH SKEWS REFUSE BY NAME, and they are opposite: a caller OLDER than the interface has a hole, a caller
     NEWER than it has a key nobody reads — and a key nobody reads is a value its author believes it sent.
     `place` IS THE HOST'S, BECAUSE MINTING AN OPERAND IS THE ONE PART THAT IS NOT A PROPERTY OF THE
     DECLARATION. A renderer holds retained pointers for the life of its frame and frees transient ones after
     the call; a Node driver mallocs into the module it just booted. So the walk is here and the minting is
     theirs, and the contract between them is asserted rather than trusted: `place` is handed the value, the
     placement and the parameter, and must answer with EXACTLY `pl.operands` operands — an answer of another
     length is an operand count nobody knows, which reads every later argument of the call one slot early. */
  function abiOperands(m, rec, place) {
    var out = [], declared = new Set(), i, k, keys, pl, got;

    DCHECK(!!m && typeof m.name === "string" && Array.isArray(m.params),
           "an ABI call was placed from something that is not a mojom method declaration — the declaration is " +
           "the only list of what the entry takes, and a caller that supplies its own is the hand-aligned copy " +
           "this walk replaces");
    DCHECK(!!rec && typeof rec === "object" && !Array.isArray(rec),
           "an ABI call to " + (m && m.name) + " was placed from something other than a parameter record — an " +
           "array here would be the positional list this walk stopped taking, and it stopped taking it " +
           "because a short one can only report a count and never the name of what is missing");
    DCHECK(typeof place === "function",
           "an ABI call to " + (m && m.name) + " was placed with no minting function — how a value becomes a " +
           "pointer is the host's (retained for the life of a frame, or freed after the call) and the one " +
           "thing this walk must not do is guess it");
    for (i = 0; i < m.params.length; i++) {
      declared.add(m.params[i].name);
      DCHECK(Object.prototype.hasOwnProperty.call(rec, m.params[i].name),
             "an ABI call to " + m.name + " has no value for the parameter `" + m.params[i].name + "`, which " +
             "its mojom declares — " + m.params[i].why + ". This caller is OLDER than the interface, and the " +
             "one thing it must not do is place the values it does have anyway: that reads every later " +
             "parameter one operand early, which is a wrong call rather than a missing one");
      pl = abiPlacement(m.params[i]);
      got = place(rec[m.params[i].name], pl, m.params[i]);
      DCHECK(Array.isArray(got) && got.length === pl.operands,
             "an ABI call to " + m.name + " minted " + (Array.isArray(got) ? got.length : "no") + " operand(s) " +
             "for the parameter `" + m.params[i].name + "`, whose declared `" + m.params[i].type + "` is " +
             pl.operands + " — a byte sequence is the PAIR the C entry takes (a pointer and a LENGTH, because " +
             "a length recovered with `strlen` would end at a 0x00 the sequence may legitimately contain) and " +
             "a string is one pointer, so a host that answers with another count has placed a different call");
      for (k = 0; k < got.length; k++) out.push(got[k]);
    }
    keys = Object.keys(rec);
    for (i = 0; i < keys.length; i++)
      DCHECK(declared.has(keys[i]),
             "an ABI call to " + m.name + " carries `" + keys[i] + "`, which its mojom declares no parameter " +
             "of — this caller is NEWER than the interface or has misspelled a name, and either way the value " +
             "is minted, held, and placed nowhere while its author believes it crossed. A walk is " +
             "structurally silent about this direction: it only ever asks for the names it was given");
    return out;
  }

  /* A DECLARATION LIST — a method's parameters, or its reply's. Every entry carries a `why`, and that is not
     documentation: it is the sentence the assert prints when the value is wrong. The two superseded transports
     each held a `checkHeaderFacts` whose real content was those sentences (an absent header is §5.1's undefined
     supplied type and says so with null, never with ""), written twice and drifting; here the sentence lives
     with the DECLARATION, so both ends print it and neither can hold a different one. */
  function checkDecls(where, decls) {
    DCHECK(Array.isArray(decls),
           where + " has no parameter list — a mojom method declares its parameters even when there are none, " +
           "with an empty list, because an absent list and a list of nothing are different claims");
    for (var i = 0; i < decls.length; i++) {
      var d = decls[i];
      DCHECK(!!d && typeof d.name === "string" && d.name !== "" && typeof d.type === "string",
             where + " parameter " + i + " is not a `{name, type, why}` declaration");
      DCHECK(typeof d.why === "string" && d.why !== "",
             where + " parameter `" + d.name + "` carries no `why` — that string is what the validator prints " +
             "when the value is wrong, so a parameter without one is a crash that names a type and not a rule");
    }
  }

  /* THE SEVERITY IS A PROPERTY OF WHOSE VALUE IT IS, SO THE CALL SITE STATES IT AND THERE IS STILL ONE
     IMPLEMENTATION. A record this process PRODUCED is its own logic and is asserted DCHECK-class; a record
     that ARRIVED is the peer's, and refusing a malformed one from the peer SECURITY.md calls UNTRUSTED is a
     trust boundary, which check.js's own test puts in CHECK-class — fatal in dev AND release.
     THIS WAS DCHECK ON BOTH SIDES AND THAT MADE THE RELEASE BUILD UNVALIDATED, which is the same defect one
     layer down from the one this transport was written to fix. `content.mojom.Renderer` moved the typing onto
     the boundary the threat is on; with every incoming check compiled out of release, the untrusted renderer's
     records reached `this.impl[m.js]` unexamined there — and worse, the classification INVERTED: with the
     unknown-ordinal DCHECK stripped, `m` stayed undefined, `validating` was already false by the time the
     TypeError surfaced, and the one number that reports a refused record counted it as this process breaking
     rather than the peer. A validator that exists only in dev is a validator the shipped extension does not
     have. */
  function checkValues(assert, where, decls, vals) {
    assert(Array.isArray(vals) && vals.length === decls.length,
           where + " carried " + (Array.isArray(vals) ? vals.length : "no") + " value(s) where its mojom " +
           "declares " + decls.length + " — a short list reads every later parameter one position early, which " +
           "is a wrong call rather than a missing one");
    for (var i = 0; i < decls.length; i++)
      assert(typeOk(decls[i].type, vals[i]),
             where + " parameter `" + decls[i].name + "` is not the `" + decls[i].type + "` its mojom " +
             "declares — " + decls[i].why);
  }

  /* THE OUTGOING PARAMETER LIST, WALKED OFF THE DECLARATION AND LOOKED UP BY NAME. A positional argument list
     is a SECOND copy of the parameter list kept by hand at every call site, and the copies go short in
     silence: the only thing a positional caller can be told is a COUNT, so `checkValues` above can say 13
     against 14 and cannot say WHICH parameter is absent — and where every declared parameter is a `string`, a
     caller that pads to the right count in the WRONG ORDER passes every check this transport has, which is
     not a degraded call but a wrong one carrying plausible values. That is the same defect `defineInterface`
     refuses one layer up, where a method is identified by its ORDINAL and never by its position, and the same
     one the reply direction has never had: a reply is already handed back keyed by its declared names, and
     this file's own paragraph said so as the reason a reply field can be added without every call site
     re-counting. The two directions are now that one shape.
     A DECLARED PARAMETER WITH NO OWN PROPERTY REFUSES BY NAME, and so does a property no parameter declares.
     Both skews are real and they are opposite: a caller OLDER than the interface has a hole, a caller NEWER
     than it has a key nobody reads — and a key nobody reads is a value its author believes it sent.
     A METHOD THAT DECLARES NO PARAMETERS TAKES NO ARGUMENT AT ALL, which is a positive statement rather than
     an omitted record: there is no record to send, so `{}` would be a caller asserting the existence of one.
     The JS arity is therefore asserted as well — exactly one record where the mojom declares parameters, none
     where it declares none.
     THE COMPOSITION DOES NOT DEPEND ON THE ASSERTS, because they are DCHECK-class and a release build strips
     them. A missing name then reaches the wire as `undefined` and the RECEIVER refuses it under `checkValues`'
     CHECK-class arm, which is fatal in release — the boundary SECURITY.md calls hostile is the one that stays
     validated, and this end's asserts are this process's own logic being correct. */
  function placeParams(m, argv) {
    DCHECK(argv.length === (m.params.length === 0 ? 0 : 1),
           "a call to " + m.iface + "." + m.name + " passed " + argv.length + " JS argument(s) — a mojo call " +
           "takes ONE record keyed by the parameter names its mojom declares, and no argument at all where it " +
           "declares none (" + m.params.length + " parameter(s) here)");
    if (m.params.length === 0) return [];
    var rec = argv[0], vals = [], declared = new Set(), i, keys, k;
    DCHECK(!!rec && typeof rec === "object" && !Array.isArray(rec),
           "a call to " + m.iface + "." + m.name + " passed something other than a parameter record — an " +
           "array here would be the positional list this transport stopped taking, and it stopped taking it " +
           "because a short one can only report a count and never the name of what is missing");
    for (i = 0; i < m.params.length; i++) {
      declared.add(m.params[i].name);
      DCHECK(!!rec && Object.prototype.hasOwnProperty.call(rec, m.params[i].name),
             "a call to " + m.iface + "." + m.name + " has no value for the parameter `" + m.params[i].name +
             "`, which its mojom declares — " + m.params[i].why + ". This caller is OLDER than the interface, " +
             "and the one thing it must not do is send the values it does have anyway: that reads every later " +
             "parameter one position early, which is a wrong call rather than a missing one");
      vals.push(rec === null || rec === undefined ? undefined : rec[m.params[i].name]);
    }
    keys = (rec === null || rec === undefined) ? [] : Object.keys(rec);
    for (i = 0; i < keys.length; i++) {
      k = keys[i];
      DCHECK(declared.has(k),
             "a call to " + m.iface + "." + m.name + " carries `" + k + "`, which its mojom declares no " +
             "parameter of — this caller is NEWER than the interface or has misspelled a name, and either " +
             "way the value goes nowhere while its author believes it crossed");
    }
    return vals;
  }

  /* HANDLES ARE COLLECTED INTO THE TRANSFER LIST, which is what makes `handle<message_pipe>` a real pipe pass
     rather than a clone: a transferred port is DETACHED in the sender and re-materialized in the receiver, so
     the endpoint genuinely moves and the sender provably no longer holds it. */
  function collectHandles(decls, vals, into) {
    for (var i = 0; i < decls.length; i++)
      if (isHandle(decls[i].type) && vals[i] !== null) into.push(vals[i]);
  }

  /* ── THE IDL. `defineInterface` is what a generated `*.mojom.js` module does in Chromium: it is the single
     description both processes validate against, and it exists per realm because both realms load mojom.js. */
  var _defs = new Map();
  function lowerFirst(s) { return s.charAt(0).toLowerCase() + s.slice(1); }

  function defineInterface(def) {
    DCHECK(!!def && typeof def.name === "string" && /^[a-z]+\.mojom\.[A-Z][A-Za-z0-9]*$/.test(def.name),
           "a mojom interface must be named `<module>.mojom.<Interface>` — the module half is what says which " +
           "layer owns it (a network-service interface and a content-layer interface are not interchangeable) " +
           "and it is the name a bind request carries");
    /* NO `version` FIELD, AND ITS ABSENCE IS THE MECHANISM. It was declared here, asserted here, posted on
       every bind and compared by the peer — and it decided nothing, because it was the literal `0` in both
       interfaces and no change to a method list moves it. What says whether the two processes were built
       together is `wireContract()` below, which is DERIVED from these declarations and therefore cannot be
       forgotten. A number a human maintains beside the thing it describes is the thing that drifts. */
    DCHECK(!_defs.has(def.name),
           def.name + " is defined twice in this realm — an interface is ONE description that both ends " +
           "validate against, and a second definition means two of them with nothing to say which is on the wire");
    DCHECK(Array.isArray(def.methods) && def.methods.length > 0,
           def.name + " declares no methods — an interface IS its method list");
    var byOrd = new Map(), byJs = new Map();
    for (var i = 0; i < def.methods.length; i++) {
      var m = def.methods[i];
      DCHECK(!!m && typeof m.ordinal === "number" && (m.ordinal | 0) === m.ordinal && m.ordinal >= 0,
             def.name + " has a method with no ordinal — the ORDINAL is what the wire carries (mojom's `@0`), " +
             "so a method identified by its position would change identity the day the list is reordered");
      DCHECK(!byOrd.has(m.ordinal),
             def.name + " gives ordinal " + m.ordinal + " to two methods — an ordinal is the method's identity " +
             "on the wire and a repeat routes one call to whichever was registered last");
      DCHECK(typeof m.name === "string" && /^[A-Z][A-Za-z0-9]*$/.test(m.name),
             def.name + " has a method whose name is not mojom's CamelCase");
      checkDecls(def.name + "." + m.name, m.params);
      DCHECK(m.reply === null || Array.isArray(m.reply),
             def.name + "." + m.name + " must declare a reply list or `null` — null is the positive statement " +
             "that this method is fire-and-forget, which is a different contract from a reply carrying nothing");
      if (m.reply !== null) checkDecls(def.name + "." + m.name + "'s reply", m.reply);
      m.iface = def.name;
      m.js = lowerFirst(m.name);   /* Chromium's JS bindings lower the first letter; `CheckCorb` → `checkCorb` */
      byOrd.set(m.ordinal, m);
      byJs.set(m.js, m);
    }
    def.byOrd = byOrd;
    def.byJs = byJs;
    _defs.set(def.name, def);
    return def;
  }

  /* ── THE WIRE CONTRACT OF THIS REALM: everything two peers must agree on for a record to mean the same thing
     at both ends, DERIVED from the declarations above and from this transport's own constants. Mojo gets this
     from its build — both ends are generated from one `.mojom` and a mismatch does not link. Here the
     description is READ TWICE (see divergence (3) at the top of this file), so the agreement is asserted at
     runtime instead, on the handshake, once per direction.
     WHAT IS IN IT IS EXACTLY WHAT THE WIRE CARRIES, AND NOTHING ELSE, because a contract that fires on a change
     the wire cannot see is a confident false red. So: the transport's wire version and its two flag VALUES (a
     peer whose `kMessageIsResponse` differed would route every reply as a request); the interface NAME, which
     this transport puts on the wire deliberately (divergence (1)); each method's ORDINAL, which is the method's
     identity here; and the declared TYPE of every parameter and every reply field, in order, since position is
     how a value is read back. Deliberately ABSENT: a method's NAME (this file's own rule at the top — renaming
     is not a wire change; a rename that reaches only one of mojom.js and renderer.html is caught inside the
     frame by `rendererImpl`'s own binding asserts), a parameter's name (the record keyed by it never crosses —
     only the positional array does), and every `why` (a sentence a validator prints, not a shape).
     THE COMPARISON IS OVER THE TEXT AND NOT OVER A HASH, so a mismatch NAMES the method and the two shapes
     rather than reporting that two opaque numbers differ. `wireDigest` exists only to put a short handle in a
     diagnostic; it decides nothing, which is why a hand-rolled fold is not a security primitive here. */
  function typeList(decls) {
    var t = [];
    for (var i = 0; i < decls.length; i++) t.push(decls[i].type);
    return t.join(",");
  }

  function wireContract() {
    var lines = ["transport " + WIRE + " " + F_EXPECTS_RESPONSE + " " + F_IS_RESPONSE], names = [];
    _defs.forEach(function (d, n) { names.push(n); });
    /* SORTED, so the contract is a property of the DECLARATIONS and not of the order a realm happened to load
       them in — two realms that define the same interfaces must not disagree over insertion order. */
    names.sort();
    for (var i = 0; i < names.length; i++) {
      var d = _defs.get(names[i]), ords = [];
      d.byOrd.forEach(function (m, o) { ords.push(o); });
      ords.sort(function (a, b) { return a - b; });
      for (var j = 0; j < ords.length; j++) {
        var m = d.byOrd.get(ords[j]);
        /* A FIRE-AND-FORGET METHOD HAS NO ARROW AND A REPLY OF NOTHING HAS AN EMPTY ONE — the two are
           different contracts (one side would park on a reply the other never sends), so they must be
           different lines. */
        lines.push(d.name + "@" + m.ordinal + "(" + typeList(m.params) + ")" +
                   (m.reply === null ? "" : "->(" + typeList(m.reply) + ")"));
      }
    }
    return lines.join("\n");
  }

  /* A SHORT HANDLE FOR A HUMAN READING A STATS RECORD, and nothing rests on it: the handshake compares the
     CONTRACT. FNV-1a over the contract text, 32-bit, printed base 36. */
  function wireDigest(contract) {
    var h = 0x811c9dc5;
    for (var i = 0; i < contract.length; i++) {
      h ^= contract.charCodeAt(i);
      h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
    }
    return h.toString(36);
  }

  /* THE FIRST LINE THE TWO CONTRACTS DISAGREE ON, which is what the refusal has to say to be worth having. A
     peer that is a whole generation away differs on many lines and the first one is where a reader starts; a
     peer that changed one parameter type differs on exactly one and this names it. */
  function wireFirstDiff(mine, theirs) {
    var a = mine.split("\n"), b = theirs.split("\n"), n = Math.max(a.length, b.length);
    for (var i = 0; i < n; i++)
      if (a[i] !== b[i])
        return "first difference at line " + (i + 1) + ": this process declares `" +
               (a[i] === undefined ? "(nothing — its contract ends here)" : a[i]) + "` and the peer declares `" +
               (b[i] === undefined ? "(nothing — its contract ends here)" : b[i]) + "`";
    return "the contracts are equal, which contradicts the comparison that produced this message";
  }

  /* THE REFUSAL, ONE FUNCTION SO BOTH DIRECTIONS PRINT THE SAME SENTENCE. It is a CHECK and not a DCHECK: a
     skew is a PACKAGING failure, and release is precisely the build that cannot fix one and in which a
     stripped check would let a call be dispatched to another generation's ordinal — the untrusted renderer
     placing operands into its own linear memory by a declared type the caller did not send. */
  function requireWireAgreement(peerName, where, theirs) {
    var mine = wireContract();
    CHECK(typeof theirs === "string" && theirs !== "",
          where + " from " + peerName + " carried no wire contract — every record that opens a channel states " +
          "the description this process speaks, because mojom.js is read once by the offscreen and once again " +
          "to build the frame's invitation, and nothing else in this extension ties those two reads to one " +
          "generation");
    CHECK(theirs === mine,
          peerName + " speaks a different wire contract than this process (" + where + ") — both ends ship out " +
          "of ONE extension, so this is a build that packaged two generations of mojom.js/mojo.js and not a " +
          "compatibility case to negotiate. " + wireFirstDiff(mine, theirs));
  }

  /* ── THE BAD-MESSAGE COUNT, WHICH IS THE ONE NUMBER A VALIDATOR OWES THE OUTSIDE. A validation failure kills
     the connection (see `_crash`), so from inside there is nothing left to ask; from OUTSIDE, "how many records
     did this process's validator refuse" is what tells a healthy boundary from one whose peer is sending shapes
     the mojom does not declare. It reads 0 in every healthy run, and the direction it protects is the one
     SECURITY.md names: the UNTRUSTED renderer is the peer whose messages must be assumed hostile, so the count
     that matters is the one taken in the TRUSTED zone.
     IT COUNTS VALIDATION AND NOT EVERY CRASH, and the two are told apart by WHERE the throw came from: a record
     that failed a declared type is the PEER being broken, while an implementation that threw while serving a
     well-formed call is THIS process being broken (for a renderer, a WASM abort — a recorded outcome, not a bad
     message). Both kill the connection, exactly as Mojo's ReportBadMessage does; only the first is counted.
     AND IT COUNTS IN RELEASE, WHICH IT DID NOT. Every incoming check was DCHECK-class, so a release build had
     no validator at all on the boundary SECURITY.md calls untrusted — and the classification INVERTED there
     rather than merely weakening: with the unknown-ordinal check stripped, `m` stayed undefined until the
     dispatch line, by which point `validating` was false, so the one number that reports a refused record read
     0 for the very records it exists to count. `checkValues` now takes the severity from its call site and
     every read of an incoming record is CHECK-class. */
  var _rejected = 0;

  function interfaceOf(name) {
    var d = _defs.get(name);
    DCHECK(d !== undefined,
           "no mojom interface named `" + name + "` is defined in this realm — extension/mojom.js is the one " +
           "place they are declared and every realm that speaks this transport loads it, so a missing one is a " +
           "realm that did not load the IDL rather than a name to be looked up somewhere else");
    return d;
  }

  /* ── THE PROCESS-WIDE BINDER MAP, which is `content::BinderMap`: what THIS process implements, registered by
     the component that implements it, consulted for every peer that asks. It is not per-connection because an
     interface is a capability of the PROCESS — a second peer asking for the same name must reach the same
     implementation, and a per-connection registry would let two of them diverge. */
  var _binders = new Map();
  function exposeInterface(name, impl) {
    var def = interfaceOf(name);
    DCHECK(!_binders.has(name),
           name + " is exposed twice in this realm — the binder map answers a bind request by NAME, so a second " +
           "registration silently decides which implementation every future peer reaches");
    DCHECK(!!impl && typeof impl === "object", name + " was exposed with no implementation object");
    def.byJs.forEach(function (m) {
      DCHECK(typeof impl[m.js] === "function",
             "the implementation exposed for " + name + " has no `" + m.js + "` — an interface is the WHOLE " +
             "list of its methods, and a peer calling a missing one would be answered by a TypeError inside a " +
             "message handler, which reaches nobody: the caller stays parked on a reply that is never coming");
    });
    _binders.set(name, impl);
  }

  /* ── LIVE ENDPOINTS OF THIS REALM. Each `Remote` and each `Receiver` holds ONE END of one pipe, so this is
     the realm's open-endpoint count and it is what a probe reads to say how many pipes this process has. It is
     kept here rather than per connection because a pipe outlives the bind that created it and belongs to the
     process, exactly as a handle count does. */
  var _endpoints = new Set();
  function stats() {
    var remotes = [], receivers = [];
    _endpoints.forEach(function (h) { (h.kind === "remote" ? remotes : receivers).push(h.def.name); });
    /* `wire` IS WHICH DESCRIPTION THIS REALM HOLDS, so a reader of any stats record can see WHETHER two
       processes are the same generation rather than being told that they are. It replaces the `@0` that used
       to be appended to every interface name: a constant printed beside twenty names said nothing, and the one
       fact it pretended to carry is this one. */
    return { remotes: remotes.sort(), receivers: receivers.sort(), endpoints: _endpoints.size,
             badMessages: _rejected, wire: wireDigest(wireContract()) };
  }

  /* ── THE CALLER END. One JS method per mojom method, named by Chromium's JS-binding rule, taking ONE RECORD
     keyed by the parameter's declared names and answering an OBJECT keyed by the reply's — the same shape in
     both directions, for the reason `placeParams` states: a name can report what is missing and a position
     cannot. It used to take the parameters positionally, and this paragraph used to argue for the reply's
     naming on exactly the ground the send side then lacked. A fire-and-forget method (`reply: null`) returns
     nothing at all: there is no promise to await, because there is no answer, and handing one back would
     invite a caller to wait on it. */
  function Remote(conn, def, port) {
    var self_ = this;
    this.kind = "remote";
    this.conn = conn;
    this.def = def;
    this.port = port;
    this._next = 1;
    this._await = new Map();
    port.onmessage = function (e) { self_._onmessage(e.data); };
    _endpoints.add(this);
    conn._endpoints.add(this);
    def.byJs.forEach(function (m) {
      self_[m.js] = function () { return self_._send(m, arguments); };
    });
  }

  Remote.prototype._send = function (m, argv) {
    DCHECK(!this.conn.dead,
           "a call was made on " + m.iface + "." + m.name + " after " + this.conn.name + " died (" +
           this.conn.deadReason + ") — what failed is that process, so this call would be a second crash " +
           "reported as a first, and its caller would park on a reply nothing is left to produce");
    var args = placeParams(m, argv);
    checkValues(DCHECK, "a call to " + m.iface + "." + m.name, m.params, args);
    var xfer = [];
    collectHandles(m.params, args, xfer);
    var id = 0, p;
    if (m.reply !== null) {
      var self_ = this;
      id = this._next++;
      p = new Promise(function (res, rej) { self_._await.set(id, { resolve: res, reject: rej }); });
    }
    this.port.postMessage(this.conn._envelope({ w: WIRE, i: m.iface, o: m.ordinal, r: id,
                                                f: m.reply !== null ? F_EXPECTS_RESPONSE : 0, a: args }), xfer);
    return p;
  };

  /* EVERY ASSERT ON THIS PATH READS THE INCOMING RECORD, so every one is CHECK-class — see `checkValues`. */
  Remote.prototype._onmessage = function (env) {
    try {
      CHECK(!!env && env.w === WIRE && typeof env.i === "string" && typeof env.o === "number" &&
            typeof env.r === "number" && typeof env.f === "number" && Array.isArray(env.a),
            "a record on a " + this.def.name + " pipe is not this transport's — a message is a wire version, " +
            "the interface it belongs to, a method ordinal, a request id, flags and the parameter list");
      this.conn._absorb(env);
      CHECK(env.i === this.def.name,
            "a " + this.def.name + " pipe carried a message for " + env.i + " — the interface name is on the " +
            "wire precisely so a broker that bound the wrong implementation to this pipe names both " +
            "interfaces here rather than diverging silently at whichever ordinal they stop sharing");
      CHECK((env.f & F_IS_RESPONSE) !== 0,
            "a Remote received a REQUEST on " + this.def.name + " — a pipe has one Remote end and one " +
            "Receiver end, so a request arriving here is two Remotes bound to one pipe and the real receiver " +
            "is bound to nothing");
      var m = this.def.byOrd.get(env.o);
      CHECK(m !== undefined,
            this.def.name + " has no method at ordinal " + env.o + " — both ends of this pipe are built from " +
            "one extension and agreed their whole wire contract at bind time, so an unknown ordinal here is a " +
            "peer that changed generation after the handshake");
      CHECK(m.reply !== null,
            m.iface + "." + m.name + " is declared fire-and-forget and answered anyway");
      checkValues(CHECK, "the reply to " + m.iface + "." + m.name, m.reply, env.a);
      var w = this._await.get(env.r);
      CHECK(w !== undefined,
             "a reply arrived for " + m.iface + "." + m.name + " request id " + env.r + ", which this endpoint " +
             "never made — the request id is the whole routing table for an answer, so an unknown one means " +
             "the call that IS outstanding will never be resolved");
      this._await.delete(env.r);
      var rec = {};
      for (var i = 0; i < m.reply.length; i++) rec[m.reply[i].name] = env.a[i];
      w.resolve(rec);
      /* EVERYTHING ABOVE IS VALIDATION — a Remote runs no implementation, so a throw on this path is always the
         peer having sent a record this interface does not declare, and `resolve` runs no reaction here. */
    } catch (e) { _rejected++; this.conn._crash(e); }
  };

  /* ── THE IMPLEMENTATION END. An impl method returns the reply as an object keyed by the declared names, or a
     promise of one; a fire-and-forget impl returns nothing. Both directions are validated, at BOTH ends: Mojo
     validates on receive, and this validates on send as well, because CLAUDE.md puts the assert at the value's
     ORIGIN — a producer that stopped writing a reply field is caught in its own process, on the line that
     returned it, rather than one hop later in a process that can only say it was handed something wrong. */
  function Receiver(conn, def, impl, port) {
    var self_ = this;
    this.kind = "receiver";
    this.conn = conn;
    this.def = def;
    this.impl = impl;
    this.port = port;
    port.onmessage = function (e) { self_._onmessage(e.data); };
    _endpoints.add(this);
    conn._endpoints.add(this);
  }

  Receiver.prototype._onmessage = function (env) {
    var self_ = this;
    /* THE LINE THAT SPLITS "THE PEER IS BROKEN" FROM "THIS PROCESS IS BROKEN". Everything before the dispatch
       reads the INCOMING record, so a throw there is a bad message and is counted as one; everything from the
       dispatch on is this process's own implementation, whose failure is its own. Both still kill the
       connection — a bad message is not an error return — and only the classification differs. */
    var validating = true;
    try {
      CHECK(!!env && env.w === WIRE && typeof env.i === "string" && typeof env.o === "number" &&
            typeof env.r === "number" && typeof env.f === "number" && Array.isArray(env.a),
            "a record on a " + this.def.name + " pipe is not this transport's — a message is a wire version, " +
            "the interface it belongs to, a method ordinal, a request id, flags and the parameter list");
      this.conn._absorb(env);
      CHECK(env.i === this.def.name,
            "a " + this.def.name + " pipe carried a message for " + env.i + " — see the Remote's copy of this " +
            "assert: the name is on the wire so a crossed bind names both interfaces");
      CHECK((env.f & F_IS_RESPONSE) === 0,
            "a Receiver received a RESPONSE on " + this.def.name + " — nothing on this end ever called out, " +
            "so this pipe has two Receivers and the Remote is bound to nothing");
      var m = this.def.byOrd.get(env.o);
      CHECK(m !== undefined,
            this.def.name + " has no method at ordinal " + env.o + " — both ends are built from one extension " +
            "and agreed their whole wire contract at bind time, so an unknown ordinal here is a peer that " +
            "changed generation after the handshake");
      CHECK(((env.f & F_EXPECTS_RESPONSE) !== 0) === (m.reply !== null),
            "the caller of " + m.iface + "." + m.name + " disagrees with the mojom about whether it answers — " +
            "one side would park on a reply the other will never send, or send one nobody is waiting for");
      /* THE REQUEST ID IS PART OF THE INCOMING RECORD, SO IT IS CHECKED BEFORE THE IMPLEMENTATION RUNS. It was
         asserted AFTER the dispatch, which meant a call declaring a reply and carrying id 0 executed its C
         entry — `qjs_init` parsing a document, `qjs_provide` handing a body to a parked flow — and only then
         refused the record. A bad message must be refused before it has an effect; a refusal that arrives
         after the side effect is a report, not a gate. */
      CHECK(m.reply === null || env.r !== 0,
            "a call to " + m.iface + "." + m.name + " expects a reply and carried request id 0, which is the " +
            "id a fire-and-forget message uses — there is no id to answer on");
      checkValues(CHECK, "a call to " + m.iface + "." + m.name, m.params, env.a);
      validating = false;
      var ret = this.impl[m.js].apply(this.impl, env.a);
      if (m.reply === null) {
        DCHECK(ret === undefined,
               m.iface + "." + m.name + " is declared fire-and-forget and its implementation returned a value " +
               "— nothing carries it anywhere, so a result computed there is a result discarded");
        return;
      }
      Promise.resolve(ret).then(function (rec) {
        DCHECK(!!rec && typeof rec === "object",
               "the implementation of " + m.iface + "." + m.name + " answered with no reply record — a method " +
               "that declares a reply returns an object keyed by the reply's declared names");
        var vals = [];
        for (var i = 0; i < m.reply.length; i++) vals.push(rec[m.reply[i].name]);
        checkValues(DCHECK, "the reply " + m.iface + "." + m.name + "'s implementation returned", m.reply, vals);
        var xfer = [];
        collectHandles(m.reply, vals, xfer);
        self_.port.postMessage(self_.conn._envelope({ w: WIRE, i: m.iface, o: m.ordinal, r: env.r,
                                                      f: F_IS_RESPONSE, a: vals }), xfer);
      }, function (e) { self_.conn._crash(e); });
    } catch (e) { if (validating) _rejected++; this.conn._crash(e); }
  };

  /* ── THE CONNECTION: one peer process, reached over the primordial pipe the platform's process creation
     already gave us. It carries exactly three kinds of record and no capability at all — an invitation
     acceptance, a bind request, and the child's abort — because every capability rides a pipe of its own.
     THE TWO ROLES ARE NOT SYMMETRIC AND ARE ASSERTED APART. A PARENT holds a child's output (nothing else can
     read it) and decorates the errors it raises with the tail of it; a CHILD drains its own output into every
     record it posts and reports its own death, because there is nothing above a parent to report to. Each role
     asserts the halves it must have rather than defaulting a missing hook to a no-op. */
  function Connection(transport, opts) {
    var self_ = this;
    DCHECK(!!transport && typeof transport.post === "function" && typeof transport.listen === "function",
           "a mojo Connection needs a primordial-pipe transport — `post(record, transfer)` and `listen(cb)` " +
           "over whatever the platform's process creation handed back (a Worker handle, a MessagePort)");
    DCHECK(!!opts && (opts.role === "parent" || opts.role === "child"),
           "a mojo Connection must state which side of the process boundary it is — the roles are not " +
           "symmetric (only a parent holds the child's output, only a child can report its own death)");
    DCHECK(!!opts && typeof opts.name === "string" && opts.name !== "",
           "a mojo Connection must name its PEER — every assert and every rejection this connection raises " +
           "says which process it is about, and `undefined died` names nothing");
    this.role = opts.role;
    this.name = opts.name;
    this.dead = false;
    this.deadReason = null;
    this._t = transport;
    this._endpoints = new Set();
    if (this.role === "parent") {
      DCHECK(typeof opts.onStdio === "function",
             "a parent connection to " + opts.name + " must say where the child's output goes — the `@WHY` a " +
             "CHECK prints immediately before abort() is the only thing that says what broke, and a child that " +
             "printed it into a dropped field is a crash that took its own cause with it");
      DCHECK(typeof opts.decorate === "function",
             "a parent connection to " + opts.name + " must say how an error it raises is decorated — the " +
             "rejection a caller sees is the only place the child's last lines can be attached");
      this._onStdio = opts.onStdio;
      this._decorate = opts.decorate;
      this._drainStdio = null;
      this.ready = new Promise(function (res, rej) { self_._readyRes = res; self_._readyRej = rej; });
    } else {
      DCHECK(typeof opts.drainStdio === "function",
             "a child connection must say how to drain its own output — it rides every record this process " +
             "posts, because which pipe answers next is not knowable and lines left here are lines nobody reads");
      this._drainStdio = opts.drainStdio;
      this._onStdio = null;
      this._decorate = null;
      this.ready = null;
    }
    transport.listen(function (m) { self_._onprimordial(m); });
  }

  Connection.prototype._envelope = function (e) {
    if (this.role === "child") e.out = this._drainStdio();
    return e;
  };

  /* THE INCOMING RECORD AGAIN, SO CHECK-CLASS AGAIN — and this one is why the severity matters even for a
     field nothing dispatches on: with the assert stripped, a child record missing `out` reached `_onStdio`
     as `undefined` and the parent's absorber indexed it. */
  Connection.prototype._absorb = function (env) {
    if (this.role === "child") {
      CHECK(env.out === undefined,
            "the parent sent stdio to a child — output flows one way across a process boundary, and a field " +
            "arriving in this direction is a parent that has started narrating into its own child");
      return;
    }
    CHECK(Array.isArray(env.out),
          "a record from " + this.name + " carried no output field — every record a child posts drains the " +
          "process's output with it, so a missing one is a child that stopped reporting and a crash whose " +
          "cause stays inside the process that died");
    this._onStdio(env.out);
  };

  Connection.prototype._post = function (m, xfer) {
    /* THE TRANSFER LIST IS STATED AT EVERY CALL, including where it is empty. A `xfer || []` default here
       would be one line, and it would be the line on which a caller that forgot the handle it MEANT to send
       posts a record whose port is silently cloned-and-refused rather than moved. */
    DCHECK(Array.isArray(xfer),
           "a record on the primordial pipe to " + this.name + " was posted with no transfer list — a message " +
           "either carries handles or states that it carries none, and the two are different messages");
    this._t.post(this._envelope(m), xfer);
  };

  /* THE CHILD'S ACCEPTANCE, which is `mojo::IncomingInvitation::Accept` and is also the thing a bare `onload`
     could never say: a worker script that LOADED is not a module that INSTANTIATED, and the parent must not
     send a call into a program that does not exist. A failure travels IN the acceptance rather than being
     swallowed, because there is no timeout on the far side — a wall clock there would report a loaded machine
     as a broken transport (CLAUDE.md §Testing). */
  /* IT CARRIES THIS PROCESS'S WIRE CONTRACT, which is the FIRST record that travels child->parent and
     therefore the earliest point the parent can refuse a peer of another generation — before it binds a
     single interface, so no ordinal is ever dispatched across a skew. A refused acceptance (`ok:false`)
     carries none: the program that did not start may BE mojo.js, so there is no contract to state. */
  Connection.prototype.acceptInvitation = function (ok, err) {
    DCHECK(this.role === "child", "only a child accepts an invitation");
    DCHECK(typeof ok === "boolean" && (err === null || typeof err === "string"),
           "an invitation acceptance states whether this process started and, when it did not, why");
    this._post({ w: WIRE, k: "accept-invitation", ok: ok, err: err,
                 wire: ok ? wireContract() : null }, []);
  };

  /* THE BROKER'S ONE REQUEST. A name goes out with one end of a fresh pipe; the peer's binder map answers it.
     This is the whole of what the primordial pipe carries besides life and death, and it is why a new
     capability is a new INTERFACE rather than a new `op` hand-written into two switch statements. */
  Connection.prototype.bindInterface = function (name) {
    DCHECK(!this.dead,
           "an interface was requested from " + this.name + " after it died (" + this.deadReason + ")");
    var def = interfaceOf(name);
    var ch = new MessageChannel();
    /* THE CONTRACT RIDES THE BIND, which is the first and only record that travels parent->child, so each
       direction asserts the peer's description exactly once, on the earliest record that carries it. Neither
       is a fallback for the other and neither is redundant: the parent's refusal gates `ready`, so no
       interface is bound across a skew by a host that awaits it, and this one gates the PIPE, so a host that
       binds without awaiting `ready` — one line, and `bindInterface` asks only that the peer is alive — is
       refused here instead of being served an ordinal from another generation. */
    this._post({ w: WIRE, k: "bind", iface: name, wire: wireContract(), port: ch.port2 }, [ch.port2]);
    return new Remote(this, def, ch.port1);
  };

  Connection.prototype._onprimordial = function (m) {
    try {
      CHECK(!!m && m.w === WIRE && typeof m.k === "string",
            "a record on the primordial pipe to " + this.name + " is not this transport's — it carries a wire " +
            "version and a kind");
      this._absorb(m);
      if (m.k === "accept-invitation") {
        CHECK(this.role === "parent", "a child received an invitation acceptance");
        CHECK(typeof m.ok === "boolean", "an invitation acceptance from " + this.name + " states no outcome");
        if (m.ok) {
          /* THE PEER IS REFUSED HERE OR NOT AT ALL, and this is before `ready` resolves — so a caller that
             awaited a renderer either has one built from this generation or has an error, never a live pipe
             onto a description this process does not speak. */
          requireWireAgreement(this.name, "its invitation acceptance", m.wire);
          this._readyRes(this);
          return;
        }
        this._die(this.name + " did not start: " + m.err);
        return;
      }
      if (m.k === "bind") { this._onbind(m); return; }
      if (m.k === "abort") {
        CHECK(this.role === "parent",
              "a parent reported its own death to a child — there is nothing above a parent to report to, and " +
              "a child that believed it would go on making calls into a process that is gone");
        CHECK(typeof m.reason === "string", "an abort from " + this.name + " carries the reason it died");
        this._die(this.name + " died: " + m.reason);
        return;
      }
      CHECK_FAIL("the primordial pipe to " + this.name + " carried a record kind it does not serve: `" + m.k +
                 "` — a capability is an INTERFACE brokered onto its own pipe, never a new kind here. It is " +
                 "CHECK-class with the rest of this path: as a DFAIL it was stripped from release, where the " +
                 "record then fell out of this function and was DROPPED, so a peer that asked for something " +
                 "waited on an answer nothing was left to produce");
      /* THE PRIMORDIAL PIPE RUNS NO IMPLEMENTATION EITHER — an acceptance, a bind and an abort are all records
         this READS — so a throw here is the peer, and it is counted with the rest. */
    } catch (e) { _rejected++; this._crash(e); }
  };

  Connection.prototype._onbind = function (m) {
    CHECK(typeof m.iface === "string" && m.port instanceof MessagePort,
          "a bind request from " + this.name + " is not one — it names an interface, states the wire contract " +
          "it was built from, and carries the pipe end this process is to bind its implementation to");
    /* THE CONTRACT IS CHECKED BEFORE THE NAME IS LOOKED UP, because a peer of another generation may well ask
       for an interface this one does not define, and "no such interface" would then be a true statement that
       names the wrong fault. */
    requireWireAgreement(this.name, "a bind request for " + m.iface, m.wire);
    var def = interfaceOf(m.iface);
    var impl = _binders.get(m.iface);
    CHECK(impl !== undefined,
          this.name + " asked this process for `" + m.iface + "`, which nothing here implements — an interface " +
          "reaches a peer only if a component registered it with mojo.exposeInterface, and a bind nobody " +
          "answers leaves the caller holding a pipe with no receiver on the far end and no error either");
    new Receiver(this, def, impl, m.port);
  };

  /* DEATH. One place, whichever way it arrives: the child said so, the child never started, or an invariant in
     this process broke. Every outstanding call rejects — Mojo's peer-closed — and the connection stays dead,
     which is the fail-closed direction a security gate must have: a browser process that is not there must
     refuse every later fetch with the same reason rather than intermittently re-provisioning into a state
     where nothing judges the body. */
  Connection.prototype._die = function (reason) {
    if (this.dead) return;
    this.dead = true;
    this.deadReason = reason;
    var self_ = this;
    this._endpoints.forEach(function (h) {
      /* AND THE ENDPOINT LEAVES THE REALM'S OPEN SET WITH THE CONNECTION IT BELONGED TO. `stats().endpoints` is
         "open pipe endpoints in this process", and a dead connection's are not open — nothing may be sent on
         them and nothing can arrive. This was invisible while the only peer was a SINGLETON worker that never
         dies; a renderer is provisioned and destroyed per agent cluster, so a count that only ever rose would
         report a document's whole history of instances as its current IPC surface. */
      _endpoints.delete(h);
      if (h.kind !== "remote") return;
      h._await.forEach(function (w) {
        var e = new Error(reason);
        if (self_.role === "parent") self_._decorate(e);
        w.reject(e);
      });
      h._await.clear();
    });
    if (this.role === "parent") {
      var e2 = new Error(reason);
      this._decorate(e2);
      this._readyRej(e2);   /* a no-op once ready has resolved */
    }
  };

  /* AN ORDERLY SHUTDOWN, WHICH IS A DIFFERENT EVENT FROM A DEATH AND REACHES THE SAME STATE. A peer this
     process deliberately tears down — a renderer frame removed from the document — is not a peer that broke,
     but everything true afterwards is identical: no call may be made into it, no reply can arrive from it, and
     both must SAY so rather than posting into a closed port. It is `mojo::Remote::reset` at the scale of a
     connection, and it exists so that "this peer is gone" has ONE representation instead of a second `_dead`
     flag kept beside it by whoever owns the process. */
  Connection.prototype.close = function (reason) {
    DCHECK(typeof reason === "string" && reason !== "",
           "a connection to " + this.name + " was closed with no reason — the reason is what every later call " +
           "into it prints, and `undefined` names neither who closed it nor why");
    this._die(this.name + " was closed: " + reason);
  };

  /* THE CALLS THIS CONNECTION IS STILL WAITING ON, summed across its Remotes. It is asked by the owner of the
     PROCESS at teardown: a peer torn down with a call outstanding is a caller parked on an answer that can no
     longer be produced, which is silent everywhere. */
  Connection.prototype.outstandingCalls = function () {
    var n = 0;
    this._endpoints.forEach(function (h) { if (h.kind === "remote") n += h._await.size; });
    return n;
  };

  /* A BROKEN MESSAGE IS THE PEER BEING BROKEN, so it kills the connection rather than returning an error — Mojo
     calls this ReportBadMessage and it kills the child. A CHILD posts the abort first, so the parent learns of
     the death it is about to have; a PARENT has nobody to tell. Both then RETHROW: the assert is the mechanism
     and a transport that swallowed it would be the one place the ONE assertion mechanism is locally disabled. */
  Connection.prototype._crash = function (e) {
    var reason = String((e && e.stack) || e);
    if (this.role === "child" && !this.dead) this._post({ w: WIRE, k: "abort", reason: reason }, []);
    this._die(this.name + " connection aborted: " + reason);
    throw e;
  };

  g.mojo = {
    defineInterface: defineInterface,
    exposeInterface: exposeInterface,
    /* THE DESCRIPTION ITSELF, READABLE. An implementation built by WALKING the interface's own method list —
       rather than by writing one property per method and hoping the two lists stay equal — is how a mojom
       method that gained no binding crashes at its own process naming itself, instead of reaching a peer as a
       TypeError inside a message handler, which reaches nobody: the caller stays parked on a reply that is
       never coming. It is the same descriptor `Remote`/`Receiver` validate against, so there is one of it. */
    interfaceOf: interfaceOf,
    /* THE PLACEMENT, ASKED RATHER THAN COPIED — see `abiPlacement` above for why one hand-aligned list per
       caller is the shape that went short twice. Every zone that turns a declared parameter into an operand of
       a `qjs_*` entry reads it from here: the renderer that performs the call, and the Node drivers that count
       operands against what the built glue declares it accepts. */
    abiPlacement: abiPlacement,
    /* AND THE WALK OVER A WHOLE DECLARED PARAMETER LIST — see `abiOperands` above. `abiPlacement` answers for
       one parameter and a caller could still hold its own array of values in declaration order; this is what
       replaces that array, so the raw-ccall drivers place by the same names the transport's own `placeParams`
       does and neither direction of skew can be silent. */
    abiOperands: abiOperands,
    Connection: Connection,
    stats: stats,
  };
})(self);
