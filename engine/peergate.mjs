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
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
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
   for an address the page's own code composed on a path that stood on no contradicted arm is `derived`. The
   bytes then come through the ONE chokepoint like every other byte, which is also what makes the observation
   trustworthy: it is `safe-fetch.js` that fetched it.
   THIS SENTENCE ENDED "and a `derived` navigation is one trusted.mjs performs", AND THAT CLAUSE IS RETIRED
   BY MEASUREMENT RATHER THAN BY ANYBODY DISAGREEING WITH IT — kept in its own words because it is the
   reading a reader re-derives from that file's own `navigate` banner, which says the same thing twice. Asked
   of the real chokepoint with `navigate`'s own facts at the table that file states (`{}`), a DERIVED
   navigation is REFUSED `destination=value`: the `provenance=observed` arm wants `observed` and the
   `destination=value` arm wants `witness=unpinned`, where a navigation composes no witness mark at all. So a
   `derived` navigation waits on the per-origin widening exactly as a `forced` one does, which is why the
   child below is spawned with `--explore` for both of this fixture's authorities. Both sentences describe
   the single-switch control this model replaced; the refusal `navigate` writes has named `--explore` the
   whole time.

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

/* ── WHAT THIS GATE PERMITS AT ITS OWN FIXTURE, STATED ROW BY ROW ────────────────────────────────────────────
   CLAUDE.md §THE-PER-ORIGIN-OPT-IN-GOVERNS-EGRESS draws the default at PROGRAM LOADS ONLY, and this gate's
   probe is not one: it is a `document` load, which Fetch §2.2.5 "Requests" makes neither script-like nor a
   subresource, so `destination` reads `value` — this tool spending somebody else's server. Measured against
   the real file rather than reasoned about: at the table `trusted.mjs` states (`{}`) the probe is REFUSED
   `destination=value`, and the two default arms that could carry a `value` request cannot, because one wants
   `provenance=observed` (the probe is `derived`) and the other wants `actor=page` (the probe is `tool`). So
   SPEAKING AN EMPTY TABLE IS NOT ENOUGH HERE and a reader who lands one has moved the verdict from an
   uncaught abort to a `pna` WRONG.
   IT IS ONE ORIGIN AND ONE VALUE PER GATING ROW, WHICH IS THE NARROWEST THING THE SHAPE CAN SAY. The only
   request this zone ever makes is the `/pna-probe` at the PEER authority, `_firingRefusal` keys the table on
   that request URL's own `.origin`, and both authorities are EPHEMERAL LOOPBACK PORTS THIS PROCESS OPENED
   — an origin that does not exist outside this run and serves nothing but literals in this file. This is not
   `safeFetchWiden`, which would permit every value of every row: `actor=page`, `provenance=observed` and
   `provenance=forced` stay unpermitted here, so a later diff that changed what this gate asks would be
   REFUSED and named rather than quietly carried.
   AND IT IS A LITERAL RATHER THAN DERIVED FROM THE PROBE'S OWN VECTOR, WHICH IS THE WHOLE OF WHY `pna` IS
   STILL A CHECK. `safeFetchSignalVector` would hand back exactly these nine values for exactly these facts,
   and a table built from it could not refuse the request it was built from — §AN-ASSERT-WHOSE-TWO-SIDES-
   CANNOT-DISAGREE arriving in a permission table, wearing the shape of a derivation. Written out, the three
   diffs that SHOULD redden this gate do: a signal added to the registry, an `of` that answers differently
   for these facts, and an edit to the probe below. The direction is the safe one and the file says so at its
   own door — "a signal a stored grant does not name is not permitted ... every existing grant NARROWS, the
   refusal names the new row" — so the failure is `pna` WRONG naming the row, never a silent firing. */
const PROBE_PERMITS = {
  /* Fetch §2.2.5's `document` destination is neither script-like nor a subresource. */
  destination: ['value'],
  /* THIS GATE composed the address — see the `actor: 'tool'` beside the probe. */
  actor: ['tool'],
  /* The address is one this harness's own code composed; nothing pinned a value in it. */
  provenance: ['derived'],
  /* The document it is composed FROM is the seed, which this harness named itself. */
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
};

