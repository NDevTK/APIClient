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
 * and a `Uint8Array` as bytes — so nothing is encoded in transit. A REPLY carries three things beside its call
 * id: what the ABI answered, the engine output drained since the last one (each line tagged with the STREAM it
 * came from, so stderr can be teed live without teeing @H traffic with it), and the instance's WORKING SET,
 * which is the one fact bridge.js's admission needs and cannot read off a Module it no longer holds.
 * THIS ONE IS STILL THE AD-HOC `{op:"call"}` RECORD AND NOT MOJO YET, stated rather than left to be noticed.
 * The browser-process boundary is converted in this diff and its envelope is deleted; this one carries a
 * GENERIC relay of nineteen ABI entries of varying shapes, so converting it is a TYPING job — each qjs_* entry
 * becomes a mojom method with declared parameter types — and doing it in the same diff as the transport itself
 * would be two changes with one failure surface. It is the next step and it is named as one.
 *
 * TRANSFERABLES ARE NOT USED FOR ABI ARGUMENTS, and that is a decision rather than an omission. Transferring
 * detaches the sender's buffer, which is only sound where this zone provably holds the last reference: the
 * engine PROGRAM is cached here and handed to every renderer, so transferring it would detach it out from under
 * the second one. The place transfer becomes correct is a fetched reply body, whose only consumer is the
 * renderer parked on it — that is a change to make at that call site, with the ownership argument stated there,
 * not a default here. IT IS NOT UNIFORMLY CORRECT EVEN FOR BYTES THIS ZONE FETCHED: §7.4 step 14's document
 * bytes go to `qjs_init` AND are retained by bridge.js as `eng.html`, the cold recipe it writes to IndexedDB on
 * finalize, so transferring those would empty the cross-session frontier's copy of the document — a detached
 * buffer reads as a zero-length one, which is a page that parsed to nothing. THE ONE THING THAT IS TRANSFERRED
 * is the renderer's own PIPE, on the way out to the browser process and back, and there transfer is the point:
 * a handle that moves is a handle this zone provably does not hold in between.
 *
 * THE ORIGIN STAMP STAYS THIS ZONE'S. A record arriving from a frame may not state who it is: the frame's
 * `event.origin` is the literal "null" (it is opaque) and everything in the payload is written by the untrusted
 * side. Identity here is the browser's — `event.source` is the WindowProxy of the frame THIS file created, and
 * once the boot record has handed over a `MessagePort` the channel is point-to-point and there is no source to
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
      var names = ["check.js", "lib/qjs/qjs.mjs", "lib/qjs/qjs.wasm"];
      var rs = await Promise.all(names.map(function (n) { return fetch(n); }));
      for (var i = 0; i < rs.length; i++)
        CHECK(rs[i].ok, "the trusted zone could not read the renderer's own program off its own origin (" +
                        names[i] + ") — every renderer is built out of these three and there is no other way " +
                        "to get them into an opaque-origin frame");
      var bufs = await Promise.all(rs.map(function (r) { return r.arrayBuffer(); }));
      return {
        check: new Blob([bufs[0]], { type: "text/javascript" }),
        glue: new Blob([bufs[1]], { type: "text/javascript" }),
        wasm: new Uint8Array(bufs[2]),
      };
    })();
    return _programP;
  }

  /* A REPLY. Every one carries the engine's stdout/stderr since the last one — bridge.js reads @RESULT, @E and
     @WHY out of exactly those lines and a crash's ROOT line is the last @WHY printed before abort(), so lines
     left in the frame would be a crashed renderer taking its own cause with it. */
  function onReply(r, m) {
    DCHECK(m && m.v === 1 && typeof m.id === "number" && Array.isArray(m.out),
           "a renderer answered with something that is not this transport's reply — a reply is a version, the " +
           "call id it answers, and the engine output drained with it");
    for (var i = 0; i < m.out.length; i++) {
      var ln = m.out[i];
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
    var w = r._await.get(m.id);
    DCHECK(w !== undefined,
           "a renderer answered a call id this zone never made — the id is the whole routing table for an " +
           "answer, so an unknown one means the frame echoed something other than what it was handed and the " +
           "call that is actually outstanding will never be resolved");
    if (!w) return;
    r._await.delete(m.id);
    if (m.ok) {
      /* THE INSTANCE'S WORKING SET, RECORDED WHERE IT ARRIVES. bridge.js's Level-1 admission is the sum of this
         number over the pool and it used to read `M.HEAPU8.length` off the Module it held; there is no Module
         on this side any more, so the renderer states it on every reply and the last one is what the pool
         holds. Asserted rather than defaulted: an absent one is renderer.html having stopped writing it, and
         the shape that hides is "this instance occupies no memory", which admits another engine against RAM
         that is already spent. */
      DCHECK(typeof m.heapBytes === "number",
             "a renderer answered a call without stating its working set — every successful reply carries " +
             "HEAPU8.length because a call is the only thing that can grow it, and the trusted zone's RAM " +
             "floor is the sum of that number across the pool");
      r.heapBytes = m.heapBytes;
      w.resolve(m.ret);
      return;
    }
    /* A FAILED CALL IS A REJECTION, because that is the shape the seam it replaces already has: `M.ccall`
       THROWS when the WASM aborts, and bridge.js's engineCrash is written against that throw. The renderer is
       dead after one — its linear memory is what aborted — so every later call into it is a bug here, not a
       second crash there. */
    r._dead = true;
    var e = new Error(m.err);
    e.rendererLines = r.lines.slice(-8);
    w.reject(e);
  }

  /* ONE ABI CALL, in the shape `M.ccall` has, so the conversion of each of bridge.js's call sites is the word
     `await` and nothing else. `args` declares each argument's SHAPE (see renderer.html for what each places);
     `bodies` is the byte list those shapes index, held beside the record so a caller cannot deliver one
     without the other. */
  function rendererCall(r, fn, ret, args, bodies) {
    DCHECK(!r._dead,
           "an ABI call was made into a renderer that has already aborted — its linear memory is the thing " +
           "that failed, so this call would be a second crash reported as a first");
    DCHECK(r.port !== null,
           "an ABI call was made into a renderer whose pipe is not bound — between the fork order and the " +
           "browser process handing the pipe back, this zone provably does not hold it, and a call in that " +
           "window is a caller reaching for a renderer it has not been given yet");
    DCHECK(typeof fn === "string" && fn.lastIndexOf("qjs_", 0) === 0,
           "a renderer was asked to call something outside the engine ABI (`" + fn + "`) — build.mjs's " +
           "QJS_ABI is the whole list and every entry is named qjs_*");
    DCHECK(ret === null || ret === "number" || ret === "string",
           "a renderer call declared a return type this transport cannot carry (`" + ret + "`) — the ABI " +
           "answers a number, a C string, or nothing");
    DCHECK(Array.isArray(args), "a renderer call carried no argument list");
    var id = r._next++;
    var p = new Promise(function (res, rej) { r._await.set(id, { resolve: res, reject: rej }); });
    r.port.postMessage({ v: 1, id: id, op: "call", fn: fn, ret: ret, args: args, bodies: bodies || [] });
    return p;
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
    DCHECK(r._await.size === 0,
           "a renderer was destroyed with " + r._await.size + " call(s) still outstanding — each one is a " +
           "caller parked on an answer that can no longer be produced, which is silent everywhere");
    if (r.port) r.port.close();
    if (r.frame.parentNode) r.frame.parentNode.removeChild(r.frame);
    r._dead = true;
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
    /* `heapBytes` IS NOT IN THIS LITERAL AND MUST NOT BE. There is no number that means "this instance has not
       said yet" which a consumer would not then have to branch on, and there is no window in which one is
       needed: the boot reply is the first record to arrive on this port and carries it, and this function does
       not answer the fork order until that record has landed. onReply is its only writer. */
    var r = { name: clusterKey, routingId: routingId, frame: f, port: null, lines: [],
              _next: 1, _await: new Map(), _dead: false };
    _live.add(r);
    _provisioned++;
    var booted = new Promise(function (resolve, reject) {
      function onHello(e) {
        /* THE BROWSER-SET IDENTITY, AND THE ONLY ONE THERE IS. The frame's origin is opaque so `event.origin`
           is the literal "null" for every renderer; `event.source` is the WindowProxy of the frame this
           closure created. Another renderer booting concurrently posts its hello here too and is matched by
           ITS own listener, so a mismatch is that and not a message to drop on the floor. */
        if (e.source !== f.contentWindow) return;
        removeEventListener("message", onHello);
        /* A DEDICATED CHANNEL FROM HERE ON. The hello is the one message that has to travel window-to-window
           (there is nothing else yet to travel on); every call after it rides a MessagePort, which is
           point-to-point by construction — no target origin to state, no source to check, and no other
           listener on this window can see it. */
        var ch = new MessageChannel();
        r.port = ch.port1;
        ch.port1.onmessage = function (ev) { onReply(r, ev.data); };
        r._await.set(0, { resolve: resolve, reject: reject });
        /* "*" IS FORCED: the frame's origin is opaque and postMessage's targetOrigin parses as a URL unless it
           is "*" or "/", so there is no origin string that names it. It is not a broadcast — the message goes
           to this frame's window and to nothing else. */
        e.source.postMessage({ v: 1, id: 0, op: "boot",
                               check: prog.check, glue: prog.glue, wasm: prog.wasm }, "*", [ch.port2]);
      }
      addEventListener("message", onHello);
    });
    (document.body || document.documentElement).appendChild(f);
    r.call = function (fn, ret, args, bodies) { return rendererCall(r, fn, ret, args, bodies); };
    r.destroy = function () { rendererDestroy(r); };
    /* A RENDERER THAT DID NOT BOOT IS STILL A FRAME IN THIS DOCUMENT, so the element and its port go first, or
       a caller that retried would accumulate dead renderers under a document that never reloads. The failure
       travels on as the fork order's `error` — swallowing it would report a page as analysed by an instance
       that never existed — and it travels as a VALUE rather than a rejection because a page whose engine
       aborted its boot is a RECORDED outcome (bridge.js's engineBootFailed writes a crash record for it),
       which is a different thing from this transport being broken. */
    try { await booted; }
    catch (e) {
      frameTeardown(r);
      RETHROW_FATAL(e);
      return { pipe: null, error: String((e && e.message) || e) };
    }
    /* AND THE PIPE LEAVES THIS REALM. It is TRANSFERRED to the browser process, which relays it to whoever
       asked for the renderer — so between this line and `rendererLaunch`'s adoption the endpoint is detached
       here and live nowhere in this document, which is the property that makes "the browser process handed
       back the pipe" a fact rather than a description. `r.port` is nulled and `onmessage` is deliberately NOT,
       so a stray record arriving in that window still reaches an assert instead of being dropped on a handler
       that was quietly removed. */
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
    r.port = v.pipe;
    v.pipe.onmessage = function (ev) { onReply(r, ev.data); };
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
     AND IT NOW GOES THROUGH THE BROWSER PROCESS, which is the point of the change it reports: `routingId` is a
     number this document could not have produced, and its presence in `content.mojom.RendererHost`'s own
     registry is what `rendererPoolProbe` cross-checks. */
  self.rendererProbe = async function rendererProbe(html) {
    var ADDR = "https://renderer.probe/app/index.html";
    var doc = typeof html === "string" ? html
            : '<!doctype html><html><head><script src="/static/app.7c1f9b.js"></script>' +
              '<script src="/static/vendor.20f3aa.js"></script></head><body>renderer probe</body></html>';
    var rec = { provisioned: false, routingId: null, initrc: null, bundleId: null, torndown: false };
    var r = await rendererLaunch("probe");
    rec.provisioned = true;
    rec.routingId = r.routingId;
    try {
      rec.initrc = await r.call("qjs_init", "number",
        [{ t: "cbytes", i: 0 },        /* the document, as BYTES — qjs_init strlen()s a NUL-terminated pointer */
         { t: "cstr", v: ADDR },       /* §4.4's document address, which the engine derives the origin from */
         { t: "cstr", v: "probe" },    /* the name this agent's root document is known by */
         { t: "cstr", v: "" },         /* the response's header field lines: this document had no response */
         { t: "cstr", v: ADDR }],      /* §8.1.3.1's top-level creation URL — a root document is its own */
        [new TextEncoder().encode(doc)]);
      rec.bundleId = ((await r.call("qjs_bundle_id", "number", [], [])) >>> 0).toString(36);
      /* THE WORKING SET THE FRAME STATED, which is the fact bridge.js's RAM floor is summed out of. A probe
         that never looked at it would leave the pool's one non-ABI input unexercised. */
      rec.heapBytes = r.heapBytes;
      /* AND THE TEARDOWN, because it is the call that makes the runtime walk gc_obj_list and report a leaked
         GC object as a failure. A probe that provisioned an instance and walked away from it would be
         measuring the transport with the one check that judges the instance switched off. */
      await r.call("qjs_teardown", null, [], []);
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
    rec.lines = r.lines;
    return rec;
  };

  console.debug("[renderer-host] ready (content.mojom.Zygote exposed; self.rendererLaunch + " +
                "self.rendererProbe installed)");
})();
