/* renderer_host_gate.mjs — THE BROWSER-PROCESS LAYER, DRIVEN. `node engine/renderer_host_gate.mjs`, after any
 * build: it imports `extension/lib/qjs/qjs.mjs` off disk exactly as route.mjs does, so a run against a stale
 * artifact measures a program no revision of this tree contains (§Testing) — which is why build.mjs owns it as
 * a stage that SKIPS when the ABI program did not link, rather than a thing anyone runs by hand after a build
 * they hope succeeded.
 *
 * WHY IT EXISTS. `engine/route.mjs` is the only thing in this tree that provisions a second instance, and it
 * plays the offscreen's role ITSELF — it calls `makeEngine` directly. So the transport that is green is not the
 * transport the extension uses: `extension/render-process-host.js` (the renderer REGISTRY: which agent clusters
 * have an instance, what routing id each was given, and the refusal of a second one) and
 * `extension/renderer-host.js` (the RenderFrameHost: the only thing that materializes a frame and speaks to it)
 * were compiled by nothing and run by nothing. `engine/build.mjs`'s own closing paragraph said so — "this build
 * compiles no JavaScript … The REFUSALS have no caller that fires them" — which is SECURITY.md's sentence about
 * a host that cannot provision a second instance, one layer up from the engine that already answered it.
 *
 * WHAT IS REAL AND WHAT IS SUBSTITUTED, because that split is the whole worth of this gate. REAL, byte for byte
 * off `extension/`, loaded in ast-worker.html's own order: check.js, mojo.js, mojom.js, render-process-host.js,
 * renderer-host.js — so the ADMISSION DECISION, the routing-id mint, the duplicate-cluster refusal, the fork
 * order, the invitation handshake, the mojo wire and its validator are the shipped code and not a model of it.
 * REAL on the far side too: `renderer.html`'s own inline bootstrap and the production ABI program this build
 * just linked, so each renderer is a genuine second WASM instance answering genuine ABI calls.
 * SUBSTITUTED: only the browser primitives that carry those bytes — an `<iframe>` whose insertion starts a
 * frame (a Node worker thread, which is a real separate realm with real structured clone and real MessagePort
 * transfer), the window channel the invitation rides, and the same-origin `fetch` of this extension's own
 * program. A substitute for a TRANSPORT is a substitute for a wire; a substitute for the registry's decision
 * logic would be this gate answering its own question, so there is none.
 *
 * AND THE SUBSTITUTES CRASH RATHER THAN APPROXIMATE. The DOM here answers exactly the operations
 * renderer-host.js performs and DFAILs on every other one, because a shim that quietly returns something
 * plausible for an operation nobody implemented is how a harness reports a working seam that was never asked.
 */
