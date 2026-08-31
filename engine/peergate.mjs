/* THE NATIVE CROSS-INSTANCE TRANSPORT'S GATE — two PROCESSES, two ORIGINS, one synchronous read that cannot be
 * answered locally. `node engine/peergate.mjs [path-to-native-binary]`, after `node engine/build.mjs native`.
 *
 * IT EXISTS BECAUSE THE CAPABILITY IT MEASURES EXISTS AND NOTHING REACHED IT. SECURITY.md's rule is that a host
 * which cannot PROVISION a second instance has not tested the transport, and that every cross-instance
 * mechanism is then a design that has never run. `engine/trusted.mjs` provisions one — as a child `--abi`
 * process of the SHIPPED `qjs_*` ABI — and it was reachable only by hand, against a live URL, from no gate at
 * all. `engine/route.mjs` is this file's WASM sibling and drives two emscripten instances inside ONE process;
 * this one drives two OS PROCESSES over pipes, which is the shape SECURITY.md actually names ("in a process
 * that means a child PROCESS ... over a pipe") and the shape whose failures are lifetime failures rather than
 * heap ones.
 *
 * ── WHAT WOULD MAKE THIS FILE PASS FOR THE WRONG REASON, AND WHAT MAKES EACH OF THOSE IMPOSSIBLE ────────────
 * This section is the point of the file. The defect it was written against is not a bug in the engine: it is
 * that `route.mjs`'s fixture was COMPENSATING for one. Its peer document fetched `/hold`, a request the driver
 * never answered, so the peer's boot flow stayed PARKED and therefore stayed ALIVE long enough to answer a
 * cross-origin read. A real peer has no `/hold`. The gate was green on a property of the fixture, and the
 * missing `qjs_set_referenced` ABI entry — without which a provisioned peer runs its scripts, drains, closes,
 * and the creator's first `otherW.length` arrives at an instance with no timeline to run the getter in —
 * survived underneath it. So:
 *
 *   W1  A PEER KEPT ALIVE BY UNFINISHED WORK RATHER THAN BY `referenced`. Closed TWICE, and the second is the
 *       one that holds if this file is edited later. (a) NEITHER peer document makes any request: no script
 *       fetch, no subresource, no `src` on either iframe. (b) The gate asserts AT THE WIRE that the set of
 *       paths this server was asked for at the PEER AUTHORITY is exactly {/peer, /peer-closed} — so a `/hold`
 *       added to a fixture is a third path and FAILS, and it fails whether it was added on purpose or by
 *       someone reaching for a way to make a red gate green. Note also that the native zone cannot reproduce
 *       route.mjs's artifact even if asked to: a script-initiated `fetch()` is DECLINED by trusted.mjs, a
 *       declined round pays nothing, and `test_forced.c`'s `--abi` arm treats an unpaid stall at an
 *       UNREFERENCED instance as `abi_stalled()`'s DFAIL. A `/hold` here aborts the peer; it cannot hold it.
 *
 *   W2  THE READ ANSWERED OUT OF THE ASKING AGENT'S OWN RECORD. Both crossing members are chosen so that the
 *       local answer is a DIFFERENT VALUE from the true one, which is the only construction that can tell them
 *       apart. `peerA.length` is HTML §7.2.2.2 "Indexed access on the Window object"'s "number of
 *       document-tree child navigables" OF THE PEER'S ACTIVE DOCUMENT: `/peer` carries two srcless `<iframe>`s,
 *       so the true answer is 2, while window_proxy.c's local arms answer 0 for every state in which they
 *       answer at all. `peerB.closed` is §7.2.2.1 "Opening and closing windows"' `closed`, whose is-closing
 *       half is written BY THE AGENT THAT RAN close() — `/peer-closed` closes itself — so the creator's own
 *       record says `false` for ever and only the peer's says `true`. A gate that asserted `length === 0` or
 *       `closed === false` would be satisfied by a local answer and would be measuring nothing.
 *
 *   W3  A CROSS-ORIGIN PAIR THAT IS NOT ACTUALLY CROSS-ORIGIN. CLAUDE.md: "provisioning one for a same-origin
 *       child tests a transport that must never carry it" — a same-origin pair is ONE agent cluster, answered
 *       in-heap by navigable.c's `child_in_this_agent`, and no process is provisioned at all. The gate asserts
 *       the two serialized origins differ, and then asserts it again where it cannot be faked: every request
 *       this server received carries the `Host` field the client sent, and the peer documents must arrive
 *       under `localhost:<port>` while the seed arrives under `127.0.0.1:<port>`.
 *
 *   W4  A §7.2.1 FILTER THAT THROWS FOR EVERYTHING. CLAUDE.md names this exactly: "a filter that threw for
 *       everything passes a test that only checks the throw". So BOTH halves are asserted from ONE page, in
 *       one run: `peerA.document` must throw a SecurityError (§7.2.1.3.1 CrossOriginProperties ( O )'s list
 *       does not contain `document`, and §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s last step is "Throw a
 *       SecurityError DOMException"), and `peerA.self === peerA` must answer TRUE without throwing (`self` is
 *       on that list with [[NeedsGetter]] true). Either one alone is a test of nothing.
 *
 *   W5  A DRIVER THAT DIED BEFORE THE CHECKS IT LOOKS LIKE IT MADE. CLAUDE.md's named shape: "a driver
 *       aborting on its FIRST reply because a required field was added to the reply record, so every check
 *       below that line is unreachable while the file still looks like a passing gate." Every check in this
 *       file is DECLARED up front in one table, every one of them RUNS, and the gate refuses to report a
 *       verdict unless the number of checks that produced a result equals the number declared. An ABSENT
 *       observation and a FALSE one are never the same verdict: each check reports `missing` or `wrong` with
 *       the value it actually saw.
 *
 *   W6  THE HARNESS'S OWN CLOCK REPORTED AS THE ENGINE'S DEFECT. §Testing: a measurement a loaded machine can
 *       falsify is not a measurement. There is a backstop here because a deadlocked pipe consumes no CPU and
 *       no other signal can see it, and it is GENEROUS, it is reset by any PROGRESS (a request arriving, a
 *       byte written by the child) rather than by elapsed time alone, and it reports through a verdict of its
 *       OWN — `BACKSTOP`, with the load average beside it — which is never collapsed into a check failure.
 *
 * ── WHAT THIS GATE DOES NOT MEASURE, SAID RATHER THAN IMPLIED ────────────────────────────────────────────────
 * CLAUDE.md requires that a peer answer BY RUNNING A PROGRAM — the IDL getter §7.2.1 defines the member as, on
 * a flow of the peer's own frontier — and this file proves that the answer came FROM the peer's document (W2)
 * without independently witnessing the flow base it ran on. The evidence for that is in the PEER's own result
 * document (`_flows` greater than its boot count, `_worldSegmentsMade` non-zero because the getter runs under
 * the ASKING world's segment), and `trusted.mjs` deliberately does not print a peer's `@RESULT`: it reports
 * only its LENGTH, on the ground that merging two documents' finding sets is a grammar it does not have. That
 * is right about merging and it leaves the counters unreadable from out here. Closing it is a change to
 * `trusted.mjs`'s per-peer report — the peer's `@RESULT` is already parsed into `i.result` there and nothing
 * reads it — and it is named here rather than left as an assertion this file appears to make and does not.
 *
 * ── THE BYTES ARE FROZEN, WHICH IS WHY THIS IS A GATE AND NOT A RUN AGAINST A SITE ──────────────────────────
 * §Testing: "ONE RUN OF A LIVE SITE IS NOT A MEASUREMENT, AND A BEFORE/AFTER BUILT FROM TWO OF THEM IS AN
 * ARTIFACT OF THE SITE." Every document below is a literal in this file. The ONE thing that varies between
 * runs is the loopback PORT, which the OS assigns; it is substituted once, and the exact bytes served are
 * printed with the verdict so a failing run's input is not a thing anybody has to reconstruct. */
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { createContext, runInContext } from 'node:vm';
import { loadavg } from 'node:os';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const EXT_DIR = join(ENGINE, '..', 'extension');

