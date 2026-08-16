/* browser-process.js — THE BROWSER PROCESS. A dedicated Worker of the offscreen document holding the network
 * service's own WASM module, reached only by postMessage.
 *
 * WHY A WORKER, AND WHY NOT THE SHAPE renderer.html USES. Those two boundaries face opposite ways.
 * `extension/renderer.html` is a sandboxed frame with a UNIQUE OPAQUE ORIGIN because the thing inside it —
 * QuickJS running an attacker's bundle — is UNTRUSTED and must be confined; Site Isolation is then free to put
 * it in its own OS-sandboxed process. This program is the thing that confinement PROTECTS: SECURITY.md's
 * network chokepoint, the algorithm CORB gates on. Giving it an opaque origin would confine the wrong side and
 * would cost it exactly what a network service needs — an opaque origin is same-origin with nothing, so
 * `connect-src`, credentialed fetch and same-origin reads of the extension's own resources all go with it. A
 * Worker of the extension origin keeps every one of those and still gives the property the whole exercise is
 * about: its OWN realm, its OWN module instance and its OWN thread, with no `HEAPU8` exported to anybody. That
 * last clause is the entire difference from the deleted `browser_process/` link, where two Modules sat in the
 * offscreen's own realm with the host holding a view over each — one address space, two file names.
 *
 * IT READS ITS OWN PROGRAM, which the renderer cannot. `browser-process.js` and `lib/bproc/bproc.mjs` are
 * same-origin extension resources here, so the manifest's `require-corp` has nothing to refuse (COEP constrains
 * CROSS-origin subresources) and `script-src 'self'` allows both. The offscreen therefore hands this worker
 * nothing at all — no blobs, no wasm bytes — which is the shape a trusted counterpart should have and is why
 * the boot record renderer.html needs does not exist here.
 *
 * THE TRANSPORT IS renderer-host.js's VOCABULARY, and the two places it differs are stated rather than left to
 * be noticed. A record carries `v`, an `id` and an `op`; a reply carries `v`, the `id` it answers, `ok`, and
 * the module's drained output. Then:
 *
 * NO MessageChannel. That exists in the renderer because a window's `message` event is a shared target — every
 * frame's hello lands on the same listener and `event.source` is the only thing that separates them — whereas a
 * Worker handle IS point-to-point by construction. A port here would be ceremony answering a question this
 * boundary does not have. The `hello` stays, because it answers a different one: a worker script that LOADED is
 * not a module that INSTANTIATED, and the trusted side must not send a call into a program that does not exist.
 *
 * NAMED OPERATIONS, NOT `{op:"call", fn, ret, args, bodies}`. The renderer's record is a generic `M.ccall`
 * relay because bridge.js drives nineteen ABI entries of varying shapes and the transport must carry all of
 * them without knowing any; here the operations are few and their arguments are TYPED FACTS — a nullable
 * `Content-Type` value, a nullable `X-Content-Type-Options` value, a browser-stated boolean, a byte sequence.
 * A generic `args:[{t:"cstr",v:…}]` list would flatten exactly the distinctions this boundary has to assert
 * (null is §5.1's undefined supplied type and not `""`; `sameOrigin` is a browser statement and not a string)
 * and would put every DCHECK below out of reach of the thing it is about. That paragraph said "here there is
 * ONE operation" and ended "a second one arrives as a second `op` with its own named fields and its own
 * asserts", which is exactly what `classify` is: the same three facts about a response, asked a different
 * question — what a body IS FOR rather than whether a code loader may have it.
 *
 * BOTH TAKE THE HEADER VALUE AND NEITHER TAKES THE FLAG. `noSniff` used to cross as a boolean, which put
 * Fetch's determine-nosniff in `lib/safe-fetch.js` as `_cto.toLowerCase().indexOf("nosniff") >= 0` — a
 * substring test where the standard splits the header and matches its FIRST value. A second op needing the
 * same fact would have been a second copy of that, so the derivation moved to where the other algorithms are
 * (browser_process/network/nosniff.c) and what crosses is the header this zone READ.
 */
import "./check.js";
import createBrowserProcess from "./lib/bproc/bproc.mjs";

