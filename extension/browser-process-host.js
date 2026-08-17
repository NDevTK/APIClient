/* browser-process-host.js — THE TRUSTED SIDE OF THE BROWSER PROCESS. `renderer-host.js` is the same object for
 * the untrusted renderer, and this is its counterpart: the offscreen — the one fully-trusted zone
 * (SECURITY.md) — provisions the browser process's Worker, accepts its Mojo invitation, and holds the Remotes
 * every other component in this document reaches it through.
 *
 * WHAT THIS BOUNDARY IS FOR, AND WHY IT IS A WORKER. SECURITY.md puts every analyzer-driven request through
 * `safeFetch`, and one of that chokepoint's guarantees reads BYTES: CORB must refuse a cross-origin HTML/JSON
 * body that a code loader asked for as script. Deciding what a body IS is WHATWG MIME Sniffing §7, an algorithm
 * that in a real browser runs in the NETWORK SERVICE and never in a renderer — a renderer that classifies for
 * itself can mine a cross-origin body it would otherwise have been handed empty. It also may not live in this
 * file: CLAUDE.md §Architecture leaves this zone a BRIDGE and never logic, and the JS that used to hold it
 * (`_jsMime`, `_corbProtectedMime`, `_sniffsProtected`, `_corbAllowsScript` in lib/safe-fetch.js) was a
 * hand-rolled second implementation of a standard. So it moves into C, into a program of its own, behind a
 * boundary the browser holds: a dedicated Worker with its own realm, its own module instance and its own
 * thread. Nobody holds a `HEAPU8` over it — which is precisely what a second wasm-ld link out of one object set
 * could never say, and why that attempt was deleted rather than kept.
 *
 * THE TRANSPORT IS MOJO NOW, and the `{v:1,id,op:"corb"}` records this file used to build are DELETED with it.
 * `bpCall` was a hand-written request-id table, `onReply` was a hand-written demultiplexer, `op` was a
 * hand-written capability list and `checkHeaderFacts` was this side's copy of three rules the far side also
 * held a copy of. Every one of those has a name in Mojo — `Remote`, `Receiver`, an interface broker, and a
 * mojom parameter type — so extension/mojo.js holds the machinery, extension/mojom.js holds the IDL, and what
 * is left here is provisioning, the two shipped entries, and the probe.
 *
 * WHAT STAYS ON THIS SIDE, DELIBERATELY. The ORIGIN COMPARISON. SECURITY.md makes the same-origin principal the
 * requesting frame's `MessageSender.origin` — browser-set, opaque-unique, "NEVER re-parsed from a URL" — so
 * what crosses to the browser process is a browser-stated BOOLEAN, exactly as a delivered cross-document
 * message carries an origin this zone stamped rather than one the far side minted. The far side has no URL, no
 * principal and no way to invent either.
 *
 * ONE PROCESS FOR THE WHOLE OFFSCREEN, not one per document. A renderer is per agent cluster because it IS that
 * cluster's heap; the network service holds no per-document state at all — every entry it serves is a pure
 * function of its arguments — and the RENDERER REGISTRY that process also holds is per-document by definition:
 * a registry split across two processes would be two authorities answering "does this agent cluster already
 * have an instance?", and the first disagreement is two heaps behind one principal. What keeps two pages'
 * fetches from contaminating each other is that the principal travels PER CALL, which it does.
 */