/* TWO AUTHORITIES, ONE PORT, AND THE PAIR IS A CHOICE WITH A REASON RATHER THAN A CONVENIENCE. HTML's origin
   is the (scheme, host, port) tuple, and `127.0.0.1` and `localhost` are two different HOSTS — an IPv4 address
   and a domain — so one listening socket serves two ORIGINS with no second port, no TLS and no /etc/hosts
   edit. They also both classify PRIVATE under `safe-fetch.js` (`_isPrivateHost` answers true for the literal
   "localhost" and for the IPv4 loopback block), which is what makes the pair reachable at all: that file
   blocks a private target only when the PAGE principal is not itself private, so private->private is allowed
   by its own rule. That is asserted below against the real file rather than asserted here in prose. */
const SEED_HOST = '127.0.0.1';
const PEER_HOST = 'localhost';

/* ── THE FIXTURES ────────────────────────────────────────────────────────────────────────────────────────────
   THE ORDER OF THE STATEMENTS IN THE SEED IS LOAD-BEARING AND IS NOT STYLE. Each `window.open` of a `/beacon/`
   address is this page reporting one observation to the wire, and a read that never comes back parks the flow
   that would have made the reports after it. So the two LOCAL reads (W4's pair) are reported FIRST, the
   crossing `length` next, and the crossing `closed` — the one with a real teardown question under it, see
   below — LAST. A failure of the last therefore leaves the first three measured instead of erasing them.

   WHY THE BEACON IS A NAVIGATION AND NOT A `fetch()`. `fetch()` would be the obvious channel and it cannot be
   used: trusted.mjs fires only OBSERVED parks (the seed, and a parser-inserted `<script src>` of it), a
   script-initiated fetch is DECLINED, a declined round pays nothing, and `abi_pay() == 0` at an unreferenced
   instance is `abi_stalled()`'s DFAIL. The seed would abort before printing anything. A same-origin
   `window.open` instead reaches navigable.c's `child_in_this_agent` arm, becomes §7.4.5's load job for a
   navigable THIS instance holds, and asks the zone `document.fetch` with the load's own provenance — which
   for an address the page's own code composed on a path that stood on no contradicted arm is `derived`, and a
   `derived` navigation is one trusted.mjs performs. The bytes then come through the ONE chokepoint like every
   other byte, which is also what makes the observation trustworthy: it is `safe-fetch.js` that fetched it.

   AND THE OBSERVATIONS RIDE THE PATH RATHER THAN A QUERY STRING, because the whole address is what this
   server sees and the path is the half no consumer of a result document has to be asked about. */
