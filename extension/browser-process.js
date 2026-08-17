/* browser-process.js — THE BROWSER PROCESS. A dedicated Worker of the offscreen document holding the RENDERER
 * REGISTRY's own WASM module, reached only over a Mojo primordial pipe.
 *
 * WHY A WORKER, AND WHY NOT THE SHAPE renderer.html USES. Those two boundaries face opposite ways.
 * `extension/renderer.html` is a sandboxed frame with a UNIQUE OPAQUE ORIGIN because the thing inside it —
 * QuickJS running an attacker's bundle — is UNTRUSTED and must be confined; Site Isolation is then free to put
 * it in its own OS-sandboxed process. This program is the thing that confinement PROTECTS: the authority for
 * SECURITY.md's one-instance-per-agent-cluster rule. Giving it an opaque origin would confine the wrong side
 * and would cost it what a trusted extension zone needs — an opaque origin is same-origin with nothing, so
 * `connect-src` and same-origin reads of the extension's own resources go with it. A
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
 * THE TRANSPORT IS MOJO, and the ad-hoc `{v:1,id,op}` vocabulary this file used to carry is DELETED in
 * the same diff rather than kept beside it. That record was a hand-written routing table (`id`), a hand-written
 * capability list (`op`), a hand-written error convention (`ok`/`err`) and a hand-written output drain — and
 * the renderer boundary held a second, differently-spelled copy of every one of them. Two transports for one
 * problem is the dual system CLAUDE.md forbids: each capability a boundary gains is a new `op` written into two
 * switch statements, and each omission is silent. Mojo is what a browser already uses here, so extension/mojo.js
 * is the machinery, extension/mojom.js is the IDL, and what is left in this file is three implementations and
 * no transport at all. The paragraph this replaces argued for named operations over a generic relay and was
 * right about the disease; a mojom interface is that argument's conclusion, with the types declared once
 * instead of asserted twice.
 *
 * WHAT THIS PROCESS OWNS. The RENDERER REGISTRY and nothing else. It held a second thing — WHATWG MIME
 * Sniffing §7 and Chromium's CORB analyzer, behind `network.mojom.ContentSniffer` — and that is deleted along
 * with the C it had been transliterated out of working JavaScript into. CLAUDE.md §Architecture: "TYPE
 * SNIFFING STAYS IN JAVASCRIPT, in `safeFetch`, where SECURITY.md puts it." The trusted zone reads the bytes,
 * decides once, and STAMPS what it decided onto the reply record it hands the renderer.
 *
 * WHAT IT CANNOT DO, AND WHAT FOLLOWS. A dedicated Worker's global is `DedicatedWorkerGlobalScope`: no
 * `document`, no DOM, no `createElement`. So this process cannot materialize a renderer frame — exactly as
 * Chromium's browser process cannot fork a renderer by itself and asks the ZYGOTE to. The offscreen document is
 * that zygote (`content.mojom.Zygote`, implemented in renderer-host.js) and it holds no admission rule of its
 * own: the DECISION, the registry, the routing id, and the refusal of a second renderer for one agent cluster
 * are all here.
 */
import "./check.js";
import "./mojo.js";
import "./mojom.js";
import createBrowserProcess from "./lib/bproc/bproc.mjs";

let M = null;
let out = [];          /* the module's stdout/stderr since the last record; drained into every record posted */

/* THE MODULE'S OUTPUT BELONGS TO THE TRUSTED ZONE, so it rides every record this process posts rather than
   staying here. A CHECK or DCHECK in this program prints `@E`/`@WHY` and then aborts, and the line printed
   immediately before the abort is the only thing that says what broke — left in the worker, a crashed browser
   process would take its own cause with it. EACH LINE NOW CARRIES THE STREAM IT CAME FROM, `[fd, text]`, which
   is renderer.html's rule and is worth having here for its reason: the two streams stay in ONE order (the last
   @WHY before an abort is what names the cause) while stderr ALONE can be teed live to the trusted zone's
   console, which is where a native diagnostic is capturable while the run is happening. */
function drain() { const o = out; out = []; return o; }

/* THE BYTES, PLACED AND NEVER ENCODED — the same rule renderer.html states at its own `cbytes`. Everything
   this file hands the program is a byte sequence with a LENGTH, and no entry takes a NUL-terminated string.
   (It served two callers while §7's resource header also crossed here; that entry is deleted with the sniffing
   C, and the rule is unchanged by having one caller — it is a property of the boundary, not of the count.) */
