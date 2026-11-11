/* The native cross-instance transport's gate: two processes, two origins, and one synchronous read that
 * cannot be answered locally. `node engine/peergate.mjs [path-to-native-binary]`, after
 * `node engine/build.mjs native`.
 *
 * SECURITY.md's rule is that a host which cannot provision a second instance has not tested the transport.
 * `engine/trusted.mjs` provisions one as a child `--abi` process of the shipped `qjs_*` ABI, and this gate
 * drives it: two OS processes over pipes, where `engine/route.mjs`, its WASM sibling, drives two emscripten
 * instances in one process. Every document served is a literal in this file; only the loopback port varies,
 * and the bytes served are printed with a failing verdict.
 *
 * The checks are built so that the gate cannot pass for the wrong reason. Their descriptions cite these:
 *   W1  A peer kept alive by unfinished work and not by `referenced`. Neither peer document makes a request,
 *       and the gate asserts at the wire that the peer authority was asked for exactly {/peer, /peer-closed}.
 *   W2  A read answered out of the asking agent's own record. Each crossing member's local answer differs
 *       from the true one. `peerA.length` is HTML §7.2.2.2 "Indexed access on the Window object"'s "number of
 *       document-tree child navigables" of the peer's active document: 2, where window_proxy.c's local arms
 *       answer 0. `peerB.closed` is §7.2.2.1 "Opening and closing windows"' `closed`, whose is-closing half is
 *       written by the agent that ran close(): true, where the creator's own record says false.
 *   W3  A cross-origin pair that is not cross-origin. A same-origin pair is one agent cluster, answered
 *       in-heap by navigable.c's `child_in_this_agent`, and provisions no process. The two serialized origins
 *       must differ, and the `Host` field of each request must show the peer documents arriving under
 *       `localhost:<port>` and the seed under `127.0.0.1:<port>`.
 *   W4  A §7.2.1 filter that throws for everything. Both halves are asserted from one page: `peerA.document`
 *       throws a SecurityError (§7.2.1.3.1 CrossOriginProperties ( O )'s list does not contain `document`,
 *       and §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s last step is "Throw a SecurityError
 *       DOMException"), and `peerA.self === peerA` answers true without throwing (`self` is on that list
 *       with [[NeedsGetter]] true).
 *   W5  A driver that died before the checks it appears to make. Every check is declared in one table, and
 *       no verdict is reported unless every declared check produced a result. An absent observation
 *       (`missing`) and a false one (`wrong`) are separate verdicts.
 *   W6  The harness's own clock reported as the engine's defect. The backstop measures no progress (no
 *       request, no byte from the child), because a deadlocked pipe consumes no CPU; it reports through its
 *       own `BACKSTOP` verdict with the load average, never as a check failure. */
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { createContext } from 'node:vm';
import { loadZone, processSiteRpc } from './zone.mjs';
import { loadavg } from 'node:os';

const ENGINE = dirname(fileURLToPath(import.meta.url));

/* Two authorities on one port. HTML's origin is the (scheme, host, port) tuple, and `127.0.0.1` and
   `localhost` are different hosts, so one listening socket serves two origins. Both are `loopback` to
   `safe-fetch.js` (`_ipAddressSpace`), which refuses a target only when it is less public than the page
   principal; the `pna` check asks the real file. */
const SEED_HOST = '127.0.0.1';
const PEER_HOST = 'localhost';

/* ── The fixtures ────────────────────────────────────────────────────────────────────────────────────────────
   Which document a read reaches is decided by when it runs. HTML §7.2.2.1 "Opening and closing windows"'
   window open steps navigate the new traversable at step 15.5 and return its WindowProxy at step 19, and
   §7.4.2.2 "Beginning navigation"'s navigate fetches in parallel (step 24), so a read on the line after
   `window.open` reaches the traversable's initial about:blank, which has the creator's origin: `document` is
   readable, `length` is 0 and `closed` is false. The `blank-*` rows state those answers. A read reaches the
   peer's document only after an event that document caused, so each peer posts to its opener once it has run
   (§9.3.3 "Posting messages"), and the crossing rows are read in the seed's `message` handler. The post from
   `/peer-closed` follows its close(), so in every timeline that delivered it, is closing is true.

   Each `window.open` of a `/beacon/` address reports one observation to the wire, and a read that never comes
   back parks the flow that would have made the reports after it, so the handler reports the two local-answer
   rows (W4's pair) before the crossing `length`.

   The beacon is a navigation because trusted.mjs fires only observed parks and declines a script-initiated
   `fetch()`; a declined round pays nothing, and `abi_pay() == 0` at an unreferenced instance is
   `abi_stalled()`'s DFAIL. A same-origin `window.open` reaches navigable.c's `child_in_this_agent` arm,
   becomes §7.4.5's load job for a navigable this instance holds, and asks the zone `document.fetch` with the
   load's own provenance, which is `derived` for an address the page composed on no contradicted arm, so the
   bytes come through `safe-fetch.js`. A `derived` navigation is refused `destination=value` at an empty
   table, which is why the child is widened per signal at both authorities (RUN_PERMITS). The observation
   rides the path, which is the whole of what this server sees, and not a query string. */