let M = null;
let out = [];          /* the module's stdout/stderr since the last reply; drained into every reply */

/* THE MODULE'S OUTPUT BELONGS TO THE TRUSTED ZONE, so it rides every reply rather than staying here. A CHECK or
   DCHECK in this program prints `@E`/`@WHY` and then aborts, and the line printed immediately before the abort
   is the only thing that says what broke — left in the worker, a crashed browser process would take its own
   cause with it. */
function drain() { const o = out; out = []; return o; }

/* §5.2's RESOURCE HEADER is the first 1445 bytes, and the TRUNCATION IS THE SENDER'S — browser-process-host.js
   slices there, because the point of slicing is to keep a multi-megabyte bundle out of the structured clone,
   and a clone that already happened cannot be undone here. So this side ASSERTS the bound rather than
   re-applying it: a longer header means the sender stopped truncating, and silently clamping would turn that
   into a message nobody ever reads. */
const RESOURCE_HEADER_MAX = 1445;

/* THE BYTES, PLACED AND NEVER ENCODED — the same rule renderer.html states at its own `cbytes`. A resource
   header is a byte sequence; running it through `stringToUTF8` would answer §7 a different question about a
   different resource, and every non-ASCII signature in §6's tables is exactly what would be destroyed. */
function withHeader(bytes, fn) {
  DCHECK(bytes.length <= RESOURCE_HEADER_MAX,
         "a resource header longer than §5.2's 1445 bytes reached the browser process — the sender truncates " +
         "so the clone stays small, and one that did not has already copied a whole bundle across a thread to " +
         "answer a question defined over its first bytes");
  const p = M._malloc(bytes.length + 1);
  CHECK(p !== 0, "OOM placing a resource header in the browser process's linear memory — the CORB decision " +
                 "would otherwise be taken over a body this program never saw");
  M.HEAPU8.set(bytes, p);
  try { return fn(p, bytes.length); } finally { M._free(p); }
}

/* THE HEADER FACTS EVERY OPERATION TAKES, asserted ONCE because they are one record's fields and not one
   operation's arguments. Each is `string | null`, and null is a POSITIVE statement that the response carried
   no such header — §5.1's "the supplied MIME type is undefined" for the first, Fetch's "values is null" for
   the second — never an empty string standing in for one, which is a value a server can actually send. */
function checkHeaderFacts(m, what) {
  DCHECK(m.contentType === null || typeof m.contentType === "string",
         "a " + what + " call arrived with a Content-Type that is neither a string nor null — §5.1 " +
         "distinguishes an ABSENT supplied type from a present one, so an absent header says so with null " +
         "and never with \"\"");
  DCHECK(m.xContentTypeOptions === null || typeof m.xContentTypeOptions === "string",
         "a " + what + " call arrived with an X-Content-Type-Options that is neither a string nor null — " +
         "this boundary carries the header VALUE so that Fetch's determine-nosniff runs beside the other " +
         "algorithms, and an absent header is null exactly as an absent Content-Type is");
  DCHECK(m.header instanceof Uint8Array,
         "a " + what + " call arrived without its resource header as BYTES — the whole reason this decision " +
         "is made here is that it reads the body, and anything but a byte sequence is a zone that ran a decode");
}

/* ONE `JSON.parse` OF ONE DOCUMENT per answer, which is the discipline CLAUDE.md §Architecture blesses for
   `@RESULT`: the record is built where the decision is taken, so no consumer re-derives a field from another. */

/* CORB. `sameOrigin` is the trusted zone's comparison of the browser-stated page origin with the response's,
   which SECURITY.md keeps on that side and this program therefore never re-derives. */
function opCorb(m) {
  checkHeaderFacts(m, "CORB");
  DCHECK(typeof m.sameOrigin === "boolean",
         "a CORB call arrived without its browser-stated principal comparison — `sameOrigin` is a fact the " +
         "trusted zone MADE, and it has no default this program may invent");
  const json = withHeader(m.header, (p, n) =>
    M.ccall("bp_corb_check", "string", ["string", "string", "number", "number", "number"],
            [m.contentType, m.xContentTypeOptions, m.sameOrigin ? 1 : 0, p, n]));
  const r = JSON.parse(json);
  DCHECK(r && typeof r.allow === "boolean" && typeof r.computed === "string" && typeof r.reason === "string",
         "the browser process's CORB entry answered a record missing one of its three fields — the verdict, " +
         "§7's computed essence and the rule that decided are written together by corb.c and a missing one is " +
         "a producer that stopped writing it rather than a value with a default");
  return r;
}