function place(what, bytes, fn) {
  const p = M._malloc(bytes.length + 1);
  CHECK(p !== 0, "OOM placing " + what + " in the browser process's linear memory — the decision it is for " +
                 "would otherwise be taken over bytes this program never received");
  M.HEAPU8.set(bytes, p);
  try { return fn(p, bytes.length); } finally { M._free(p); }
}

/* THE AGENT CLUSTER KEY IS BYTES, AND THAT IS THE ONE MARSHALLING THIS BOUNDARY CAN GET SILENTLY WRONG.
   `clusterKeyOf` joins the browsing-context group and the origin with a NUL "because neither half can contain
   one", so the key holds an interior NUL — and `ccall`'s `"string"` marshalling ends a C string at the first
   one. Marshalled that way, every origin in one tab would arrive as the same key, and the registry whose
   entire job is to refuse a merged agent cluster would perform one. `TextEncoder` emits that NUL as the byte
   it is. */
const KEY_ENC = new TextEncoder();
function withClusterKey(clusterKey, fn) { return place("an agent cluster key", KEY_ENC.encode(clusterKey), fn); }

/* NOTHING IS RE-ASSERTED IN THIS FILE, and that is the transport rather than a loss of rigour. Every shape a
   parameter has is ONE declaration in mojom.js, where the sentence explaining the rule is the string the
   validator PRINTS, on both sides of the pipe, at the origin of the wrong value; and `JSON.parse` of ONE
   document per answer is the discipline CLAUDE.md blesses for `@RESULT` — the record is built where the
   decision is taken, so no consumer re-derives a field. A field registry.c stopped writing still crashes on
   the line that produced it. */

/* ────────────────────────────────────────────────────────────────────────────────────────────────────────
   THE RENDERER REGISTRY — the browser process's own state, and what makes this program's name a description
   rather than an aspiration. SECURITY.md fixes the unit: "One WASM instance per ORIGIN-KEYED AGENT CLUSTER —
   (browsing-context group, origin)". That rule is held by the process whose job it is to hold it. The
   offscreen's pool ASKS; this DECIDES; the zygote OBEYS.
   AND THE DECIDING IS C. What stood here was a `Map`, a `_nextRoutingId++`, three counters and a duplicate
   check — which is LOGIC, and CLAUDE.md §Architecture leaves this zone a BRIDGE and never that: "Lexbor (DOM)
   + quickjs (JS) in C own ALL semantics… A JS orchestration layer is unwanted context-switching across the
   JS↔WASM boundary; DELETE it." Deciding which renderers exist is computation over a string and an integer,
   so it is `engine/host/browser_process/renderer/registry.c` and nothing of it is kept here as a mirror or a
   fallback. Two consequences are worth naming. The duplicate refusal was a `DCHECK` whose release path
   OVERWROTE the map entry — it is a `CHECK` in C, fatal in every build, because SECURITY.md's rule is a
   production invariant. And the slot is now taken before the fork order STRUCTURALLY rather than by comment: a
   `ccall` cannot suspend, so a second request for one cluster arriving while a fork is outstanding cannot find
   an empty registry.
   WHAT IS IRREDUCIBLE HERE IS ONE LINE — `zygote().forkRenderer(...)`. A dedicated Worker's global is
   `DedicatedWorkerGlobalScope`: no `document`, no `createElement`, no frame. So this process can ORDER a
   renderer materialized and can never materialize one, which is a bridge edge in exactly SECURITY.md's sense —
   the same shape as `safeFetch`, where the capability lives in another zone and the decision does not. What
   crosses it is an integer this program minted and a key it was given; what comes back is a pipe or a reason,
   and both are handed straight back to C.
   ──────────────────────────────────────────────────────────────────────────────────────────────────────── */

/* THE ZYGOTE IS BOUND ON FIRST USE, and that is an ordering requirement rather than laziness: the offscreen
   registers its Zygote implementation when renderer-host.js loads, while this process is provisioned by
   whichever caller needs it first. A bind request nobody can answer is a DFAIL on the far side; binding here
   means the first one cannot precede the registration that answers it. */
let _zygote = null;
function zygote() {
  if (_zygote === null) _zygote = conn.bindInterface("content.mojom.Zygote");
  return _zygote;
}