const seedDoc = (port) => `<!doctype html>
<script>
var peerA = window.open("http://${PEER_HOST}:${port}/peer", "peerA");
var onlist;
try { onlist = "ok-" + (peerA.self === peerA); } catch (e) { onlist = "throw-" + e.name; }
window.open("/beacon/onlist-" + onlist);
var offlist;
try { offlist = "value-" + (typeof peerA.document); } catch (e) { offlist = "throw-" + e.name; }
window.open("/beacon/offlist-" + offlist);
window.open("/beacon/len-" + (typeof peerA.length) + "-" + peerA.length);
var peerB = window.open("http://${PEER_HOST}:${port}/peer-closed", "peerB");
window.open("/beacon/closed-" + (typeof peerB.closed) + "-" + peerB.closed);
</script>`;

/* THE PEER WHOSE `length` IS A FACT THE ASKER CANNOT COMPUTE. Two SRCLESS iframes: §7.4 creates each child
   navigable with the initial `about:blank` and no response, so this document makes NO request of any kind —
   which is W1's first half — and §7.2.2.2's count of them is 2 in the peer and 0 in anything answering
   locally. There is no script here at all, deliberately: a peer with no script has ONE timeline, so the read
   is answered once and the asking flow does not fork per answer, and a `length` that came back as anything
   other than 2 is a defect in the transport rather than in this file's arithmetic. */
const PEER_DOC = `<!doctype html><iframe></iframe><iframe></iframe>`;

/* THE PEER THAT CLOSES ITSELF — §7.2.2.1's close(), run in the agent that holds the document, which is the
   whole content of the member. The creator holds a WindowProxy for the same traversable and its own `closing`
   byte is never written, so its local answer is `false` for ever about a window that has closed.
   THIS ONE HAS A REAL QUESTION UNDER IT AND THE ANSWER IS THE GATE'S TO REPORT RATHER THAN TO AVOID. §7.2.2.1
   step 6.2 queues DEFINITELY CLOSE, whose step 3 destroys the traversable — so this peer is asked a
   cross-instance read AFTER its own document has been through §7.3.1.6 "Navigable destruction". That is
   exactly the case CLAUDE.md says reclamation must be a MECHANISM for rather than a `free`, and if this check
   fails while the three before it pass, the finding is about destroy-a-navigable under a held reference and
   not about this fixture. It is placed last for that reason. */
const PEER_CLOSED_DOC = `<!doctype html><script>window.close();</script>`;

/* A BEACON'S REPLY IS A DOCUMENT WITH NOTHING IN IT — no script, no subresource, nothing to fetch — so a
   beacon can never itself become a request this gate then has to explain. */
const BEACON_DOC = `<!doctype html><title>beacon</title>`;

/* ── THE CHOKEPOINT, IN A REALM OF ITS OWN ───────────────────────────────────────────────────────────────────
   Loaded exactly as `engine/trusted.mjs` loads it and for the same reason: this gate makes a CLAIM about
   `safe-fetch.js`'s private-network rule (that `127.0.0.1` -> `localhost` is allowed because both classify
   private), and a claim about a file is checked by running that file rather than by restating its rule here.
   A second copy of the rule would be the drift SECURITY.md's one-chokepoint design exists to prevent, and it
   would drift in the one direction that matters: this gate would go on passing after the real file stopped
   allowing the pair, and would report a transport failure as a policy that had not changed. */
