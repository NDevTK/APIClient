/* browser-process-host.js — THE TRUSTED SIDE OF THE BROWSER PROCESS. `renderer-host.js` is the same object for
 * the untrusted renderer, and this is its counterpart: the offscreen — the one fully-trusted zone
 * (SECURITY.md) — provisions the network service's Worker and relays one operation to it.
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
 * WHAT STAYS ON THIS SIDE, DELIBERATELY. The ORIGIN COMPARISON. SECURITY.md makes the same-origin principal the
 * requesting frame's `MessageSender.origin` — browser-set, opaque-unique, "NEVER re-parsed from a URL" — so
 * what crosses to the browser process is a browser-stated BOOLEAN, exactly as a delivered cross-document
 * message carries an origin this zone stamped rather than one the far side minted. The far side has no URL, no
 * principal and no way to invent either.
 *
 * ONE PROCESS FOR THE WHOLE OFFSCREEN, not one per document. A renderer is per agent cluster because it IS that
 * cluster's heap; a network service holds no per-document state at all — every entry it serves is a pure
 * function of its arguments — so a second one would be a second copy of a table, and the thing that keeps two
 * pages' fetches from contaminating each other is that the principal travels PER CALL, which it does.
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
     instantiating awaits the same one rather than starting a second worker. There is no timeout on the hello,
     for renderer-host.js's reason — a wall clock here would report a loaded machine as a broken transport
     (CLAUDE.md §Testing), and the failure it would be covering (a policy that refused the worker) is already
     reported by Chrome, by name, on this document's console.
     A REJECTION IS STICKY, AND THAT IS THE FAIL-CLOSED DIRECTION. A browser process that did not load leaves a
     rejected promise here forever, so every later script fetch rejects out of safeFetch's CORB gate with the
     same reason rather than retrying into a state where nothing judges the body. Re-provisioning on demand
     would turn "the network service is missing" into an intermittent condition, which is the one shape a
     security gate must never have. */
  var _bpP = null;
  function browserProcessOnce() {
    if (_bpP) return _bpP;
    _bpP = new Promise(function (resolve, reject) {
      /* A MODULE WORKER, because the program it loads is an ES module: `engine/build.mjs` links the browser
         process with -sMODULARIZE -sEXPORT_ES6, whose default export is the factory. Both this script and that
         module are same-origin extension resources, so `script-src 'self'` allows them and the manifest's
         `require-corp` has nothing to refuse — COEP constrains CROSS-origin subresources, and the offscreen
         document's own policy carries `worker-src 'self'`. */
      var w = new Worker(WORKER_URL, { type: "module" });
      var bp = { worker: w, lines: [], _next: 1, _await: new Map(), _dead: false };
      /* A WORKER THAT FAILED TO LOAD ITS SCRIPT REPORTS HERE AND NOWHERE ELSE. Without this the boot promise
         would never settle and every caller would park forever on an answer that is not coming — the one
         outcome with no symptom anywhere. */
      w.onerror = function (e) {
        bp._dead = true;
        reject(new Error("the browser process's worker did not load: " + ((e && e.message) || "(no message)")));
      };
      w.onmessage = function (ev) {
        var m = ev.data;
        if (m && m.hello === 1) {
          DCHECK(m.v === 1 && typeof m.ok === "boolean" && Array.isArray(m.out),
                 "the browser process's hello is not this transport's — it is a version, whether the module " +
                 "instantiated, and the output drained with it");
          for (var i = 0; i < m.out.length; i++) bp.lines.push(String(m.out[i]));
          if (m.ok) { resolve(bp); return; }
          bp._dead = true;
          reject(new Error("the browser process did not instantiate its module: " + m.err));
          return;
        }
        onReply(bp, m);
      };
    });
    return _bpP;
  }

  /* A REPLY. Every one carries the module's stdout/stderr since the last one, for renderer-host.js's reason: a
     crash's ROOT line is the last `@WHY` printed before abort(), and lines left in the worker would be a
     crashed browser process taking its own cause with it. */
  function onReply(bp, m) {
    DCHECK(m && m.v === 1 && typeof m.id === "number" && Array.isArray(m.out),
           "the browser process answered with something that is not this transport's reply — a reply is a " +
           "version, the call id it answers, and the output drained with it");
    for (var i = 0; i < m.out.length; i++) bp.lines.push(String(m.out[i]));
    var wt = bp._await.get(m.id);
    DCHECK(wt !== undefined,
           "the browser process answered a call id this zone never made — the id is the whole routing table " +
           "for an answer, so an unknown one means the worker echoed something other than what it was handed " +
           "and the call that is actually outstanding will never be resolved");
    if (!wt) return;
    bp._await.delete(m.id);
    if (m.ok) { wt.resolve(m.ret); return; }
    /* A FAILED CALL IS A REJECTION, and the process is dead after one: what failed is its linear memory. */
    bp._dead = true;
    var e = new Error(m.err);
    e.browserProcessLines = bp.lines.slice(-8);
    wt.reject(e);
  }

  function bpCall(bp, rec) {
    DCHECK(!bp._dead,
           "a call was made into a browser process that has already aborted — its linear memory is the thing " +
           "that failed, so this call would be a second crash reported as a first");
    var id = bp._next++;
    var p = new Promise(function (res, rej) { bp._await.set(id, { resolve: res, reject: rej }); });
    rec.v = 1;
    rec.id = id;
    bp.worker.postMessage(rec);
    return p;
  }

  /* THE HEADER FACTS BOTH SHIPPED ENTRIES TAKE, asserted where they are handed over. Each is `string | null`
     and null is a POSITIVE statement that the response carried no such header — §5.1's "the supplied MIME type
     is undefined" for the first, Fetch's "values is null" for the second — never an empty string standing in
     for one, which is a value a server can really send and which means something else.
     THE SECOND ONE IS THE HEADER AND NOT THE FLAG, deliberately. `noSniff` used to arrive here already decided,
     which meant Fetch's determine-nosniff lived in lib/safe-fetch.js as an `indexOf("nosniff")` over a
     lowercased value — a substring test where the standard splits the header and matches its FIRST value, so
     `foo, nosniff` set the flag and should not have. With a second entry needing the same fact that would have
     become two copies of one wrong algorithm, so the derivation is now C beside the algorithms it feeds
     (browser_process/network/nosniff.c) and what this zone hands over is what this zone did: it read a header. */
  function checkHeaderFacts(what, contentType, xContentTypeOptions, body) {
    DCHECK(contentType === null || typeof contentType === "string",
           "the " + what + " gate was handed a Content-Type that is neither a string nor null — an absent " +
           "header is §5.1's undefined supplied type and says so with null, which is a different input from " +
           "\"\"");
    DCHECK(xContentTypeOptions === null || typeof xContentTypeOptions === "string",
           "the " + what + " gate was handed an X-Content-Type-Options that is neither a string nor null — " +
           "this boundary carries the VALUE so the standard's own split-and-match runs in one place, and an " +
           "absent header says so with null exactly as an absent Content-Type does");
    DCHECK(body instanceof Uint8Array,
           "the " + what + " gate was handed a body that is not a byte sequence — the decision reads the " +
           "body, and a string here is a caller having run a decode it does not own");
  }

  /* THE CORB GATE, called by lib/safe-fetch.js at its `opts.as === "script"` gate. `sameOrigin` is this zone's
     own comparison of the browser-stated principal with the response's origin. `body` is Fetch §2.2.5's byte
     sequence as the chokepoint read it. */
  self.browserProcessCorb = async function browserProcessCorb(contentType, xContentTypeOptions, sameOrigin,
                                                              body) {
    checkHeaderFacts("CORB", contentType, xContentTypeOptions, body);
    DCHECK(typeof sameOrigin === "boolean",
           "the CORB gate was handed a principal comparison that is not a boolean — it is a comparison the " +
           "caller MADE, so it may not arrive as undefined");
    var bp = await browserProcessOnce();
    var v = await bpCall(bp, { op: "corb", contentType: contentType,
                               xContentTypeOptions: xContentTypeOptions, sameOrigin: sameOrigin,
                               header: body.slice(0, RESOURCE_HEADER_MAX) });
    DCHECK(v && typeof v.allow === "boolean" && typeof v.computed === "string" && typeof v.reason === "string",
           "the browser process answered a CORB verdict missing one of its three fields — the decision, §7's " +
           "computed essence and the rule that decided are written together by corb.c, so a missing one is a " +
           "producer that stopped writing it and not a value with a default");
    return v;
  };

  /* THE CLASSIFICATION GATE, called by lib/response-decode.js for every captured HTTP response. It answers
     what a body is FOR — an asset with no schema in it, or API data to learn from — which is a question about
     the resource's ACTUAL type and therefore a question about its bytes. That is why it is here and not in the
     offscreen's own JS: the user's ruling is that type checking is safeFetch's job and the only source of
     sniffing, and `classifyResponseAsset` in lib/discovery.js was a second source of it — a hand-rolled
     magic-byte table beside a standard, plus SVG/CSS/WebVTT/HLS/DASH sniffs no standard has. All three of its
     functions are deleted; the algorithm is browser_process/network/resource_kind.c.
     `opaque` is Fetch §2.2.6: the response is an opaque filtered response, so its body is null and its header
     list is empty by construction. Only the zone holding the Response can state that, which is why it crosses
     as a fact rather than being inferred from an empty body on the far side. */
  self.browserProcessClassify = async function browserProcessClassify(contentType, xContentTypeOptions, opaque,
                                                                      body) {
    checkHeaderFacts("classify", contentType, xContentTypeOptions, body);
    DCHECK(typeof opaque === "boolean",
           "the classify gate was handed a filtered-response fact that is not a boolean — Fetch §2.2.6 is " +
           "either true of this response or it is not, and an undefined here would be read as `false` and " +
           "would learn an unreadable body as an endpoint with no fields");
    var bp = await browserProcessOnce();
    var v = await bpCall(bp, { op: "classify", contentType: contentType,
                               xContentTypeOptions: xContentTypeOptions, opaque: opaque,
                               header: body.slice(0, RESOURCE_HEADER_MAX) });
    DCHECK(v && typeof v.asset === "boolean" && typeof v.reason === "string",
           "the browser process answered a classification missing one of its two fields — the verdict and the " +
           "rule that decided are written together by resource_kind.c, so a missing one is a producer that " +
           "stopped writing it and not a value with a default");
    return v;
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
     AND IT REPORTS WHAT THIS ZONE HOLDS OF THE OTHER PROGRAM, because that is the claim the deleted attempt
     could not have made and the one a reader cannot check by looking at a build flag. `handleKeys` is the
     literal key list of the object this file keeps per browser process: a `Module` or a `HEAPU8` in it would be
     this zone reaching into the other program's linear memory, which is precisely what two wasm-ld links out
     of one object set gave and what a Worker cannot give. */
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
    rec.lines = bp.lines;
    return rec;
  };

  console.debug("[browser-process] ready (self.browserProcessCorb + self.browserProcessClassify + self.browserProcessProbe installed)");
})();