/* WHAT THE RESOURCE IS FOR — asset or API data. `opaque` is Fetch §2.2.6: the response is an opaque filtered
   response, whose body is null and whose header list is empty, which is a fact about the Response OBJECT and
   therefore one only the zone holding it can state. */
function opClassify(m) {
  checkHeaderFacts(m, "classify");
  DCHECK(typeof m.opaque === "boolean",
         "a classify call arrived without Fetch §2.2.6's filtered-response fact — `opaque` says the body " +
         "could not be read at all, which no amount of looking at the bytes this program was given can tell " +
         "it apart from a body that was read and found empty");
  const json = withHeader(m.header, (p, n) =>
    M.ccall("bp_classify", "string", ["string", "string", "number", "number", "number"],
            [m.contentType, m.xContentTypeOptions, m.opaque ? 1 : 0, p, n]));
  const r = JSON.parse(json);
  DCHECK(r && typeof r.asset === "boolean" && typeof r.reason === "string",
         "the browser process's classify entry answered a record missing one of its two fields — the verdict " +
         "and the rule that decided are written together by resource_kind.c, so a missing one is a producer " +
         "that stopped writing it rather than a value with a default");
  return r;
}

function serve(m) {
  let rec;
  try {
    DCHECK(m && m.v === 1 && typeof m.id === "number" && typeof m.op === "string",
           "a record on the browser-process port is not this transport's — it carries a version, a call id " +
           "and an op");
    /* ROUTING, and the distinction CLAUDE.md §C-stack draws is worth naming because the shape looks alike:
       this picks WHICH ONE implementation runs and CRASHES on a name it cannot answer for, and there is no
       second implementation anywhere for it to fall back to. Deleting `opClassify` would not leave this switch
       still needed — it would leave it with one arm — which is the test that says routing rather than gate. */
    if (m.op === "corb") {
      rec = { v: 1, id: m.id, ok: true, ret: opCorb(m), out: drain() };
    } else if (m.op === "classify") {
      rec = { v: 1, id: m.id, ok: true, ret: opClassify(m), out: drain() };
    } else {
      DFAIL("the offscreen asked the browser process for an op it does not serve: `" + m.op + "`");
      /* RELEASE PATH UNDER THE ASSERT, and it REACHES the caller. Saying nothing would park the offscreen on
         an answer that is never coming, which is the one outcome with no symptom anywhere. */
      rec = { v: 1, id: m.id, ok: false, err: "unserved browser-process op: " + m.op, out: drain() };
    }
  } catch (e) {
    /* EVERY FAILURE IN HERE IS THIS PROGRAM'S — a WASM abort (its own CHECK/DCHECK reaching abort()), or one of
       the asserts above. Both cross as a REJECTION on the far side, with the drained lines, because the `@WHY`
       printed immediately before an abort is the only thing that says what actually broke. */
    rec = { v: 1, id: m && m.id, ok: false, err: String((e && e.stack) || e), out: drain() };
  }
  self.postMessage(rec);
}

/* BOOT. The module is instantiated BEFORE the hello is posted and before any listener is installed, so the
   trusted side cannot send a call into a program that does not exist. A failure travels in the hello rather
   than being swallowed: a browser process that did not instantiate must not look like one that is merely slow,
   because there is no timeout on the far side (a wall clock there would report a loaded machine as a broken
   transport — CLAUDE.md §Testing). */
createBrowserProcess({
  print: (s) => { out.push(String(s)); },
  printErr: (s) => { out.push(String(s)); },
  noInitialRun: true,
}).then(
  (mod) => {
    M = mod;
    self.onmessage = (e) => { serve(e.data); };
    self.postMessage({ v: 1, hello: 1, ok: true, out: drain() });
  },
  (err) => {
    self.postMessage({ v: 1, hello: 1, ok: false, err: String((err && err.stack) || err), out: drain() });
  });
