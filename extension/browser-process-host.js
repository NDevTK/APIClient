/* browser-process-host.js — THE TRUSTED SIDE OF THE BROWSER PROCESS. `renderer-host.js` is the same object for
 * the untrusted renderer, and this is its counterpart: the offscreen — the one fully-trusted zone
 * (SECURITY.md) — provisions the browser process's Worker, accepts its Mojo invitation, and holds the Remotes
 * every other component in this document reaches it through.
 *
 * WHAT THIS BOUNDARY IS FOR, AND WHY IT IS A WORKER. The browser process owns the RENDERER REGISTRY —
 * SECURITY.md's "One WASM instance per ORIGIN-KEYED AGENT CLUSTER" rule, held by the program whose job it is
 * to hold it rather than by the zone that wants the renderer. That is STATE and it is a DECISION, which is
 * exactly what CLAUDE.md §Architecture takes out of a bridge; it lives behind a boundary the browser holds, a
 * dedicated Worker with its own realm, its own module instance and its own thread. Nobody holds a `HEAPU8`
 * over it — which is precisely what a second wasm-ld link out of one object set could never say, and why that
 * attempt was deleted rather than kept.
 *
 * IT HELD A SECOND THING AND NO LONGER DOES. `self.browserProcessCorb` and `self.browserProcessClassify`
 * relayed WHATWG MIME Sniffing §7, Chromium's CORB analyzer and an asset classifier into C, and both entries
 * are deleted with that C. They had been transliterated out of JavaScript that was shipping and working, on a
 * migration mandate that no longer exists: CLAUDE.md §Architecture now says "TYPE SNIFFING STAYS IN
 * JAVASCRIPT, in `safeFetch`, where SECURITY.md puts it." The chokepoint that READ the bytes decides once and
 * STAMPS what it decided onto the reply record (`computedType`), so nothing downstream — not this file, not
 * the renderer — asks the question a second time.
 *
 * THE TRANSPORT IS MOJO, and the `{v:1,id,op}` records this file used to build are DELETED with it.
 * `bpCall` was a hand-written request-id table, `onReply` was a hand-written demultiplexer, `op` was a
 * hand-written capability list. Every one of those has a name in Mojo — `Remote`, `Receiver`, an interface
 * broker, and a mojom parameter type — so extension/mojo.js holds the machinery, extension/mojom.js holds the
 * IDL, and what is left here is provisioning and the probe.
 *
 * ONE PROCESS FOR THE WHOLE OFFSCREEN, not one per document. A renderer is per agent cluster because it IS that
 * cluster's heap; the RENDERER REGISTRY is per-document by definition, and a registry split across two
 * processes would be two authorities answering "does this agent cluster already have an instance?", whose
 * first disagreement is two heaps behind one principal.
 */