const seedDoc = (port) => `<!doctype html>
<script>
var peerA, peerB;
window.addEventListener("message", function (e) {
  if (e.data === "peer-active") {
    var onlist;
    try { onlist = "ok-" + (peerA.self === peerA); } catch (x) { onlist = "throw-" + x.name; }
    window.open("/beacon/onlist-" + onlist);
    var offlist;
    try { offlist = "value-" + (typeof peerA.document); } catch (x) { offlist = "throw-" + x.name; }
    window.open("/beacon/offlist-" + offlist);
    window.open("/beacon/len-" + (typeof peerA.length) + "-" + peerA.length);
  } else if (e.data === "peer-closed") {
    window.open("/beacon/closed-" + (typeof peerB.closed) + "-" + peerB.closed);
  }
});
peerA = window.open("http://${PEER_HOST}:${port}/peer", "peerA");
var blankDoc;
try { blankDoc = "value-" + (typeof peerA.document); } catch (x) { blankDoc = "throw-" + x.name; }
window.open("/beacon/blankdoc-" + blankDoc);
window.open("/beacon/blanklen-" + (typeof peerA.length) + "-" + peerA.length);
peerB = window.open("http://${PEER_HOST}:${port}/peer-closed", "peerB");
window.open("/beacon/blankclosed-" + (typeof peerB.closed) + "-" + peerB.closed);
</script>`;

/* The peer whose `length` the asker cannot compute. Two srcless iframes: §7.4 creates each child navigable
   with the initial `about:blank` and no response, so this document makes no request (W1), and §7.2.2.2's
   count is 2 in the peer and 0 in anything answering locally. Its script runs after both iframes are in the
   tree, so the post says the count is 2. `unsent` is never called, so the post in it is reached only by this
   tool's drive of uncalled code: `actor=tool`. */
const PEER_DOC = `<!doctype html><iframe></iframe><iframe></iframe>` +
                 `<script>function unsent() { opener.postMessage("driven", "*"); }` +
                 `opener.postMessage("peer-active", "*");</script>`;

/* The peer that closes itself: §7.2.2.1's close(), run in the agent that holds the document. The creator
   holds a WindowProxy for the same traversable and its own `closing` byte is never written, so its local
   answer stays `false`. Step 6.1 sets is closing before the post and step 6.2 queues definitely close, which
   destroys the traversable (§7.3.1.6 "Navigable destruction"), so the read may reach a document already
   destroyed. */
const PEER_CLOSED_DOC = `<!doctype html><script>window.close(); opener.postMessage("peer-closed", "*");</script>`;

/* A beacon's reply is a document with no script and no subresource, so a beacon never causes a request. */
const BEACON_DOC = `<!doctype html><title>beacon</title>`;

/* What this gate permits its own probe, row by row. The probe is a `document` load, which Fetch §2.2.5
   "Requests" makes neither script-like nor a subresource, so `destination` reads `value`. At an empty table
   it is refused, and neither default arm carries it: one wants `provenance=observed` and the other
   `actor=page`. The table is one origin and one value per gating row: `_firingRefusal` keys it on the request
   URL's origin, here the peer authority, an ephemeral loopback port this process opened. It is not
   `safeFetchWiden`: `actor=page`, `provenance=observed` and `provenance=forced` stay unpermitted. It is a
   literal and not derived from `safeFetchSignalVector`, because a table built from the probe's own vector
   could not refuse the probe. Written out, a signal added to the registry, an `of` that answers differently
   or an edit to the probe turns `pna` wrong and names the row. */
const PROBE_PERMITS = {
  /* Fetch §2.2.5's `document` destination is neither script-like nor a subresource. */
  destination: ['value'],
  /* This gate composed the address; see the `actor: 'tool'` beside the probe. */
  actor: ['tool'],
  /* The address is one this harness's own code composed; nothing pinned a value in it. */
  provenance: ['derived'],
  /* The document it is composed from is the seed, which this harness named itself. */
  'doc-reach': ['observed'],
  /* No cookie jar in this process, and the probe states `credentialed: false`. */
  cookies: ['no'],
  /* The probe passes no header list, so this zone adds no authority beyond the jar it does not have. */
  'header-authority': ['none'],
  /* A `/pna-probe` path holds no witness this engine determined. */
  witness: ['unpinned'],
  /* No signature parameter and no JWS in `http://localhost:<port>/pna-probe`. */
  'url-authority': ['unknown'],
  /* The one value this row has today; the chokepoint's own residual says why. */
  lineage: ['unknown'],
  /* This process configures no attacker site, so `localhost` is another party's server. */
  reaches: ['another-party'],
};

/* What the child may fire at both authorities: every grade of the page's own act, and no act of this tool's.
   A literal, as PROBE_PERMITS is, so a signal added to the registry is a row this table does not name and the
   run's navigations are refused by name. */
const RUN_PERMITS = {
  destination: ['value'],
  actor: ['page'],
  provenance: ['derived', 'forced'],
  'doc-reach': ['observed'],
  cookies: ['no'],
  'header-authority': ['none'],
  witness: ['unstated'],
  'url-authority': ['unknown'],
  lineage: ['unknown'],
  reaches: ['another-party'],
};

/* The chokepoint, in a realm of its own, loaded as `engine/trusted.mjs` loads it: this gate's claim about
   `safe-fetch.js`'s private-network rule (`127.0.0.1` -> `localhost` is allowed because both classify
   private) is checked by running that file and not by restating its rule. The zone's egress table is stated
   before this returns, so no caller holds an unstated zone; `peerOrigin` is an argument for that. */