function loadChokepoint() {
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  createContext(sandbox);
  for (const f of ['check.js', 'lib/safe-fetch.js'])
    runInContext(readFileSync(join(EXT_DIR, f), 'utf8'), sandbox, { filename: join(EXT_DIR, f) });
  if (typeof sandbox.safeFetch !== 'function')
    throw new Error('extension/lib/safe-fetch.js installed no `safeFetch` — this gate asks that file whether ' +
                    'the two loopback authorities may reach each other, and a load that installs nothing ' +
                    'leaves the question unasked rather than answered');
  return sandbox;
}

/* ── THE CHECK TABLE ─────────────────────────────────────────────────────────────────────────────────────────
   DECLARED BEFORE ANYTHING RUNS, which is W5's mechanism. Each entry states what it is evidence OF and which
   wrong-reason it closes; each produces exactly one of `pass`, `wrong` (an observation was made and it is not
   the one required) or `missing` (no observation was made at all). Those last two are held apart everywhere
   they are reported, because §@S's rule — a rung whose ABSENCE and whose ZERO read alike is three states
   behind one answer — is about precisely this, and the report is where a gate performs it. */
const CHECKS = [
  ['pna', 'the real `safe-fetch.js` performs a DERIVED document load from the seed authority to the PEER ' +
          'authority — both its private-network rule (private->private is allowed; a public page reaching a ' +
          'private host is not) and its firing policy for that grade. If it refuses, everything below is ' +
          'measuring a refused fetch and not a transport (W3)'],
  ['origins', 'the two authorities serialize to DIFFERENT origins, so the peer is a second agent cluster and ' +
              'not a second realm of the seed\'s heap (W3)'],
  ['exit', '`trusted.mjs` ended with status 0 — a peer that aborted, a seed that reached `abi_stalled()`, or ' +
           'a zone that threw all land here, and every check below one of those would be unreachable (W5)'],
  ['result', 'the seed printed an `@RESULT` document and it parses — an ABSENT result and a result that found ' +
             'nothing are different facts, and this gate must never report the first as the second (W5)'],
  ['seedwire', 'the seed document was fetched under the Host field `' + SEED_HOST + ':<port>` (W3)'],
  ['peerwire', 'both peer documents were fetched under the Host field `' + PEER_HOST + ':<port>` — the ' +
               'cross-origin half happened on the wire and not only in a comment (W3)'],
  ['nohold', 'the peer authority was asked for NOTHING but its two documents — no `/hold`, no subresource, ' +
             'no third path keeping a peer alive by unfinished work instead of by `referenced` (W1)'],
  ['onlist', '`peerA.self === peerA` answered TRUE across origins without throwing — §7.2.1.3.1 lists `self` ' +
             'with [[NeedsGetter]] true, and without this half the SecurityError check below proves nothing ' +
             'about a filter that simply throws for everything (W4)'],
  ['offlist', '`peerA.document` threw a SecurityError across origins — §7.2.1.3.1\'s list does not contain ' +
              '`document`, and §7.2.1.3.2\'s last step is the throw rather than `undefined` (W4)'],
  ['length', '`peerA.length` crossed the process boundary and came back as the NUMBER 2 — HTML §7.2.2.2\'s ' +
             'count of the PEER\'s document-tree child navigables. This is the check `qjs_set_referenced` ' +
             'exists for: without it the peer drains and closes before the read arrives. A local answer is 0 ' +
             'and a relayed string would be "2" rather than 2 (W1, W2)'],
  ['closed', '`peerB.closed` crossed and came back as the BOOLEAN true — §7.2.2.1 writes is-closing in the ' +
             'agent that ran close(), so the asker\'s own record says false for ever (W2)'],
  ['routed', '`trusted.mjs` reported no record it could not route — a held operation is an asking flow parked ' +
             'on a question nothing answered, which is a failure and not a note'],
  ['peers', 'every peer instance ended with status 0 — a peer that aborts prints its own `@WHY` above this ' +
            'gate\'s verdict, and that is the diagnosis rather than this line'],
];

