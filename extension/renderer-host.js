/* renderer-host.js — THE TRUSTED SIDE OF EVERY RENDERER FRAME. Chromium's name for the object that owns a
 * frame in a renderer and is the only thing that may speak to it is `RenderFrameHost`, and this is that
 * object. The offscreen — the one trusted zone (SECURITY.md) — materializes a sandboxed frame, hands it the
 * engine program, and relays the qjs_* ABI to it. It holds no analysis logic; every decision that is not
 * "which renderer, and what did it answer" stays on this side of the boundary in bridge.js.
 *
 * IT DOES NOT DECIDE THAT A RENDERER SHOULD EXIST, AND THAT IS STILL TRUE WITH NO PIPE IN IT. It used to
 * export `rendererCreate(name)`, which bridge.js called whenever its pool wanted an instance — nothing held
 * the registry, so nothing could refuse. The authority is `extension/render-process-host.js`
 * (Chromium's `RenderProcessHost`: the browser-side object that owns renderer processes and mints their IDs),
 * and `rendererLaunch` below asks it before it builds anything. What that inversion cost for one session was
 * a dedicated Worker holding the table in WASM, ordering this file back over `content.mojom.Zygote` to
 * materialize each frame; both interfaces are deleted with that Worker, because the offscreen writes the
 * Worker's program and a pipe that separates a `Map` from its only caller is a boundary that isolates
 * nothing. What SURVIVES the deletion is the property that mattered: this file cannot mint a routing id, and
 * `registerRenderer` — whose refusal is a CHECK, fatal in every build — has exactly one caller, which is the
 * only path in this extension to a renderer frame.
 *
 * AND THE DELETION CLOSED A WINDOW RATHER THAN OPENING ONE. Registration and frame creation are now in one
 * realm and one turn: `rendererLaunch` reads the renderer's program FIRST, then registers, then calls
 * `rendererFork`, whose whole prologue — the record, `_live.add`, the `appendChild` — runs synchronously
 * before its first await. So there is no moment in which the registry names a renderer this document holds no
 * frame for, which is exactly the disagreement `rendererPoolProbe`'s cross-check reports. With the fork order
 * on a pipe there was such a moment, on every boot, spanning a `fetch` of five files.
 *
 * WHAT THIS BOUNDARY IS FOR. SECURITY.md calls the QuickJS/WASM engine UNTRUSTED because it EXECUTES the
 * attacker bundle, and confines it to "the WASM sandbox + a fixed set of host edges". While every instance is
 * built by `createQJS()` in the offscreen's OWN realm that confinement is a convention: the host JS holds the
 * `Module` handle, `M.HEAPU8` is an exported view of the whole linear memory, and two instances in one realm
 * are a NAMESPACE rather than a sandbox. An `<iframe sandbox="allow-scripts">` with no `allow-same-origin` has
 * a UNIQUE OPAQUE ORIGIN — cross-origin to the extension origin — which is what lets Site Isolation give it its
 * own renderer process. The boundary then holds because the browser holds it, and what crosses is exactly what
 * this file sends.
 *
 * ONE RENDERER PER ORIGIN-KEYED AGENT CLUSTER, AND THE RULE IS NOT KEPT HERE. SECURITY.md fixes the unit: "One
 * WASM instance per ORIGIN-KEYED AGENT CLUSTER — (browsing-context group, origin)", which `bridge.js`'s
 * `clusterKeyOf` computes off browser-stated facts. This file takes that key as an ARGUMENT — it does not
 * compute it, does not admit, does not rank, and does not ask whether the cluster already has a renderer,
 * because the table that can answer that is render-process-host.js's.
 *
 * WHAT CROSSES, AND WHY IT IS SHAPED LIKE THE REST OF THIS ENGINE'S SEAMS. A record of primitives carrying its
 * TYPE, with BYTES BESIDE IT — the discipline `qjs_provide`/`qjs_host_answer` already use, and the reason is
 * theirs: JSON (and every text form) can say none of the 256 values a byte has without an algorithm run over
 * them, and the algorithm this zone must never run is a decode. `postMessage` carries both natively — structured
 * clone reproduces a number as a number (so `otherW.length === 0` still distinguishes one from the string "0")
 * and a `Uint8Array` as bytes — so nothing is encoded in transit.
 *
 * AND THE ENGINE ABI IS NOW A MOJOM INTERFACE, WHICH IS THE CHANGE THIS FILE EXISTS TO REPORT. The record it
 * used to build was `{v:1,id,op:"call",fn,ret,args,bodies}` — a `fn` string naming any qjs_* entry, a `ret`
 * string naming its return, and an `args` list of SHAPE TAGS the frame trusted to say how each operand should
 * be placed — with a hand-written id table, a hand-written demultiplexer (`onReply`) and a hand-written
 * ok/err convention. Not one parameter had a declared type. That was backwards in exactly the direction
 * SECURITY.md cares about: the QuickJS/WASM engine is UNTRUSTED because it EXECUTES the attacker bundle, so the
 * renderer is the one peer whose messages must be assumed hostile — and it was the one peer nothing validated,
 * while mojo's validator (declared parameter types, ReportBadMessage killing the connection rather than the
 * call) guarded the TRUSTED worker. `content.mojom.Renderer` declares all twenty entries, mojo.js validates
 * both directions of every one of them, and `rendererCall`/`onReply` are deleted with the envelope.
 * WHAT IS LEFT HERE IS THE PROCESS AND NOT THE PROTOCOL: fork a frame when the browser process orders one, hand
 * it its program and the primordial pipe, absorb its stdio, adopt the pipe that comes back, and bind two
 * interfaces on it. The instance's WORKING SET is a declared reply field of every method (`workingSetBytes`)
 * rather than a field of this transport, because it is a fact this file no longer has to know about: bridge.js
 * reads it off the reply it already awaits, on the line that ranks the engine.
 *
 * TRANSFERABLES ARE NOT USED FOR ABI ARGUMENTS, and that is now a property of the mojom's TYPES rather than a
 * caveat kept here. mojo.js builds a message's transfer list from the declared types — which is what makes
 * `handle<message_pipe>` a real pipe pass — and Mojo's `array<uint8>` is a COPIED byte sequence; the type for
 * bytes that MOVE is `mojo_base.mojom.BigBuffer`, a different declaration. So the question is not "should this
 * call transfer" but "is this parameter's type the one that moves", and the answer differs per parameter:
 * `Init`'s document is ALSO retained by bridge.js as `eng.html`, the cold recipe it writes to IndexedDB on
 * finalize, so moving it would leave the cross-session frontier holding a page that parses to nothing (a
 * detached buffer reads as a zero-length one), while `Provide`'s body is minted by safeFetch per call and has
 * no second reference. Declaring ONE type for both would make one spelling mean two ownerships with nothing on
 * the wire to tell them apart. NOTHING IS TRANSFERRED ANYWHERE ON THIS BOUNDARY NOW: the one handle that used
 * to move was the renderer's own pipe, detached here and re-adopted from the browser process so that "the
 * endpoint travelled through the deciding process" would be a true sentence. With the deciding process gone
 * the endpoint travels nowhere, so the detach window — in which `r.port` was null and a post had nothing to
 * post on — does not exist and neither does the machinery that re-attached it.
 *
 * THE ORIGIN STAMP STAYS THIS ZONE'S. A record arriving from a frame may not state who it is: the frame's
 * `event.origin` is the literal "null" (it is opaque) and everything in the payload is written by the untrusted
 * side. Identity here is the browser's — `event.source` is the WindowProxy of the frame THIS file created, and
 * once the invitation has handed over a `MessagePort` the channel is point-to-point and there is no source to
 * confuse. SECURITY.md: "identity may be minted by the untrusted side because it is only a name, but ROUTING
 * and the ORIGIN STAMPED ON A DELIVERED MESSAGE are the trusted zone's alone."
 */