function loadChokepoint(peerOrigin) {
  /* The timers, `AbortController` and `ReadableStream` are the wire's idle end (safe-fetch.js's _wireWatched). */
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder, Headers, Response, swRpc: processSiteRpc,
                    setTimeout, clearTimeout, AbortController, ReadableStream };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  createContext(sandbox);
  loadZone(sandbox);
  /* This gate keeps no store between runs, so the HTTP cache restores nothing. */
  sandbox.httpCacheStated(null, null);
  if (typeof sandbox.safeFetch !== 'function')
    throw new Error('extension/lib/safe-fetch.js installed no `safeFetch` — this gate asks that file whether ' +
                    'the two loopback authorities may reach each other, and a load that installs nothing ' +
                    'leaves the question unasked rather than answered');
  /* Asserted at load: without this name the gate would reach the network with the egress decision missing. */
  if (typeof sandbox.safeFetchEgressStated !== 'function')
    throw new Error('extension/lib/safe-fetch.js installed no `safeFetchEgressStated` — this gate cannot ' +
                    'SPEAK its per-origin egress table, so `_firingRefusal` would abort on the probe in a ' +
                    'dev build and answer it from a table nobody stated in a release one');
  /* The statement, once, in the shape that file takes: `{ origin: { signal: [values] } }`. The chokepoint
     refuses an array by name. */
  sandbox.safeFetchEgressStated({ [peerOrigin]: PROBE_PERMITS });
  return sandbox;
}

/* What this gate left running. `trusted.mjs` spawns every `--abi` instance with stderr `inherit` and awaits
   each instance's channel, so an instance that never returns to `abi_line()` outlives the zone, holds this
   process's stderr pipe and burns a core. Leftovers are found by process group, read out of `/proc`, and
   never by a command-line pattern: the child is spawned `detached`, so it is a group leader and its
   descendants are in its group. `comm` is field 2 of `/proc/<pid>/stat`, is parenthesised and may contain
   spaces, so every numeric field is taken after the last `)`. A zombie is excluded: it holds no fd and burns
   nothing. */
const CLK_TCK = 100;   /* `getconf CLK_TCK` on this platform; only ever used to render ticks as seconds */
function procStat(pid) {
  let raw;
  try { raw = readFileSync(`/proc/${pid}/stat`, 'utf8'); } catch { return null; }
  const close = raw.lastIndexOf(')');
  if (close < 0) return null;
  const f = raw.slice(close + 2).split(' ');
  return { pid, comm: raw.slice(raw.indexOf('(') + 1, close), state: f[0], pgrp: Number(f[2]),
           /* utime + stime */
           cpuS: (Number(f[11]) + Number(f[12])) / CLK_TCK };
}
function groupMembers(pgid) {
  if (!Number.isInteger(pgid) || pgid <= 1) return [];
  return readdirSync('/proc').filter((d) => /^\d+$/.test(d)).map((d) => procStat(Number(d)))
    .filter((r) => r !== null && r.pgrp === pgid && r.state !== 'Z');
}

/* The check table, declared before anything runs (W5). Each entry states what it is evidence of and which
   wrong reason it closes, and produces exactly one of `pass`, `wrong` (an observation was made and is not
   the one required) or `missing` (no observation was made). The last two are reported apart everywhere. */
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
  ['closed', '`peerB.closed`, read when `/peer-closed`\'s post arrived, crossed and came back as the BOOLEAN ' +
             'true — §7.2.2.1 writes is-closing in the agent that ran close(), so the asker\'s own record says ' +
             'false for ever (W2)'],
  ['posted', 'both peers\' posts to their opener were delivered to the seed\'s `message` handler: a ' +
             '`windowproxy.post` routed across instances and judged under its sending document\'s record'],
  ['refused', 'a cross-document act this tool composed (a post or operation from a drive of uncalled code) ' +
              'was refused `actor=tool` by the one firing rule, which RUN_PERMITS does not widen; `posted` is ' +
              'its control, the page\'s own posts delivered under the same rule in the same run'],
  ['blankdoc', '`peerA.document` on the line after `window.open` is the initial about:blank, whose origin is ' +
               'the creator\'s (§7.2.2.1 window open steps 15.5 and 19), so it is readable'],
  ['blanklen', '`peerA.length` on the line after `window.open` is the NUMBER 0 — the initial about:blank has ' +
               'no child navigables'],
  ['blankclosed', '`peerB.closed` on the line after `window.open` is the BOOLEAN false — no close() has run in ' +
                  'any document of that traversable yet'],
  ['routed', '`trusted.mjs` reported no record it could not route — a held operation is an asking flow parked ' +
             'on a question nothing answered, which is a failure and not a note'],
  ['peers', 'every peer instance ended with status 0 — a peer that aborts prints its own `@WHY` above this ' +
            'gate\'s verdict, and that is the diagnosis rather than this line'],
  /* The processes this gate left behind. `trusted.mjs` awaits every instance's channel, so a run that ended
     cleanly has an empty process group and this row passes. A non-empty group is an `--abi` instance that
     never returned to its ABI loop and so never observed the closed channel; nothing else makes it visible.
     An instance that reached the loop owing nothing also ends without an `@E`, so silence alone does not
     identify a spinner. */
  ['orphans', 'the child\'s process group was EMPTY once it ended — no `--abi` instance outlived the zone that ' +
              'provisioned it. An instance spinning inside the engine never returns to `abi_line()`, so it ' +
              'observes no closed channel, prints no `@E`, holds this process\'s stderr pipe and burns a core ' +
              'with no payer until something else ends it. This row is the only place such a process is ' +
              'visible at all, and it states each one\'s consumed CPU beside it'],
  /* Named residual: not covered — why an instance stops returning to its ABI loop. The `orphans` row reports
     the process and its CPU. A `referenced` instance whose document has been destroyed owes nothing, is
     blocked on nothing and may not finish (solver/engine.c's `g_referenced` arm turns the finish into
     `FLOW_STEP_OWED`/`STEP_UNIT_AWAIT_PEER` so a later `windowproxy.get` has somewhere to arrive), and this
     gate cannot tell an instance resting there from one going round a rung of the work ladder above it.
     Next diff — a per-instance census row saying whether a member is at the referenced-await arm or above
     it, read off the artifact the zone already prints. Absence shows as — an `orphans` row naming a process
     with minutes of CPU, beside a peer whose census says its frontier owes nothing and is blocked on
     nothing. */
  /* The flow base. `length` and `closed` prove the answer came from the peer's document (W2) and not what
     ran there; the requirement is that a peer answer by running a program on a flow of its own frontier,
     under the asking flow's world. Only that raises `_worldSegmentsMade`: solver/world.c's `world_segment`
     DCHECKs `w.doc != g_doc`, so a segment is materialized for a foreign world only. It is read from the
     peer's own `@RESULT`, which `trusted.mjs` prints under that peer's tag. */
  ['peerflow', 'a peer instance\'s OWN result document reports `_worldSegmentsMade` at least 1 — the asking ' +
               'agent\'s world arrived in the peer process and a segment was materialized for it, which is ' +
               'the witness that the read was performed as a PROGRAM on the peer\'s frontier under the ' +
               'asker\'s timeline rather than answered out of anything either side already held. No local ' +
               'flow of the peer can raise it (world_segment refuses a world of its own document) and neither ' +
               'can this gate'],
];

