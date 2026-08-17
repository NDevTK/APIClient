/* renderer-host.js — THE ZYGOTE, AND THE TRUSTED SIDE OF EVERY RENDERER FRAME. Chromium's name for the object
 * that owns a renderer and is the only thing that may speak to it is `RenderFrameHost`, and this is that
 * object; it is ALSO the zygote, which is the part that changed. The offscreen — the one trusted zone
 * (SECURITY.md) — materializes a sandboxed frame when the BROWSER PROCESS orders one, hands it the engine
 * program, and relays the qjs_* ABI to it. It holds no analysis logic; every decision that is not "which
 * renderer, and what did it answer" stays on this side of the boundary in bridge.js.
 *
 * WHY THIS FILE IS A ZYGOTE AND NOT A CREATOR. It used to export `rendererCreate(name)`, which bridge.js called
 * whenever its pool wanted an instance: the offscreen decided that a renderer should exist, and the browser
 * process — a program named for owning exactly that decision — was not consulted. Chromium's browser process
 * does not create renderer processes by asking a document to do it, and it does not create them by itself
 * either: on Linux it asks the ZYGOTE, a helper holding the state a renderer starts from, to do the fork on its
 * behalf. The same split is FORCED here rather than chosen. `extension/browser-process.js` is a dedicated
 * Worker, and a dedicated Worker's global is `DedicatedWorkerGlobalScope` — no `document`, no DOM, no
 * `createElement` — so it CANNOT materialize an iframe. It can decide, register, route and order; the offscreen
 * document is the only zone that can obey. So `content.mojom.Zygote` is implemented here, called by the browser
 * process, and it is the ONLY code path in this extension that creates a renderer frame: a renderer exists if
 * and only if the browser process ordered one, and its ROUTING ID was minted there.
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
 * `clusterKeyOf` computes off browser-stated facts. This file takes that key as an ARGUMENT of the fork order —
 * it does not compute it, does not admit, does not rank, and no longer even asks whether the cluster already
 * has a renderer, because the registry that can answer that is the browser process's.
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
 * the wire to tell them apart. THE ONE THING THAT IS TRANSFERRED is the renderer's own PIPE, on the way out to
 * the browser process and back, and there transfer is the point: a handle that moves is a handle this zone
 * provably does not hold in between.
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
      /* THE TRANSPORT IS PART OF THE PROGRAM, and it is the SAME BYTES this document loaded by <script src>.
         mojo.js's own rule is that an interface exists only if both ends agree on it, and one description is
         what makes that true — so the frame is handed this extension's mojo.js and mojom.js rather than a copy
         inlined into renderer.html, where a second generation could live. */
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

  /* THE PRIMORDIAL PIPE'S TRANSPORT, AND THE ONE THING ABOUT IT THAT IS NOT A ONE-LINER: THE ENDPOINT MOVES.
     Between answering a fork order and being handed the pipe back by the browser process, this renderer's
     endpoint is DETACHED here and live nowhere in this document — which is the whole evidence that it
     travelled — so the transport reads `r.port` at post time rather than closing over one port object, and the
     Connection's single `listen` callback is re-installed on whatever endpoint is current. A post in that
     window is a caller reaching for a renderer it has not been given yet, and it says so. */
  function rendererTransport(r) {
    return {
      post: function (rec, xfer) {
        DCHECK(r.port !== null,
               "a record was posted to renderer " + r.routingId + " while its pipe was in flight — between " +
               "the fork order and the browser process handing the endpoint back, this zone provably does not " +
               "hold it, so there is nothing here to post on");
        r.port.postMessage(rec, xfer);
      },
      listen: function (cb) {
        r._onwire = cb;
        rendererAttachPipe(r, r.port);
      },
    };
  }
  /* AND THE (RE-)ATTACHMENT, in ONE place because it happens twice — at the fork, and again when the endpoint
     comes back from the browser process — and the second one is where a mistake would be silent: a Connection
     whose callback was not re-installed is a renderer that answers nothing, with every caller parked forever. */
  function rendererAttachPipe(r, port) {
    DCHECK(port instanceof MessagePort,
           "a renderer's pipe was attached with something that is not a MessagePort — the endpoint is what the " +
           "browser process transferred back, and anything else here is a clone of a port rather than the port");
    DCHECK(r._onwire !== null,
           "a renderer's pipe was attached before its Connection began listening — the callback is what reads " +
           "the wire, so an endpoint attached without one delivers every record to nobody");
    r.port = port;
    port.onmessage = function (ev) { r._onwire(ev.data); };
  }

  /* THE LIVE RENDERERS OF THIS ZONE, AND THE COUNTERS THAT MAKE A CLEAN TEARDOWN DISTINGUISHABLE FROM NOTHING
     EVER HAVING RUN. "no frames and no engines" is what a pool that finalized every instance looks like AND
     what an extension that never provisioned one looks like, which is precisely the shape CLAUDE.md names —
     a number that reads the same whether the mechanism worked or was never reached. So the counters are kept:
     `forked` rises where a fork ORDER arrives, `provisioned` where a frame is created and `destroyed` where one
     is removed, and `rendererStats` CHECKS the set against the DOM, so a renderer whose element outlived its
     record (or the reverse) is caught here rather than as a slow leak of frames under a document that never
     reloads.
     `_awaitingAdopt` HOLDS A RENDERER WHOSE PIPE IS IN FLIGHT. Between answering a fork order and being handed
     the pipe back by the browser process, a renderer has a frame, a booted engine and NO endpoint in this
     realm — which is the whole evidence that the endpoint travelled. It is a Map and not a field on the record
     because the thing that comes back names the renderer by its ROUTING ID, which is the browser's name for it
     and the only one it uses. */
  var _live = new Set();
  var _awaitingAdopt = new Map();
  var _forked = 0, _provisioned = 0, _destroyed = 0;

  /* THE FRAME GOES, AND NOTHING IS TOLD. There is no free list on the other side and none is wanted: a pointer
     a call RETAINED lives as long as the module does and the module dies with the document, so removing the
     element is the whole teardown. `qjs_teardown` is a separate ABI call the OWNER makes first — it is the
     engine asking its own runtime to walk gc_obj_list and report leaks, which is a finding, not cleanup.
     THIS HALF IS SPLIT OUT FROM `rendererDestroy` FOR ONE CALLER: a renderer that failed to BOOT is reclaimed
     while the browser process is still inside the fork order that created it, and its death is reported by that
     order's `error` reply. Telling the registry twice would free the agent cluster twice — the second time out
     from under whatever holds it next. */
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

  /* AND A RENDERER THAT LIVED IS ALSO A REGISTRATION IN ANOTHER PROCESS. A real renderer's death is OBSERVED
     by the browser rather than ordered by it — a child can exit on its own — and here the offscreen owns the
     frame, so this is the zone that notices. The notice is fire-and-forget for that reason, and it rides the
     SAME interface as `CreateRendererForCluster` so one pipe orders it against the next request for this agent
     cluster, which would otherwise be refused as a duplicate of a renderer that no longer exists. */
  function rendererDestroy(r) {
    DCHECK(!_awaitingAdopt.has(r.routingId),
           "a renderer was destroyed while its pipe was still on its way back from the browser process — the " +
           "endpoint would be delivered into this realm with nothing left to bind it to, and the record that " +
           "was waiting for it is gone");
    frameTeardown(r);
    var bp = self.browserProcessNow();
    DCHECK(bp !== null,
           "a renderer was destroyed with no live connection to the browser process — that registry is the only " +
           "thing that can free this agent cluster, and a termination nobody hears leaves the cluster refused " +
           "for the life of this document");
    bp.rendererHost.rendererTerminated(r.routingId);
  }

  /* WHAT THIS ZONE IS HOLDING, ASKED FROM OUTSIDE. The DOM is the second opinion and the assert is what makes
     it one: `_live` is this file's record of the renderers it forked and the frames are what the browser
     actually has, so the two disagreeing means a destroy removed a record without its element or an element
     without its record. THE ROUTING IDS ARE REPORTED because they are not this zone's to mint: every one came
     off a fork order, so the list is checkable against the browser process's own registry, and a frame with no
     routing id would be one this document created for itself. */
  function rendererStats() {
    var frames = document.querySelectorAll('iframe[title^="renderer "]').length;
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
    return { forked: _forked, provisioned: _provisioned, destroyed: _destroyed, live: _live.size,
             frames: frames, awaitingAdopt: _awaitingAdopt.size,
             routingIds: ids.sort(function (a, b) { return a - b; }), names: names };
  }
  self.rendererStats = rendererStats;

  /* ────────────────────────────────────────────────────────────────────────────────────────────────────────
     `content.mojom.Zygote` — THE ONLY PATH THAT MATERIALIZES A RENDERER. It is called by the browser process
     and never from this document, which is what the inversion IS: this function takes the routing id rather
     than allocating one, takes the agent cluster key rather than computing one, and asks nothing about whether
     the cluster should have a renderer at all.
     THE HANDSHAKE IS THE FRAME'S, NOT `onload`'s. `onload` fires for a document whose script CSP blocked, so it
     would report a renderer that does not exist as ready; the frame's own hello is the proof its bootstrap
     ran. There is no timeout on it: a wall clock here would report a loaded machine as a broken transport
     (§Testing), and the failure it would be covering — a policy that refused the bootstrap — is already
     reported by Chrome, by name, on this document's console.
     ──────────────────────────────────────────────────────────────────────────────────────────────────────── */
  async function forkRenderer(routingId, clusterKey) {
    DCHECK(clusterKey !== "",
           "a fork order named no agent cluster — a renderer IS an agent cluster's instance and the key is how " +
           "the browser process says which, and how a frame in this document is identified as that instance");
    DCHECK(!_awaitingAdopt.has(routingId),
           "the browser process ordered a fork with routing id " + routingId + ", which is already the id of a " +
           "renderer in this document whose pipe has not come back — an id is minted once per renderer, so a " +
           "repeat would give two frames one name and the second adoption would bind the wrong one");
    _forked++;
    var prog = await programOnce();
    var f = document.createElement("iframe");
    /* THE SANDBOX ATTRIBUTE IS SET BEFORE THE ADDRESS AND BEFORE INSERTION, because the flags are read when the
       load begins: an `allow-same-origin`-less frame whose attribute lands late is a document that already has
       the extension's own origin. `allow-scripts` alone — the manifest's sandbox policy grants this page
       `allow-popups`/`allow-popups-to-escape-sandbox`/`allow-modals` as well (poc-sandbox.html needs them), and
       the effective flag set is the UNION of both restrictions, so this attribute is what takes them away. */
    f.setAttribute("sandbox", "allow-scripts");
    f.setAttribute("title", "renderer " + clusterKey);
    f.setAttribute("src", RENDERER_URL);
    f.style.cssText = "position:absolute;left:0;top:0;width:1px;height:1px;border:0;visibility:hidden";
    /* NO WORKING-SET FIELD IS IN THIS LITERAL AND NONE MAY BE. There is no number meaning "this instance has
       not said yet" that a consumer would not then have to branch on, and there is no window in which one is
       needed: it is a DECLARED REPLY FIELD of every `content.mojom.Renderer` method, so every answer carries
       the current one and the caller reads it off the reply it already awaited. */
    var r = { name: clusterKey, routingId: routingId, frame: f, port: null, lines: [],
              conn: null, renderer: null, childProcess: null, _onwire: null };
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
          r.conn = new self.mojo.Connection(rendererTransport(r), {
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
    /* A RENDERER THAT DID NOT BOOT IS STILL A FRAME IN THIS DOCUMENT, so the element and its port go first, or
       a caller that retried would accumulate dead renderers under a document that never reloads. The failure
       travels on as the fork order's `error` — swallowing it would report a page as analysed by an instance
       that never existed — and it travels as a VALUE rather than a rejection because a page whose engine
       aborted its boot is a RECORDED outcome (bridge.js's engineBootFailed writes a crash record for it),
       which is a different thing from this transport being broken. */
    DCHECK(typeof stopListening === "function",
           "the invitation handshake installed no way to stop listening — the executor above runs " +
           "synchronously, so a missing one is a `message` listener that outlives every renderer this document " +
           "ever forks and answers for a frame that is gone");
    try { await booted; }
    catch (e) {
      stopListening();
      frameTeardown(r);
      RETHROW_FATAL(e);
      return { pipe: null, error: String((e && e.message) || e) };
    }
    stopListening();
    /* AND THE PIPE LEAVES THIS REALM. It is TRANSFERRED to the browser process, which relays it to whoever
       asked for the renderer — so between this line and `rendererLaunch`'s adoption the endpoint is detached
       here and live nowhere in this document, which is the property that makes "the browser process handed
       back the pipe" a fact rather than a description. `r.port` is nulled and the Connection's read callback is
       deliberately KEPT, so a stray record arriving in that window still reaches the transport's asserts
       instead of being dropped on a handler that was quietly removed. */
    var pipe = r.port;
    r.port = null;
    _awaitingAdopt.set(routingId, r);
    return { pipe: pipe, error: null };
  }

  self.mojo.exposeInterface("content.mojom.Zygote", { forkRenderer: forkRenderer });

  /* ASK THE BROWSER PROCESS FOR A RENDERER, AND ADOPT THE PIPE IT HANDS BACK. This holds no decision at all —
     it cannot produce a frame, it cannot name one, and everything it returns came out of another process. It
     exists so there is ONE way to obtain a renderer (bridge.js's pool and the probe below take the same one)
     rather than two spellings of the same four lines. */
  async function rendererLaunch(clusterKey) {
    DCHECK(typeof clusterKey === "string" && clusterKey !== "",
           "a renderer was asked for with no agent cluster key — a renderer IS a cluster's instance, and the " +
           "key is the only thing the browser process's registry decides on");
    var bp = await self.browserProcess();
    var v = await bp.rendererHost.createRendererForCluster(clusterKey);
    DCHECK((v.pipe === null) !== (v.error === null),
           "the browser process answered a renderer launch with both a pipe and an error, or with neither — a " +
           "renderer that booted has exactly one pipe and one that did not has exactly one reason");
    if (v.error !== null) {
      DCHECK(!_awaitingAdopt.has(v.routingId),
             "a fork that reported an error left its record awaiting adoption — the frame is gone, so the " +
             "record is a renderer nothing can reach and nothing will ever tear down");
      throw new Error(v.error);
    }
    var r = _awaitingAdopt.get(v.routingId);
    DCHECK(r !== undefined,
           "the browser process handed back a pipe for routing id " + v.routingId + ", which no fork order in " +
           "this document created — a renderer this zone did not fork on the browser's order is one the " +
           "browser process never decided on, and that inversion is the whole reason this transport exists");
    if (r === undefined) throw new Error("unforked routing id " + v.routingId);   // release path under the assert
    _awaitingAdopt.delete(v.routingId);
    DCHECK(r.name === clusterKey,
           "the browser process handed back renderer " + v.routingId + " under agent cluster `" + clusterKey +
           "` while this document forked it as `" + r.name + "` — the two names are one identity, and a " +
           "routing table that disagrees with the DOM is a document analysed behind the wrong principal");
    DCHECK(r.port === null,
           "a renderer's pipe came back while this zone still held one — the endpoint is transferred, so " +
           "holding one here means it never left and what arrived is a second, unentangled port");
    rendererAttachPipe(r, v.pipe);
    /* AND THE BROKER, USED ON THE RENDERER BOUNDARY TOO. Two names go out on the primordial pipe and two bound
       pipes come back, which is the whole replacement for a `fn` string naming any qjs_* entry: the ABI is an
       INTERFACE whose every parameter is declared and validated, and "how much IPC does that process have
       open" is a second interface because it is a fact about the PROCESS and not about the ABI.
       IT IS DONE HERE AND NOT AT THE FORK, because a bind request travels on the primordial pipe and that
       endpoint spends the fork inside the browser process. This is the first line at which this realm holds it
       again, which is exactly the property the relay exists to demonstrate. */
    r.renderer = r.conn.bindInterface("content.mojom.Renderer");
    r.childProcess = r.conn.bindInterface("content.mojom.ChildProcess");
    return r;
  }
  self.rendererLaunch = rendererLaunch;

  /* THE PROVISIONING, EXERCISED. SECURITY.md: "A HOST THAT CANNOT PROVISION A SECOND INSTANCE HAS NOT TESTED
     THE TRANSPORT, AND EVERY CROSS-INSTANCE MECHANISM ABOVE IS THEN A DESIGN THAT HAS NEVER RUN." This is the
     caller that makes that sentence false for this boundary, and it is driven from outside — `harness offscreen
     "return await self.rendererProbe()"` — rather than from a self-test, because a mechanism whose only caller
     is its own test is exactly what that paragraph is about.
     IT IS A REAL ROUND TRIP AND NOT A PING. The document crosses as BYTES (the shape a child navigable's
     document already takes, and the byte path cee1af3e had to export HEAPU8 for), the engine PARSES it with
     Lexbor and runs `document_bundle_id`'s <script> scan over the result, and a number computed inside the
     frame comes back. `qjs_init` runs no page script, so nothing here needs the reply/notice edges that are
     still synchronous in bridge.js.
     AND IT NOW GOES THROUGH THE BROWSER PROCESS, which is the point of an earlier change it reports:
     `routingId` is a number this document could not have produced, and its presence in
     `content.mojom.RendererHost`'s own registry is what `rendererPoolProbe` cross-checks.
     AND THE TYPING IS OBSERVABLE, which is the point of THIS one. `iface`/`ifaceVersion`/`ifaceMethods` are
     read off the bound interface's own descriptor rather than restated here, so they answer "which interface
     is this pipe actually carrying, at which version" — the question a broker that bound the wrong
     implementation would get wrong. `badMessages` is the count of records this TRUSTED zone's validator
     refused, and it is the number that would move if a renderer ever sent something the mojom does not
     declare: SECURITY.md makes the renderer the peer whose messages must be assumed hostile, so a healthy run
     reads 0 and a non-zero is that peer being broken. `child` is the same IPC surface asked of the frame ITSELF
     over a second brokered pipe, because a transport whose only evidence is that a message arrived is one
     whose shape nobody can check from outside. */
  self.rendererProbe = async function rendererProbe(html) {
    var ADDR = "https://renderer.probe/app/index.html";
    var doc = typeof html === "string" ? html
            : '<!doctype html><html><head><script src="/static/app.7c1f9b.js"></script>' +
              '<script src="/static/vendor.20f3aa.js"></script></head><body>renderer probe</body></html>';
    var rec = { provisioned: false, routingId: null, initrc: null, bundleId: null, torndown: false };
    var r = await rendererLaunch("probe");
    rec.provisioned = true;
    rec.routingId = r.routingId;
    rec.iface = r.renderer.def.name;
    rec.ifaceVersion = r.renderer.def.version;
    rec.ifaceMethods = r.renderer.def.byOrd.size;
    try {
      /* THE DOCUMENT CROSSES AS BYTES — `Init`'s `array<uint8>`, which is what `qjs_init` takes (a
         NUL-terminated pointer it strlen()s). The four strings beside it are §4.4's address, the name this
         agent's root document is known by, the response's header field lines (empty: this document had no
         response) and §8.1.3.1's top-level creation URL, which for a root document is its own. */
      var v = await r.renderer.init(new TextEncoder().encode(doc), ADDR, "probe", "", ADDR);
      rec.initrc = v.rc;
      var b = await r.renderer.getBundleId();
      rec.bundleId = (b.bundleId >>> 0).toString(36);
      /* THE WORKING SET THE FRAME STATED, read off the reply that carried it — the fact bridge.js's RAM floor
         is summed out of. A probe that never looked at it would leave the pool's one non-ABI input untested. */
      rec.heapBytes = b.workingSetBytes;
      rec.child = await r.childProcess.getMojoStats();
      /* AND THE TEARDOWN, because it is the call that makes the runtime walk gc_obj_list and report a leaked
         GC object as a failure. A probe that provisioned an instance and walked away from it would be
         measuring the transport with the one check that judges the instance switched off. */
      await r.renderer.teardown();
      rec.torndown = true;
    } catch (e) {
      /* AN ENGINE ABORT IS A RECORDED OUTCOME AND A HOST INVARIANT FAILURE IS NOT — the same split
         engineFinalize/crashResult already make, for the same reason: a WASM abort means this instance failed
         and the honest report of it is the crash PLUS everything it printed on the way down, while a DCHECK in
         THIS file is our own contract breaking and must not be reported as the engine's. Discarding the record
         here would leave an empty tail where a crash leaves the output that preceded it. */
      RETHROW_FATAL(e);
      rec.crashed = true;
      rec.err = String((e && e.message) || e);
    } finally {
      r.destroy();
    }
    /* READ AFTER THE TEARDOWN ON PURPOSE: a destroyed renderer's endpoints leave this realm's open set with the
       connection they belonged to, so `mojo` here is what the offscreen holds once this probe's instance is
       gone — the browser process's three Remotes and nothing of the renderer's. An `endpoints` that kept
       climbing across probes would be a document reporting its whole history of instances as its live IPC. */
    rec.mojo = self.mojo.stats();
    rec.badMessages = rec.mojo.badMessages;
    rec.lines = r.lines;
    return rec;
  };

  console.debug("[renderer-host] ready (content.mojom.Zygote exposed; self.rendererLaunch + " +
                "self.rendererProbe installed)");
})();