async function main() {
  const bin = process.argv[2] || join(ENGINE, 'host', 'out', 'qjs-native-none');
  if (!existsSync(bin)) {
    /* NOT A SKIP. A gate that reports green because it could not run is the excluded-test defect with the
       total still looking complete, so an absent binary ENDS this process with a status and a sentence
       naming what to build — CLAUDE.md §Testing puts the build in the main agent's hands, so the sentence
       has to be enough for the party that owns it. */
    console.error(`[peergate] no native host at ${bin}\n` +
                  '[peergate] build it with `node engine/build.mjs native` (which needs `node engine/wpt.mjs` ' +
                  'once, for the vendored lexbor archive). This gate drives the SHIPPED ABI — main.c\'s ' +
                  '`qjs_*` entries through test_forced.c\'s `--abi` arm — over engine/trusted.mjs, and there ' +
                  'is no second engine here to measure instead.');
    process.exitCode = 2;
    return;
  }
  /* THE ARTIFACT THIS RUN MEASURED, NAMED WITH THE RESULT. §Testing: a number quoted without the revision it
     came from is not a measurement, and this gate does not build — so what it can state is exactly which
     binary it drove and when that binary was linked, which is the pair a reader needs to know whether the
     verdict belongs to the tree they are looking at. */
  const st = statSync(bin);
  console.error(`[peergate] driving ${bin} (${st.size} bytes, linked ${st.mtime.toISOString()})`);

  const results = new Map();
  const record = (id, verdict, saw) => {
    if (!CHECKS.some(([c]) => c === id)) throw new Error(`peergate recorded an undeclared check \`${id}\``);
    if (results.has(id)) throw new Error(`peergate recorded the check \`${id}\` twice`);
    results.set(id, { verdict, saw });
  };

  /* EVERY REQUEST THIS SERVER WAS SHOWN, WITH THE AUTHORITY THE CLIENT ADDRESSED IT TO. The Host field is the
     load-bearing part: it is the one place the cross-origin claim is a fact about bytes on a socket rather
     than about a string this file composed. */
  const wire = [];
  let lastProgress = Date.now();
  const progress = () => { lastProgress = Date.now(); };

  const server = createServer((req, res) => {
    const host = String(req.headers.host || '');
    const path = String(req.url || '');
    wire.push({ host, path, method: req.method });
    progress();
    const html = (body) => {
      res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
      res.end(body);
    };
    if (path === '/creator') return html(seedDoc(port));
    if (path === '/peer') return html(PEER_DOC);
    if (path === '/peer-closed') return html(PEER_CLOSED_DOC);
    if (path.startsWith('/beacon/')) return html(BEACON_DOC);
    /* THE CHOKEPOINT PROBE'S OWN TARGET, ANSWERED 200 SO THE `pna` CHECK IS ABOUT ONE THING. A 404 would also
       prove the request was not BLOCKED, and it would make that check's own failure ambiguous between a
       refused fetch and a fixture that stopped being served — two states behind one answer, at the one seam
       whose whole subject is telling a refusal from an absence. */
    if (path === '/pna-probe') return html(BEACON_DOC);
    /* ANYTHING ELSE IS A 404 AND IS STILL RECORDED, which is deliberate: `nohold` is decided over what was
       ASKED FOR and not over what was served, so a fixture that grew a `/hold` fails this gate whether or not
       this server would have answered it. */
    res.writeHead(404, { 'content-type': 'text/plain' });
    res.end('no such fixture');
  });
  /* DUAL-STACK ON PURPOSE: listening with no host binds `::`, so `localhost` reaches this server whether the
     resolver hands back ::1 or 127.0.0.1, and `127.0.0.1` reaches it as the mapped address. A gate bound to
     one of the two would fail on a machine whose resolver prefers the other, which is a property of the box
     and not of the engine — exactly the artifact §Testing says must not be reported as a defect. */
  await new Promise((res, rej) => { server.once('error', rej); server.listen(0, res); });
  const port = server.address().port;
  const seedOrigin = `http://${SEED_HOST}:${port}`;
  const peerOrigin = `http://${PEER_HOST}:${port}`;
  const seedUrl = `${seedOrigin}/creator`;

  console.error(`[peergate] serving frozen fixtures on port ${port}: seed ${seedUrl}, peers ${peerOrigin}/peer ` +
                `and ${peerOrigin}/peer-closed`);

  /* ── W3, HALF ONE: THE ORIGINS ─────────────────────────────────────────────────────────────────────────── */
  if (new URL(seedOrigin).origin === new URL(peerOrigin).origin)
    record('origins', 'wrong', `${new URL(seedOrigin).origin} === ${new URL(peerOrigin).origin}`);
  else
    record('origins', 'pass', `${new URL(seedOrigin).origin} vs ${new URL(peerOrigin).origin}`);

  /* ── THE PNA QUESTION, PUT TO THE FILE THAT OWNS IT ────────────────────────────────────────────────────────
     Asked with the same three options `trusted.mjs` passes for a peer's document load, because a different
     destination or credential state is a different decision and this gate would then be answering about a
     request nobody makes. The probe's own path is `/pna-probe`, which is neither peer document, so `nohold`
     below excludes it BY NAME rather than by a count this line would have to keep in step with. */
  const ZONE = loadChokepoint();
  /* THE GRADE IS `derived`, WHICH IS THE ONE THE RUN BELOW ACTUALLY DEPENDS ON AND IS NOT A FORMALITY. That
     file reads the provenance BEFORE any byte moves and refuses a `forced` address at an unwidened origin, so
     a probe that stated a different grade — or, since `_provenanceOf` is a fatal `CHECK`, none at all — would
     answer about a request this gate never makes. Every load this run performs is `derived`: the peer
     addresses are literals the seed's own code composed, and each `/beacon/` address is composed from a value
     the run computed on a path that stood on no contradicted arm. */
  const probe = await ZONE.safeFetch(`${peerOrigin}/pna-probe`,
                                     { pageUrl: seedUrl, destination: 'document',
                                       provenance: 'derived', credentialed: false });
  if (!probe || typeof probe.status !== 'number')
    record('pna', 'missing', 'safeFetch returned no reply record at all');
  else if (probe.status === 0)
    record('pna', 'wrong', `the chokepoint refused it: ${probe.statusText}`);
  else
    record('pna', 'pass', `status ${probe.status} — private->private is allowed by that file's own rule`);
  const probeCount = wire.length;

  /* ── THE RUN ───────────────────────────────────────────────────────────────────────────────────────────── */
  const child = spawn(process.execPath, [join(ENGINE, 'trusted.mjs'), seedUrl, bin],
                      { stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '', err = '', spawnError = null;
  child.stdout.on('data', (d) => { out += d; progress(); });
  child.stderr.on('data', (d) => { err += d; process.stderr.write(d); progress(); });
  /* A CHILD THAT NEVER STARTED IS NOT A CHILD THAT EXITED, and Node reports the first as an `error` event and
     no `close` code worth reading. Without this the gate would report `trusted.mjs ended with code null` and
     send the reader hunting a transport failure in a process that does not exist. */
  child.on('error', (e) => { spawnError = e; });

  /* W6's BACKSTOP, AND IT IS A BACKSTOP RATHER THAN A BUDGET. It measures NO PROGRESS — no request arriving,
     no byte written by the child — because a deadlocked pipe is the one failure that consumes no CPU and
     emits no signal, and it is the only thing here a clock can see that nothing else can. It is generous, it
     reports through a verdict of its own, and the load average travels with it: §Testing's four worked
     examples are all one machine under load reporting HOW a thing ran as WHAT ran. */
  const IDLE_MS = 180000;
  let backstop = null;
  const ended = await new Promise((res) => {
    child.on('close', (code, signal) => res({ code, signal }));
    const tick = setInterval(() => {
      if (Date.now() - lastProgress < IDLE_MS) return;
      clearInterval(tick);
      backstop = { idleMs: Date.now() - lastProgress, load: loadavg() };
      child.kill('SIGTERM');
    }, 2000);
    child.on('close', () => clearInterval(tick));
  });
  server.close();
  /* THE KEEP-ALIVE SOCKETS TOO. `close` stops ACCEPTING and waits for live connections to end, and this
     process made one itself (the chokepoint probe, through Node's own pooling `fetch`), so a gate that only
     called `close` would print its verdict and then sit holding a socket open against itself. */
  server.closeAllConnections?.();

  if (spawnError) {
    console.error(`\n[peergate] the child was never started: ${spawnError.message}`);
    process.exitCode = 2;
    return;
  }

  if (backstop) {
    console.error(`\n[peergate] BACKSTOP — this gate's own idle watchdog fired after ${
      Math.round(backstop.idleMs / 1000)}s with no request and no output from the child, load average [${
      backstop.load.map((n) => n.toFixed(2)).join(' ')}]. This is a verdict about the HARNESS and it is NOT ` +
      'one of the checks below: nothing here says the engine is wrong, only that nothing moved. A kill leaves ' +
      'an EMPTY tail where a crash leaves the output that preceded it — the child\'s stderr above this line ' +
      'is the place to read which of the two happened.');
    printFixtures(port);
    process.exitCode = 3;
    return;
  }

  /* ── EXIT AND RESULT ───────────────────────────────────────────────────────────────────────────────────── */
  const ending = ended.signal ? `on ${ended.signal}` : `with code ${ended.code}`;
  record('exit', ended.signal === null && ended.code === 0 ? 'pass' : 'wrong', `trusted.mjs ended ${ending}`);

  /* THE SEED'S RESULT DOCUMENT, READ WHERE THE ZONE PUTS IT AND NOT WHERE THE ENGINE PRINTS IT. The `--abi`
     child writes `@RESULT <json>`; `trusted.mjs` takes that line apart at its own entry and re-emits the JSON
     ALONE on its stdout, reserving stderr for everything it says in its own voice. So a reader that grepped
     for the marker here would find nothing and report an ABSENT result for a session that produced one — the
     defect of measuring what a harness prints instead of what the shipped path writes, one process further
     out than usual. Everything else this process emits is stderr, so the last non-empty stdout line IS the
     document. */
  const resultLine = out.split('\n').map((l) => l.trim()).filter((l) => l !== '').pop();
  if (resultLine === undefined) {
    record('result', 'missing', 'trusted.mjs wrote nothing to stdout — it emits the seed\'s result document ' +
                                'there and nothing else, so this session produced no document at all');
  } else {
    let parsed = null;
    try { parsed = JSON.parse(resultLine); } catch (e) { parsed = null; }
    if (parsed === null || !Array.isArray(parsed.fetchCallSites))
      record('result', 'wrong',
             `trusted.mjs's stdout is not a result document with a \`fetchCallSites\` array: ${
               resultLine.slice(0, 120)}`);
    else
      record('result', 'pass', `${resultLine.length} bytes, ${parsed.fetchCallSites.length} fetch call site(s)`);
  }

  /* ── THE WIRE ──────────────────────────────────────────────────────────────────────────────────────────── */
  const asked = wire.slice(probeCount);
  const at = (host) => asked.filter((r) => r.host === host);
  const seedAsked = at(`${SEED_HOST}:${port}`);
  const peerAsked = at(`${PEER_HOST}:${port}`);
  const strayHosts = [...new Set(asked.map((r) => r.host))]
    .filter((h) => h !== `${SEED_HOST}:${port}` && h !== `${PEER_HOST}:${port}`);

  const seenSeed = seedAsked.some((r) => r.path === '/creator');
  record('seedwire', seenSeed ? 'pass' : (asked.some((r) => r.path === '/creator') ? 'wrong' : 'missing'),
         `Host fields seen: ${JSON.stringify([...new Set(asked.map((r) => r.host))])}`);

  const peerPaths = [...new Set(peerAsked.map((r) => r.path))].sort();
  const wantPeer = ['/peer', '/peer-closed'];
  if (!peerAsked.length)
    record('peerwire', 'missing', 'the peer authority was never asked for anything — no peer was provisioned');
  else if (wantPeer.every((p) => peerPaths.includes(p)))
    record('peerwire', 'pass', `${peerAsked.length} request(s) at ${PEER_HOST}:${port} for ${JSON.stringify(peerPaths)}`);
  else
    record('peerwire', 'wrong', `the peer authority was asked only for ${JSON.stringify(peerPaths)}`);

  /* W1's WIRE HALF. The comparison is over the SET of paths and not over a count, deliberately: a page whose
     flow forked may legitimately have its document loaded more than once and that is exploration rather than
     a defect, while a path that is NEITHER peer document is work keeping an instance alive, which is the
     thing this whole file exists to make impossible to be green underneath. A request at a THIRD authority is
     the same failure wearing a different field and is reported here too. */
  const extraPeerPaths = peerPaths.filter((p) => !wantPeer.includes(p));
  /* "WAS IT ASKED FOR ANYTHING ELSE" IS UNDECIDABLE WHEN IT WAS ASKED FOR NOTHING, and answering it `pass`
     anyway is a VACUOUS pass — the exact three-states-behind-one-answer shape this file is against, performed
     by the very check that exists to prevent it. A run that provisioned no peer has not established that no
     peer was held open by unfinished work; it has established nothing about peers at all. */
  if (!peerAsked.length)
    record('nohold', 'missing', 'the peer authority was never asked for anything, so there is no set of paths ' +
                                'for this check to be about');
  else if (extraPeerPaths.length || strayHosts.length)
    record('nohold', 'wrong',
           `paths at the peer authority outside its two documents: ${JSON.stringify(extraPeerPaths)}; ` +
           `requests at authorities this gate does not serve: ${JSON.stringify(strayHosts)}`);
  else
    record('nohold', 'pass', 'the peer authority was asked for its two documents and nothing else');

  /* ── THE BEACONS, WHICH ARE THE PAGE'S OWN REPORT OF WHAT THE READS ANSWERED ────────────────────────────── */
  const beacons = seedAsked.map((r) => r.path).filter((p) => p.startsWith('/beacon/'));
  const beacon = (id, prefix, want) => {
    const hits = beacons.filter((p) => p.startsWith(`/beacon/${prefix}-`));
    if (!hits.length) return record(id, 'missing', `no /beacon/${prefix}-… request was ever made — the read ` +
                                    'before it never came back, so the page never reached this line');
    if (hits.includes(`/beacon/${prefix}-${want}`))
      return record(id, 'pass', `/beacon/${prefix}-${want}`);
    return record(id, 'wrong', `saw ${JSON.stringify(hits)}, required /beacon/${prefix}-${want}`);
  };
  /* THE CHECK ID AND THE BEACON PREFIX ARE TWO NAMES AND ARE PASSED SEPARATELY, because they answer to two
     different readers — the table above and the page's own source — and `record` refuses an id the table does
     not declare, so a rename on one side stops this gate rather than quietly measuring nothing. */
  beacon('onlist', 'onlist', 'ok-true');
  beacon('offlist', 'offlist', 'throw-SecurityError');
  beacon('length', 'len', 'number-2');
  beacon('closed', 'closed', 'boolean-true');

  /* ── WHAT THE ZONE ITSELF SAID IT COULD NOT DO ─────────────────────────────────────────────────────────────
     `trusted.mjs` reports a record it held for the whole session in two shapes — a document no instance was
     ever provisioned for, and one an instance HELD AND THEN LEFT — and the second is the exact lifetime
     failure this gate is about. Both are matched, because a gate that watched for only one of them would go
     green on the other. */
  const heldLine = /record\(s\) named a document/.test(err);
  record('routed', heldLine ? 'wrong' : 'pass',
         heldLine ? 'trusted.mjs reported held records; its own lines are above this verdict'
                  : 'no held records reported');

  const peerLines = [...err.matchAll(/peer instance \[([^\]]+)\] at (\S+) ended (with code \d+|on \w+)/g)];
  if (!peerLines.length)
    record('peers', 'missing', 'trusted.mjs reported no peer instance at all — none was provisioned');
  else {
    const bad = peerLines.filter((m) => m[3] !== 'with code 0');
    record('peers', bad.length ? 'wrong' : 'pass',
           peerLines.map((m) => `${m[1]} at ${m[2]} ended ${m[3]}`).join(' ; '));
  }

  report(port);

  function report(p) {
    /* W5's GUARD. The verdict is refused unless every declared check produced a result — a driver that died
       between two of these would otherwise leave the ones below it silently unmade while this summary still
       printed a total. */
    const missingChecks = CHECKS.filter(([id]) => !results.has(id)).map(([id]) => id);
    console.error('\n[peergate] ' + '-'.repeat(96));
    for (const [id, why] of CHECKS) {
      const r = results.get(id);
      const mark = !r ? 'NOT RUN' : r.verdict === 'pass' ? 'pass' : r.verdict === 'wrong' ? 'WRONG ' : 'ABSENT';
      console.error(`[peergate] ${mark.padEnd(7)} ${id.padEnd(9)} ${r ? r.saw : ''}`);
      if (!r || r.verdict !== 'pass') console.error(`[peergate]                   ${why}`);
    }
    if (missingChecks.length) {
      console.error(`\n[peergate] FAILED: ${missingChecks.length} declared check(s) never produced a result (${
        missingChecks.join(', ')}). A gate that reports a total over checks it did not make is the shape ` +
        'CLAUDE.md names — a driver aborting on its first reply while the file still looks like a passing ' +
        'gate — so this is a failure of the gate, distinct from a failure of the engine.');
      printFixtures(p);
      process.exitCode = 4;
      return;
    }
    const failed = CHECKS.filter(([id]) => results.get(id).verdict !== 'pass');
    if (failed.length) {
      console.error(`\n[peergate] FAILED: ${failed.length} of ${CHECKS.length} checks — ${
        failed.map(([id]) => `${id}:${results.get(id).verdict}`).join(' ')}`);
      printFixtures(p);
      process.exitCode = 1;
      return;
    }
    console.error(`\n[peergate] OK — ${CHECKS.length}/${CHECKS.length}. A cross-origin peer was provisioned as ` +
                  'a second PROCESS, it outlived its own boot because the zone stated `referenced`, and it ' +
                  'answered a synchronous read whose true value neither the asking agent nor this gate could ' +
                  'have produced locally.');
  }
}

/* THE INPUT, PRINTED WITH EVERY FAILING VERDICT. A gate that reports a number without the bytes it measured
   leaves the next reader reconstructing the fixture out of this file's source, and the port — the one thing
   that is not frozen — is precisely the part they cannot reconstruct. */
function printFixtures(port) {
  console.error('\n[peergate] the exact bytes served this run (the port is the only thing that varies):\n' +
                `--- ${SEED_HOST}:${port}/creator ---\n${seedDoc(port)}\n` +
                `--- ${PEER_HOST}:${port}/peer ---\n${PEER_DOC}\n` +
                `--- ${PEER_HOST}:${port}/peer-closed ---\n${PEER_CLOSED_DOC}\n`);
}

await main();