async function main() {
  const bin = process.argv[2] || join(ENGINE, 'host', 'out', 'qjs-native-none');
  if (!existsSync(bin)) {
    /* Not a skip: an absent binary ends this process with a status and a sentence naming what to build. */
    console.error(`[peergate] no native host at ${bin}\n` +
                  '[peergate] build it with `node engine/build.mjs native` (which needs `node engine/wpt.mjs` ' +
                  'once, for the vendored lexbor archive). This gate drives the SHIPPED ABI — main.c\'s ' +
                  '`qjs_*` entries through test_forced.c\'s `--abi` arm — over engine/trusted.mjs, and there ' +
                  'is no second engine here to measure instead.');
    process.exitCode = 2;
    return;
  }
  /* Names the artifact this run measured: the binary driven and when it was linked. */
  const st = statSync(bin);
  console.error(`[peergate] driving ${bin} (${st.size} bytes, linked ${st.mtime.toISOString()})`);

  const results = new Map();
  const record = (id, verdict, saw) => {
    if (!CHECKS.some(([c]) => c === id)) throw new Error(`peergate recorded an undeclared check \`${id}\``);
    if (results.has(id)) throw new Error(`peergate recorded the check \`${id}\` twice`);
    results.set(id, { verdict, saw });
  };

  /* Every request this server was shown, with the authority the client addressed it to. The Host field is
     the cross-origin claim as bytes on a socket. */
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
    /* The chokepoint probe's target, answered 200: a 404 would leave `pna`'s failure ambiguous between a
       refused fetch and a fixture that stopped being served. */
    if (path === '/pna-probe') return html(BEACON_DOC);
    /* Anything else is a 404 and is still recorded: `nohold` is decided over what was asked for. */
    res.writeHead(404, { 'content-type': 'text/plain' });
    res.end('no such fixture');
  });
  /* Dual-stack: listening with no host binds `::`, so `localhost` reaches this server whether the resolver
     hands back ::1 or 127.0.0.1, and `127.0.0.1` reaches it as the mapped address. */
  await new Promise((res, rej) => { server.once('error', rej); server.listen(0, res); });
  const port = server.address().port;
  const seedOrigin = `http://${SEED_HOST}:${port}`;
  const peerOrigin = `http://${PEER_HOST}:${port}`;
  const seedUrl = `${seedOrigin}/creator`;

  console.error(`[peergate] serving frozen fixtures on port ${port}: seed ${seedUrl}, peers ${peerOrigin}/peer ` +
                `and ${peerOrigin}/peer-closed`);

  /* ── W3, half one: the origins ── */
  if (new URL(seedOrigin).origin === new URL(peerOrigin).origin)
    record('origins', 'wrong', `${new URL(seedOrigin).origin} === ${new URL(peerOrigin).origin}`);
  else
    record('origins', 'pass', `${new URL(seedOrigin).origin} vs ${new URL(peerOrigin).origin}`);

  /* ── The PNA question, put to the file that owns it ──
     Asked with the options `trusted.mjs` passes for a peer's document load, since a different destination or
     credential state is a different decision. The probe's request is cut off the front of `wire` by
     `probeCount` before the wire checks read it. */
  /* The table is keyed on the origin the probe's URL serializes to, and the port is known only once the
     server listens, so the peer authority is handed in. The seed authority is absent from this zone's table:
     this process makes one request, at the peer. */
  const ZONE = loadChokepoint(peerOrigin);
  /* The grade is `derived`, the one the run depends on: the peer addresses are literals the seed's code
     composed, and each `/beacon/` address is composed from a value the run computed on no contradicted arm.
     The reach grade is `observed`: the document the address is composed from is the seed, which this harness
     named on its command line. */
  const probe = await ZONE.safeFetch(`${peerOrigin}/pna-probe`,
                                     { destination: 'document', mode: 'navigate',
                                       provenance: 'derived', pinned: 'unpinned',
                                       /* This gate composed the address, so the act is this tool's
                                          (safe-fetch.js's `_actorOf`). */
                                       actor: 'tool', driven: 'unstated',
                                       doc: ZONE.docRecord('observed', 'undriven', 'unpinned', 'none',
                                                           ZONE.safeFetchDocumentAddressSpace(seedUrl, null)),
                                       credentialed: false });
  if (!probe || typeof probe.status !== 'number')
    record('pna', 'missing', 'safeFetch returned no reply record at all');
  else if (probe.status === 0)
    record('pna', 'wrong', `the chokepoint refused it: ${probe.statusText}`);
  else
    record('pna', 'pass', `status ${probe.status} — loopback->loopback is allowed by that file's own rule`);
  const probeCount = wire.length;

  /* ── The run ── */
  /* The child's table at the fixture's two authorities, one row per `--explore-signal`: the page's own acts and
     nothing this tool composes. Every observation reaches the wire as a navigation the page composed (`derived`,
     or `forced` on an arm the run forked), which no default arm carries (`navigate` composes no witness mark),
     so those rows are permitted; `actor=tool` is not, so a drive of uncalled code is judged as a fetch would be
     and refused (`refused`, beside `posted` as its control). */
  /* The child is `detached`, which makes it a process group leader, so `-child.pid` names the zone and every
     instance it provisioned; `trusted.mjs` spawns each `--abi` instance in its caller's group. It is not
     `unref`'d: the child is still this process's to wait for. */
  const child = spawn(process.execPath,
                      [join(ENGINE, 'trusted.mjs'), seedUrl, bin,
                       ...[seedOrigin, peerOrigin].flatMap((o) => Object.entries(RUN_PERMITS)
                         .flatMap(([signal, values]) => values.map((v) => ['--explore-signal', o, `${signal}=${v}`]))
                         .flat())],
                      { stdio: ['ignore', 'pipe', 'pipe'], detached: true });
  let out = '', err = '', spawnError = null;
  child.stdout.on('data', (d) => { out += d; progress(); });
  child.stderr.on('data', (d) => { err += d; process.stderr.write(d); progress(); });
  /* A child that never started raises `error` and no useful `close` code. */
  child.on('error', (e) => { spawnError = e; });

  /* The group id, read from the kernel while the child is alive. Every group-wide signal is gated on it: if
     `detached` did not take effect, `-child.pid` would signal this process's own group, so the kill is
     refused unless the child is a group leader and its group is not ours. */
  const ourPgrp = procStat(process.pid)?.pgrp ?? -1;
  const childStat = child.pid === undefined ? null : procStat(child.pid);
  const pgid = (childStat !== null && childStat.pgrp === child.pid && childStat.pgrp !== ourPgrp)
                 ? child.pid : null;
  if (child.pid !== undefined && pgid === null)
    console.error(`[peergate] the child ${child.pid} is not a process group leader of its own (its group is ` +
                  `${childStat === null ? 'unreadable' : childStat.pgrp}, this process's is ${ourPgrp}), so ` +
                  'no group-wide signal will be sent: an `--abi` instance this run leaves behind will be ' +
                  'reported by the `orphans` row and left for the caller\'s RLIMIT_CPU. A group kill here ' +
                  'would name the group this gate is itself in.');
  /* The one spelling of a group-wide signal, so none skips the gate above. */
  const signalGroup = (sig) => {
    if (pgid === null) return 'no group to signal';
    try { process.kill(-pgid, sig); return `${sig} to group ${pgid}`; }
    catch (e) { return `${sig} to group ${pgid} refused: ${e.code || e.message}`; }
  };
  /* This gate's own death reaps the group too: `engine/build.mjs` wraps each stage in a wall backstop that
     signals the stage, and a peergate killed there would leave the zone and its instances running. `exit`
     covers the ordinary paths; the two signals are handled because their default disposition would skip it,
     and each re-exits so a caller's kill still ends this process. */
  const reapOnOurWayOut = () => { if (pgid !== null) signalGroup('SIGKILL'); };
  process.on('exit', reapOnOurWayOut);
  for (const sig of ['SIGTERM', 'SIGINT'])
    process.on(sig, () => {
      console.error(`[peergate] ${sig} — reaping the zone's process group before exiting: ${
        signalGroup('SIGKILL')}`);
      process.exit(3);
    });

  /* W6's backstop. It measures no progress (no request arriving, no byte written by the child), because a
     deadlocked pipe consumes no CPU and emits no signal. It reports through a verdict of its own, with the
     load average. */
  const IDLE_MS = 180000;
  let backstop = null;
  /* Resolves on `exit` and not `close`. Node's `close` waits for every stdio stream to close, and an `--abi`
     instance that outlives the zone holds a dup of this process's stderr pipe, so `close` would not arrive
     while it lives. The order is load-bearing: learn that the child ended, reap the group (nothing else
     releases the fd), then drain. */
  const ended = await new Promise((res) => {
    child.on('exit', (code, signal) => res({ code, signal }));
    const tick = setInterval(() => {
      if (Date.now() - lastProgress < IDLE_MS) return;
      clearInterval(tick);
      backstop = { idleMs: Date.now() - lastProgress, load: loadavg() };
      /* The group and not the child: a spinning instance does not notice its channel closing. */
      backstop.termed = signalGroup('SIGTERM');
    }, 2000);
    child.on('exit', () => clearInterval(tick));
  });

  /* ── The reap ──
     Leftovers are enumerated before anything is signalled, because they are the finding. Each one's consumed
     CPU is stated beside it: it says whether the process was wedged or spinning. */
  const leftBehind = pgid === null ? [] : groupMembers(pgid).filter((r) => r.pid !== child.pid);
  const reaped = leftBehind.length ? signalGroup('SIGKILL') : 'nothing to reap';
  /* `missing` where there is no group to look in: that run has shown nothing about instances. */
  if (pgid === null)
    record('orphans', 'missing', 'this gate established no process group for its child, so there is no set of ' +
                                 'processes for this row to be about — the line above says why');
  else
    record('orphans', leftBehind.length ? 'wrong' : 'pass',
           leftBehind.length
             ? `${leftBehind.length} process(es) outlived the zone: ${
                 leftBehind.map((r) => `${r.pid}:${r.comm} ${r.cpuS.toFixed(1)}s CPU`).join(', ')} — ${reaped}`
             : `group ${pgid} held nothing but the child once it ended`);

  /* ── The drain ──
     `exit` does not say this process has read everything the child wrote, and `result`, `routed`, `peers`
     and `peerflow` are decided out of `err` and `out`. The wait is bounded and its outcome is printed, so a
     truncated buffer is not read as a zone that said nothing. */
  const DRAIN_MS = 5000;
  const drainStream = (st) => new Promise((res) => {
    if (st === null || st.readableEnded || st.destroyed) return res(true);
    st.once('end', () => res(true));
    st.once('close', () => res(true));
  });
  const drained = await Promise.race([
    Promise.all([drainStream(child.stdout), drainStream(child.stderr)]).then(() => true),
    new Promise((res) => setTimeout(() => res(false), DRAIN_MS)),
  ]);
  if (!drained)
    console.error(`[peergate] the child's stdio did not reach EOF within ${DRAIN_MS} ms of it exiting, so ` +
                  '`result`, `routed`, `peers` and `peerflow` below are decided over what had arrived by then ' +
                  'and not over everything the zone wrote. Something still holds the write end of one of ' +
                  'these pipes; the `orphans` row above names what.');
  server.close();
  /* The keep-alive sockets too: `close` only stops accepting, and this process's own probe holds one. */
  server.closeAllConnections?.();

  if (spawnError) {
    console.error(`\n[peergate] the child was never started: ${spawnError.message}`);
    /* `origins`, `pna` and `orphans` are decided before or without the child, so they are printed. Nothing
       else is decidable: a child that never started produced no `ended`, `out` or `err`. */
    printRows();
    process.exitCode = 2;
    return;
  }

  if (backstop) {
    console.error(`\n[peergate] BACKSTOP — this gate's own idle watchdog fired after ${
      Math.round(backstop.idleMs / 1000)}s with NO REQUEST AND NO OUTPUT from the child, load average [${
      backstop.load.map((n) => n.toFixed(2)).join(' ')}]. This is a verdict about the HARNESS and it is NOT ` +
      'one of the checks below. A kill leaves an EMPTY tail where a crash leaves the output that preceded ' +
      'it — the child\'s stderr above this line is the place to read which of the two happened.\n' +
      '[peergate] IT MEASURES SILENCE AND NOT PROGRESS, AND THIS SENTENCE USED TO SAY "only that nothing ' +
      'moved" — which is a claim about the CHILD that this watchdog cannot make. It is rewritten rather ' +
      'than deleted because a reader re-derives it from the word `idle`. What is timed is the absence of a ' +
      'REQUEST and of a BYTE; a child spinning at a full core emits neither, so `nothing moved` is exactly ' +
      'the reading this line must not offer. Read the child\'s own CPU before concluding anything from it.');
    /* The rows are printed on this arm too (W5 under W6): `origins` and `pna` are decided before the child is
       spawned. The exit code stays 3 and no verdict is composed.
       Named residual: not covered — the BACKSTOP line does not say which process is consuming CPU, so it
       does not tell a deadlocked zone from a zone idling in front of a spinning instance. The busy process
       can be a grandchild while the child idles, and the caller's RLIMIT_CPU is per process, so neither the
       child's CPU nor that limit decides it. Next diff — the tick states which member of the group is
       consuming CPU when it fires, read once per window from the group it already enumerates. Absence shows
       as — a BACKSTOP line with a high load average beside an `orphans` row naming a process with minutes of
       CPU, and nothing on the BACKSTOP line connecting the two. */
    /* Every row that can still be decided is decided first; see `decideFromWhatIsHeld`. */
    decideFromWhatIsHeld();
    printRows();
    /* The failed list is printed, but not `report`'s verdict or exit code: the harness ended this run, so
       the status stays 3. A `wrong` row here is an observation about a run cut short. */
    /* W5's guard on this arm: a declared row with no result is the gate's own failure and outranks the
       harness verdict, so the status is `report`'s 4. */
    const unmade = CHECKS.filter(([id]) => !results.has(id)).map(([id]) => id);
    if (unmade.length) {
      console.error(`\n[peergate] FAILED: ${unmade.length} declared check(s) produced no result even though ` +
                    `this process was holding \`wire\`, \`err\`, \`out\` and the child's ending when its ` +
                    `backstop fired (${unmade.join(', ')}). Every row is decidable from those four, so this ` +
                    'is a failure of the gate — a check declared and never decided — and it is distinct from ' +
                    'both the backstop above it and from anything the transport did.');
      printFixtures(port);
      process.exitCode = 4;
      return;
    }
    const bad = CHECKS.filter(([id]) => results.get(id).verdict !== 'pass');
    console.error(`\n[peergate] the BACKSTOP above ended this run, so the status is the HARNESS's and not ` +
                  `these rows'. All ${CHECKS.length} were decided from what this process was holding when it ` +
                  `fired, and ${bad.length} did not pass: ${
                    bad.map(([id]) => `${id}:${results.get(id).verdict}`).join(' ') || '(none)'}. Each is an ` +
                  'observation about a run this gate cut short — read it as that and not as a verdict on the ' +
                  'transport, which is what the exit code below refuses to claim.');
    printFixtures(port);
    process.exitCode = 3;
    return;
  }

  /* Decides every row that can be decided from `wire`, `err`, `out` and `ended`, all of which this process
     holds whenever the run ends, the backstop included. After it runs every declared row has a result, so a
     `NOT RUN` in a report means this function did not run. Both ending arms call it before `printRows`. */
  function decideFromWhatIsHeld() {
    /* ── Exit and result ── */
    /* The signal names its sender where this gate sent it, so its own backstop is not read as a crash. */
    const ending = ended.signal ? `on ${ended.signal}` : `with code ${ended.code}`;
    record('exit', ended.signal === null && ended.code === 0 ? 'pass' : 'wrong',
           `trusted.mjs ended ${ending}` +
           (backstop ? ' — sent BY THIS GATE at its own backstop, so this row is the kill and not a crash; ' +
                       'the engine\'s own aborts, if any, are on the stderr above the BACKSTOP line'
                     : ''));

    /* The seed's result document is the last non-empty stdout line: the `--abi` child writes
       `@RESULT <json>`, and `trusted.mjs` re-emits the JSON alone on its stdout and says everything else on
       stderr. */
    const resultLine = out.split('\n').map((l) => l.trim()).filter((l) => l !== '').pop();
    let seedResult = null;
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
      else {
        seedResult = parsed;
        record('result', 'pass', `${resultLine.length} bytes, ${parsed.fetchCallSites.length} fetch call site(s)`);
      }
    }

    /* ── The wire ── */
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

    /* W1's wire half. The comparison is over the set of paths and not a count: a forked flow may load its
       document more than once. A path that is neither peer document is work keeping an instance alive, and
       a request at a third authority is the same failure. */
    const extraPeerPaths = peerPaths.filter((p) => !wantPeer.includes(p));
    /* Asked for nothing, the question is undecidable: `missing`, and not a vacuous pass. */
    if (!peerAsked.length)
      record('nohold', 'missing', 'the peer authority was never asked for anything, so there is no set of paths ' +
                                  'for this check to be about');
    else if (extraPeerPaths.length || strayHosts.length)
      record('nohold', 'wrong',
             `paths at the peer authority outside its two documents: ${JSON.stringify(extraPeerPaths)}; ` +
             `requests at authorities this gate does not serve: ${JSON.stringify(strayHosts)}`);
    else
      record('nohold', 'pass', 'the peer authority was asked for its two documents and nothing else');

    /* ── What spoke, read off the instance tags ──
       `trusted.mjs` echoes every census as it arrives, prefixed with the instance's tag
       `[<document>/s<serial>]`, and mints the serial only in its `provision`, so a tag reading `s3` is three
       instances provisioned whatever a later report says. An absent end-of-session report is therefore not
       an absence of instances. A child document's name says which history created it and counts nothing
       (solver/world.c's world_mint_doc). */
    const tagged = [...err.matchAll(/^\[trusted\] \[([^\]/]+)\/s([0-9]+)\] /gm)];
    const spokeFirst = new Map();
    for (const m of tagged) if (!spokeFirst.has(m[1])) spokeFirst.set(m[1], Number(m[2]));
    const serialsSeen = tagged.map((m) => Number(m[2]));
    const highestSerial = serialsSeen.length ? Math.max(...serialsSeen) : 0;
    /* Which `window.open` of the seed a beacon is, derived from the fixture and not counted by hand. */
    const seedOpenOrdinal = (prefix) => {
      const src = seedDoc(port);
      const marker = src.indexOf(`"/beacon/${prefix}-"`);
      if (marker < 0) return null;
      const n = [...src.matchAll(/window\.open\(/g)].filter((m) => m.index < marker).length;
      return n > 0 ? n : null;
    };

    /* ── The beacons: the page's own report of what the reads answered ── */
    const beacons = seedAsked.map((r) => r.path).filter((p) => p.startsWith('/beacon/'));
    const beacon = (id, prefix, want) => {
      const hits = beacons.filter((p) => p.startsWith(`/beacon/${prefix}-`));
      if (!hits.length) {
        /* The message names both readings the evidence leaves open. */
        const nth = seedOpenOrdinal(prefix);
        return record(id, 'missing',
               `no /beacon/${prefix}-… request was ever made, and nothing held here says which of two: the ` +
               'page never reached the line, or it reached it and the address never became a concrete ' +
               `string. This is the seed's \`window.open\` number ${
                 nth === null ? '(unlocatable in the fixture)' : nth}`);
      }
      if (hits.includes(`/beacon/${prefix}-${want}`))
        return record(id, 'pass', `/beacon/${prefix}-${want}`);
      return record(id, 'wrong', `saw ${JSON.stringify(hits)}, required /beacon/${prefix}-${want}`);
    };
    /* The check id and the beacon prefix are passed separately: one answers to the table and one to the
       page's source, and `record` refuses an id the table does not declare. */
    beacon('onlist', 'onlist', 'ok-true');
    beacon('offlist', 'offlist', 'throw-SecurityError');
    beacon('length', 'len', 'number-2');
    beacon('closed', 'closed', 'boolean-true');
    beacon('blankdoc', 'blankdoc', 'value-object');
    beacon('blanklen', 'blanklen', 'number-0');
    beacon('blankclosed', 'blankclosed', 'boolean-false');
    /* The seed's own count of routed deliveries, beside the handler's beacons: a beacon from each arm of the
       handler shows the post reached the page, and the count shows it arrived as a routed delivery. */
    {
      const handled = ['onlist', 'closed'].filter((p) => beacons.some((b) => b.startsWith(`/beacon/${p}-`)));
      const delivered = seedResult === null ? null : seedResult._routedDelivered;
      if (seedResult === null || typeof delivered !== 'number')
        record('posted', 'missing', 'the seed printed no result document with a numeric `_routedDelivered`');
      else
        record('posted', handled.length === 2 && delivered >= 2 ? 'pass' : 'wrong',
               `_routedDelivered=${delivered}, handler arms that reported: ${JSON.stringify(handled)}`);
    }
    {
      const posts = (err.match(/a CROSS-DOCUMENT MESSAGE this origin's egress policy refuses on `actor=tool`/g) ||
                     []).length;
      const ops = (err.match(/a CROSS-AGENT OPERATION this origin's egress policy refuses on `actor=tool`/g) ||
                   []).length;
      record('refused', posts + ops > 0 ? 'pass' : 'missing',
             `${posts} post refusal(s) and ${ops} operation refusal(s) on \`actor=tool\` in the log`);
    }

    /* ── What the zone said it could not do ──
       `trusted.mjs` reports a record it held for the whole session in two shapes, a document no instance was
       provisioned for and one an instance held and then left; the pattern matches both. */
    const heldLine = /record\(s\) named a document/.test(err);
    record('routed', heldLine ? 'wrong' : 'pass',
           heldLine ? 'trusted.mjs reported held records; its own lines are above this verdict'
                    : 'no held records reported');

    const peerLines = [...err.matchAll(/peer instance \[([^\]]+)\] at (\S+) ended (with code \d+|on \w+)/g)];
    if (!peerLines.length)
      /* An absent report is not an absence of instances; see the tag block above. */
      record('peers', 'missing', highestSerial
        ? `trusted.mjs printed no per-instance ending at all, and it provisioned ${highestSerial}: the tags ` +
          `${JSON.stringify([...spokeFirst.keys()])} spoke on this gate's stderr and the serial they carry ` +
          'is minted only at `provision`. That report is written in its end-of-session block, so this row ' +
          'is that block not having been reached and is NOT a statement that nothing was provisioned'
        : 'no instance tag appeared on this gate\'s stderr at all, so trusted.mjs neither provisioned an ' +
          'instance nor echoed a census from one — which is a statement about the zone and not about a peer');
    else {
      const bad = peerLines.filter((m) => m[3] !== 'with code 0');
      record('peers', bad.length ? 'wrong' : 'pass',
             peerLines.map((m) => `${m[1]} at ${m[2]} ended ${m[3]}`).join(' ; '));
    }

    /* ── The peer's own flow base, read off the peer's own document ──
       Three verdicts: `missing` is no peer stating the row, and `wrong` is every peer stating zero. A zero is
       a transport finding, the read answered without the asker's world materialized in the peer; an absence
       sends the reader to the peer's `@WHY`. It is matched on `trusted.mjs`'s separate line, which the
       `peers` pattern (it carries ` at <url> ended `) cannot collide with. */
    const flowLines = [...err.matchAll(/peer instance \[([^\]]+)\] flow base: (.+)$/gm)];
    const madeOf = (s) => { const m = /_worldSegmentsMade=(\d+)/.exec(s); return m ? Number(m[1]) : null; };
    const stated = flowLines.map((m) => ({ tag: m[1], made: madeOf(m[2]), saw: m[2] }))
                            .filter((r) => r.made !== null);
    if (!flowLines.length)
      record('peerflow', 'missing', 'trusted.mjs printed no per-peer flow-base line at all — either no peer ' +
                                    'instance was provisioned, or that report is not being made');
    else if (!stated.length)
      record('peerflow', 'missing',
             `no peer stated a \`_worldSegmentsMade\` count: ${
               flowLines.map((m) => `[${m[1]}] ${m[2]}`).join(' ; ')}`);
    else if (stated.some((r) => r.made >= 1))
      record('peerflow', 'pass',
             stated.map((r) => `[${r.tag}] ${r.saw}`).join(' ; '));
    else
      record('peerflow', 'wrong',
             'every peer that stated the row reports ZERO foreign world segments materialized, so no asking ' +
             'agent\'s world ever arrived in a peer process — the read was answered without one: ' +
             stated.map((r) => `[${r.tag}] ${r.saw}`).join(' ; '));
  }

  decideFromWhatIsHeld();
  report(port);

  /* Prints the table. Both ending arms call it; `NOT RUN` marks a check with no result. */
  function printRows() {
    console.error('\n[peergate] ' + '-'.repeat(96));
    for (const [id, why] of CHECKS) {
      const r = results.get(id);
      const mark = !r ? 'NOT RUN' : r.verdict === 'pass' ? 'pass' : r.verdict === 'wrong' ? 'WRONG ' : 'ABSENT';
      console.error(`[peergate] ${mark.padEnd(7)} ${id.padEnd(9)} ${r ? r.saw : ''}`);
      if (!r || r.verdict !== 'pass') console.error(`[peergate]                   ${why}`);
    }
  }

  function report(p) {
    /* W5's guard: the verdict is refused unless every declared check produced a result. */
    const missingChecks = CHECKS.filter(([id]) => !results.has(id)).map(([id]) => id);
    printRows();
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

/* The input, printed with every failing verdict: the port is the one part a reader cannot reconstruct. */
function printFixtures(port) {
  console.error('\n[peergate] the exact bytes served this run (the port is the only thing that varies):\n' +
                `--- ${SEED_HOST}:${port}/creator ---\n${seedDoc(port)}\n` +
                `--- ${PEER_HOST}:${port}/peer ---\n${PEER_DOC}\n` +
                `--- ${PEER_HOST}:${port}/peer-closed ---\n${PEER_CLOSED_DOC}\n`);
}

await main();