(function () {
  "use strict";

  var RENDERER_URL = "renderer.html";

  /* THE RENDERER'S PROGRAM, READ ONCE BY THE ZONE THAT CAN READ IT. The frame cannot fetch these itself — the
     manifest's `cross_origin_embedder_policy: require-corp` is served on every extension resource response and
     Chromium emits `Cross-Origin-Resource-Policy`/`Access-Control-Allow-Origin` only for WEB-ACCESSIBLE
     resources (extensions/browser/extension_protocols.cc), so from an opaque origin a classic subresource fails
     the CORP check and a module or `fetch` fails CORS. Here the load is same-origin and passes both.
     NOT `safeFetch`: that chokepoint exists for ANALYZER-driven requests to the web and rejects every scheme
     but http(s). This is the extension reading its own program off its own origin, which is not a request the
     analysis makes and not one a page can influence.
     THE BLOB CARRIES ITS TYPE AND ITS BYTES ARE NEVER DECODED. `response.blob()` would take the type from a
     Content-Type this zone does not control; the type is stated instead, because an `import()` of a blob: URL
     requires a JavaScript MIME type and a guess here would fail at the far end with nothing to say why. */
  var _programP = null;
  function programOnce() {
    if (_programP) return _programP;
    _programP = (async function () {
      /* THE TRANSPORT IS PART OF THE PROGRAM, so the frame is handed this extension's mojo.js and mojom.js
         rather than a copy inlined into renderer.html, where a second generation could live.
         IT IS NOT "THE SAME BYTES this document loaded by <script src>", WHICH IS WHAT THIS COMMENT CLAIMED
         AND COULD NOT SHOW. It is a SECOND READ of the same three URLs: ast-worker.html loads check.js,
         mojo.js and mojom.js when it parses, and this `fetch` runs at the FIRST rendererLaunch, which is a
         later instant — and the extension is served out of a live directory, so nothing in this tree makes
         the two reads one generation. The frame's own document is a THIRD read (`src=RENDERER_URL` below),
         though that one is covered from inside: renderer.html's `rendererImpl` walks the mojom's own method
         list and crashes naming itself if its ABI table and the declarations disagree.
         WHAT MAKES THE TWO READS ONE GENERATION IS ASSERTED AND NO LONGER ASSUMED. The `version: 0` that used
         to stand in the mojom was a constant nothing incremented, so it compared equal across every skew it
         named; mojo.js derives the WIRE CONTRACT from the declarations instead and refuses a peer whose
         contract differs — at the invitation acceptance, before a single interface is bound. A frame from
         another generation therefore fails its boot, is torn down by the catch below, and frees its agent
         cluster, rather than being handed ABI calls whose operands its own mojom places differently. */
      var names = ["check.js", "mojo.js", "mojom.js", "lib/qjs/qjs.mjs", "lib/qjs/qjs.wasm"];
      var rs = await Promise.all(names.map(function (n) { return fetch(n); }));
      for (var i = 0; i < rs.length; i++)
        CHECK(rs[i].ok, "the trusted zone could not read the renderer's own program off its own origin (" +
                        names[i] + ") — every renderer is built out of these five and there is no other way " +
                        "to get them into an opaque-origin frame");
      var bufs = await Promise.all(rs.map(function (r) { return r.arrayBuffer(); }));
      return {
        check: new Blob([bufs[0]], { type: "text/javascript" }),
        mojo: new Blob([bufs[1]], { type: "text/javascript" }),
        mojom: new Blob([bufs[2]], { type: "text/javascript" }),
        glue: new Blob([bufs[3]], { type: "text/javascript" }),
        wasm: new Uint8Array(bufs[4]),
      };
    })();
    return _programP;
  }

  /* THE FRAME'S OUTPUT, ABSORBED WHERE IT ARRIVES. Every record a mojo child posts drains its process's stdio
     with it (mojo.js's second stated divergence), and this is the parent hook that takes it: bridge.js reads
     @RESULT, @E and @WHY out of exactly these lines and a crash's ROOT line is the last @WHY printed before
     abort(), so lines left in the frame would be a crashed renderer taking its own cause with it. */
  function absorbStdio(r, lines) {
    for (var i = 0; i < lines.length; i++) {
      var ln = lines[i];
      DCHECK(Array.isArray(ln) && (ln[0] === 1 || ln[0] === 2) && typeof ln[1] === "string",
             "a renderer's output line did not carry the stream it came from — a line crosses as `[fd, text]` " +
             "so the two streams stay in ONE order (the last @WHY before an abort is what names the cause, and " +
             "@RESULT may sit either side of it) while stderr alone is teed live");
      r.lines.push(ln[1]);
      /* STDERR, TEE'D LIVE TO THIS ZONE'S CONSOLE. It is the same tee bridge.js's `errsink` performed while it
         held the Module: @E/@WHY abort lines, ASan's alloc/free stacks and native diagnostics are CAPTURABLE
         while the run is happening (`harness diag`) instead of only in a crash record whose findings are about
         to be discarded. stdout is NOT teed — a hot engine's @H traffic would bury it. */
      if (ln[0] === 2) console.debug(ln[1]);
    }
  }

  /* THE PRIMORDIAL PIPE'S TRANSPORT, WHICH IS NOW A ONE-LINER BECAUSE THE ENDPOINT NO LONGER MOVES. It used
     to read `r.port` at post time and re-install the Connection's read callback on whatever endpoint was
     current, because between answering a fork order and being handed the pipe back by the browser process the
     endpoint was detached here and live nowhere in this document. That was the evidence the pipe had
     travelled through the deciding process; with the decision in this realm there is nothing for it to travel
     through, so the transport CLOSES OVER the endpoint it was built with — which is what makes "attached
     without a reader" and "posted into a detached window" unrepresentable rather than asserted. */
  function rendererTransport(port) {
    DCHECK(port instanceof MessagePort,
           "a renderer's transport was built over something that is not a MessagePort — the endpoint is one " +
           "half of the channel this file just created, and anything else here is a clone of a port rather " +
           "than the port");
    return {
      post: function (rec, xfer) { port.postMessage(rec, xfer); },
      listen: function (cb) { port.onmessage = function (ev) { cb(ev.data); }; },
    };
  }

  /* THE LIVE RENDERERS OF THIS ZONE, AND THE COUNTERS THAT MAKE A CLEAN TEARDOWN DISTINGUISHABLE FROM NOTHING
     EVER HAVING RUN. "no frames and no engines" is what a pool that finalized every instance looks like AND
     what an extension that never provisioned one looks like, which is precisely the shape CLAUDE.md names —
     a number that reads the same whether the mechanism worked or was never reached. So the counters are kept:
     `forked` rises where a fork begins, `provisioned` where a frame is created and `destroyed` where one is
     removed, and `rendererStats` CHECKS the set against the DOM, so a renderer whose element outlived its
     record (or the reverse) is caught here rather than as a slow leak of frames under a document that never
     reloads.
     `_awaitingAdopt` IS GONE WITH THE PIPE RELAY. It held a renderer that had a frame, a booted engine and NO
     endpoint in this realm, because the endpoint was in flight between the zygote's reply and the browser
     process handing it back — a state that existed only to make that journey observable. Nothing travels now,
     so the state is unrepresentable rather than tracked.

     A FORK IS COUNTED AT BOTH ENDS, AND THE SECOND COUNTER IS WHY. `forked` was ONE number, raised where a
     fork BEGINS, and `rendererPoolProbe` asserts it against the registry's `launched + failed`, which are both
     raised where a fork ENDS. That identity is therefore FALSE for the whole duration of every boot — it held
     only because nothing had ever read the probe while a fork was outstanding, which with one renderer at a
     time and a probe run between analyses is most of the time and, with a document embedding a cross-origin
     iframe, none of it. MEASURED: sampling the pool every 25 ms across the analysis of such a page fires that
     DCHECK on the first sample, "admitted 6 while this document forked 7", accusing this file of materializing
     a frame outside the admission path — a true-looking accusation about a fork that was simply still running.
     So the fact is recorded as the two facts it is: `_forkBegun` rises in the fork's preamble (before anything
     that can throw — its caller frees the registry slot on every way out, so a fork that aborted in its own
     preamble must still be a fork) and `_forkSettled` rises in `rendererLaunch`, in the SAME TURN as the
     registry transition that settles it. `forked` is the SETTLED count, so the probe's identity is now exactly
     true at every instant rather than only at rest, and `forking` reports the in-flight forks the identity used
     to be silently missing. The two are tied to the SET and not merely to each other: a renderer whose fork has
     not settled is exactly one that has not bound its interfaces, which `rendererStats` asserts. */
  var _live = new Set();
  var _forkBegun = 0, _forkSettled = 0, _provisioned = 0, _destroyed = 0;

  /* THE FRAME GOES, AND NOTHING IS TOLD. There is no free list on the other side and none is wanted: a pointer
     a call RETAINED lives as long as the module does and the module dies with the document, so removing the
     element is the whole teardown. `qjs_teardown` is a separate ABI call the OWNER makes first — it is the
     engine asking its own runtime to walk gc_obj_list and report leaks, which is a finding, not cleanup.
     THIS HALF IS SPLIT OUT FROM `rendererDestroy` FOR ONE CALLER: a renderer that failed to BOOT is reclaimed
     inside the fork that created it, and its death reaches the registry as `rendererLaunchFailed` on the way
     out. Telling the registry twice would free the agent cluster twice — the second time out from under
     whatever holds it next — and the two transitions are not interchangeable either: one asserts it is
     burying a renderer that booted and the other one that never did. */
  function frameTeardown(r) {
    DCHECK(r.conn !== null,
           "a renderer was destroyed before its Connection existed — the frame is created and its invitation " +
           "offered in one step, so a record with a frame and no connection is a teardown reaching a renderer " +
           "this file never finished forking");
    var owed = r.conn.outstandingCalls();
    DCHECK(owed === 0,
           "a renderer was destroyed with " + owed + " call(s) still outstanding — each one is a caller parked " +
           "on an answer that can no longer be produced, which is silent everywhere");
    /* THE CONNECTION IS CLOSED AND NOT MERELY ABANDONED, so "this peer is gone" has ONE representation: every
       later call into it rejects with the reason it was closed rather than posting into a dead port, and this
       realm's open-endpoint count stops including a pipe nothing can travel on. */
    r.conn.close("its frame was removed from the offscreen document");
    if (r.port) r.port.close();
    if (r.frame.parentNode) r.frame.parentNode.removeChild(r.frame);
    DCHECK(_live.has(r),
           "a renderer left this document twice — the second teardown removes an element that is already gone " +
           "and reports a termination the browser process has already recorded");
    _live.delete(r);
    _destroyed++;
  }

  /* AND A RENDERER THAT LIVED IS ALSO A REGISTRATION. A real renderer's death is OBSERVED by the browser
     rather than ordered by it — a child can exit on its own — and here the offscreen owns the frame, so this
     is the zone that notices; the registry is where the agent cluster is freed, which is what lets the next
     request for that cluster be admitted instead of refused as a duplicate of a renderer that no longer
     exists. IT IS ORDERED BY BEING A CALL. This used to be a fire-and-forget mojo method deliberately placed
     on the SAME interface as `CreateRendererForCluster`, because Mojo orders messages within a pipe and
     across none, so a termination could otherwise be overtaken by the next admission for its cluster. A
     synchronous call in a run-to-completion realm is that ordering and strictly more of it: the cluster is
     free before this function returns. */
  function rendererDestroy(r) {
    frameTeardown(r);
    DCHECK(!!self.renderProcessHost,
           "a renderer was destroyed with no renderer registry in this realm — that table is the only thing " +
           "that can free this agent cluster, and a termination nobody records leaves the cluster refused for " +
           "the life of this document");
    self.renderProcessHost.rendererTerminated(r.routingId);
  }

  /* WHAT THIS ZONE IS HOLDING, ASKED FROM OUTSIDE. The DOM is the second opinion and the assert is what makes
     it one: `_live` is this file's record of the renderers it forked and the frames are what the browser
     actually has, so the two disagreeing means a destroy removed a record without its element or an element
     without its record. THE ROUTING IDS ARE REPORTED because they are not this zone's to mint: every one came
     off a fork order, so the list is checkable against the browser process's own registry, and a frame with no
     routing id would be one this document created for itself. */
  function rendererStats() {
    var frames = document.querySelectorAll('iframe[title^="renderer "]').length;
    /* THE IN-FLIGHT FORKS, COUNTED TWICE OVER TWO DIFFERENT THINGS AND COMPARED. A fork that has not settled
       is exactly a renderer that has not bound its interfaces — `rendererFork` binds them on the line after
       the invitation is accepted and `rendererLaunch` settles the counter in the turn after that — so the
       difference between the two counters must equal the number of live records with nothing bound. A counter
       raised without its record, or a record left unbound by a fork that finished, is caught here rather than
       as an identity failure in the pool probe that names this file for a fork that was merely running. */
    var forking = _forkBegun - _forkSettled, unbound = 0;
    _live.forEach(function (r) { if (r.renderer === null) unbound++; });
    DCHECK(forking >= 0 && forking === unbound,
           "this zone has begun " + _forkBegun + " fork(s) and settled " + _forkSettled + " while " + unbound +
           " live renderer(s) have bound no interface — a fork settles in the same turn as the registry " +
           "transition that admits or buries it, so a difference is a counter raised without its renderer or a " +
           "renderer left half-forked with nothing waiting on it");
    DCHECK(frames === _live.size,
           "this zone holds " + _live.size + " renderer(s) but the document carries " + frames + " renderer " +
           "frame(s) — a frame that outlives its record is a WASM instance nothing can reach and nothing will " +
           "ever tear down, and a record that outlives its frame is a caller about to post into a dead window");
    var ids = [], names = [];
    _live.forEach(function (r) {
      DCHECK(typeof r.routingId === "number",
             "a renderer in this document carries no routing id — an id is minted by the browser process's " +
             "registry and is the only name a renderer has, so one without it is a frame this document made " +
             "for itself");
      ids.push(r.routingId);
      names.push(r.name);
    });
    return { forked: _forkSettled, forking: forking, provisioned: _provisioned, destroyed: _destroyed,
             live: _live.size, frames: frames,
             routingIds: ids.sort(function (a, b) { return a - b; }), names: names };
  }
  self.rendererStats = rendererStats;

  /* ────────────────────────────────────────────────────────────────────────────────────────────────────────
     THE ONLY PATH THAT MATERIALIZES A RENDERER, and it takes the routing id rather than allocating one, takes
     the agent cluster key rather than computing one, and asks nothing about whether the cluster should have a
     renderer at all. Its one caller is `rendererLaunch`, which has just registered that id.
     IT TAKES THE PROGRAM AS AN ARGUMENT, WHICH IS THE WHOLE OF WHY THERE IS NO SUSPENSION IN ITS PROLOGUE.
     `programOnce()` reads five files off this extension's own origin; awaited HERE it would sit between the
     registration and `_live.add`, so the registry would name a renderer this document holds no frame for for
     the length of a fetch — exactly the disagreement `rendererPoolProbe` cross-checks for, on every boot.
     `rendererLaunch` awaits it before it decides anything, and everything down to the `appendChild` below
     then runs in the turn that registered.
     THE HANDSHAKE IS THE FRAME'S, NOT `onload`'s. `onload` fires for a document whose script CSP blocked, so it
     would report a renderer that does not exist as ready; the frame's own hello is the proof its bootstrap
     ran. There is no timeout on it: a wall clock here would report a loaded machine as a broken transport
     (§Testing), and the failure it would be covering — a policy that refused the bootstrap — is already
     reported by Chrome, by name, on this document's console.
     ──────────────────────────────────────────────────────────────────────────────────────────────────────── */
  async function rendererFork(routingId, clusterKey, prog) {
    /* COUNTED ON THE FIRST LINE, BEFORE ANYTHING THAT CAN THROW. Its caller frees the registry slot on EVERY
       way out of this function, so a fork that aborted in its own preamble must still be a fork that BEGAN —
       otherwise the registry would record a failure this document never counted. What the pool probe's
       identity is asserted against is the SETTLED counter, raised beside that registry transition. */
    _forkBegun++;
    DCHECK(clusterKey !== "",
           "a fork named no agent cluster — a renderer IS an agent cluster's instance and the key is how the " +
           "registry says which, and how a frame in this document is identified as that instance");
    DCHECK(typeof routingId === "number" && routingId > 0,
           "a fork named no routing id — the id is minted by the renderer registry when it decides this " +
           "cluster gets an instance, and a frame without one is a renderer this file created on its own " +
           "authority");
    DCHECK(!!prog && !!prog.check && !!prog.mojo && !!prog.mojom && !!prog.glue && !!prog.wasm,
           "a fork was handed an incomplete renderer program — the invitation carries five things a " +
           "opaque-origin frame can obtain no other way, and one missing is a bootstrap that fails inside the " +
           "frame with the reason sitting in this realm");
    var f = document.createElement("iframe");
    /* THE SANDBOX ATTRIBUTE IS SET BEFORE THE ADDRESS AND BEFORE INSERTION, because the flags are read when the
       load begins: an `allow-same-origin`-less frame whose attribute lands late is a document that already has
       the extension's own origin. `allow-scripts` alone — the manifest's sandbox policy grants this page
       `allow-popups`/`allow-popups-to-escape-sandbox`/`allow-modals` as well (poc-sandbox.html needs them), and
       the effective flag set is the UNION of both restrictions, so this attribute is what takes them away. */
    f.setAttribute("sandbox", "allow-scripts");
    /* THE ROUTING ID LEADS THE TITLE because the cluster key's own separator is a NUL, which every console and
       every element inspector renders as nothing at all — so a document holding several renderers showed a
       list of titles that ran two halves together and could not be told apart by eye. The id is the one name
       this file did not mint and is unique by the registry's own arithmetic, so it is what identifies a frame
       to a human; the key follows it unaltered, because reshaping it here would be a second place that knows
       what a cluster key is made of. `rendererStats` matches on the `renderer ` prefix, which is unchanged. */
    f.setAttribute("title", "renderer " + routingId + " " + clusterKey);
    f.setAttribute("src", RENDERER_URL);
    f.style.cssText = "position:absolute;left:0;top:0;width:1px;height:1px;border:0;visibility:hidden";
    /* NO WORKING-SET FIELD IS IN THIS LITERAL AND NONE MAY BE. There is no number meaning "this instance has
       not said yet" that a consumer would not then have to branch on, and there is no window in which one is
       needed: it is a DECLARED REPLY FIELD of every `content.mojom.Renderer` method, so every answer carries
       the current one and the caller reads it off the reply it already awaited. */
    var r = { name: clusterKey, routingId: routingId, frame: f, port: null, lines: [],
              conn: null, renderer: null, childProcess: null };
    _live.add(r);
    _provisioned++;
    /* DECLARED HERE AND FILLED BY THE PROMISE EXECUTOR ON THE NEXT LINE (which runs synchronously), which is a
       stated hole rather than a placeholder: a no-op stand-in would be a cleanup that silently did nothing if
       the executor ever stopped installing one, and the listener would then outlive every renderer this
       document forks. It is asserted where it is used. */
    var stopListening = null;
    var booted = new Promise(function (resolve, reject) {
      function onFrame(e) {
        /* THE BROWSER-SET IDENTITY, AND THE ONLY ONE THERE IS. The frame's origin is opaque so `event.origin`
           is the literal "null" for every renderer; `event.source` is the WindowProxy of the frame this
           closure created. Another renderer booting concurrently posts here too and is matched by ITS own
           listener, so a mismatch is that and not a message to drop on the floor. */
        if (e.source !== f.contentWindow) return;
        var d = e.data;
        DCHECK(!!d && d.v === 1,
               "a renderer frame posted a window record that is not this handshake's — the window channel " +
               "carries exactly two things (the frame's hello, and a refusal from a bootstrap that could not " +
               "reach the pipe) and everything else in this document's IPC rides a mojo pipe");
        if (d.hello === 1) {
          /* THE INVITATION. `mojo::OutgoingInvitation`: creating the child gives the parent ONE primordial
             pipe and nothing else, and every capability afterwards is brokered onto a pipe of its own. It
             also carries this process's PROGRAM, which is what a real child gets from the filesystem and an
             opaque-origin frame can obtain no other way.
             "*" IS FORCED: the frame's origin is opaque and postMessage's targetOrigin parses as a URL unless
             it is "*" or "/", so there is no origin string that names it. It is not a broadcast — the message
             goes to this frame's window and to nothing else. */
          var ch = new MessageChannel();
          r.port = ch.port1;
          r.conn = new self.mojo.Connection(rendererTransport(ch.port1), {
            role: "parent", name: "renderer " + routingId,
            onStdio: function (lines) { absorbStdio(r, lines); },
            /* AND EVERY ERROR THIS CONNECTION RAISES CARRIES THAT TAIL, because the reason a call failed is
               almost never in the exception and almost always in the lines before it: bridge.js's
               engineBootFailed and engineCrash both scan `rendererLines` backwards for the ROOT @WHY. */
            decorate: function (err) { err.rendererLines = r.lines.slice(-8); },
          });
          r.conn.ready.then(resolve, reject);
          e.source.postMessage({ v: 1, invitation: 1, check: prog.check, mojo: prog.mojo, mojom: prog.mojom,
                                 glue: prog.glue, wasm: prog.wasm }, "*", [ch.port2]);
          return;
        }
        /* A BOOTSTRAP THAT NEVER REACHED THE PIPE. It is the one failure a mojo acceptance cannot carry — the
           program that did not load may BE mojo.js — so it travels on the invitation channel, and without it
           the parent would park forever on an acceptance that is not coming, which is the one outcome with no
           symptom anywhere. */
        DCHECK(typeof d.bootstrapFailed === "string" && Array.isArray(d.out),
               "a renderer frame posted a refusal that is not one — a bootstrap failure states why it failed " +
               "and drains the output printed before it, which is the only thing that says what broke");
        absorbStdio(r, d.out);
        reject(new Error(d.bootstrapFailed));
      }
      addEventListener("message", onFrame);
      /* THE LISTENER OUTLIVES NEITHER OUTCOME. It is removed once the handshake has settled either way rather
         than inside the hello arm, because the refusal arrives AFTER the hello and a listener taken down at
         the hello could not hear it. */
      stopListening = function () { removeEventListener("message", onFrame); };
    });
    (document.body || document.documentElement).appendChild(f);
    r.destroy = function () { rendererDestroy(r); };
    /* WHETHER THIS RENDERER MAY BE DESTROYED YET, ANSWERED BY THE CONNECTION THAT KNOWS. `frameTeardown`
       REFUSES a teardown with a call outstanding — each one is a caller parked on an answer that can no longer
       be produced — so an owner that wants to reclaim a frame has to be able to ask the same question the
       assert asks, rather than inferring it from a pool state that does not mean what it says. The Clear path
       inferred it (`state === "fetching"`) and was wrong for the whole of every scheduler round, so the assert
       fired on the ordinary case instead of on a bug. It is the CONNECTION'S count and not a second tally kept
       beside it: two numbers for one fact is how the inference got to be wrong in the first place. */
    r.outstandingCalls = function () {
      DCHECK(r.conn !== null,
             "a renderer was asked what it is waiting on before its Connection existed — the frame is created " +
             "and its invitation offered in one step, so a record with a frame and no connection is this " +
             "question reaching a renderer this file never finished forking");
      return r.conn.outstandingCalls();
    };
    /* A RENDERER THAT DID NOT BOOT IS STILL A FRAME IN THIS DOCUMENT, so the element and its port go first, or
       a caller that retried would accumulate dead renderers under a document that never reloads. The failure
       then TRAVELS ON: swallowing it would report a page as analysed by an instance that never existed.
       IT LEAVES AS A THROW AND NO LONGER AS A VALUE, AND THAT RECOVERS THE ONE THING THE WIRE DESTROYED. The
       shape was `{pipe:null, error}` because a mojo call rejects when the TRANSPORT broke, and a page whose
       engine aborted its boot is a RECORDED outcome rather than a broken pipe — so the two had to be told
       apart on the wire. The cost was that `error` is a `string?`: this caught error was stringified to its
       `.message`, crossed as text, and `rendererLaunch` built a FRESH `Error` from it — so `rendererLines`,
       which the Connection had just attached and which is the only place the engine's own `@WHY` ROOT line
       survives a rejected boot, was thrown away at the one hop that held it. bridge.js's engineBootFailed
       reads exactly that field and had never once been given it. The error now travels as itself, and the
       recorded-vs-invariant split stays where it already was: engineCreate's catch, which writes the crash
       record and then RETHROW_FATALs. */
    DCHECK(typeof stopListening === "function",
           "the invitation handshake installed no way to stop listening — the executor above runs " +
           "synchronously, so a missing one is a `message` listener that outlives every renderer this document " +
           "ever forks and answers for a frame that is gone");
    try { await booted; }
    catch (e) {
      stopListening();
      frameTeardown(r);
      throw e;
    }
    stopListening();
    /* AND THE BROKER, USED ON THE RENDERER BOUNDARY TOO. Two names go out on the primordial pipe and two bound
       pipes come back, which is the whole replacement for a `fn` string naming any qjs_* entry: the ABI is an
       INTERFACE whose every parameter is declared and validated, and "how much IPC does that process have
       open" is a second interface because it is a fact about the PROCESS and not about the ABI.
       IT IS DONE HERE, on the line after the invitation was accepted, because this realm has held the
       endpoint the whole time. It used to be deferred to `rendererLaunch` for one reason — a bind request
       travels on the primordial pipe, and that endpoint spent the fork inside the browser process — and that
       reason is gone with the relay. */
    r.renderer = r.conn.bindInterface("content.mojom.Renderer");
    r.childProcess = r.conn.bindInterface("content.mojom.ChildProcess");
    return r;
  }

  /* ASK THE REGISTRY FOR A RENDERER, THEN MATERIALIZE THE ONE IT DECIDED ON. This holds no decision at all —
     it cannot mint an id and it cannot admit a cluster — and it exists so there is ONE way to obtain a
     renderer (bridge.js's pool and the probe below take the same one) rather than two spellings of the same
     four lines.
     THE ORDER OF THESE THREE STATEMENTS IS THE WHOLE CONTRACT. The program is read FIRST, so that nothing
     between the registration and the frame can suspend. The registration is SECOND, because it is the
     admission decision and every refusal in it (an empty key, a cluster that already has an instance, an
     exhausted id space) is a CHECK that must fire before a frame exists to leak. The fork is THIRD and takes
     the id it was given. */
  async function rendererLaunch(clusterKey) {
    DCHECK(typeof clusterKey === "string" && clusterKey !== "",
           "a renderer was asked for with no agent cluster key — a renderer IS a cluster's instance, and the " +
           "key is the only thing the renderer registry decides on");
    DCHECK(!!self.renderProcessHost,
           "render-process-host.js is not loaded in this zone — it holds SECURITY.md's " +
           "one-instance-per-agent-cluster rule and mints every routing id, so without it this file would " +
           "materialize renderers nothing had decided on and refuse none of them");
    var prog = await programOnce();
    var routingId = self.renderProcessHost.registerRenderer(clusterKey);
    var r;
    try { r = await rendererFork(routingId, clusterKey, prog); }
    catch (e) {
      /* THE CLUSTER IS FREED ON EVERY WAY OUT OF THE FORK. `rendererFork` reclaims its own frame, so what is
         left registered here is a slot whose renderer never booted — and leaving it would refuse this agent
         cluster a renderer for the life of the document, with nothing anywhere to say why. That is true
         whether what failed was the page's engine or one of this zone's own invariants, which is why the
         registry transition is unconditional and this catch re-throws whatever it caught rather than
         classifying it. The recorded-vs-invariant split is engineCreate's, one frame up, where it already
         was; making a second one here would be two answers to one question.
         AND THE FORK IS SETTLED IN THE SAME TURN AS THE TRANSITION, on both exits. The two statements are one
         fact — this fork is over, and here is which way — and nothing may observe one without the other: the
         pool probe is synchronous, so a pair written in one turn is a pair it can only ever read together. */
      _forkSettled++;
      self.renderProcessHost.rendererLaunchFailed(routingId);
      throw e;
    }
    _forkSettled++;
    self.renderProcessHost.rendererLaunched(routingId);
    return r;
  }
  self.rendererLaunch = rendererLaunch;

  /* THE PROVISIONING, EXERCISED — AND EXERCISED PLURAL, WHICH IS THE ONLY VERSION OF THIS THAT PROVES
     ANYTHING. SECURITY.md: "A HOST THAT CANNOT PROVISION A SECOND INSTANCE HAS NOT TESTED THE TRANSPORT, AND
     EVERY CROSS-INSTANCE MECHANISM ABOVE IS THEN A DESIGN THAT HAS NEVER RUN." This probe provisioned exactly
     ONE renderer, so the sentence it quoted was still true of it: everything in this file that is about there
     being SEVERAL of them — the registry keyed by agent cluster, a `_live` set rather than a slot, one
     `message` listener per fork matching on `event.source`, a DOM count compared against that set — was
     reachable in principle and reached by nothing. It provisions TWO, and it holds them AT THE SAME TIME,
     because a probe that tears the first down before asking for the second exercises reuse of one slot and
     calls it a pool.
     THE TWO ARE CROSS-ORIGIN TO EACH OTHER AND IN ONE BROWSING-CONTEXT GROUP, which is the pair the key exists
     to tell apart. Same group, same origin would be REFUSED (fatally, by the registry, which is the rule); two
     groups would prove only that two unrelated names produce two rows. One group and two origins is the case
     SECURITY.md's key is FOR: "an iframe or a popup gets its own instance the moment its origin differs".
     THE FORKS RUN CONCURRENTLY, ON PURPOSE. Two frames boot at once, so two `hello` records arrive on this
     document's ONE window channel with both listeners installed — the per-fork `event.source` match is the
     only thing that routes each to its own handshake, and sequential launches would never put a record in
     front of the wrong listener. It is also what makes the invitation's `MessageChannel` per fork rather than
     per file, which is unobservable when only one exists.
     EACH IS A REAL ROUND TRIP AND NOT A PING, and the two documents DIFFER so the answers can be compared. The
     document crosses as BYTES, the engine PARSES it with Lexbor and runs `document_bundle_id`'s <script> scan
     over the result, and a number computed inside the frame comes back — so two DIFFERENT script sets must
     produce two different numbers. One number appearing twice would be one engine answering for both, which is
     precisely the failure "two instances" is supposed to make impossible and is otherwise invisible.
     `routingId` IS THE REGISTRY'S AND NOT THIS FILE'S. It is minted by render-process-host.js when it decides
     the cluster gets an instance, and its presence in that table's own snapshot is what `rendererPoolProbe`
     cross-checks. That check no longer spans two PROGRAMS — the table is a Map in this realm — so what it
     proves is stated where it is made, at that probe.
     AND THE TYPING IS OBSERVABLE. `iface`/`ifaceMethods` are read off each bound interface's own descriptor
     rather than restated here, so they answer "which interface is this pipe actually carrying" — the question
     a broker that bound the wrong implementation would get wrong. `wire` is the digest of the DESCRIPTION this
     realm holds, and it is reported because the property it stands for is the one this boundary cannot get
     from a build: mojom.js is read once by this document's `<script src>` and once again by `programOnce`, and
     the mojo handshake refuses a frame whose contract differs from this one. It replaces `ifaceVersion`, which
     reported the literal 0 both interfaces declared and moved for no change anybody could make to them.
     `badMessages` is the count of records this TRUSTED zone's validator refused: SECURITY.md makes the
     renderer the peer whose messages must be assumed hostile, so a healthy run reads 0 — and the probe now
     ASSERTS that rather than reporting it, because a number carried in a record and compared by nobody is the
     shape CLAUDE.md names. `child` is the same IPC surface asked of
     each frame ITSELF over a second brokered pipe, because a transport whose only evidence is that a message
     arrived is one whose shape nobody can check from outside — and asked of BOTH frames it is also how this
     document sees that each renderer holds its OWN endpoints rather than a shared set.
     WHAT IT IS NOT: the browser's own answer. The two cluster keys here are written by this probe, and
     SECURITY.md says both halves of a real one are BROWSER-STATED — so this exercises the provisioning, the
     registry and the transport, and it does NOT exercise `clusterKeyOf` deciding that two live documents are
     two clusters. That is the product path (a page embedding a cross-origin iframe, `rendererPoolProbe`
     reading the pool it produced), and it is a different claim, made elsewhere. */
  self.rendererProbe = async function rendererProbe() {
    var NUL = String.fromCharCode(0), GROUP = "probe-group";
    /* THE PEERS. Two origins, one group, and a DIFFERENT document each — the `<script src>` set is what
       `document_bundle_id` hashes, so these two pages cannot answer with one bundle id unless one engine
       answered twice. */
    var PEERS = [
      { origin: "https://a.renderer.probe", addr: "https://a.renderer.probe/app/index.html",
        doc: '<!doctype html><html><head><script src="/static/a.7c1f9b.js"></script>' +
             '<script src="/static/vendor.20f3aa.js"></script></head><body>renderer probe A</body></html>' },
      { origin: "https://b.renderer.probe", addr: "https://b.renderer.probe/app/index.html",
        doc: '<!doctype html><html><head><script src="/static/b.30e2cc.js"></script>' +
             '</head><body>renderer probe B</body></html>' },
    ];
    var i, rs = [], settled, firstErr = null;
    /* EVERY FIELD OF THE RECORD IS DECLARED HERE, INCLUDING THE ONES ONLY ONE OUTCOME FILLS. A field a
       consumer has to default is a field nobody notices is never written, so `torndown` counts (0 is "none
       did", which is what a crash means) and `err` is `null` rather than absent. The peer slots are declared
       one per peer and filled BY INDEX, so a peer that answered nothing is a `null` sitting in the report
       rather than a shorter list that reads as if fewer were asked. */
    var rec = { group: GROUP, asked: PEERS.length, provisioned: 0,
                peers: PEERS.map(function () { return null; }),
                whileLive: null, after: null, torndown: 0, crashed: false, err: null, badMessages: null,
                wire: null, rendererLines: null };

    /* ONE ORDER, ONE ARM PER OUTCOME. `Promise.all` would reject on the first failed fork while the others
       kept building frames nobody holds a record of, so a probe that failed once would leave this document
       carrying WASM instances until it reloads — which it does not. Every fork is allowed to settle, the ones
       that produced a renderer are reclaimed, and the first failure travels on as itself. */
    settled = await Promise.all(PEERS.map(function (p) {
      return rendererLaunch(GROUP + NUL + p.origin).then(
        function (r) { return { r: r }; }, function (e) { return { e: e }; });
    }));
    for (i = 0; i < settled.length; i++) {
      if (settled[i].r) { rs.push(settled[i].r); PEERS[i].r = settled[i].r; }
      else if (firstErr === null) firstErr = settled[i].e;
    }
    rec.provisioned = rs.length;
    if (firstErr !== null) {
      for (i = 0; i < rs.length; i++) rs[i].destroy();
      throw firstErr;
    }

    try {
      /* ── BOTH ALIVE, ASSERTED WHILE THEY ARE. Every number below is read in ONE turn with no await in it, so
         the registry's answer and this document's frames are the same instant rather than two snapshots. */
      var reg = self.renderProcessHost.getRegistry();
      var st = rendererStats();
      DCHECK(reg.live === PEERS.length && st.live === PEERS.length && st.frames === PEERS.length,
             "the probe holds " + rs.length + " renderer(s) but the registry reports " + reg.live + " and this " +
             "document " + st.live + " record(s) over " + st.frames + " frame(s) — provisioning a SECOND " +
             "instance is the whole claim, and any of those three reading one is a pool that reuses a slot");
      DCHECK(reg.clusters.length === PEERS.length &&
             reg.clusters.every(function (c) { return c.group === GROUP; }),
             "the registry's live clusters are not the ones this probe asked for — both peers name ONE " +
             "browsing-context group and differ only by origin, which is the pair the key exists to tell apart");
      var origins = reg.clusters.map(function (c) { return c.origin; }).sort();
      DCHECK(origins.length === 2 && origins[0] !== origins[1],
             "the registry holds two renderers whose agent clusters carry ONE origin — that is the duplicate " +
             "the admission refuses, so seeing it here means the two halves of the key did not reach the table");
      DCHECK(rs[0].routingId !== rs[1].routingId,
             "two live renderers carry ONE routing id — the id is minted by the registry's counter and is the " +
             "only name a renderer has, so a collision is a termination burying whichever the scan reaches");
      DCHECK(rs[0].frame !== rs[1].frame && rs[0].frame.parentNode !== null && rs[1].frame.parentNode !== null &&
             rs[0].frame.contentWindow !== rs[1].frame.contentWindow,
             "the two renderers are not two frames in this document — each instance is a WASM heap the browser " +
             "isolates behind its own opaque origin, and two records over one frame would be one heap wearing " +
             "two names");
      DCHECK(rs[0].port !== rs[1].port && rs[0].conn !== rs[1].conn && rs[0].renderer !== rs[1].renderer,
             "the two renderers share a transport — the invitation mints a MessageChannel per fork and every " +
             "call after it is point-to-point, so one endpoint answering for both is two engines behind one " +
             "pipe with nothing to say which replied");
      rec.whileLive = { registry: reg, renderers: st };

      /* ── BOTH ANSWERING, AT THE SAME TIME. The two ABI conversations are started together and interleave on
         two pipes; a document that could only serve one at a time would deadlock here rather than pass. */
      await Promise.all(PEERS.map(async function (p, idx) {
        var r = p.r;
        /* THE DOCUMENT CROSSES AS BYTES — `Init`'s `array<uint8>`, which is what `qjs_init` takes (a
           NUL-terminated pointer it strlen()s). The four strings beside it are §4.4's address, the name this
           agent's root document is known by, the response's header field lines (empty: this document had no
           response) and §8.1.3.1's top-level creation URL, which for a root document is its own. */
        var v = await r.renderer.init(new TextEncoder().encode(p.doc), p.addr, "probe", "", p.addr);
        var b = await r.renderer.getBundleId();
        var child = await r.childProcess.getMojoStats();
        rec.peers[idx] = { origin: p.origin, routingId: r.routingId, name: r.name, addr: p.addr,
                           iface: r.renderer.def.name, ifaceMethods: r.renderer.def.byOrd.size,
                           initrc: v.rc, bundleId: (b.bundleId >>> 0).toString(36),
                           /* THE WORKING SET THE FRAME STATED, read off the reply that carried it — the fact
                              bridge.js's RAM floor is summed out of. A probe that never looked at it would
                              leave the pool's one non-ABI input untested. */
                           heapBytes: b.workingSetBytes, child: child, lines: r.lines.slice() };
      }));
      DCHECK(rec.peers.indexOf(null) < 0,
             "a peer answered no ABI call at all — every renderer this probe provisioned is initialized with " +
             "its own document and asked for the bundle id it computed, and an empty slot is an instance " +
             "reported as live that nothing ever spoke to");
      DCHECK(rec.peers[0].bundleId !== rec.peers[1].bundleId,
             "two renderers computed ONE bundle id from two DIFFERENT documents (" + rec.peers[0].bundleId +
             ") — the id is a Lexbor <script> scan of the document each was handed, so one answer for both is " +
             "one engine answering twice, which is the shared heap this whole boundary exists to make " +
             "impossible");
      /* AND THE TEARDOWN, because it is the call that makes each runtime walk gc_obj_list and report a leaked
         GC object as a failure. A probe that provisioned two instances and walked away from them would be
         measuring the transport with the one check that judges an instance switched off. */
      await Promise.all(rs.map(function (r) { return r.renderer.teardown(); }));
      rec.torndown = rs.length;
    } catch (e) {
      /* AN ENGINE ABORT IS A RECORDED OUTCOME AND A HOST INVARIANT FAILURE IS NOT — the same split
         engineFinalize/crashResult already make, for the same reason: a WASM abort means that instance failed
         and the honest report of it is the crash PLUS everything it printed on the way down, while a DCHECK in
         THIS file is our own contract breaking and must not be reported as the engine's. Discarding the record
         here would leave an empty tail where a crash leaves the output that preceded it. */
      RETHROW_FATAL(e);
      rec.crashed = true;
      rec.err = String((e && e.message) || e);
    } finally {
      for (i = 0; i < rs.length; i++) rs[i].destroy();
    }
    /* ── READ AFTER BOTH TEARDOWNS ON PURPOSE. A destroyed renderer's endpoints leave this realm's open set
       with the connection they belonged to, and its agent cluster leaves the registry — so this is the state
       of a document that provisioned two instances and finished with them, which is what a NEXT provisioning
       for either cluster depends on. `live` reading 0 with `terminated` reading 2 is the pair of numbers that
       tells a clean teardown apart from an extension that never ran; a cluster still listed here would be one
       refused an instance for the life of this document, and a climbing `endpoints` would be a document
       reporting its whole history of instances as its live IPC. */
    /* THE TAIL OF WHAT EACH FRAME PRINTED, KEPT WHATEVER HAPPENED. A record is still a record after its frame
       is gone, so this reads the same lines a rejected ABI call would have decorated its error with — and on
       the crash path it is the ONLY place the engine's own `@WHY` ROOT line survives, which is the failure
       CLAUDE.md names: an empty tail where a crash leaves the output that preceded it. */
    rec.rendererLines = rs.map(function (r) { return { routingId: r.routingId, tail: r.lines.slice(-8) }; });
    var after = { registry: self.renderProcessHost.getRegistry(), renderers: rendererStats(),
                  mojo: self.mojo.stats() };
    DCHECK(after.registry.live === 0 && after.renderers.live === 0 && after.renderers.frames === 0,
           "this probe's renderers did not all leave — " + after.registry.live + " agent cluster(s) still " +
           "registered over " + after.renderers.frames + " frame(s), which is a cluster refused its next " +
           "instance and a WASM heap resident under a document that does not reload");
    rec.after = after;
    rec.badMessages = after.mojo.badMessages;
    rec.wire = after.mojo.wire;
    /* THE VALIDATOR'S OWN NUMBER, DECIDED ON AND NOT MERELY CARRIED. It is the one fact this file can state
       about the direction SECURITY.md calls hostile — how many records this TRUSTED zone's validator refused
       — and it was written into the record with nothing anywhere comparing it, which reads identically whether
       the validator held or was never reached. A refusal kills the connection, so the calls above would have
       rejected and this probe would be on its crash path; a non-zero here with a record still being returned
       is a refusal that killed a pipe nothing was waiting on, which is the one way it stays silent. */
    DCHECK(after.mojo.badMessages === 0,
           "this document's mojo validator refused " + after.mojo.badMessages + " record(s) from its " +
           "renderers — a record that does not match a declared type is the peer being broken and kills the " +
           "connection (Mojo's ReportBadMessage), so a probe that provisioned two frames, spoke the whole ABI " +
           "to both and still saw one is a renderer speaking a shape content.mojom.Renderer does not declare");
    return rec;
  };

  console.debug("[renderer-host] ready (self.rendererLaunch + self.rendererProbe installed)");
})();
