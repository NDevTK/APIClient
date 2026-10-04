// WHICH DEFAULT EGRESS ARM A REQUEST TAKES, ASKED OF THE CHOKEPOINT ITSELF.
//
// `extension/lib/safe-fetch.js` decides whether an act may be spent, and it decides from a SIGNAL VECTOR
// whose fields the two trusted zones type out at each call site. Those two facts live in two files, so
// "would this request fire?" is a question nobody could answer without running the thing — and the answer
// had been reasoned about in prose at three sites, with one of the three WRONG in the direction nothing
// catches (see the cost clauses in `_pinnedOf` and in `bridge.js`'s navigation relay, both corrected by
// this probe's own output).
//
// IT RESTATES NOTHING, which is CLAUDE.md §AN-AUDITOR-DERIVES-THE-RULE: it loads the chokepoint VERBATIM
// into a realm of its own, exactly as `engine/trusted.mjs` does, states the table a host states, and prints
// the arms off `safeFetchDefaultArms()` rather than off a copy. A second table here would be the copy that
// drifts, and the thing that drifted is what this exists to find.
//
// THE VECTORS ARE NAMED FOR THE CALL SITE THEY WERE READ OFF, and that is the one thing here a reader must
// check rather than trust: a vector is a transcription, so `git grep -n 'actor: "page"' extension/bridge.js`
// is what says whether the rows below still describe this tree. A row whose call site has moved is a row
// about nothing, which is why each one carries the site in its label.
//
// IT REFUSES TO PUBLISH A CLEAN BILL UNLESS ITS CONTROL SPOKE. CLAUDE.md §AND-THE-WAY-YOU-ESTABLISH-WHICH-
// HALF-A-FAILURE-LANDS-IN: probing a policy for a hole rests on demonstrating that the probe can make the
// policy REFUSE at all, or a FIRES everywhere reads identically to a probe that never reached the walk.
// Two controls run, one each way, and `main` exits nonzero if either fails to behave.
//
//   node testing/egress_arm_probe.mjs
"use strict";
import { readFileSync } from 'node:fs';
import { createContext, runInContext } from 'node:vm';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const EXT = join(dirname(fileURLToPath(import.meta.url)), '..', 'extension');

/* THE CHOKEPOINT, LOADED THE WAY A HOST LOADS IT. `engine/trusted.mjs`'s own ZONE does exactly this — the
   same two files, in the same order, into a context whose `self` is itself — because `ast-worker.html` loads
   them in that order and a probe that assembled the zone differently would be measuring a different
   program. The sandbox is minimal on purpose: nothing below opens a socket. */
function zone() {
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  createContext(sandbox);
  for (const f of ['check.js', 'lib/safe-fetch.js'])
    runInContext(readFileSync(join(EXT, f), 'utf8'), sandbox, { filename: join(EXT, f) });
  for (const n of ['safeFetchEgressStated', 'safeFetchFiringRefusal', 'safeFetchSignalVector',
                   'safeFetchDefaultArms'])
    if (typeof sandbox[n] !== 'function')
      throw new Error(`extension/lib/safe-fetch.js installed no \`${n}\` — this probe reads the policy's own ` +
                      'answer rather than restating its rules, so a load that installs nothing leaves it ' +
                      'with nothing to ask and a clean-looking table of silence');
  /* THE DEFAULT TABLE, STATED ONCE AND EMPTY, WHICH IS WHAT AN UNCONFIGURED ORIGIN HAS. `trusted.mjs` states
     `{}` before it reads `--explore`, and the chokepoint ABORTS on a firing question asked before any host
     has spoken — so this is not setup, it is the one thing that makes the answers below be about the
     DEFAULT policy rather than about a table nobody stated. */
  sandbox.safeFetchEgressStated({});
  return sandbox;
}

/* WHAT EACH ZONE TYPES AT ITS CALL SITE, TRANSCRIBED WITH THE SITE NAMED. Every field is a literal in
   trusted-zone source at the site named, which is why a transcription is the honest form: there is no
   derivation to read it off, and a reader checks it with one grep rather than trusting this file. */