import { readFileSync, mkdtempSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { Worker } from 'node:worker_threads';
import { runInThisContext } from 'node:vm';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const EXT = join(ENGINE, '..', 'extension');
const TAG = '[renderer-host-gate]';

/* EVERYTHING `shutdown` TOUCHES IS DECLARED BEFORE THE FIRST THING THAT CAN CALL IT, and that ordering is the
   whole of why these three lines are here rather than beside the frames they belong to. `fail` runs `shutdown`,
   the earliest `fail` is the one that cannot find renderer.html's inline script — which is BEFORE the frame
   list in every reading order that puts the declarations with their subject — and a `const` read from its own
   temporal dead zone throws a ReferenceError that REPLACES the message naming what actually broke. That is the
   §Testing failure exactly: a real defect reported as something else, with the confident text discarded. */
const _frames = [];      /* every live frame element, in document order */
const _workerErrors = [];

/* A PHASE THAT CANNOT REACH THE STATE THE NEXT ONE MEASURES STOPS HERE — route.mjs's `fail`, for its reason:
   carrying a half-built precondition into an assertion about something else produces a verdict about nothing. */
const fail = (why) => { console.error(`${TAG} FAILED: ` + why); shutdown(); process.exit(1); };

/* A REFUSAL IS NOT A VERDICT, AND A ONE-LINE STAGE TABLE IS EXACTLY WHERE THE TWO COLLAPSE INTO ONE. `fail`
   says this gate asked its question and the browser-process layer answered it wrongly; `refuse` says the gate
   DECLINED TO ASK, because the program in front of it is one no revision of this tree names. Both exit
   non-zero — a stage that did not ask its question must never read like one that asked and liked the answer —
   but a reader who cannot tell them apart goes hunting a defect in render-process-host.js when what happened
   is that some other lane had a file open while the artifact was linked. That is §Testing's own defect twice
   over: a harness condition reported as a property of the thing measured, and two states behind one number.
   THE MARKER IS A LINE OF OUTPUT AND NOT A BARE EXIT CODE, on the argument build.mjs already makes for its
   RLIMIT install: an exit status is a value a program may legitimately produce for some entirely other
   reason, so the statement has to be one this file WROTE. */
const REFUSED_MARK = 'REFUSED TO MEASURE:';
const refuse = (why) => { console.error(`${TAG} ${REFUSED_MARK} ` + why); shutdown(); process.exit(1); };

/* ─────────────────────────────────────────────────────────────────────────────────────────────────────────
   THE OFFSCREEN DOCUMENT'S REALM. Everything below installs the browser surface renderer-host.js reads, and
   nothing else — the extension's own files are then loaded into it unmodified.
   ───────────────────────────────────────────────────────────────────────────────────────────────────────── */
globalThis.self = globalThis;

const TMP = mkdtempSync(join(tmpdir(), 'apiclient-renderer-gate-'));

/* THE REVISION OF THE PROGRAM UNDER TEST, STATED BEFORE ANYTHING CAN FAIL. §Testing: a result quoted without
   the revision it came from is not a measurement — and this gate does not build, it IMPORTS a program some
   EARLIER build linked, so the revision its verdict belongs to is that build's and not whatever the checkout
   holds while the gate runs. It is read HERE, above every `fail` in the file, so the revision is the first line
   of output and therefore stands above the verdict whichever verdict it turns out to be; carrying it INTO
   `fail` instead would put a `const` in the reach of the earliest failure's own temporal dead zone, and a
   ReferenceError there replaces the message naming what actually broke (the trap this file's opening paragraph
   is about).
   THE ARTIFACT'S REVISION AND THE CHECKOUT'S LEGITIMATELY DIFFER — build.mjs rebuilds on a CONE and this gate
   is outside it — so a mismatch is not a failure and must not be made one. What IS a failure is a verdict
   nobody can attach to a revision at all, and `dirty` is the same statement one step further: a program linked
   from an edited tree is one no revision contains, so every verdict below it would be about nothing. */
const BUILT = (() => {
  let raw = null;
  try { raw = readFileSync(join(EXT, 'lib', 'qjs', 'qjs.mjs.build.json'), 'utf8'); }
  catch (e) {
    /* ENOENT IS "NOBODY HAS BUILT THIS" AND EVERY OTHER ERRNO IS NOT — the same split this file's `fetch` shim
       makes, and for the same reason: a permission error reported as a missing build accuses the wrong thing. */
    if (e.code !== 'ENOENT') throw e;
    refuse('the artifact this gate was pointed at carries no `lib/qjs/qjs.mjs.build.json` — that record is ' +
           'what says WHICH revision the program under test was linked from, and a browser-process verdict ' +
           'with no revision beside it is not a measurement of anything (§Testing)');
  }
  const b = JSON.parse(raw);
  /* NO DEFAULTS ON THE PRODUCER'S FIELDS. build.mjs writes all three on every build, so an absent `head` is
     that contract broken — never an artifact that merely happens not to know its own revision, which is what a
     `|| "unknown"` here would turn it into. */
  if (typeof b.head !== 'string' || typeof b.qjsHead !== 'string' || !Array.isArray(b.dirty))
    fail('the artifact\'s build record does not carry the revision build.mjs writes (`head`, `qjsHead`, ' +
         '`dirty`) — a field defaulted here would turn "this program came from nowhere nameable" into a ' +
         'plausible revision, which is indistinguishable afterwards from a real one');
  if (b.dirty.length)
    refuse(`the program under test was linked from an EDITED tree (${b.dirty.length} dirty path(s): ` +
           `${b.dirty.join(' ')}), so it is a program no revision of this tree contains and every verdict this ` +
           'gate could reach would be about nothing. NOTHING BELOW THIS LINE RAN, so this says nothing ' +
           'whatever about the browser-process layer: rebuild on a tree no lane is mid-edit in, and the ' +
           'stage after that one is the first real verdict (§Testing)');
  return b;
})();
const REV = `host ${BUILT.head.slice(0, 12)} qjs ${BUILT.qjsHead.slice(0, 12)}`;
console.log(`${TAG} the program under test was linked at ${REV} (${BUILT.at})`);

const RENDERER_SCRIPT = (() => {
  const html = readFileSync(join(EXT, 'renderer.html'), 'utf8');
  const m = html.match(/^<script>\r?\n([\s\S]*?)\r?\n<\/script>\s*$/m);
  if (!m) fail('extension/renderer.html carries no single inline <script> — the frame program is that script, ' +
               'and a gate that cannot find it would be measuring a renderer it wrote itself');
  return m[1];
})();

/* THE SAME-ORIGIN READ OF THIS EXTENSION'S OWN PROGRAM. `programOnce` reads five names off its own origin; in
   the browser that is a `fetch` this zone may make and the frame may not, and here it is the directory. Any
   other name is a request this gate has no answer for and must not invent one for. */
globalThis.fetch = async (name) => {
  if (typeof name !== 'string' || /^[a-z]+:/i.test(name) || name.startsWith('/') || name.includes('..'))
    fail(`the trusted zone asked for \`${name}\`, which is not a relative path into this extension — this gate ` +
         'answers the same-origin read of the extension\'s own program and nothing else');
  /* A FILE THAT IS NOT THERE IS A 404 AND EVERY OTHER FILESYSTEM ERROR IS NOT ONE. `programOnce` CHECKs
     `response.ok`, so a bare catch here would report a permission error, a directory in place of a file or a
     read fault as "the extension does not ship this resource" — a true-sounding accusation about the wrong
     component, which is the one outcome this gate exists to stop producing. ENOENT is the only outcome a
     browser fetch of a missing extension resource has; anything else is this harness's own machine breaking
     and travels on as itself. */
  let bytes = null;
  try { bytes = readFileSync(join(EXT, name)); }
  catch (e) { if (e.code !== 'ENOENT') throw e; bytes = null; }
  return {
    ok: bytes !== null,
    arrayBuffer: async () => {
      if (bytes === null) fail(`arrayBuffer() on a response this gate reported as not ok (${name})`);
      return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    },
  };
};

/* ── THE WINDOW CHANNEL. One listener list, one dispatch, and `event.source` is the frame's own window object
   — which is the identity renderer-host.js routes every concurrent handshake by. */
const _winListeners = [];
globalThis.addEventListener = (type, fn) => {
  if (type !== 'message') fail(`the trusted zone listened for \`${type}\` on its window and this gate carries ` +
                               'only the message channel the invitation rides');
  _winListeners.push(fn);
};
globalThis.removeEventListener = (type, fn) => {
  const i = _winListeners.indexOf(fn);
  if (i >= 0) _winListeners.splice(i, 1);
};
const dispatchWindowMessage = (ev) => { for (const fn of _winListeners.slice()) fn(ev); };

/* ── THE FRAMES. An element whose INSERTION starts the load, exactly as the browser's does: renderer-host.js
   installs its handshake listener before `appendChild` and relies on nothing arriving until after it.
   (`_frames` and `_workerErrors` are declared at the top of this file, with `shutdown`'s reason.) */

class FrameElement {
  constructor() { this.tagName = 'IFRAME'; this._attrs = new Map(); this.style = { cssText: '' };
                  this.parentNode = null; this.contentWindow = null; this._worker = null; }
  setAttribute(n, v) { this._attrs.set(String(n), String(v)); }
  getAttribute(n) { return this._attrs.has(String(n)) ? this._attrs.get(String(n)) : null; }
}

/* ─────────────────────────────────────────────────────────────────────────────────────────────────────────
   THE FRAME'S SHIM — the only code in this file that runs inside a renderer, and it installs four things
   renderer.html reads and nothing else: `parent`, the frame's own message listener list, and the object-URL
   pair `importBlob` mints its module URLs with.
   THE BLOBS ARE MATERIALIZED BEFORE THE RECORD IS DISPATCHED, which is what makes `createObjectURL`
   SYNCHRONOUS as the spec has it: a Blob's bytes are only readable asynchronously here, so the shim writes each
   one out on delivery and the mint is then a lookup. They become real files because the engine glue is an
   emscripten module that calls `createRequire(import.meta.url)` on its first line — a data: URL has no path to
   resolve from, so the program would fail to load for a reason that has nothing to do with what is being
   measured.
   AND A STAGING FAULT IS A DIFFERENT ACCUSATION FROM A PROGRAM FAULT, WHICH IS WHY THE SHIM CARRIES ITS OWN
   MARKER. `materialize` below is this gate's substitute for the browser's Blob store, and when a substitute
   breaks it must say so in its own name: "the module I staged is complete and readable" and "the module I
   staged exported no factory" are statements about DIFFERENT components, and a harness that delivers both
   through one signal reports its own machine as a defect in the thing it measures (§Testing, the same defect as
   a timeout kill and a DCHECK sharing one column).
   ───────────────────────────────────────────────────────────────────────────────────────────────────────── */
const STAGING_MARK = '@E [harness-staging]';
const FRAME_SHIM = `
'use strict';
const { parentPort, workerData, threadId } = require('node:worker_threads');
const fs = require('node:fs'), path = require('node:path'), nodeUrl = require('node:url');
const STAGING_MARK = ${JSON.stringify(STAGING_MARK)};
globalThis.self = globalThis;

/* THE THIRD ARGUMENT IS OPTIONAL AND ITS ABSENCE IS A POSITIVE STATEMENT — HTML §9.3.3 "Posting messages" declares \`postMessage(message, targetOrigin, transfer)\` with \`transfer\` defaulting to an empty
   sequence, so "this post carries no handles" is what \`undefined\` MEANS here. The default is written on the
   PRODUCING side, once, and every consumer below then reads a \`ports\` field that is always present. */
const parent = {
  postMessage(rec, targetOrigin, transfer) {
    const ports = transfer === undefined ? [] : transfer;
    parentPort.postMessage({ rec, ports }, ports);
  },
};
globalThis.parent = parent;

const listeners = [];
globalThis.addEventListener = (type, fn) => { listeners.push(fn); };
globalThis.removeEventListener = (type, fn) => {
  const i = listeners.indexOf(fn);
  if (i >= 0) listeners.splice(i, 1);
};

let serial = 0;
const objectUrls = new Map();
URL.createObjectURL = (b) => {
  const u = objectUrls.get(b);
  if (!u) throw new Error('@E this frame minted an object URL for a Blob its embedder never delivered');
  return u;
};
URL.revokeObjectURL = () => {};

/* THE STAGED MODULE IS NAMED BY THE FRAME AND PUBLISHED ATOMICALLY, and both halves of that are one defect this
   file shipped: \`process.pid\` is a fact about a PROCESS and every frame here is a THREAD of one process, so
   \`frame-<pid>-<serial>\` named the SAME FOUR FILES in every frame — and phase 1 forks two renderers
   CONCURRENTLY, which is the point of phase 1. Both truncated those paths and one imported what the other had
   opened for writing and not yet filled. An empty module has no default export, so renderer.html's
   "the engine glue module exported no factory" DCHECK fired against an artifact whose glue exports one: a false
   red naming the ENGINE, manufactured entirely by this harness, and the busier the machine the wider the window
   (§Testing — a measurement a loaded machine can falsify is not a measurement). \`threadId\` is the frame's own
   name, and the temp directory is already this process's own, so the published name is unique by construction.
   THE PUBLISH IS \`link\` AND NOT \`rename\`, because it must assert that as well as achieve it. Both are atomic
   within a directory, so no reader can observe a half-written file at the published name; \`link\` additionally
   REFUSES an existing target in the kernel, with no window between the question and the act — so "no two frames
   named one file" is checked rather than assumed, and EEXIST is the only errno that means it. Every other errno
   is this harness's machine breaking and travels on as itself. */
async function stage(bytes) {
  const name = 'frame-' + threadId + '-' + (++serial) + '.mjs';
  const p = path.join(workerData.tmp, name), part = p + '.part';
  fs.writeFileSync(part, bytes, { flag: 'wx' });
  try { fs.linkSync(part, p); }
  catch (e) {
    if (e.code !== 'EEXIST') throw e;
    throw new Error(STAGING_MARK + ' two renderer frames staged \`' + name + '\`, so one of them imported a ' +
                    'module the other was still writing — the name carries the staging frame\\'s own threadId ' +
                    'and the directory is this process\\'s own, so a collision is this harness minting one ' +
                    'name twice');
  }
  fs.unlinkSync(part);
  /* THE MODULE I STAGED IS COMPLETE — asserted HERE, where staging happens, so it can never again arrive as a
     sentence about what the module exported. The link above published a fully written inode, so a short file
     here is a write that did not write what it was given, which is a statement about this harness and about
     nothing else. */
  const got = fs.statSync(p).size;
  if (got !== bytes.length)
    throw new Error(STAGING_MARK + ' staged ' + got + ' of ' + bytes.length + ' byte(s) to \`' + name +
                    '\` — the frame is about to import a truncated module, and an incomplete module and a ' +
                    'module built without an export are different faults in different components');
  return nodeUrl.pathToFileURL(p).href;
}

async function materialize(rec) {
  for (const k of Object.keys(rec)) {
    const v = rec[k];
    if (!(v instanceof Blob) || objectUrls.has(v)) continue;
    objectUrls.set(v, await stage(Buffer.from(await v.arrayBuffer())));
  }
}

parentPort.on('message', async (m) => {
  await materialize(m.rec);
  /* NO DEFAULT ON \`m.ports\`: the only producer is the embedder's own \`contentWindow.postMessage\` below, which
     writes the field on every post. A \`|| []\` here would turn a producer that stopped writing it into an
     invitation that plausibly carried no pipe — and renderer.html's own refusal for that ("an invitation
     carried no pipe") would then name the embedder for this shim's hole. */
  if (!Array.isArray(m.ports))
    throw new Error('@E a renderer frame was posted a window record with no ports list — its embedder writes ' +
                    'one on every post, so a record without it is this harness losing the primordial pipe');
  const ev = { data: m.rec, source: parent, origin: 'null', ports: m.ports };
  for (const fn of listeners.slice()) fn(ev);
});

eval(workerData.script);
`;

/* THE FRAME'S REALM. A worker thread is a real second realm — its own globals, its own module graph, its own
   WebAssembly instance — reached by structured clone with real MessagePort transfer, which is what makes the
   invitation an invitation rather than a shared object handed across a function call. */
function startFrame(f) {
  const src = f.getAttribute('src');
  if (src !== 'renderer.html')
    fail(`a frame was inserted with src=\`${src}\` — renderer-host.js materializes exactly renderer.html`);
  const w = new Worker(FRAME_SHIM, { eval: true, workerData: { script: RENDERER_SCRIPT, tmp: TMP } });
  const contentWindow = {
    postMessage(rec, targetOrigin, transfer) {
      if (targetOrigin !== '*')
        fail(`the trusted zone posted to a frame with targetOrigin \`${targetOrigin}\` — the frame's origin is ` +
             'opaque, so "*" is the only one that names it and anything else is a post that never arrives');
      /* THE OPTIONAL ARGUMENT'S DEFAULT, WRITTEN ONCE ON THE PRODUCING SIDE — HTML §9.5's `transfer` sequence
         again, in the direction that carries the invitation's primordial pipe. */
      const ports = transfer === undefined ? [] : transfer;
      w.postMessage({ rec, ports }, ports);
    },
  };
  f.contentWindow = contentWindow;
  f._worker = w;
  /* AND NO DEFAULT ON THE WAY BACK EITHER. The frame's `parent.postMessage` writes `ports` on every post, so a
     record without one is this harness dropping a field rather than a frame that posted no handles — which is
     the shape §Offensive names: a hole a `||` fills, indistinguishable afterwards from a measurement. */
  w.on('message', (m) => {
    if (!Array.isArray(m.ports))
      fail('a renderer frame posted a window record with no ports list — the frame shim writes one on every ' +
           'post, so a record without it is this harness losing a field rather than a frame sending none');
    dispatchWindowMessage({ data: m.rec, source: contentWindow, origin: 'null', ports: m.ports });
  });
  /* AN UNCAUGHT THROW IN THE FRAME IS THE FRAME DYING, AND THE GATE SAYS SO ON THE SPOT. renderer.html's two
     bare `@E` throws leave the embedder parked forever by design — it refuses a wall clock on the handshake,
     and in a browser a frame that died posts nothing — so a harness that only collected this would report the
     one failure with no symptom anywhere as a hang with no cause. It is collected AND named. */
  w.on('error', (e) => {
    _workerErrors.push(String((e && e.stack) || e));
    /* AND A FAULT IN THIS HARNESS'S OWN SUBSTITUTE IS REPORTED AS ONE, THROUGH ITS OWN SENTENCE. The shim's
       staging asserts (`STAGING_MARK`) fire where this gate stands in for the browser's Blob store, and what
       they say is "the module I staged is not the module I was handed" — which is a DIFFERENT accusation from
       renderer.html's "the module exported no factory", and pointing at a different component. Folding both
       into "a renderer frame died" is how a staging race got read as a broken build for a whole session, and it
       is §Testing's own example: a kill and a DCHECK reported in one column tell nobody which one happened. */
    if (String((e && e.message) || e).includes(STAGING_MARK))
      fail('THIS HARNESS could not stage the renderer\'s program, so no renderer was ever asked anything and ' +
           'this run states NOTHING about the artifact it was pointed at — the fault is in the substitute this ' +
           'gate provides for the browser\'s blob: URL store, not in the program that was being staged:\n' +
           _workerErrors[_workerErrors.length - 1]);
    fail('a renderer frame died on an uncaught throw, so the handshake it owed can never arrive:\n' +
         _workerErrors[_workerErrors.length - 1]);
  });
}

const _body = {
  appendChild(node) {
    if (!(node instanceof FrameElement)) fail('this document was handed a node that is not a renderer frame');
    node.parentNode = _body;
    _frames.push(node);
    startFrame(node);
    return node;
  },
  removeChild(node) {
    const i = _frames.indexOf(node);
    if (i < 0) fail('a frame was removed from this document twice — the browser throws NotFoundError here, and ' +
                    'renderer-host.js guards it with `if (r.frame.parentNode)`');
    _frames.splice(i, 1);
    node.parentNode = null;
    /* A FRAME IN THIS DOCUMENT HAS A REALM, ASSERTED RATHER THAN TESTED. `appendChild` starts the worker in the
       same statement that adopts the node, so a frame reaching here without one is this harness holding an
       element for a renderer it never started — which would leave `rendererStats`'s DOM census counting a frame
       nothing can ever answer for. */
    if (node._worker === null)
      fail('a frame was removed that this document never started a realm for — the insertion starts the frame, ' +
           'so an element without one is a renderer counted by the census and reachable by nothing');
    node._worker.terminate();
    node._worker = null;
    return node;
  },
};

const FRAME_TITLE_SELECTOR = 'iframe[title^="renderer "]';
globalThis.document = {
  body: _body,
  documentElement: _body,
  createElement(tag) {
    if (String(tag).toLowerCase() !== 'iframe')
      fail(`this document was asked for a <${tag}> — renderer-host.js creates iframes and nothing else`);
    return new FrameElement();
  },
  querySelectorAll(sel) {
    if (sel !== FRAME_TITLE_SELECTOR)
      fail(`this document was asked for \`${sel}\` — the one selector it answers is renderer-host.js's frame ` +
           `census, ${FRAME_TITLE_SELECTOR}, and answering a selector nobody implemented is a census that ` +
           'counts whatever this shim happened to return');
    /* AN ABSENT ATTRIBUTE IS `null` AND IS READ AS THE POSITIVE STATEMENT IT IS — "this element carries no
       title", which the attribute-prefix selector does not match. Coercing it to `""` first would be a default
       standing where the absence itself is the answer, and it would read identically if `setAttribute` ever
       stopped being called. */
    return _frames.filter((f) => { const t = f.getAttribute('title');
                                   return t !== null && t.startsWith('renderer '); });
  },
};

/* ── THE EXTENSION'S OWN FILES, IN ast-worker.html's ORDER AND BY THEIR OWN BYTES. `runInThisContext` so a
   DCHECK that fires names the real file and line. */
for (const f of ['check.js', 'mojo.js', 'mojom.js', 'render-process-host.js', 'renderer-host.js'])
  runInThisContext(readFileSync(join(EXT, f), 'utf8'), { filename: join(EXT, f) });

if (typeof self.renderProcessHost !== 'object' || typeof self.rendererLaunch !== 'function' ||
    typeof self.rendererProbe !== 'function' || typeof self.rendererStats !== 'function')
  fail('the browser-process layer did not install itself — this gate loads render-process-host.js and ' +
       'renderer-host.js by their own bytes, so a missing export is one of them failing to evaluate');

/* ─────────────────────────────────────────────────────────────────────────────────────────────────────────
   THE RUN
   ───────────────────────────────────────────────────────────────────────────────────────────────────────── */
/* THE REALMS THIS PROCESS STARTED, STOPPED. A frame whose worker is already null is one `startFrame` refused
   before it built a realm (`_frames` adopts the node first) — a real state, not a hole. The temp directory is
   removed WITHOUT a catch: `force: true` already makes a directory that is not there a no-op (fs.rm's own
   `force` option), so anything left to throw here is this harness's machine breaking, and swallowing it would
   be a cleanup that silently did nothing on every run after the first failure. */
function shutdown() {
  for (const f of _frames.slice()) if (f._worker !== null) { f._worker.terminate(); f._worker = null; }
  rmSync(TMP, { recursive: true, force: true });
}
const NUL = String.fromCharCode(0);
const mintedIds = [];
const noteId = (id, where) => {
  if (mintedIds.includes(id))
    fail(`routing id ${id} was handed out twice (${where}) — an id is the only name a renderer has, so a reuse ` +
         'is a termination burying whichever renderer the registry\'s scan reaches first');
  mintedIds.push(id);
};
const registryLine = (label) => {
  const r = self.renderProcessHost.getRegistry();
  const s = self.rendererStats();
  console.log(`${TAG}   ${label.padEnd(34)} registry live=${r.live} launched=${r.launched} ` +
              `terminated=${r.terminated} failed=${r.failed} nextRoutingId=${r.nextRoutingId} | ` +
              `frames forked=${s.forked} forking=${s.forking} provisioned=${s.provisioned} ` +
              `destroyed=${s.destroyed} live=${s.live} frames=${s.frames}`);
  return { reg: r, st: s };
};

let probe = null;
try {
  /* ── PHASE 1 — TWO CROSS-ORIGIN RENDERERS, PROVISIONED THROUGH THE REGISTRY. `rendererProbe` is
     renderer-host.js's own: two peers in ONE browsing-context group differing only by origin, forked
     CONCURRENTLY so two handshakes race on this document's one window channel, each initialized with a
     DIFFERENT document and asked for the bundle id it computed. Same group + same origin would be refused
     (phase 2); two groups would prove only that two unrelated names produce two rows. */
  console.log(`${TAG} ── phase 1: two cross-origin renderers, provisioned through the registry ──`);
  registryLine('before');
  probe = await self.rendererProbe();
  console.log(`${TAG}   asked=${probe.asked} provisioned=${probe.provisioned} torndown=${probe.torndown} ` +
              `crashed=${probe.crashed} badMessages=${probe.badMessages} wire=${probe.wire}`);
  /* THE CRASH IS READ BEFORE THE PEERS, WHICH IS THE DIFFERENCE BETWEEN NAMING THE FAULT AND NAMING A SYMPTOM
     OF IT. `rendererProbe` fills `peers` BY INDEX and leaves a slot `null` for a peer it never reached, so an
     instance that aborted mid-ABI leaves both a `crashed` flag and a `null` — and iterating first reported
     "a peer answered no ABI call at all" while the engine's own `@WHY` ROOT line sat unread in
     `rendererLines`. That is §Testing's empty tail: the output that preceded the crash discarded in favour of
     a confident sentence about the wrong component. */
  if (probe.crashed)
    fail(`the probe reported a crashed instance: ${probe.err}\n${TAG}   renderer lines: ` +
         JSON.stringify(probe.rendererLines));
  for (const p of probe.peers) {
    if (p === null) fail('a peer answered no ABI call at all — an instance reported live that nothing spoke to');
    noteId(p.routingId, 'phase 1');
    console.log(`${TAG}   peer ${p.origin} routingId=${p.routingId} iface=${p.iface}/${p.ifaceMethods} ` +
                `initrc=${p.initrc} bundleId=${p.bundleId} heapBytes=${p.heapBytes} ` +
                `childEndpoints=${p.child.endpoints}`);
  }
  registryLine('after phase 1');

  if (probe.provisioned !== 2)
    fail(`${probe.provisioned} renderer(s) were provisioned where two were asked for — provisioning a SECOND ` +
         'instance is the whole claim SECURITY.md makes the precondition for believing any cross-instance ' +
         'mechanism has ever run');
  if (probe.torndown !== 2)
    fail(`${probe.torndown} of 2 instances were torn down — teardown is the call that makes each runtime walk ` +
         'gc_obj_list and report a leaked GC object, so a probe that walked away measured the transport with ' +
         'the one check that judges an instance switched off missing');

  /* ── PHASE 2 — THE REFUSAL. SECURITY.md's one-instance-per-`(browsing-context group, origin)` rule is a
     `CHECK` in render-process-host.js — fatal in dev AND release — and until this gate nothing in the tree ever
     fired it. It is asked here of a cluster that HAS a live renderer, which is the state the rule is about. */
  console.log(`${TAG} ── phase 2: a second renderer for a cluster that already has one ──`);
  const DUP = 'gate-group' + NUL + 'https://dup.renderer.gate';
  const first = await self.rendererLaunch(DUP);
  noteId(first.routingId, 'phase 2 first launch');
  const before = registryLine('one renderer for the cluster');

  let refusal = null;
  try { const second = await self.rendererLaunch(DUP); noteId(second.routingId, 'phase 2 second launch'); }
  catch (e) { refusal = e; }
  const after = registryLine('after the second request');

  if (refusal === null)
    fail('the registry ADMITTED a second renderer for an agent cluster that already has one — two heaps for ' +
         'one similar-origin window agent is the split SECURITY.md\'s one-instance-per-cluster rule exists to ' +
         'forbid, and this is the table that already held the answer');
  /* THE REFUSAL IS THE REGISTRY'S AND NOT ANY THROW. A gate that checks only THAT it threw passes when the
     shim above threw for a reason of its own — render-process-host.js's own paragraph makes exactly this
     point about a filter that threw for everything. */
  if (!refusal.apiclientFatal || !/^@E CHECK failed:/.test(String(refusal.message)) ||
      !String(refusal.message).includes('SECOND renderer for agent cluster'))
    fail('the second request failed, but not on the registry\'s duplicate-cluster CHECK — a refusal that is ' +
         'any other throw is this gate passing on an accident: ' + String((refusal && refusal.stack) || refusal));
  console.log(`${TAG}   REFUSED: ${String(refusal.message).slice(0, 120)}…`);

  /* THE REFUSAL COST NOTHING. An id minted for a request that was refused would be an id no renderer ever
     answers to, and a frame materialized for it would be a WASM instance nothing can reach. */
  if (after.reg.nextRoutingId !== before.reg.nextRoutingId)
    fail(`the refused request still MINTED a routing id (nextRoutingId ${before.reg.nextRoutingId} -> ` +
         `${after.reg.nextRoutingId}) — the refusal is asserted before the counter moves, so an id issued here ` +
         'names a renderer that was never admitted and the registry\'s own accounting is short by one');
  if (after.reg.live !== before.reg.live || after.reg.launched !== before.reg.launched ||
      after.reg.failed !== before.reg.failed || after.reg.terminated !== before.reg.terminated)
    fail('the refused request moved the registry\'s counters — a refusal is a decision taken before anything ' +
         'exists, so every one of them must read exactly as it did before the request');
  if (after.st.frames !== before.st.frames || after.st.provisioned !== before.st.provisioned)
    fail(`the refused request materialized a frame (frames ${before.st.frames} -> ${after.st.frames}, ` +
         `provisioned ${before.st.provisioned} -> ${after.st.provisioned}) — the admission decision stands in ` +
         'front of the fork precisely so a refused cluster never gets a WASM instance this document cannot reach');

  /* ── AND THE REFUSAL IS NOT PERMANENT, WHICH IS THE OTHER HALF OF THE SAME RULE. A termination frees the
     agent cluster; if it did not, the cluster would be refused an instance for the life of the document with
     nothing anywhere to say why — which reads exactly like the rule working. */
  first.destroy();
  const freed = registryLine('after the first terminated');
  if (freed.reg.live !== before.reg.live - 1 || freed.reg.terminated !== before.reg.terminated + 1)
    fail('a terminated renderer did not free its agent cluster in the registry');
  const third = await self.rendererLaunch(DUP);
  noteId(third.routingId, 'phase 2 re-admission');
  const readmitted = registryLine('re-admitted');
  /* THE RE-ADMISSION IS READ, NOT MERELY PERFORMED. A `registryLine` whose record nothing compares is a log
     line wearing the shape of a check — the cluster being back in the table is the half of "a termination
     frees its cluster" that the counters above cannot state. */
  if (readmitted.reg.live !== freed.reg.live + 1 ||
      !readmitted.reg.clusters.some((c) => c.origin === 'https://dup.renderer.gate'))
    fail('the re-admitted renderer is not in the registry under the cluster that was freed — a termination ' +
         'that frees a cluster only matters if the next request for it is ADMITTED, and a table that lost the ' +
         'row would read exactly like a launch that never happened');
  if (third.routingId === first.routingId)
    fail(`the re-admitted renderer was given the id of the one that died (${third.routingId}) — an id is the ` +
         'only name a renderer has, and reusing a buried one makes a late reply from the dead instance land on ' +
         'the live one');
  console.log(`${TAG}   re-admitted with a NEW id: ${first.routingId} died, ${third.routingId} minted`);
  third.destroy();

  /* ── PHASE 3 — THE TWO REFUSALS A ROUTING ID CAN EARN, WHICH `slotRequire` TELLS APART ON PURPOSE AND WHICH
     NOTHING IN THIS TREE HAD EVER FIRED. build.mjs named them where the deleted C driver's coverage was
     recorded: "an id this table never minted" and "a renderer reported dead twice". They are different
     accusations and a caller reading one `@E` line is standing where the fix has to be made — an id at or
     beyond the counter came from OUTSIDE this table, and an id below it names a renderer already buried, so
     acting on it would free an agent cluster that has a live instance or is about to be given one.
     THEY ARE ASKED OF THE REGISTRY DIRECTLY, and that is the honest shape rather than a shortcut: no caller in
     the extension can produce either id — `rendererDestroy` passes the id its own record carries — so the
     state these refusals are about is reachable only by a component that has already gone wrong. Driving it
     through a renderer would be inventing a broken caller and then measuring the invention. */
  console.log(`${TAG} ── phase 3: the refusals a routing id earns ──`);
  const beforeRefusals = registryLine('before the id refusals');
  /* ONE HELPER, TWO CASES, AND THE MESSAGE IS PART OF THE ASSERTION. A gate that checks only THAT a call threw
     passes when it threw for a reason of its own — the same point render-process-host.js makes about a filter
     that threw for everything, and the reason phase 2 matches its refusal's text too. */
  const refuses = (what, fragment, fn) => {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (e === null)
      fail(`the registry ACCEPTED ${what} — that is a CHECK in render-process-host.js, fatal in dev and in ` +
           'release, and a table that takes it acts on a renderer some other component decided existed');
    if (!e.apiclientFatal || !/^@E CHECK failed:/.test(String(e.message)) ||
        !String(e.message).includes(fragment))
      fail(`the registry refused ${what}, but not on the CHECK this asks about (expected text containing ` +
           `\`${fragment}\`) — a refusal that is any other throw is this gate passing on an accident: ` +
           String((e && e.stack) || e));
    console.log(`${TAG}   REFUSED (${what}): ${String(e.message).slice(0, 110)}…`);
  };
  /* AN ID AT THE COUNTER IS THE NEXT ONE THIS TABLE WOULD MINT AND THEREFORE ONE IT NEVER HAS. */
  refuses('a routing id it never minted', 'never minted',
          () => self.renderProcessHost.rendererTerminated(beforeRefusals.reg.nextRoutingId));
  /* AND `first` IS A RENDERER THIS GATE ALREADY BURIED, so its id is below the counter and names no slot —
     which is exactly one death reported twice. */
  refuses('a renderer reported dead twice', 'already buried',
          () => self.renderProcessHost.rendererTerminated(first.routingId));
  const afterRefusals = registryLine('after the id refusals');
  if (afterRefusals.reg.nextRoutingId !== beforeRefusals.reg.nextRoutingId ||
      afterRefusals.reg.live !== beforeRefusals.reg.live ||
      afterRefusals.reg.launched !== beforeRefusals.reg.launched ||
      afterRefusals.reg.terminated !== beforeRefusals.reg.terminated ||
      afterRefusals.reg.failed !== beforeRefusals.reg.failed)
    fail('a refused report still moved the registry\'s counters — `slotRequire` asserts before it returns a ' +
         'slot and every transition mutates only after it, so a counter that moved here is a burial the table ' +
         'performed on a renderer it had just refused to recognise');

  /* ── PHASE 4 — A FLOW RUNNING BEHIND THE FRAME BOUNDARY, WHICH NOTHING IN THIS TREE HAD EVER DONE.
     Every instance this layer has ever provisioned was `init`'d, asked for its bundle id and its stats, and
     torn down: `begin` and `step` were never called through the mojo wire by anything. So the phases above
     prove the transport carries a renderer INTO EXISTENCE and prove nothing whatever about a renderer doing
     WORK, and any assertion about a record routed BETWEEN two renderers written before this one would be an
     assertion against a peer that has never had a frontier to route into.
     WHAT IS NEW HERE IS THE WIRE, NOT THE SEAM. route.mjs already drives a frontier and already routes a
     `navigable.create` — by raw `ccall` into a module it instantiated itself. The entries that carry a routed
     record are DECLARED methods of `content.mojom.Renderer` (`Route`, `Perform`, `HostAnswerRemote`,
     `WorldGone`), and the validator that exists because SECURITY.md calls the renderer the hostile peer has
     never seen one of them. This phase is the first half of closing that: a real flow, in a sandboxed
     opaque-origin frame, whose one-way notice crosses the typed boundary and is READ on this side.
     THE DOCUMENT IS THE SMALLEST ONE THAT MUST EMIT THE NOTICE. A cross-origin `window.open` puts the child in
     another agent cluster, so navigable.c announces it rather than creating it here — one `engine_host_notify`,
     nothing negotiated, nothing to park on. No fetch, no read back, no post: this phase asserts that a flow RAN
     and that its notice CROSSED, and every statement beyond that belongs to the phase that provisions the peer.
     TERMINATION IS AN EMITTED OUTPUT AND NOT A STEP CAP (§NO BOUNDS). The drive ends when the notice arrives —
     which is the whole claim — or when the engine itself reports ENGINE_STEP_DONE, which is the engine saying
     its frontier drained; a drained frontier that announced nothing is the FINDING, printed as one. There is
     deliberately no third exit: a drive that neither emits nor drains is a HANG, and it must be reported by the
     harness's own hang signal rather than folded into this stage's verdict, because a step budget here could not
     tell a flow that is working from one that is stuck (§Testing: measure the thing the invariant is about). */
  console.log(`${TAG} ── phase 4: a flow behind the frame boundary ──`);
  const OPENER_ADDR = 'https://opener.renderer.gate/app/index.html';
  const CHILD_ADDR = 'https://peer.renderer.gate/child';
  const OPENER_DOC = '<!doctype html><script>window.open("' + CHILD_ADDR + '","child");</script>';
  const opener = await self.rendererLaunch('gate-group' + NUL + new URL(OPENER_ADDR).origin);
  noteId(opener.routingId, 'phase 4');
  /* THE ARGUMENTS ARE `qjs_init`'s, and they are the same ones the probe passes: the document as BYTES (a
     pointer and a LENGTH, which the mojom layer places from the array below),
     §4.4's address, the name this agent's root document is known by, the response's header field lines (empty
     — this document had no response), §8.1.3.1's top-level creation URL, which for a root document is its
     own address, the two halves of HTML §7.1.7's inherited policy container — both empty, because this
     document has no creator and an empty pair is the positive statement of that — §7.1.4's EMBEDDER POLICY
     item of that same container, which has no empty spelling: §7.1.7 gives every container one, so a document
     with no creator states the section's own "a new embedder policy" in as many words — and HTML §7.3.1.3's
     PARENT NAVIGABLE, `u`, and its CONTAINER, `null`, because this gate loaded the opener itself and nothing
     embeds it — no parent and no element presenting it are one document's two facts, stated one link each.
     Neither is part of the container: a policy container is five policies and says nothing about a frame tree,
     and Permissions Policy §9.5's answer is a sixth thing again, set by a different step of §7.3.2.1. */
  /* AND ITS ANCESTOR ORIGINS, `none`, WHICH IS THE THIRD MEMBER OF THE SET THE TWO ABOVE BELONG TO. This call
     was one argument short of the mojom from the moment §3.1.3's list joined the record, and `mojo.js`'s
     send-side check could not say so while the artifact it compared against still declared thirteen — the
     assert was right and had nothing to be right about yet. The rule is the one this file already states at
     its CHILD path: no parent, no container, no ancestors are true or false TOGETHER, so a document this gate
     loaded itself states all three, one link each. `none` is the word and not the empty string, because an
     empty field is "a host that stopped writing this" and `none` is "this document has no ancestors" — the
     distinction the mojom's own `why` draws, and the one that decides whether a peer answers
     `location.ancestorOrigins` with an empty list or refuses.
     AND ITS §7.1.5 CREATION SANDBOXING FLAG SET, `none`, WHICH IS A FOURTH MEMBER OF THAT SET AND TRUE BY A
     DIFFERENT SENTENCE. The three above are `u`/`null`/`none` because this document has no CONTAINER; this one
     is empty because §7.1.5 answers a navigable with no embedder element from its POPUP sandboxing flag set,
     which is empty when a browsing context is created and which only §7.3.1.7's rules for choosing a navigable
     ever fill — this gate loaded the document itself, so nothing chose it. `none` and not the empty string,
     for the reason the three above give: an empty field is "a host that stopped writing this" and would be
     read as "nothing about this document is sandboxed", which is a claim rather than a silence. */
  const initReply = await opener.renderer.init({
    document: new TextEncoder().encode(OPENER_DOC), url: OPENER_ADDR, docId: 'opener', headers: '',
    topLevelUrl: OPENER_ADDR, inheritedCsp: '', inheritedCspSelfOrigin: '',
    inheritedCoep: 'unsafe-none', inheritedCoepEndpoint: '',
    inheritedCoepReportOnly: 'unsafe-none', inheritedCoepReportOnlyEndpoint: '',
    parentNavigable: 'u', containerPolicy: 'null', ancestorOrigins: 'none',
    creationSandboxFlags: 'none' });
  if (initReply.rc !== 0)
    fail(`the renderer refused the document phase 4 handed it (rc=${initReply.rc}) — every precondition in ` +
         '`qjs_init` aborts rather than returning, so a non-zero return is a contract that changed');
  /* THE EMPTY STRING IS A FRESH FRONTIER, which the mojom declares is a different thing from a resumed one
     that happened to hold nothing. This gate has no residue to replay. */
  await opener.renderer.begin({ recipes: '' });
  let create = null, steps = 0, drained = false;
  while (create === null) {
    const st = await opener.renderer.step();
    steps++;
    /* THE REQUESTS FLOWS ARE PARKED ON, ANSWERED THE WAY THE PRODUCTION CONSUMER ANSWERS THEM — one
       `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>CREDENTIALS<TAB>URL` line each, and the reply record crossing
       as JSON with the BODY BESIDE
       IT as bytes, because §2.2.5 makes a response's body a byte sequence and every way of putting one inside JSON is an
       encode or a decode run by the zone that fetched. `computedType` is this zone's own decision and is
       asserted rather than defaulted at the far end: this zone minted the two characters below and
       `application/json` is what it computed them to be.
       IT SPLITS EVERY FIELD, AND IT DID NOT. This reader took the FIRST tab and called the whole remainder the
       URL, so from the moment the line grew a middle field it delivered against an address beginning with a
       token — matching no park, which is `qjs_provide`'s "a reply was provided for a request no flow is parked
       on" abort, and leaving the flow that IS parked waiting for the rest of the session. It was invisible
       because this phase's opener happens to park nothing on most runs: a reader that is wrong only when it is
       exercised is the shape a grammar change leaves behind, which is why the split is over the field COUNT.
       The DESTINATION may legitimately be EMPTY (Fetch §2.2.5's default, which is what a `fetch()` has), so
       the two fields tested for content are the ends. The CREDENTIALS MODE joined the line after the
       provenance and this gate answers every park whatever it says, for the reason it answers every
       provenance: it serves an opener of its own making over a loopback socket, where there is no session
       to spend — what it owes is the FIELD COUNT, which is the half a grammar change breaks. */
    for (const line of (await opener.renderer.getPending()).requests.split('\n').filter(Boolean)) {
      const t = line.split('\t');
      if (t.length !== 6 || t[0] === '' || t[5] === '')
        fail('a pending line is not ' +
             '`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>CREDENTIALS<TAB>URL`, and the ' +
             `reply is delivered against the (method, url) pair it names: ${line}`);
      const method = t[0], u = t[5];
      console.log(`${TAG}   pending: ${method} ${u}`);
      await opener.renderer.provide({
        method, url: u,
        reply: JSON.stringify({ status: 200, statusText: 'OK', headers: [],
                                urlList: [new URL(u, OPENER_ADDR).href], computedType: 'application/json' }),
        body: new TextEncoder().encode('{}') });
    }
    /* DRAINED BY THE READ, so a notice this zone does not act on is one nothing else will ever see. */
    for (const n of (await opener.renderer.getHostNotices()).notices.split('\n').filter(Boolean)) {
      const f = n.split('\t');
      console.log(`${TAG}   notice: ${f[0]} ${f.slice(1, 4).join(' ')}`);
      if (f[0] === 'navigable.create' && create === null) create = f;
    }
    if (create !== null) break;
    if (st.code === 0) { drained = true; break; }
  }
  if (create === null)
    fail(`the opener's frontier reported DONE after ${steps} step(s) without ever announcing a child ` +
         'navigable — a cross-origin `window.open` puts the child in another agent cluster, so ' +
         'core/frame/navigable.c announces it with a one-way notice and the host provisions the instance ' +
         'under that name. A drive that drained without one is either a flow that never ran behind the frame ' +
         'boundary or a notice that did not survive the typed wire, and until this phase existed neither ' +
         `would have been visible to anything (drained=${drained})`);
  /* THE RECORD'S OWN GRAMMAR, ASSERTED FIELD BY FIELD — `navigable.create<TAB>child<TAB>creator<TAB>addr<TAB>
     origin<TAB>topLevelCreationURL<TAB>cspSelfOrigin<TAB>coep<TAB>coepEndpoint<TAB>coepReportOnly<TAB>
     coepReportOnlyEndpoint<TAB>parentNavigable<TAB>containerPolicy<TAB>ancestorOrigins<TAB>
     creationSandboxFlags<TAB>provenance<TAB>policy`, built by
     core/frame/navigable.c. The policy is LAST
     because it is the record's remainder: a raw CSP header may itself contain HTAB, so it cannot be a middle
     field. Everything that is not the policy sits before it — an origin's serialization cannot contain a tab,
     HTML §7.1.4's three values are fixed tokens, RFC 8941 §3.3.3 "Strings" excludes a tab from the `report-to`
     endpoint those two fields carry, a navigable identity is a one-letter tag over '.'-terminated base64,
     Permissions Policy §4.1's feature tokens and §4.2's `Enabled`/`Disabled` hold none either, and HTML
     §3.1.3's ancestor list is origin serializations joined by SPACE — URL §3.2 "Host miscellaneous" makes both
     TAB and SPACE forbidden host code points, so neither byte can occur inside one — and §7.1.5's flag set is
     that section's own flag names joined by COMMA, which is the one field that may NOT use SPACE, because
     those names contain spaces ("sandboxed navigation browsing context flag").
     THE FIELD COUNT IS CHECKED FIRST because every read below it would otherwise be `undefined` compared
     against a string, which is a false PASS shaped exactly like a real one. */
  if (create.length < 17)
    fail(`the create notice carries ${create.length} field(s) where the record has seventeen — ` +
         `\`${create.join(' | ')}\``);
  if (create[3] !== CHILD_ADDR)
    fail(`the child navigable was announced at \`${create[3]}\` and this document opened \`${CHILD_ADDR}\` — ` +
         'the address is what the peer instance would be provisioned to load, so an operation that took it ' +
         'from anywhere but the call that named it would materialize the wrong document');
  if (create[4] !== new URL(CHILD_ADDR).origin)
    fail(`the child's origin was announced as \`${create[4]}\` and its address serializes to ` +
         `\`${new URL(CHILD_ADDR).origin}\` — the origin is the peer's PRINCIPAL and is the half of the agent ` +
         'cluster key SECURITY.md requires to be browser-stated');
  if (create[2] !== 'opener')
    fail(`the notice names \`${create[2]}\` as the creator and this document was init'd as \`opener\` — the ` +
         'creator is what says which instance the child is a child OF, and a wrong one routes the peer\'s ' +
         'answers to a document that did not open it');
  if (create[1] === create[2] || create[1] === '')
    fail(`the child navigable was announced under the creator's own name (\`${create[1]}\`) — the name is ` +
         'minted in the creating instance and is the whole of how the host knows which instance to provision ' +
         'and route to');
  if (create[5] === '')
    fail('the create notice carries no top-level creation URL — HTML §8.1.3.1 makes it the creator\'s to ' +
         'state and the peer cannot derive it, so an empty one is a document the peer would build with no ' +
         'answer for a fact it is required to have');
  /* CSP §2.2's SELF-ORIGIN OF THE INHERITED LIST, AND IT IS THE CREATOR'S — which is the whole of what this
     field exists to carry and the one thing the peer could never derive. §2.2 "Policies" makes a CSP list "a
     struct consisting of policies (a list of policies) and a self-origin (an origin which is used when
     matching the 'self' keyword)"; §2.2.2 "Parse response's Content Security Policies" would answer it with
     the RECEIVING document's URL's origin, which is the child's. So the check is not "is it non-empty" but "is
     it the OPENER's": a record that carried the child's origin here would be the defect wearing the fix's
     shape, and it is the one comparison that tells them apart.
     THIS REPLACED A CHECK THAT DEMANDED THE POLICY BE EMPTY. That check existed because navigable.c DCHECKed a
     cross-instance child could not inherit at all — the crash that named this capability — and it goes with
     the crash rather than being kept beside it. This document sends no CSP, so the policy IS empty here; what
     is asserted is the self-origin, which is present either way because §7.1.7 clones the container whole. */
  if (create[6] !== new URL(OPENER_ADDR).origin)
    fail(`the create notice's CSP self-origin is \`${create[6]}\` and the CREATOR's origin is ` +
         `\`${new URL(OPENER_ADDR).origin}\` — CSP §2.2 makes a CSP list a struct of policies AND a ` +
         'self-origin, and §7.1.7\'s clone keeps the origin it was cloned FROM. A self-origin that is the ' +
         'child\'s address means `script-src \'self\'` on the creator would permit the child\'s origin and ' +
         'refuse the creator\'s — the finding reported live where a browser blocks it, and blocked where a ' +
         'browser runs it');
  /* HTML §7.1.4's EMBEDDER POLICY ITEM OF THAT SAME CLONE, whose four fields cross for the reason the
     self-origin does: §7.1.7's clone-a-policy-container moves EVERY item of a container, and the peer instance
     can derive none of them — the item belongs to the CREATOR's response and the child's own response is the
     only place a header could be read. This opener is init'd with no response headers at all, so §7.1.4's
     obtain answered its own initial value; what is asserted is that the record SAYS so in the section's own
     token rather than leaving the field empty, because an embedder policy has no absence to spell. */
  if (create[7] !== 'unsafe-none' || create[9] !== 'unsafe-none')
    fail(`the create notice's §7.1.4 embedder policy values are \`${create[7]}\`/\`${create[9]}\` and this ` +
         'document was init\'d with no response headers, so HTML §7.1.4\'s obtain answered `unsafe-none` for ' +
         'both. A field that is empty or carries something else is an engine that stopped writing the item, ' +
         'and the peer would create the child claiming a policy no response ever stated');
  if (create[8] !== '' || create[10] !== '')
    fail(`the create notice carries a §7.1.4 reporting endpoint (\`${create[8]}\`/\`${create[10]}\`) and no ` +
         'response header on this document could have named one — §7.1.4 makes both endpoints the EMPTY ' +
         'STRING initially and only a `report-to` parameter writes one');
  /* HTML §7.3.1.3's PARENT NAVIGABLE, AND THIS PHASE IS THE ONE THAT PROVES THE `u` ARM RATHER THAN THE OTHER.
     The opener runs `window.open(CHILD_ADDR, "child")`, which §7.3.1.7 step 8 makes an AUXILIARY navigable —
     created out of a target name with no element anywhere in the algorithm — so it has a full §7.1.7 container
     and NO parent, and the record must say so in the encoding's own undefined rather than by leaving the field
     empty. That pairing is the whole reason the parent is a field of its own: an auxiliary navigable and a
     child navigable differ here and NOWHERE ELSE on this record, so a gate that only checked the container
     would pass for a build that had confused the two. */
  if (create[11] !== 'u')
    fail(`the create notice's §7.3.1.3 parent navigable is \`${create[11]}\` and this document created the ` +
         'child with `window.open`, which HTML §7.3.1.7 step 8 makes an AUXILIARY navigable — a top-level ' +
         'traversable that is its own top and links back through `opener`. `u` is core/frame/remote_object.h\'s ' +
         'undefined and is the positive statement that there is no parent; anything else here would give the ' +
         'peer instance a frame tree the creator never built');
  /* AND §7.3.1.3's OTHER LINK, ASSERTED ON THE SAME ARM AND FOR THE SAME REASON. An auxiliary navigable has no
     element presenting it either — §7.3.1.7 step 8 creates one out of a target name — so Permissions Policy
     §9.5's "container is null" is the CORRECT answer here, and the record must state it in that grammar's own
     word rather than by leaving the field empty. Checking it beside the parent is what makes the pair
     meaningful: the two links are true or false together, so a build that computed §9.5 over some element it
     found anyway would send a feature map here and fail this line. */
  if (create[12] !== 'null')
    fail(`the create notice's §7.3.1.3 container statement is \`${create[12]}\` and this document created the ` +
         'child with `window.open` — HTML §7.3.1.7 step 8 makes that an AUXILIARY navigable, which no element ' +
         'presents, so Permissions Policy §9.5\'s "container is null" is what the record must say. A feature ' +
         'map here would be §9.5 run over an element that presents some other navigable, and the peer would ' +
         'inherit a policy from a frame it is not in');
  /* AND HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST, ASSERTED ON THE SAME ARM AND
     COMPLETING THE SAME TRIPLE. An auxiliary navigable has no CONTAINER DOCUMENT — §7.3.1.7 step 8 creates it
     out of a target name, with no element anywhere in the algorithm — so §3.1.3's steps 2-3 return an EMPTY
     output, and the record must say so in that grammar's own word rather than by leaving the field blank. It
     is the third member of a set that is true or false TOGETHER: no parent, no container, no ancestors. A
     composed list here would be §3.1.3 run against a parent this navigable does not have, and the peer would
     answer `location.ancestorOrigins` with a frame tree the creator never built — which no page could tell
     from the truth, since the member's whole job is to report a tree the reader cannot otherwise see. */
  if (create[13] !== 'none')
    fail(`the create notice's §3.1.3 ancestor origins statement is \`${create[13]}\` and this document ` +
         'created the child with `window.open` — HTML §7.3.1.7 step 8 makes that an AUXILIARY navigable, ' +
         'which has no container document, so §3.1.3\'s step 3 returns the empty list and `none` is what the ' +
         'record must say. A list here would give the peer ancestors it has none of');
  /* AND HTML §7.1.5 "Sandboxing"'s CREATION SANDBOXING FLAG SET, WHICH THIS PHASE PROVES THE EMPTY ARM OF AND
     PROVES BY A DIFFERENT SENTENCE FROM THE THREE ABOVE. The other three are empty because an auxiliary
     navigable has no CONTAINER; this one is empty because §7.1.5 answers a navigable with no embedder from its
     POPUP SANDBOXING FLAG SET, and §7.1's rules for choosing a navigable fill that only "if sandboxingFlagSet's
     sandbox propagates to auxiliary browsing contexts flag is set" — this opener is not sandboxed at all, so
     nothing propagates. The record must say so in that section's own word for the empty set rather than by
     leaving the field blank, for the reason the other three must: an empty field is a hole a reader defaults,
     and the value a reader would default it to is exactly the wrong one.
     WHAT IS NOT PROVEN HERE IS THE NON-EMPTY ARM, and this gate cannot prove it: reaching it needs an
     `<iframe sandbox>` in the opener's markup, which makes the child's origin OPAQUE (§7.3.2.1's determine the
     origin) and therefore its own agent cluster — a second cross-origin peer of a different shape than this
     phase provisions. It is named rather than left implied, because a gate that only ever sees `none` on a
     field would pass for a build that hardcoded it. */
  if (create[14] !== 'none')
    fail(`the create notice's §7.1.5 creation sandboxing flag set is \`${create[14]}\` and this document ` +
         'created the child with `window.open` from an UNSANDBOXED page — §7.1.5 answers a navigable with no ' +
         'embedder element from its POPUP sandboxing flag set, which §7.1\'s rules for choosing a navigable ' +
         'fill only when the opener\'s own set carries the sandbox-propagates-to-auxiliary flag, and this ' +
         'opener\'s set is empty. `none` is that grammar\'s word for the empty set; flags here would put the ' +
         'peer\'s document under a sandbox nothing in this fixture asks for');
  /* AND WHAT THE NAVIGATION IS EVIDENCE OF — CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three words, and
     this fixture proves the MIDDLE one, which is the arm the host's whole cross-instance surface hangs on.
     The opener RUNS `window.open(CHILD_ADDR, "child")`: code ran, so `observed` is unreachable (that word is
     "a real load of this document makes exactly this request", whose first conjunct is HTML §4.12.1 "The
     script element"'s parser-inserted flag and belongs to the park register); and no branch in this fixture
     stands on an arm its own concrete example contradicts, so the flow's `path_forced` is clear and the
     answer is `derived` rather than `forced`.
     ASSERTING THE EXACT WORD RATHER THAN MEMBERSHIP IS THE POINT. A membership test would pass for an engine
     that answered `forced` for everything, and `forced` is the arm both hosts REFUSE TO LOAD at an unwidened
     origin — so an engine that composed this field the wrong way round would provision no peer at all and
     this gate would report the refusal as a design decision. */
  if (create[15] !== 'derived')
    fail(`the create notice states the provenance \`${create[15]}\` and this document created the child by ` +
         'RUNNING `window.open` on a path that stood on no contradicted arm — solver/engine.h composes that ' +
         'as `derived`. `observed` would be an engine claiming a real load of this document makes this ' +
         'request, and `forced` is the arm both hosts decline to load at an unwidened origin, so a wrong ' +
         'word here is a peer that is never provisioned or a reply carried into the observed pool');
  if (create.slice(16).join('\t') !== '')
    fail(`the create notice carries an inherited policy (\`${create.slice(16).join('\t')}\`) and this ` +
         'document was init\'d with no response headers at all — so the creator\'s container holds a policy ' +
         'that came from nowhere this gate can name');
  console.log(`${TAG}   a flow RAN behind the frame boundary: ${steps} step(s), child \`${create[1]}\` ` +
              `announced at ${create[3]} (origin ${create[4]}, tlu ${create[5]}, csp self-origin ` +
              `${create[6]}) by creator \`${create[2]}\``);
  /* NO `teardown()` HERE, AND THAT IS DELIBERATE RATHER THAN AN OMISSION. `Teardown` is the call that makes the
     runtime walk gc_obj_list and report a leaked GC object; this frontier is mid-flight ON PURPOSE — the drive
     stopped at the notice — so its live flow state is reachable and a walk would report it as a leak. The
     probe in phase 1 tears down instances whose work is finished, which is where that check belongs. Removing
     the frame is the whole teardown of the instance either way: the module dies with the document. */
  opener.destroy();
  registryLine('after phase 4');

  /* ── PHASE 5 — THE IDS, ACCOUNTED FOR. Every id this table ever minted is registered, terminated or failed
     to launch, so the three counts must account for the whole id space it issued. The registry asserts that
     after every mutation; this is the same arithmetic read from outside, over the ids this gate actually saw. */
  console.log(`${TAG} ── phase 5: routing ids ──`);
  const end = registryLine('final');
  const sorted = mintedIds.slice().sort((a, b) => a - b);
  console.log(`${TAG}   ids observed: ${sorted.join(',')}   nextRoutingId=${end.reg.nextRoutingId}`);
  for (let i = 1; i < sorted.length; i++)
    if (sorted[i] === sorted[i - 1]) fail(`routing id ${sorted[i]} names two renderers`);
  if (end.reg.live + end.reg.terminated + end.reg.failed !== end.reg.nextRoutingId - 1)
    fail(`the registry does not account for every routing id it minted: live=${end.reg.live} ` +
         `terminated=${end.reg.terminated} failed=${end.reg.failed} nextRoutingId=${end.reg.nextRoutingId}`);
  if (end.reg.live !== 0 || end.st.live !== 0 || end.st.frames !== 0)
    fail(`this gate did not leave the document empty — ${end.reg.live} agent cluster(s) still registered over ` +
         `${end.st.frames} frame(s), which is a cluster refused its next instance and a WASM heap resident ` +
         'under a document that does not reload');
  if (end.st.forked !== end.reg.launched + end.reg.failed)
    fail(`this zone settled ${end.st.forked} fork(s) while the registry recorded ` +
         `${end.reg.launched} launch(es) and ${end.reg.failed} failure(s) — the two are one fact written in ` +
         'the same turn, so a difference is a frame materialized outside the admission path');
} catch (e) {
  /* AN ASSERTION THAT FIRED IS THE RESULT OF THIS RUN, NOT AN INTERRUPTION OF IT — it names either an
     invariant to fix at its root or a capability to build, in the file whose name it carries. */
  fail('an assertion fired while driving the browser-process layer:\n' + String((e && e.stack) || e));
}

if (_workerErrors.length)
  fail(`${_workerErrors.length} renderer frame(s) died on an uncaught throw:\n` + _workerErrors.join('\n'));

console.log(`${TAG} OK at ${REV} — two cross-origin renderers provisioned through the registry (bundle ids ` +
            `${probe.peers.map((p) => p.bundleId).join(' ')}), all three CHECK-class refusals fired and cost ` +
            `nothing (a second renderer for a live cluster, an id never minted, a renderer reported dead ` +
            `twice), a flow ran behind the frame boundary and its navigable.create notice crossed the typed ` +
            `wire, ids minted ${mintedIds.join(',')} and never reused`);
shutdown();
process.exit(0);