(function () {
  "use strict";

  var WORKER_URL = "browser-process.js";

  /* PROVISION-ONCE. The promise itself is the lock: a second caller arriving while the module is still
     instantiating awaits the same one rather than starting a second worker. There is no timeout on the
     invitation, for renderer-host.js's reason — a wall clock here would report a loaded machine as a broken
     transport (CLAUDE.md §Testing), and the failure it would be covering (a policy that refused the worker) is
     already reported by Chrome, by name, on this document's console.
     A REJECTION IS STICKY, AND THAT IS THE FAIL-CLOSED DIRECTION. A browser process that did not load leaves a
     rejected promise here forever, so every later request for a renderer rejects with the same reason rather
     than retrying into a state where no authority holds SECURITY.md's one-instance-per-agent-cluster rule.
     Re-provisioning on demand would turn "the browser process is missing" into an intermittent condition,
     which is the one shape a security gate must never have. */
  var _bpP = null;
  /* AND THE RESOLVED HANDLE IS ALSO HELD SYNCHRONOUSLY, for one caller that cannot await: a renderer's
     TERMINATION notice. `rendererDestroy` removes a frame on a synchronous path (hostClear drops the whole
     pool; a failed boot reclaims its own frame), and the browser process's registry is the only thing that can
     free that agent cluster. A renderer exists only because that process created it, so by the time anything
     can be destroyed this is non-null — which is asserted at that call rather than defaulted past. */
  var _bpNow = null;

  function browserProcessOnce() {
    if (_bpP) return _bpP;
    _bpP = new Promise(function (resolve, reject) {
      /* A MODULE WORKER, because the program it loads is an ES module: `engine/build.mjs` links the browser
         process with -sMODULARIZE -sEXPORT_ES6, whose default export is the factory. Both this script and that
         module are same-origin extension resources, so `script-src 'self'` allows them and the manifest's
         `require-corp` has nothing to refuse — COEP constrains CROSS-origin subresources, and the offscreen
         document's own policy carries `worker-src 'self'`. */
      var w = new Worker(WORKER_URL, { type: "module" });
      var bp = { worker: w, lines: [], conn: null, rendererHost: null, childProcess: null };
      /* A WORKER THAT FAILED TO LOAD ITS SCRIPT REPORTS HERE AND NOWHERE ELSE. Without this the invitation
         would never be accepted and every caller would park forever on an answer that is not coming — the one
         outcome with no symptom anywhere. */
      w.onerror = function (e) {
        reject(new Error("the browser process's worker did not load: " + ((e && e.message) || "(no message)")));
      };
      /* THE PRIMORDIAL PIPE IS THE WORKER HANDLE ITSELF, which is exactly what `mojo::OutgoingInvitation` gives
         a freshly launched child: one pipe and nothing else. Every capability rides a pipe brokered over it. */
      bp.conn = new self.mojo.Connection(
        { post: function (m, xfer) { w.postMessage(m, xfer); },
          listen: function (cb) { w.onmessage = function (e) { cb(e.data); }; } },
        { role: "parent", name: "the browser process",
          /* THE CHILD'S OUTPUT, ABSORBED WHERE IT ARRIVES. A crash's ROOT line is the last `@WHY` printed
             before abort(), and lines left in the worker would be a crashed browser process taking its own
             cause with it. stderr is teed live to this document's console — the same tee renderer-host.js
             performs, and for the same reason: a native diagnostic is capturable while the run is happening
             (`harness diag`) instead of only in a record that is about to be discarded. */
          onStdio: function (lines) {
            for (var i = 0; i < lines.length; i++) {
              var ln = lines[i];
              DCHECK(Array.isArray(ln) && (ln[0] === 1 || ln[0] === 2) && typeof ln[1] === "string",
                     "the browser process's output line did not carry the stream it came from — a line crosses " +
                     "as `[fd, text]` so the two streams stay in ONE order (the last @WHY before an abort is " +
                     "what names the cause) while stderr alone is teed live");
              bp.lines.push(ln[1]);
              if (ln[0] === 2) console.debug(ln[1]);
            }
          },
          /* AND EVERY ERROR THIS CONNECTION RAISES CARRIES THAT TAIL, because the reason a call failed is
             almost never in the exception and almost always in the eight lines before it. */
          decorate: function (err) { err.browserProcessLines = bp.lines.slice(-8); } });
      bp.conn.ready.then(function () {
        /* THE BROKER, USED. Two names go out on the primordial pipe and two bound pipes come back — which
           is the whole replacement for a hardcoded `op` list, and is why a third capability is a third
           interface rather than a third arm of two switch statements. */
        bp.rendererHost = bp.conn.bindInterface("content.mojom.RendererHost");
        bp.childProcess = bp.conn.bindInterface("content.mojom.ChildProcess");
        _bpNow = bp;
        resolve(bp);
      }, reject);
    });
    return _bpP;
  }
  self.browserProcess = browserProcessOnce;
  self.browserProcessNow = function browserProcessNow() { return _bpNow; };

  /* THE BOUNDARY, EXERCISED. SECURITY.md: "A HOST THAT CANNOT PROVISION A SECOND INSTANCE HAS NOT TESTED THE
     TRANSPORT, AND EVERY CROSS-INSTANCE MECHANISM ABOVE IS THEN A DESIGN THAT HAS NEVER RUN." This is the
     caller that makes that sentence false for this boundary, and it is driven from OUTSIDE — `harness offscreen
     "return await self.browserProcessProbe()"` — rather than from a self-test, because a mechanism whose only
     caller is its own test is exactly what that paragraph is about.
     WHAT IT ASKS IS NOW THE REGISTRY, because that is what this process holds. The nine CORB rows and ten
     classification rows that stood here went with `self.browserProcessCorb` and `self.browserProcessClassify`:
     type sniffing is `lib/safe-fetch.js`'s again (CLAUDE.md §Architecture), so the algorithm they exercised is
     not in this program to exercise. `GetRegistry` is a read of the table the C minted, and the cross-check
     `rendererPoolProbe` performs against it — the ids the offscreen holds frames for against the ids this
     table issued — only means anything because the two sides are two PROGRAMS.
     AND IT REPORTS WHAT EACH SIDE HOLDS OF THE OTHER. `handleKeys` is the literal key list of the object this
     file keeps per browser process: a `Module` or a `HEAPU8` in it would be this zone reaching into the other
     program's linear memory, which is precisely what two wasm-ld links out of one object set gave and what a
     Worker cannot give. `mojo` and `child` are the two halves of the IPC surface — which interfaces each
     process holds a Remote for, which it implements, and how many pipe endpoints are open — because a transport
     whose only evidence is that a message arrived is one whose shape nobody can check from outside. */
  self.browserProcessProbe = async function browserProcessProbe() {
    var rec = { provisioned: false, handleKeys: [] };
    var bp = await browserProcessOnce();
    rec.provisioned = true;
    rec.handleKeys = Object.keys(bp).sort();
    rec.registry = await bp.rendererHost.getRegistry();
    rec.mojo = self.mojo.stats();
    rec.child = await bp.childProcess.getMojoStats();
    rec.lines = bp.lines;
    return rec;
  };

  console.debug("[browser-process] ready (self.browserProcess + self.browserProcessNow + " +
                "self.browserProcessProbe installed)");
})();