const SITES = [
  { label: 'bridge.js navigationLoad (a child navigable)',
    grep: "git grep -n 'destination: \"document\"' extension/bridge.js",
    facts: { destination: 'document', actor: 'page', pinned: 'unstated',
             provenance: 'derived', docReach: 'observed', credentialed: true, headers: null } },
  /* THE SAME CALL SITE WITH THE WITNESS MARK STATED, AND THE WHOLE VALUE OF THESE TWO ROWS IS THAT THEY NOW
     AGREE WITH THE ONE ABOVE. They are not transcriptions of anything — the mark does not travel for a
     navigation — they are the HYPOTHETICAL the plumbing CLAUDE.md §A-REAL-NAVIGABLE calls owed would
     create. Before the destination row named navigations they read FIRES, so a correct plumbing diff would
     have begun firing every derived child navigable at every origin with nobody having decided it; the row
     that refuses them is now the DESTINATION, which no plumbing can move. A day on which either of these
     reads FIRES again is a day an arm started naming `navigation`, which is the project owner's decision and
     would be deliberate — or a day the destination row stopped saying what a navigation is. */
  { label: '…the same, if it STATED its witness mark (must still REFUSE)',
    grep: 'solver/engine.c engine_pinned_of_running_path — the fact exists and does not travel',
    facts: { destination: 'document', actor: 'page', pinned: 'unpinned',
             provenance: 'derived', docReach: 'observed', credentialed: true, headers: null } },
  { label: '…the same, on a FORCED path, witness stated (must still REFUSE)',
    grep: 'as above; the provenance word is engine_provenance_of_running_path\'s',
    facts: { destination: 'document', actor: 'page', pinned: 'unpinned',
             provenance: 'forced', docReach: 'observed', credentialed: true, headers: null } },
  /* THE AMBIENT SEED AT BOTH ACTOR WORDS, WHICH IS THE PAIR THAT MAKES ONE REPAIR AUDITABLE. The loader used
     to answer `page` for all three of its callers out of one literal, and the seed is the one that is this
     TOOL's act — safe-fetch.js's `_actorOf` says so in its own words. The two rows are kept together because
     the whole claim the repair rests on is that they AGREE: the arm that admits a seed is keyed on the PATH
     and names no actor, so the row a person reads changes and nothing fires differently. A day on which these
     two disagree is a day the repair stopped being free, and nothing else in this tree would say so. */
  { label: 'bridge.js AMBIENT SEED, actor=tool (what it states)',
    grep: "git grep -n 'PROVENANCE_OBSERVED, PROVENANCE_OBSERVED' extension/bridge.js",
    facts: { destination: 'document', actor: 'tool', pinned: 'unstated',
             provenance: 'observed', docReach: 'observed', credentialed: true, headers: null } },
  { label: '…the same at actor=page (the retired literal)',
    grep: 'must agree with the row above, or the repair is no longer outcome-free',
    facts: { destination: 'document', actor: 'page', pinned: 'unstated',
             provenance: 'observed', docReach: 'observed', credentialed: true, headers: null } },
  { label: "bridge.js page fetch() relay, unpinned address",
    grep: "git grep -n 'docReach: msg.provenance, credentialed: false' extension/bridge.js",
    facts: { destination: '', actor: 'page', pinned: 'unpinned',
             provenance: 'derived', docReach: 'observed', credentialed: false, headers: null } },
  { label: "bridge.js page fetch() relay, PINNED address",
    grep: 'as above; the mark comes off the engine pending line',
    facts: { destination: '', actor: 'page', pinned: 'pinned',
             provenance: 'forced', docReach: 'observed', credentialed: false, headers: null } },
  { label: 'a program load (a lazy chunk)',
    grep: 'Fetch §2.2.5 destination `script` — _isScriptLike',
    facts: { destination: 'script', actor: 'page', pinned: 'unstated',
             provenance: 'forced', docReach: 'observed', credentialed: true, headers: null } },
];

/* THE TWO CONTROLS, ONE EACH WAY. A probe whose every row answers FIRES is indistinguishable from one that
   never reached the walk, and a probe whose every row answers REFUSED is indistinguishable from one whose
   table was never stated — so both directions are demonstrated before any row above is worth reading. */
const CONTROLS = [
  { label: 'CONTROL must REFUSE: this tool composing a data request',
    want: 'REFUSED',
    facts: { destination: '', actor: 'tool', pinned: 'unpinned',
             provenance: 'derived', docReach: 'observed', credentialed: false, headers: null } },
  { label: 'CONTROL must FIRE: the page loading its own code',
    want: 'FIRES',
    facts: { destination: 'script', actor: 'page', pinned: 'unstated',
             provenance: 'forced', docReach: 'observed', credentialed: true, headers: null } },
];

const URL_UNDER_TEST = 'https://example.test/x';

function ask(Z, facts) {
  const f = Object.assign({ url: URL_UNDER_TEST }, facts);
  const v = Z.safeFetchSignalVector(f);
  const r = Z.safeFetchFiringRefusal(f);
  return { verdict: r === null ? 'FIRES' : 'REFUSED', vector: v,
           /* THE REFUSING ROW IS THE WHOLE OF WHAT A PERSON ACTS ON, and the walk answers only the FIRST —
              which CLAUDE.md §AND-THE-CLAUSE-ABOVE-IS-TRUE-OF-THE-GATE-STAGES names as the hazard: a caller
              that reads one signal as the whole decision is reading a first-past-the-post answer. So the
              row is printed as the walk gave it and never summarised. */
           at: r === null ? null : (r.signal !== undefined ? r.signal + '=' + r.value
                                                           : JSON.stringify(r).slice(0, 120)) };
}

function main() {
  const Z = zone();
  const arms = Z.safeFetchDefaultArms();
  console.log('DEFAULT ARMS, read off safeFetchDefaultArms() and not copied:');
  arms.forEach((a, i) => console.log('  [' + i + '] ' +
    a.when.map((c) => c.signal + '=' + c.value).join(' AND ')));
  console.log('');

  let bad = 0;
  for (const c of CONTROLS) {
    const got = ask(Z, c.facts);
    const ok = got.verdict === c.want;
    if (!ok) bad++;
    console.log((ok ? '  ok   ' : '  FAIL ') + c.label.padEnd(52) + ' => ' + got.verdict +
                (got.at ? ' at ' + got.at : '') + (ok ? '' : '  (wanted ' + c.want + ')'));
  }
  console.log('');
  if (bad) {
    console.log('THE CONTROLS DID NOT BEHAVE, so nothing below is a reading. A probe that cannot make this');
    console.log('policy refuse, and cannot make it fire, is reporting on itself.');
    process.exitCode = 2;
    return;
  }

  for (const s of SITES) {
    const got = ask(Z, s.facts);
    console.log(s.label);
    console.log('    destination=' + got.vector.destination + '  witness=' + got.vector.witness +
                '  provenance=' + got.vector.provenance + '  actor=' + got.vector.actor +
                '  =>  ' + got.verdict + (got.at ? ' at ' + got.at : ''));
    console.log('    check the transcription: ' + s.grep);
  }
  console.log('');
  console.log('EVERY ROW ABOVE IS ABOUT THE DEFAULT (UNWIDENED) TABLE. A per-origin grant only ever WIDENS,');
  console.log('so a REFUSED row names work a person could permit and a FIRES row is permitted everywhere.');
}
main();
