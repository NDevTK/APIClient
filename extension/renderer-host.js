/* renderer-host.js — THE TRUSTED SIDE OF A RENDERER FRAME. Chromium's name for the object that owns a renderer
 * and is the only thing that may speak to it is `RenderFrameHost`, and this is that object: the offscreen — the
 * one trusted zone (SECURITY.md) — provisions a sandboxed frame, hands it the engine program, and relays the
 * qjs_* ABI to it. It holds no analysis logic; every decision that is not "which renderer, and what did it
 * answer" stays on this side of the boundary in bridge.js.
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
 * ONE RENDERER PER ORIGIN-KEYED AGENT CLUSTER. SECURITY.md fixes the unit: "One WASM instance per ORIGIN-KEYED
 * AGENT CLUSTER — (browsing-context group, origin)", which `bridge.js`'s `clusterKeyOf` already computes off
 * browser-stated facts. A renderer's lifetime is that instance's: born where the instance is (one document
 * rooting one agent, `qjs_init`), destroyed where the instance is torn down (`qjs_teardown`, and the Clear
 * button's `hostClear`). This file therefore takes a NAME and nothing else — it does not compute the key, does
 * not admit, does not rank; the pool that owns those questions passes the answer in.
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
 *
 * TRANSFERABLES ARE NOT USED, and that is a decision rather than an omission. Transferring detaches the sender's
 * buffer, which is only sound where this zone provably holds the last reference: the engine PROGRAM is cached
 * here and handed to every renderer, so transferring it would detach it out from under the second one. The
 * place transfer becomes correct is a fetched reply body, whose only consumer is the renderer parked on it —
 * that is a change to make at that call site, with the ownership argument stated there, not a default here.
 * IT IS NOT UNIFORMLY CORRECT EVEN FOR BYTES THIS ZONE FETCHED, which is why it stays a per-site decision:
 * §7.4 step 14's document bytes go to `qjs_init` AND are retained by bridge.js as `eng.html`, the cold recipe
 * it writes to IndexedDB on finalize, so transferring those would empty the cross-session frontier's copy of
 * the document — a detached buffer reads as a zero-length one, which is a page that parsed to nothing.
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

  /* THE LIVE RENDERERS OF THIS ZONE, AND THE TWO TOTALS THAT MAKE A CLEAN TEARDOWN DISTINGUISHABLE FROM NOTHING
     EVER HAVING RUN. "no frames and no engines" is what a pool that finalized every instance looks like AND
     what an extension that never provisioned one looks like, which is precisely the shape CLAUDE.md names —
     a number that reads the same whether the mechanism worked or was never reached. So the counters are kept:
     `provisioned` rises where a frame is created and `destroyed` where one is removed, and `rendererStats`
     CHECKS the set against the DOM, so a renderer whose element outlived its record (or the reverse) is caught
     here rather than as a slow leak of frames under a document that never reloads. */
  var _live = new Set();
  var _provisioned = 0, _destroyed = 0;

  /* THE RENDERER GOES WITH ITS FRAME. There is no free list on the other side and none is wanted: a pointer a
     call RETAINED lives as long as the module does and the module dies with the document, so removing the
     element is the whole teardown. `qjs_teardown` is a separate ABI call the OWNER makes first — it is the
     engine asking its own runtime to walk gc_obj_list and report leaks, which is a finding, not cleanup. */
  function rendererDestroy(r) {
    DCHECK(r._await.size === 0,
           "a renderer was destroyed with " + r._await.size + " call(s) still outstanding — each one is a " +
           "caller parked on an answer that can no longer be produced, which is silent everywhere");
    if (r.port) r.port.close();
    if (r.frame.parentNode) r.frame.parentNode.removeChild(r.frame);
    r._dead = true;
    if (_live.delete(r)) _destroyed++;
  }

  /* WHAT THIS ZONE IS HOLDING, ASKED FROM OUTSIDE. The DOM is the second opinion and the assert is what makes
     it one: `_live` is this file's record of the renderers it provisioned and the frames are what the browser
     actually has, so the two disagreeing means a destroy removed a record without its element or an element
     without its record. */
  function rendererStats() {
    var frames = document.querySelectorAll('iframe[title^="renderer "]').length;
    DCHECK(frames === _live.size,
           "this zone holds " + _live.size + " renderer(s) but the document carries " + frames + " renderer " +
           "frame(s) — a frame that outlives its record is a WASM instance nothing can reach and nothing will " +
           "ever tear down, and a record that outlives its frame is a caller about to post into a dead window");
    return { provisioned: _provisioned, destroyed: _destroyed, live: _live.size, frames: frames,
             names: Array.from(_live).map(function (r) { return r.name; }) };
  }
  self.rendererStats = rendererStats;

  /* PROVISION ONE RENDERER. The frame is created here, by the offscreen, because SECURITY.md makes this the
     only zone that knows which instance holds which document — the same reason it owns the routing and stamps
     the sender's origin.
     THE HANDSHAKE IS THE FRAME'S, NOT `onload`'s. `onload` fires for a document whose script CSP blocked, so it
     would report a renderer that does not exist as ready; the frame's own hello is the proof its bootstrap
     ran. There is no timeout on it: a wall clock here would report a loaded machine as a broken transport
     (§Testing), and the failure it would be covering — a policy that refused the bootstrap — is already
     reported by Chrome, by name, on this document's console. */
  async function rendererCreate(name) {
    DCHECK(typeof name === "string" && name !== "",
           "a renderer was provisioned with no name — a renderer IS an agent cluster's instance and the name " +
           "is how this zone says which");
    var prog = await programOnce();
    var f = document.createElement("iframe");
    /* THE SANDBOX ATTRIBUTE IS SET BEFORE THE ADDRESS AND BEFORE INSERTION, because the flags are read when the
       load begins: an `allow-same-origin`-less frame whose attribute lands late is a document that already has
       the extension's own origin. `allow-scripts` alone — the manifest's sandbox policy grants this page
       `allow-popups`/`allow-popups-to-escape-sandbox`/`allow-modals` as well (poc-sandbox.html needs them), and
       the effective flag set is the UNION of both restrictions, so this attribute is what takes them away. */
    f.setAttribute("sandbox", "allow-scripts");
    f.setAttribute("title", "renderer " + name);
    f.setAttribute("src", RENDERER_URL);
    f.style.cssText = "position:absolute;left:0;top:0;width:1px;height:1px;border:0;visibility:hidden";
    /* `heapBytes` IS NOT IN THIS LITERAL AND MUST NOT BE. There is no number that means "this instance has not
       said yet" which a consumer would not then have to branch on, and there is no window in which one is
       needed: the boot reply is the first record to arrive on this port and carries it, and this function does
       not return until that record has landed. onReply is its only writer. */
    var r = { name: name, frame: f, port: null, lines: [], _next: 1, _await: new Map(), _dead: false };
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
    /* A RENDERER THAT DID NOT BOOT IS STILL A FRAME IN THIS DOCUMENT. The failure travels on — it is the
       engine's own abort or one of the frame's asserts, and swallowing it would report a page as analysed by
       an instance that never existed — but the element and its port go first, or a caller that retries would
       accumulate dead renderers under a document that never reloads. */
    try { await booted; }
    catch (e) { r.destroy(); throw e; }
    return r;
  }

  self.rendererCreate = rendererCreate;

  /* THE PROVISIONING, EXERCISED. SECURITY.md: "A HOST THAT CANNOT PROVISION A SECOND INSTANCE HAS NOT TESTED
     THE TRANSPORT, AND EVERY CROSS-INSTANCE MECHANISM ABOVE IS THEN A DESIGN THAT HAS NEVER RUN." This is the
     caller that makes that sentence false for this boundary, and it is driven from outside — `harness offscreen
     "return await self.rendererProbe()"` — rather than from a self-test, because a mechanism whose only caller
     is its own test is exactly what that paragraph is about.
     IT IS A REAL ROUND TRIP AND NOT A PING. The document crosses as BYTES (the shape a child navigable's
     document already takes, and the byte path cee1af3e had to export HEAPU8 for), the engine PARSES it with
     Lexbor and runs `document_bundle_id`'s <script> scan over the result, and a number computed inside the
     frame comes back. `qjs_init` runs no page script, so nothing here needs the reply/notice edges that are
     still synchronous in bridge.js. */
  self.rendererProbe = async function rendererProbe(html) {
    var ADDR = "https://renderer.probe/app/index.html";
    var doc = typeof html === "string" ? html
            : '<!doctype html><html><head><script src="/static/app.7c1f9b.js"></script>' +
              '<script src="/static/vendor.20f3aa.js"></script></head><body>renderer probe</body></html>';
    var rec = { provisioned: false, initrc: null, bundleId: null, torndown: false };
    var r = await rendererCreate("probe");
    rec.provisioned = true;
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

  console.debug("[renderer-host] ready (self.rendererCreate + self.rendererProbe installed)");
})();