(function () {
  "use strict";

  var WORKER_URL = "browser-process.js";

  /* §5.2's RESOURCE HEADER: the first 1445 bytes. The slice happens HERE and not in the worker, because
     `postMessage` structured-clones a Uint8Array's WHOLE underlying buffer — `subarray` would carry the entire
     multi-megabyte chunk across the thread to answer a question defined over its first bytes, and `slice`
     copies out a new one. The far side asserts the bound rather than re-applying it. */
  var RESOURCE_HEADER_MAX = 1445;

  /* PROVISION-ONCE. The promise itself is the lock: a second caller arriving while the module is still
     instantiating awaits the same one rather than starting a second worker. There is no timeout on the
     invitation, for renderer-host.js's reason — a wall clock here would report a loaded machine as a broken
     transport (CLAUDE.md §Testing), and the failure it would be covering (a policy that refused the worker) is
     already reported by Chrome, by name, on this document's console.
     A REJECTION IS STICKY, AND THAT IS THE FAIL-CLOSED DIRECTION. A browser process that did not load leaves a
     rejected promise here forever, so every later script fetch rejects out of safeFetch's CORB gate with the
     same reason rather than retrying into a state where nothing judges the body. Re-provisioning on demand
     would turn "the browser process is missing" into an intermittent condition, which is the one shape a
     security gate must never have. */
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
      var bp = { worker: w, lines: [], conn: null, sniffer: null, rendererHost: null, childProcess: null };
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
        /* THE BROKER, USED. Three names go out on the primordial pipe and three bound pipes come back — which
           is the whole replacement for a hardcoded `op` list, and is why a fourth capability is a fourth
           interface rather than a fourth arm of two switch statements. */
        bp.sniffer = bp.conn.bindInterface("network.mojom.ContentSniffer");
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

  /* THE ONLY ASSERT LEFT ON THIS SIDE IS THE ONE MOJO CANNOT MAKE, and it is here because it guards a
     CORRUPTION rather than a type: `slice` is defined on a String too, so a caller that handed over decoded
     text would produce a plausible shorter string, and mojom's `array<uint8>` would then name the parameter one
     line after the evidence was destroyed. Every other rule these two entries used to restate — a null
     Content-Type is §5.1's undefined supplied type and not "", an absent X-Content-Type-Options is null, a
     browser-stated boolean is not undefined — is DECLARED in mojom.js and asserted by the transport at both
     ends of the pipe, with the sentence that explains it. */
  function checkBody(what, body) {
    DCHECK(body instanceof Uint8Array,
           "the " + what + " gate was handed a body that is not a byte sequence — the decision reads the body, " +
           "a string here is a caller having run a decode it does not own, and the truncation below would turn " +
           "it into a shorter string that looks like a resource header");
  }

  /* THE CORB GATE, called by lib/safe-fetch.js at its `opts.as === "script"` gate. `sameOrigin` is this zone's
     own comparison of the browser-stated principal with the response's origin. `body` is Fetch §2.2.5's byte
     sequence as the chokepoint read it. */
  self.browserProcessCorb = async function browserProcessCorb(contentType, xContentTypeOptions, sameOrigin,
                                                              body) {
    checkBody("CORB", body);
    var bp = await browserProcessOnce();
    return await bp.sniffer.checkCorb(contentType, xContentTypeOptions, sameOrigin,
                                      body.slice(0, RESOURCE_HEADER_MAX));
  };

  /* THE CLASSIFICATION GATE, called by lib/response-decode.js for every captured HTTP response. It answers
     what a body is FOR — an asset with no schema in it, or API data to learn from — which is a question about
     the resource's ACTUAL type and therefore a question about its bytes. That is why it is answered there and
     not in the offscreen's own JS: the user's ruling is that type checking is safeFetch's job and the only
     source of sniffing, and `classifyResponseAsset` in lib/discovery.js was a second source of it — a
     hand-rolled magic-byte table beside a standard, plus SVG/CSS/WebVTT/HLS/DASH sniffs no standard has. All
     three of its functions are deleted; the algorithm is browser_process/network/resource_kind.c.
     `opaque` is Fetch §2.2.6: the response is an opaque filtered response, so its body is null and its header
     list is empty by construction. Only the zone holding the Response can state that, which is why it crosses
     as a fact rather than being inferred from an empty body on the far side. */
  self.browserProcessClassify = async function browserProcessClassify(contentType, xContentTypeOptions, opaque,
                                                                      body) {
    checkBody("classify", body);
    var bp = await browserProcessOnce();
    return await bp.sniffer.classifyResource(contentType, xContentTypeOptions, opaque,
                                             body.slice(0, RESOURCE_HEADER_MAX));
  };

  /* THE BOUNDARY, EXERCISED. SECURITY.md: "A HOST THAT CANNOT PROVISION A SECOND INSTANCE HAS NOT TESTED THE
     TRANSPORT, AND EVERY CROSS-INSTANCE MECHANISM ABOVE IS THEN A DESIGN THAT HAS NEVER RUN." This is the
     caller that makes that sentence false for this boundary, and it is driven from OUTSIDE — `harness offscreen
     "return await self.browserProcessProbe()"` — rather than from a self-test, because a mechanism whose only
     caller is its own test is exactly what that paragraph is about.
     EVERY CASE IS A MISLABEL, and that is what makes it a probe rather than a ping. A body whose declared type
     and true type agree is decided by the header alone, so it proves the worker replied and nothing else; §7
     and CORB exist for the case where the two DISAGREE, so each row below serves one type and sends another's
     bytes, and the answer names what the resource actually is.
     IT GOES THROUGH THE SHIPPED ENTRY, `self.browserProcessCorb`, so what it measures is the path safeFetch
     takes. It does NOT tear the worker down afterwards: this process is a singleton the chokepoint holds, and
     a probe that terminated it would leave the next fetch to provision a second one.
     AND IT REPORTS WHAT EACH SIDE HOLDS OF THE OTHER. `handleKeys` is the literal key list of the object this
     file keeps per browser process: a `Module` or a `HEAPU8` in it would be this zone reaching into the other
     program's linear memory, which is precisely what two wasm-ld links out of one object set gave and what a
     Worker cannot give. `mojo` and `child` are the two halves of the IPC surface — which interfaces each
     process holds a Remote for, which it implements, and how many pipe endpoints are open — because a transport
     whose only evidence is that a message arrived is one whose shape nobody can check from outside. */
  self.browserProcessProbe = async function browserProcessProbe() {
    var enc = new TextEncoder();
    var HTML = enc.encode("<!doctype html><html><body><h1>login</h1></body></html>");
    var JS = enc.encode("(function(){window.__chunk=1;})();\n");
    var JSON_BODY = enc.encode('{"user":{"id":42},"token":"abc"}');
    var PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13]);
    var WOFF2 = new Uint8Array([0x77, 0x4f, 0x46, 0x32, 0, 1, 0, 0, 0, 0, 0, 0]);
    var CASES = [
      { name: "javascript served as text/plain", ct: "text/plain", nosniff: false, same: false, body: JS,
        want: { allow: true, computed: "text/plain", reason: "allowed" } },
      { name: "html served as application/javascript", ct: "application/javascript", nosniff: false,
        same: false, body: HTML,
        want: { allow: false, computed: "application/javascript", reason: "sniffed-html" } },
      { name: "json served as application/javascript", ct: "application/javascript", nosniff: false,
        same: false, body: JSON_BODY,
        want: { allow: false, computed: "application/javascript", reason: "sniffed-json" } },
      { name: "html served as text/html, cross-origin", ct: "text/html", nosniff: false, same: false,
        body: HTML, want: { allow: false, computed: "text/html", reason: "protected-type" } },
      { name: "javascript served as text/plain under nosniff", ct: "text/plain", nosniff: true, same: false,
        body: JS, want: { allow: false, computed: "text/plain", reason: "nosniff-not-js" } },
      { name: "a png served as text/plain", ct: "text/plain", nosniff: false, same: false, body: PNG,
        want: { allow: true, computed: "application/octet-stream", reason: "allowed" } },
      { name: "html with no Content-Type at all", ct: null, nosniff: false, same: false, body: HTML,
        want: { allow: false, computed: "text/html", reason: "protected-type" } },
      /* THE SAME-ORIGIN ARM, which is the one place the fact this zone keeps for itself changes the answer.
         The bytes and the declared type are identical to the row three above; only the browser-stated
         principal comparison differs, so a `sameOrigin` that failed to cross would show up here and nowhere
         else. CORB protects across origins and nowhere else — the page's own data is its to read — and the
         one thing still refused is that data reaching a CODE loader, which could not have executed anyway. */
      { name: "html served as text/html, SAME-origin", ct: "text/html", nosniff: false, same: true,
        body: HTML, want: { allow: false, computed: "text/html", reason: "same-origin" } },
      { name: "javascript served honestly, SAME-origin", ct: "text/javascript", nosniff: false, same: true,
        body: JS, want: { allow: true, computed: "text/javascript", reason: "same-origin" } },
    ];
    /* THE CLASSIFICATION ROWS, on the same rule and for the same reason: every one is a case where the answer
       is NOT the declared type, because a body whose server told the truth is decided by its header alone and
       would prove only that the worker replied. What each row is FOR:
         - A JS BUNDLE SERVED AS `application/octet-stream` must not become a boring asset. §7 answers
           `application/octet-stream` (it never upgrades a resource INTO a scriptable type — §7.2's note is
           that the refusal is the point), so nothing here can call it a script, and the rule that matters is
           that it falls to `structured` and is LEARNED. The load path is separate and unaffected: CORB allows
           the same body, so a lazy chunk served this way is still fetched and still executed, which is the
           headline surface CLAUDE.md §Attacker-sources names.
         - A PNG SERVED AS `application/json` is the row that fails if §6.1's table stopped being consulted:
           §7 hands back the declared type unchanged, so only the confirmation sniff can see it.
         - A JSON API RESPONSE SERVED AS `text/plain` is the row that fails if a confirmation sniff became
           over-eager. `text/plain` is one of §5.1's four Apache-bug values, so §7 re-derives from the bytes and
           still reaches `text/plain`; nothing in §6 or §7.1 matches JSON; it must be learned.
         - A GENUINE STATIC ASSET (a WOFF2 under `font/woff2`) is decided by §4.6's font group, which is the
           arm that catches every honestly-served asset and the one the deleted JS's hand-rolled font magic was
           standing in for.
         - The remaining rows are the JavaScript group's two sides (a script, and API data shipped under a
           JavaScript MIME type, told apart by json_sniff.c), an HTML error page returned where JSON was
           declared, and Fetch §2.2.6's opaque response, whose body no amount of sniffing can reach. */
    var CLASSIFY_CASES = [
      { name: "a javascript bundle served as application/octet-stream", ct: "application/octet-stream",
        xcto: null, opaque: false, body: JS, want: { asset: false, reason: "structured" } },
      { name: "a png served as application/json", ct: "application/json", xcto: null, opaque: false,
        body: PNG, want: { asset: true, reason: "sniffed-image/png" } },
      { name: "a json api response served as text/plain", ct: "text/plain", xcto: null, opaque: false,
        body: JSON_BODY, want: { asset: false, reason: "structured" } },
      { name: "a woff2 served as font/woff2", ct: "font/woff2", xcto: null, opaque: false, body: WOFF2,
        want: { asset: true, reason: "font" } },
      { name: "a javascript chunk served honestly", ct: "text/javascript", xcto: null, opaque: false,
        body: JS, want: { asset: true, reason: "script" } },
      { name: "json served as text/javascript", ct: "text/javascript", xcto: null, opaque: false,
        body: JSON_BODY, want: { asset: false, reason: "javascript-mime-json-body" } },
      { name: "an html error page served as application/json", ct: "application/json", xcto: null,
        opaque: false, body: HTML, want: { asset: true, reason: "sniffed-text/html" } },
      { name: "html served as text/html", ct: "text/html", xcto: null, opaque: false, body: HTML,
        want: { asset: true, reason: "markup" } },
      /* THE OPAQUE ARM. The bytes and the declared type say API data; the fact that only this zone holds says
         the body was never readable. It is the one row whose answer cannot come from the bytes at all, so a
         flag that failed to cross would show up here and nowhere else. */
      { name: "an opaque no-cors response", ct: null, xcto: null, opaque: true, body: new Uint8Array(0),
        want: { asset: true, reason: "opaque-filtered-response" } },
      /* NOSNIFF, WHOSE FIRST VALUE IS NOT `nosniff`. Fetch splits the header and matches its FIRST value, so
         this response does NOT set the flag — and the substring test this boundary used to carry in JS said it
         did. The difference is visible because §7 step 2 runs §7.1 WITH its scriptable table when the flag is
         clear and WITHOUT it when set: clear, this computes `text/html` and the rule is `markup`; set, §7 would
         reach `text/plain` and the document would be caught one rule later as `sniffed-text/html`. Same
         verdict, different rule — which is exactly what a reason field is for. */
      { name: "html under an unknown type and X-Content-Type-Options: foo, nosniff", ct: "application/unknown",
        xcto: "foo, nosniff", opaque: false, body: HTML, want: { asset: true, reason: "markup" } },
    ];

    var rec = { provisioned: false, agree: false, handleKeys: [], cases: [], classify: [] };
    var bp = await browserProcessOnce();
    rec.provisioned = true;
    rec.handleKeys = Object.keys(bp).sort();
    var all = true;
    for (var i = 0; i < CASES.length; i++) {
      var c = CASES[i];
      var v = await self.browserProcessCorb(c.ct, c.nosniff ? "nosniff" : null, c.same, c.body);
      var ok = v.allow === c.want.allow && v.computed === c.want.computed && v.reason === c.want.reason;
      if (!ok) all = false;
      rec.cases.push({ name: c.name, agree: ok, allow: v.allow, computed: v.computed, reason: v.reason,
                       want: c.want });
    }
    for (var j = 0; j < CLASSIFY_CASES.length; j++) {
      var k = CLASSIFY_CASES[j];
      var w = await self.browserProcessClassify(k.ct, k.xcto, k.opaque, k.body);
      var kok = w.asset === k.want.asset && w.reason === k.want.reason;
      if (!kok) all = false;
      rec.classify.push({ name: k.name, agree: kok, asset: w.asset, reason: w.reason, want: k.want });
    }
    rec.agree = all;
    /* THE IPC SURFACE, FROM BOTH ENDS. This document's Remotes and Receivers are read locally; the browser
       process's are ASKED FOR over a pipe, which is a fourth interface answering a question about itself. */
    rec.mojo = self.mojo.stats();
    rec.child = await bp.childProcess.getMojoStats();
    rec.lines = bp.lines;
    return rec;
  };

  console.debug("[browser-process] ready (self.browserProcess + self.browserProcessCorb + " +
                "self.browserProcessClassify + self.browserProcessProbe installed)");
})();