/* ── THE CHOKEPOINT, IN A REALM OF ITS OWN ───────────────────────────────────────────────────────────────────
   Loaded exactly as `engine/trusted.mjs` loads it and for the same reason: this gate makes a CLAIM about
   `safe-fetch.js`'s private-network rule (that `127.0.0.1` -> `localhost` is allowed because both classify
   private), and a claim about a file is checked by running that file rather than by restating its rule here.
   A second copy of the rule would be the drift SECURITY.md's one-chokepoint design exists to prevent, and it
   would drift in the one direction that matters: this gate would go on passing after the real file stopped
   allowing the pair, and would report a transport failure as a policy that had not changed.
   IT STATES THE TABLE BEFORE IT RETURNS, AND THE PEER ORIGIN IS AN ARGUMENT FOR THAT REASON RATHER THAN A
   CONVENIENCE. This function used to hand back a zone that had never SPOKEN, and `_firingRefusal`'s DCHECK
   fired on the first request of the first build that ever invoked this gate — before a single declared check
   had produced a result, so W5's whole table was ABSENT rather than failing. A load that cannot return an
   unstated zone makes that state impossible rather than leaving an ordering for the next reader to keep:
   CLAUDE.md §Fix-the-ROOT is the rule, and the alternative — state it at the call site, one line later — is
   the same window one statement narrower. */
function loadChokepoint(peerOrigin) {
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
  /* AND THE POLICY STATEMENT BESIDE IT, ASSERTED AT THE SAME DOOR AND FOR THE SHARPER REASON `trusted.mjs`
     GIVES AT ITS OWN COPY OF THIS LIST: the chokepoint's absence is a gate with no network, and THIS name's
     absence is a gate that reaches the network with the one decision CLAUDE.md puts at that chokepoint
     silently missing. It is a load-time throw rather than a TypeError one call into the probe, and it is what
     makes the statement below reachable at all — the name MOVED with the model (`safeFetchWidenStated` took a
     list of origin strings), so a zone assembled from a stale copy of that file fails HERE, loudly. */
  if (typeof sandbox.safeFetchEgressStated !== 'function')
    throw new Error('extension/lib/safe-fetch.js installed no `safeFetchEgressStated` — this gate cannot ' +
                    'SPEAK its per-origin egress table, so `_firingRefusal` would abort on the probe in a ' +
                    'dev build and answer it from a table nobody stated in a release one');
  /* THE STATEMENT ITSELF, ONCE, IN THE SHAPE THAT FILE TAKES — `{ origin: { signal: [values] } }`. `{}` and
     not `[]` for the reason `trusted.mjs` gives: an empty ARRAY is the shape the previous single-switch
     control persisted and the chokepoint refuses it by name, because a bare origin in it meant "permit
     everything" including the signals that did not exist when it was written. */
  sandbox.safeFetchEgressStated({ [peerOrigin]: PROBE_PERMITS });
  return sandbox;
}

/* ── WHAT THIS GATE LEFT RUNNING, WHICH IS A FACT ABOUT PROCESSES AND NOT ABOUT PIPES ────────────────
   `trusted.mjs` spawns every `--abi` instance with stderr `inherit`, so a native child holds THIS PROCESS'S
   stderr pipe; and it awaits each instance's channel, so an instance that never returns to `abi_line()`
   outlives the zone that provisioned it and is reparented to init. That process is not a leak in the ordinary
   sense — it is a live engine burning a core with no payer, which is CLAUDE.md
   §A-CAPABILITY-MATERIALIZED-PER-FLOW-AND-NEVER-RECLAIMED arriving as a PROCESS rather than as a realm, and
   §AND-THE-LOAD-IS-NOT-ONLY-FROM-WORK-THAT-IS-RUNNING prices it: it competes with every measurement taken on
   this box until something ends it.
   IT IS READ OUT OF `/proc` AND NEVER OUT OF A PATTERN, which is §A-PROCESS-PATTERN-MATCHES-THE-SHELL-THAT-
   NAMES-IT: a `pgrep -f` over a command string matches the shell doing the matching. A PROCESS GROUP is the
   kernel's own answer to "what did this child start" — the child is spawned `detached`, so it is a group
   leader and every descendant it does not itself detach is in that group — so membership is a lookup and not
   an inference.
   `comm` IS FIELD 2 OF `/proc/<pid>/stat` AND IS PARENTHESISED AND MAY CONTAIN SPACES, so every numeric field
   is taken AFTER THE LAST `)`; splitting the line on whitespace from the left mis-indexes every field for any
   process whose name has a space in it. A ZOMBIE IS EXCLUDED because it holds no fd and burns nothing: it is
   an accounting entry its parent has not read yet, and reporting one as a process this gate left running
   would be an absence and a presence behind one number. */