const RENDERER_HOST = {
  async createRendererForCluster(clusterKey) {
    /* THE DECISION, TAKEN IN C AND SYNCHRONOUSLY. Every refusal this admission has — an empty cluster key, a
       second renderer for a live cluster, a routing id space exhausted — is a `CHECK` in registry.c rather
       than a value returned here, because a returned refusal is a value a caller can ignore and what it would
       be ignoring is two heaps behind one principal. There is nothing left to assert on this side: the key was
       validated by mojom as it crossed the pipe, and everything after it is the registry's own contract. */
    const routingId = withClusterKey(clusterKey, (p, n) =>
      M.ccall("bp_renderer_create", "number", ["number", "number"], [p, n]));
    /* THE ORDER, WHICH IS THE BRIDGE EDGE AND THE ONLY LINE OF THIS METHOD THAT COULD NOT BE C. A dedicated
       Worker has no `document`, so the frame is materialized by the offscreen's zygote on this process's
       order — exactly as Chromium's browser process cannot fork a renderer by itself. The id it is given was
       minted above; this call decides nothing and remembers nothing. */
    const v = await zygote().forkRenderer(routingId, clusterKey);
    DCHECK((v.pipe === null) !== (v.error === null),
           "the zygote answered a fork order with both a pipe and an error, or with neither — a renderer that " +
           "booted has exactly one pipe and one that did not has exactly one reason");
    if (v.error !== null) {
      /* A LAUNCH THAT FAILED FREES ITS CLUSTER IN THE REGISTRY. The zygote has already removed the frame;
         leaving the registration would refuse this agent cluster a renderer forever, with nothing anywhere to
         say why. */
      M.ccall("bp_renderer_launch_failed", null, ["number"], [routingId]);
      return { routingId: routingId, pipe: null, error: v.error };
    }
    M.ccall("bp_renderer_launched", null, ["number"], [routingId]);
    return { routingId: routingId, pipe: v.pipe, error: null };
  },

  /* A RENDERER'S DEATH IS OBSERVED, NOT ORDERED — which is what a real one is: a child process can exit on its
     own and the browser learns of it. The offscreen owns the frame and so is what notices; the registry is
     where the agent cluster is freed, and this is a method of THIS interface so that one pipe orders it
     against the next CreateRendererForCluster for the same cluster, which would otherwise be refused as a
     duplicate of a renderer that no longer exists. A routing id the registry never minted aborts the program
     there rather than being ignored here — it is the one number only that table can produce. */
  rendererTerminated(routingId) {
    M.ccall("bp_renderer_terminated", null, ["number"], [routingId]);
  },

  /* THE TABLE, READ AS ONE RECORD. `JSON.parse` of ONE document per answer is the discipline CLAUDE.md
     blesses for `@RESULT`, and it is what stops a caller re-deriving a field: the seven fields below are
     rendered together from one scan of the table, so no two of them can be read from different moments. */
  getRegistry() {
    return JSON.parse(M.ccall("bp_registry_snapshot", "string", [], []));
  },
};

/* THE BROWSER'S CONTROL INTERFACE TO THIS CHILD. What it answers is this PROCESS's mojo surface — which
   interfaces it calls out on, which it implements, and how many pipe endpoints it holds — because a transport
   whose only evidence is that it delivered a message is one whose shape nobody can check from outside. */
const CHILD_PROCESS = {
  getMojoStats() {
    const s = self.mojo.stats();
    return { remotes: s.remotes.join(","), receivers: s.receivers.join(","), endpoints: s.endpoints };
  },
};

/* BOOT. The module is instantiated BEFORE the invitation is accepted and before any interface is exposed, so
   the trusted side cannot send a call into a program that does not exist: the parent binds nothing until this
   process has said it started. A failure travels IN the acceptance rather than being swallowed — a browser
   process that did not instantiate must not look like one that is merely slow, because there is no timeout on
   the far side (a wall clock there would report a loaded machine as a broken transport — CLAUDE.md §Testing). */
const conn = new self.mojo.Connection(
  { post: (m, xfer) => { self.postMessage(m, xfer); },
    listen: (cb) => { self.onmessage = (e) => { cb(e.data); }; } },
  { role: "child", name: "the offscreen document", drainStdio: drain });

createBrowserProcess({
  print: (s) => { out.push([1, String(s)]); },
  printErr: (s) => { out.push([2, String(s)]); },
  noInitialRun: true,
}).then(
  (mod) => {
    M = mod;
    self.mojo.exposeInterface("content.mojom.RendererHost", RENDERER_HOST);
    self.mojo.exposeInterface("content.mojom.ChildProcess", CHILD_PROCESS);
    conn.acceptInvitation(true, null);
  },
  (err) => {
    conn.acceptInvitation(false, String((err && err.stack) || err));
  });