const CLK_TCK = 100;   /* `getconf CLK_TCK` on this platform; only ever used to render ticks as seconds */
function procStat(pid) {
  let raw;
  try { raw = readFileSync(`/proc/${pid}/stat`, 'utf8'); } catch { return null; }
  const close = raw.lastIndexOf(')');
  if (close < 0) return null;
  const f = raw.slice(close + 2).split(' ');
  return { pid, comm: raw.slice(raw.indexOf('(') + 1, close), state: f[0], pgrp: Number(f[2]),
           /* utime + stime, which is the quantity §Testing says to measure a budget in */
           cpuS: (Number(f[11]) + Number(f[12])) / CLK_TCK };
}
function groupMembers(pgid) {
  if (!Number.isInteger(pgid) || pgid <= 1) return [];
  return readdirSync('/proc').filter((d) => /^\d+$/.test(d)).map((d) => procStat(Number(d)))
    .filter((r) => r !== null && r.pgrp === pgid && r.state !== 'Z');
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
  /* THE FLOW BASE, WHICH THIS FILE'S OWN HEADER NAMED AS THE THING IT DID NOT MEASURE. `length` and `closed`
     prove the answer came FROM the peer's document (W2) and are silent about WHAT RAN THERE — and CLAUDE.md's
     requirement is not that the value be the peer's, it is that a peer ANSWER BY RUNNING A PROGRAM, on a flow
     of its own frontier, under the ASKING flow's world. `_worldSegmentsMade` is the one row that can only be
     raised by that: solver/world.c's `world_segment` DCHECKs `w.doc != g_doc`, so a segment is materialized for
     a FOREIGN world and for nothing else, and the asking agent cannot raise a count inside a peer process at
     all. It is read out of the PEER's own `@RESULT` — the artifact, not this harness — which `trusted.mjs` now
     prints under that peer's tag. */
  /* THE PROCESSES THIS GATE LEFT BEHIND, DECLARED AS A CHECK BECAUSE THE COST IS MEASURED RATHER THAN
     ARGUED AND BECAUSE A HEALTHY RUN CAN PASS IT. `trusted.mjs` awaits every instance's channel, so a run
     that ended cleanly has an EMPTY process group by the time it exits and this row reads `pass` — which is
     what makes it a check and not a note. A non-empty one is the lifetime failure this whole file is about,
     seen from outside the engine: an `--abi` instance that never returned to its ABI loop, so it never
     observed the closed channel, never reached `test_forced.c`'s `rec != NULL` CHECK, and is therefore the
     one shape that leaves NO `@E` behind it. That is why the row exists: the two identical aborts in a
     wedged run are the instances that DID reach the loop, and the one that matters is the one that printed
     nothing at all. */
  ['orphans', 'the child\'s process group was EMPTY once it ended — no `--abi` instance outlived the zone that ' +
              'provisioned it. An instance spinning inside the engine never returns to `abi_line()`, so it ' +
              'observes no closed channel, prints no `@E`, holds this process\'s stderr pipe and burns a core ' +
              'with no payer until something else ends it. This row is the only place such a process is ' +
              'visible at all, and it states each one\'s consumed CPU beside it'],
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
  /* AND THE PEER AUTHORITY IS HANDED IN, BECAUSE THE TABLE IS KEYED ON THE ORIGIN THE PROBE'S OWN URL
     SERIALIZES TO and the port is not known until this server is listening — see PROBE_PERMITS for what
     the entry says and why it is one origin rather than both. The SEED authority is deliberately absent
     from this zone's table: this process makes exactly one request and it is at the peer. */
  const ZONE = loadChokepoint(peerOrigin);
  /* THE GRADE IS `derived`, WHICH IS THE ONE THE RUN BELOW ACTUALLY DEPENDS ON AND IS NOT A FORMALITY. That
     file reads the provenance BEFORE any byte moves and refuses a `forced` address at an unwidened origin, so
     a probe that stated a different grade — or, since `_provenanceOf` is a fatal `CHECK`, none at all — would
     answer about a request this gate never makes. Every load this run performs is `derived`: the peer
     addresses are literals the seed's own code composed, and each `/beacon/` address is composed from a value
     the run computed on a path that stood on no contradicted arm. */
  /* AND THE REACH GRADE IS `observed` AND NOT `derived`, WHICH IS THE PAIR THIS PROBE EXISTS TO EXERCISE.
     The address is composed by this gate, which is what `derived` says; the document it is composed FROM is
     the seed, which this harness named on its own command line, which is what `observed` says. Stating one
     word for both would have this gate testing a request shape the run never makes. */
  const probe = await ZONE.safeFetch(`${peerOrigin}/pna-probe`,
                                     { pageUrl: seedUrl, destination: 'document',
                                       provenance: 'derived', pinned: 'unpinned',
                                       /* THIS GATE COMPOSED THE ADDRESS, so the act is THIS TOOL'S and not
                                          any analysed page's — see safe-fetch.js's `_actorOf`. The peer
                                          addresses are this harness's own literals and each `/beacon/` one
                                          is built from a value this run computed, so no page's code is
                                          anywhere in the composition. */
                                       actor: 'tool',
                                       docReach: 'observed', credentialed: false });
  if (!probe || typeof probe.status !== 'number')
    record('pna', 'missing', 'safeFetch returned no reply record at all');
  else if (probe.status === 0)
    record('pna', 'wrong', `the chokepoint refused it: ${probe.statusText}`);
  else
    record('pna', 'pass', `status ${probe.status} — private->private is allowed by that file's own rule`);
  const probeCount = wire.length;

  /* ── THE RUN ───────────────────────────────────────────────────────────────────────────────────────────── */
  /* AND THE CHILD IS AUTHORIZED FOR THE TWO AUTHORITIES OF THIS GATE'S OWN FIXTURE, WHICH IS A SECOND
     STATEMENT BY A SECOND HOST AND NOT A SECOND POLICY. `trusted.mjs` states its own table (`{}`, a
     command line being a sentence for one run) and `--explore <origin>` is the ONE spelling it offers for
     adding to it, so this is that host's canonical door rather than a mechanism invented here.
     IT IS REQUIRED AND THAT IS MEASURED RATHER THAN ARGUED, AND IT REFUTES A CLAIM BOTH THIS FILE AND
     `trusted.mjs` CARRIED. This file's own fixture banner said "a `derived` navigation is one
     trusted.mjs performs", and that file's `navigate` says twice that "an OBSERVED or DERIVED address is
     navigated freely ... and only a FORCED one waits on the per-origin widening". Asked of the real
     chokepoint with `navigate`'s own facts — `document`, `derived`, `pinned: 'unstated'`, `actor: 'page'`,
     `docReach: 'observed'` — at an empty table the answer is REFUSED `destination=value`. No default arm
     carries it: the `provenance=observed` arm wants `observed`, and the `destination=value` arm wants
     `witness=unpinned` where a navigation composes no mark at all. Both sentences describe the
     single-switch control this model replaced, and the refusal message `navigate` itself writes has been
     saying "Pass `--explore <origin>` to widen it" the whole time — the CODE was right and the prose
     above it was stale. The correction is recorded at both sites.
     SO WITHOUT THESE TWO FLAGS THE CHILD REFUSES EVERY CROSSING READ AND EVERY BEACON. The two peer
     documents are `window.open`s of literals the seed's own code composed (`derived`), and each
     `/beacon/` address is composed from a value the run computed — which is how every observation in this
     gate reaches the wire. A run without them records `onlist`, `offlist`, `length` and `closed` as
     ABSENT, which is honest and measures no transport at all.
     BOTH AUTHORITIES AND NOTHING WIDER, WHICH IS THE WHOLE OF THE WIDENING THIS GATE MAKES. They are the
     two hosts of ONE ephemeral loopback port THIS PROCESS OPENED, serving nothing but literals in this
     file, for one run — origins that do not exist outside it. `--explore` is COARSE (`safeFetchWiden`
     permits every value of every gating row) and that is the only sentence this host's command line can
     say; what bounds it here is the fixture rather than the flag, and `nohold` asserts AT THE WIRE that
     the peer authority was asked for nothing but its two documents. A narrower per-signal statement
     WOULD suffice — measured — and giving this host a per-signal flag is a mechanism of its own and is
     not what this gate needs in order to run.
     NAMED RESIDUAL. WHAT IS NOT COVERED: the widening is per ORIGIN and not per SIGNAL, so at these two
     authorities a `forced` navigation would also fire — there is none in this fixture, and a third path
     at the peer authority fails `nohold` whether it fired or not. WHAT THE NEXT DIFF BUILDS: a
     `--explore-signal <origin> <signal>=<value>` on `trusted.mjs` routed to `safeFetchPermit`, which the
     chokepoint already exports as the per-row door a surface writes through, and these two lines become
     the nav vector this gate actually needs. HOW ITS ABSENCE WOULD SHOW: a person reading what this gate
     permitted at its own fixture sees every value of every row permitted at both authorities, where the
     run only ever asks for two vectors. */
  /* AND IT IS `detached`, WHICH IS WHAT GIVES THIS GATE A HANDLE ON WHAT ITS CHILD STARTED RATHER THAN ONLY
     ON THE CHILD. `trusted.mjs` spawns each `--abi` instance in its own caller's process group, so without
     this the grandchildren share THIS process's group and `-pid` would name the group peergate itself is in.
     Detaching makes the child a group LEADER, so `-child.pid` is exactly "the zone and every instance it
     provisioned" and nothing else — the kernel's own answer to the question, in place of a pattern that
     would match the shell asking it (§A-PROCESS-PATTERN-MATCHES-THE-SHELL-THAT-NAMES-IT). It is NOT
     `unref`'d: the child is still this process's to wait for. */
  const child = spawn(process.execPath,
                      [join(ENGINE, 'trusted.mjs'), seedUrl, bin,
                       '--explore', seedOrigin, '--explore', peerOrigin],
                      { stdio: ['ignore', 'pipe', 'pipe'], detached: true });
  let out = '', err = '', spawnError = null;
  child.stdout.on('data', (d) => { out += d; progress(); });
  child.stderr.on('data', (d) => { err += d; process.stderr.write(d); progress(); });
  /* A CHILD THAT NEVER STARTED IS NOT A CHILD THAT EXITED, and Node reports the first as an `error` event and
     no `close` code worth reading. Without this the gate would report `trusted.mjs ended with code null` and
     send the reader hunting a transport failure in a process that does not exist. */
  child.on('error', (e) => { spawnError = e; });

  /* THE GROUP ID, READ FROM THE KERNEL WHILE THE CHILD IS CERTAINLY ALIVE, AND EVERY GROUP-WIDE SIGNAL GATED
     ON IT. `detached` not taking effect is the one failure that turns a reaper into a suicide: `-child.pid`
     against a child that shares this process's group signals THIS PROCESS and everything beside it, so the
     group kill below is refused unless the kernel says the child IS a group leader and that its group is not
     ours. That is §A-DESTRUCTIVE-STEP-IS-GATED-BY-THE-CHECK'S-EXIT-STATUS: the value is read once, here,
     where the answer can be trusted, and every later act branches on it rather than on the intention. */
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
  /* ONE SPELLING FOR EVERY GROUP-WIDE SIGNAL, so the gate cannot acquire a second one that skips the gate. */
  const signalGroup = (sig) => {
    if (pgid === null) return 'no group to signal';
    try { process.kill(-pgid, sig); return `${sig} to group ${pgid}`; }
    catch (e) { return `${sig} to group ${pgid} refused: ${e.code || e.message}`; }
  };
  /* AND THIS GATE'S OWN DEATH REAPS THE GROUP TOO, BECAUSE A PARENT THAT EXITS IS THE CASE THE WHOLE
     `orphans` ROW IS ABOUT AND THIS PROCESS IS ONE. `engine/build.mjs` wraps each stage in a wall backstop
     that signals the STAGE, so a peergate killed there would otherwise leave the zone and every instance it
     provisioned running with no payer and no reader — the same lifetime failure one level out, arriving
     through the harness that was measuring it. `exit` is the one handler that covers every ordinary path, and
     the two signals are handled because their default disposition would skip it; each re-exits rather than
     swallowing, so a caller's kill still ends this process. */
  const reapOnOurWayOut = () => { if (pgid !== null) signalGroup('SIGKILL'); };
  process.on('exit', reapOnOurWayOut);
  for (const sig of ['SIGTERM', 'SIGINT'])
    process.on(sig, () => {
      console.error(`[peergate] ${sig} — reaping the zone's process group before exiting: ${
        signalGroup('SIGKILL')}`);
      process.exit(3);
    });

  /* W6's BACKSTOP, AND IT IS A BACKSTOP RATHER THAN A BUDGET. It measures NO PROGRESS — no request arriving,
     no byte written by the child — because a deadlocked pipe is the one failure that consumes no CPU and
     emits no signal, and it is the only thing here a clock can see that nothing else can. It is generous, it
     reports through a verdict of its own, and the load average travels with it: §Testing's four worked
     examples are all one machine under load reporting HOW a thing ran as WHAT ran. */
  const IDLE_MS = 180000;
  let backstop = null;
  /* IT RESOLVES ON `exit` AND NOT ON `close`, AND THE DIFFERENCE IS THE WHOLE OF WHY THIS GATE ONCE TOOK
     SIXTEEN MINUTES TO DELIVER A VERDICT IT HAD REACHED IN THREE. Node's `close` fires when the process has
     ended AND EVERY STDIO STREAM HAS CLOSED; `exit` fires on the process ending, which is the fact every
     reader of this gate wants and the fact `record('exit', …)` reports. `trusted.mjs` spawns each `--abi`
     instance with stderr `inherit`, so an instance is holding a DUP of the write end of this process's stderr
     pipe — and an instance that never returns to `abi_line()` is not ended by the channel closing, so it goes
     on holding it after the zone is gone. The read end therefore never reaches EOF and `close` NEVER ARRIVES,
     for as long as that process lives.
     MEASURED ON BOTH SHAPES IN THIS NODE (v22), with the failing case as the control: a three-level parent /
     child / grandchild where the child exits while the grandchild holds the inherited stderr, `exit` fires in
     under half a second and `close` DOES NOT FIRE AT ALL — not in eight seconds, not ever while the
     grandchild lives. The gate that waited on `close` was waiting on a process it had already given up on.
     AND THAT IS NOT A TIDIER SPELLING OF THE SAME WAIT: on the run this was written from, the backstop had
     decided at 182 s and the stage did not end until THIRTEEN MINUTES LATER, when the kernel's RLIMIT_CPU —
     which is per-process and therefore a FRESH budget for every descendant — killed the orphan at 900 CPU
     seconds. The gate's verdict was correct and unreadable, and what ended the stage was a budget the caller
     installs around something else.
     SO THE THREE ACTS ARE ORDERED AND THE ORDER IS LOAD-BEARING: learn that the child ENDED, REAP the group
     (nothing else can release the fd), and only then DRAIN, because a drain attempted first is the same wait
     under a different name. */
  const ended = await new Promise((res) => {
    child.on('exit', (code, signal) => res({ code, signal }));
    const tick = setInterval(() => {
      if (Date.now() - lastProgress < IDLE_MS) return;
      clearInterval(tick);
      backstop = { idleMs: Date.now() - lastProgress, load: loadavg() };
      /* THE GROUP AND NOT THE CHILD. A `SIGTERM` to the child alone leaves every instance it provisioned
         running — and the ones that are spinning are exactly the ones that will not notice their channel
         closing, so the single-process kill reaches only the instances that would have died anyway. */
      backstop.termed = signalGroup('SIGTERM');
    }, 2000);
    child.on('exit', () => clearInterval(tick));
  });

  /* ── THE REAP, WHICH IS A CHECK BEFORE IT IS AN ACT ─────────────────────────────────────────────
     ENUMERATED BEFORE ANYTHING IS SIGNALLED, because what was left behind is the FINDING and a kill that ran
     first would have destroyed the evidence for it. On a healthy run this list is empty and the row passes.
     THE CPU EACH ONE HAD BURNED IS STATED BESIDE IT, because that is the cost §AND-THE-LOAD-IS-NOT-ONLY-FROM-
     WORK-THAT-IS-RUNNING names and it is the one number that says whether a leftover process was wedged
     (nothing) or spinning (everything). */
  const leftBehind = pgid === null ? [] : groupMembers(pgid).filter((r) => r.pid !== child.pid);
  const reaped = leftBehind.length ? signalGroup('SIGKILL') : 'nothing to reap';
  /* AND `missing` WHERE THERE IS NO GROUP TO LOOK IN, WHICH IS NOT THE SAME SENTENCE AS AN EMPTY ONE. A run
     whose child never started, or whose group this process could not establish, has not shown that no
     instance was left behind — it has shown nothing about instances at all, and answering `pass` there is
     the vacuous pass the `nohold` row above refuses for the same reason. */
  if (pgid === null)
    record('orphans', 'missing', 'this gate established no process group for its child, so there is no set of ' +
                                 'processes for this row to be about — the line above says why');
  else
    record('orphans', leftBehind.length ? 'wrong' : 'pass',
           leftBehind.length
             ? `${leftBehind.length} process(es) outlived the zone: ${
                 leftBehind.map((r) => `${r.pid}:${r.comm} ${r.cpuS.toFixed(1)}s CPU`).join(', ')} — ${reaped}`
             : `group ${pgid} held nothing but the child once it ended`);

  /* ── THE DRAIN, BOUNDED, AND ITS OUTCOME STATED RATHER THAN ASSUMED ───────────────────────────
     `exit` says nothing about whether this process has READ everything the child wrote, and four checks below
     are decided out of `err` and `out` — so a resolve on `exit` with no drain would report `routed`, `peers`,
     `peerflow` and `result` about bytes that were still in a pipe. It is a BOUNDED wait and its completion is
     RECORDED, because a truncated `err` read as a zone that said nothing is the absent-versus-zero pair
     arriving in this gate's own inputs; where the drain does not complete, the rows that read those buffers
     say so in their own text rather than reporting a silence they cannot vouch for. */
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
      Math.round(backstop.idleMs / 1000)}s with NO REQUEST AND NO OUTPUT from the child, load average [${
      backstop.load.map((n) => n.toFixed(2)).join(' ')}]. This is a verdict about the HARNESS and it is NOT ` +
      'one of the checks below. A kill leaves an EMPTY tail where a crash leaves the output that preceded ' +
      'it — the child\'s stderr above this line is the place to read which of the two happened.\n' +
      '[peergate] IT MEASURES SILENCE AND NOT PROGRESS, AND THIS SENTENCE USED TO SAY "only that nothing ' +
      'moved" — which is a claim about the CHILD that this watchdog cannot make. It is rewritten rather ' +
      'than deleted because a reader re-derives it from the word `idle`. What is timed is the absence of a ' +
      'REQUEST and of a BYTE; a child spinning at a full core emits neither, so `nothing moved` is exactly ' +
      'the reading this line must not offer. Read the child\'s own CPU before concluding anything from it.');
    /* AND THE ROWS ARE PRINTED ANYWAY, WHICH IS W5 SURVIVING W6 RATHER THAN A SOFTENED VERDICT. This arm
       used to return with the table unprinted, so a kill DISCARDED every check that had already produced a
       result — `origins` and `pna` are both decided before the child is even spawned — and a reader got a
       harness sentence where two measured rows existed. That is the shape W5 is written against (a driver
       that died leaving the checks below it silently unmade while the file still looks complete), arriving
       through W6 instead of through a reply. The EXIT CODE stays 3 and no verdict is composed: `printRows`
       marks an unmade check `NOT RUN`, which is neither `pass` nor a failure, so nothing here can be read
       as the gate having answered about peers.
       THAT RESIDUAL'S REMEDY IS REFUTED BY MEASUREMENT AND IS REWRITTEN RATHER THAN DELETED, because it is
       the reading a reader re-derives from the W6 paragraph above and because §AND-THE-`WHAT-THE-NEXT-DIFF-
       BUILDS`-CLAUSE says a wrong one is not caught but EXECUTED. It read: the watchdog fires on a child
       CONSUMING A FULL CORE, so the tick should read the child's own utime+stime and fire only where the
       child has burned no CPU across the window, "leaving the busy case to the RLIMIT_CPU the caller
       installs". Its DIAGNOSIS was right and its two remedy halves are both wrong, and the run that
       established it is the one this arm was reached on:
         · WHOSE CPU. The busy process was not the child. `trusted.mjs` was idle — correctly, since its
           session-end arm fires only when EVERY live instance is stalled and one instance never stalled —
           while a native GRANDCHILD burned a full core. A tick reading the CHILD's CPU would have fired
           exactly as it did; a tick reading the TREE's would have DECLINED TO FIRE on a session that was
           genuinely wedged, which is the flattering direction and the expensive one.
         · WHOSE BUDGET. `RLIMIT_CPU` is PER PROCESS and inherited, so it is not a bound on this stage at
           all — it is a FRESH 900 seconds for every descendant. The orphan spent all of it: the backstop
           decided at 182 s and the stage did not end for another thirteen minutes, when the kernel killed
           the orphan at its own limit. Handing the busy case to the caller's RLIMIT_CPU hands it to a
           budget that does not bound the thing being measured.
       WHAT THE NEXT DIFF BUILDS, given the reap above already ends the leftover process: the tick states
       WHICH member of the group is consuming CPU when it fires, read once per window from the group it
       already enumerates, so a BACKSTOP verdict distinguishes a deadlocked zone from a zone idling in front
       of a spinning instance without a reader having to go to `ps`. HOW ITS ABSENCE WOULD SHOW: a BACKSTOP
       line whose load average is high and whose `orphans` row names a process with minutes of CPU on it,
       with nothing on the BACKSTOP line itself connecting the two. */
    printRows();
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

  /* ── THE PEER'S OWN FLOW BASE, READ OFF THE PEER'S OWN DOCUMENT ────────────────────────────────────────────
     THREE VERDICTS AND NOT TWO, which is the rule this file already performs everywhere else: `missing` is
     "no peer stated the row at all" and `wrong` is "every peer stated it and it is zero", and those take
     OPPOSITE work. A zero is a TRANSPORT finding — the read was answered without the asker's world ever being
     materialized in the peer, which is the one thing `length === 2` cannot rule out — while an absence is a
     fact about what `trusted.mjs` could read out of a peer that printed no document, and sends the reader to
     the peer's own `@WHY` above this verdict instead.
     MATCHED ON `trusted.mjs`'s SEPARATE LINE and never by widening the `peers` pattern above: that pattern
     carries ` at <url> ended `, this one does not, so the two cannot collide and a rename on either side
     leaves the other reporting `missing` rather than quietly measuring nothing. */
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

  report(port);

  /* THE ROWS, AS THEIR OWN FUNCTION, BECAUSE TWO ARMS END THIS RUN AND ONLY ONE OF THEM USED TO PRINT
     THEM. A verdict is `report`'s to compose and the TABLE is not — W5's mechanism is that every check
     is DECLARED up front and that the reader can see which of them produced a result, and that is worth
     exactly as much on a run this harness killed as on one that finished. `NOT RUN` is already the mark
     for a check that made no observation, so the same loop says the right thing on both paths. */
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
    /* W5's GUARD. The verdict is refused unless every declared check produced a result — a driver that died
       between two of these would otherwise leave the ones below it silently unmade while this summary still
       printed a total. */
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
