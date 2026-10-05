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
// AND FOUR OF ITS ROWS NOW CARRY AN EXPECTATION, WHICH MAKES ONE SENTENCE IN ITS OWN BUILD STAGE FALSE
// AND IS SAID HERE BECAUSE THAT STAGE IS WHERE A RED IS READ. `engine/build.mjs` runs this file as a SOURCE
// stage whose hint says "Nothing here stores an expected arm list: a widened policy passes, and this red
// means the PROBE lost its grip on the walk". That was exactly true while every row merely DESCRIBED the
// table. It is not true of the four rows scoring the project owner's navigation decision: two of them
// (`provenance=forced` and `actor=tool` must REFUSE) are the BOUND of that decision, so a future owner
// decision to widen either one turns this stage red and the stage's own hint then gives the WRONG
// instruction — fix the probe, where the right one is update the row.
// THE DISTINCTION IS IN THIS FILE'S EXIT CODE AND NOT YET IN THE STAGE: a control that lost its grip exits
// 2 and prints that nothing below is a reading, a scored row disagreeing exits 1 and names the row and both
// readings of it. `engine/build.mjs` treats any non-zero alike, which is a stage this lane may not edit and
// is recorded as owed rather than guessed at — CLAUDE.md's §A-REAL-NAVIGABLE paragraph carries it as a
// retirement condition with the grep that shows it absent.
// AND A SCORED ROW IS NOT THE CHANGE DETECTOR §Testing FORBIDS, WHICH IS THE OTHER HALF OF WHY THERE ARE
// FOUR AND NOT NINE. An arm only ever WIDENS, so the two rows that must FIRE can go red only if somebody
// NARROWS the default — which is a decision being silently reverted and not a better design being blocked.
// The two that must REFUSE can genuinely block a widening, and that is the point of a BOUND: a bound nothing
// checks is not a bound, and the cost of moving one is a one-line edit made by a reader this file's own
// failure message has already told which of the two things happened. Every other row carries no `want`.
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
  /* THE OWNER'S NAVIGATION DECISION, AND THESE FOUR ROWS ARE WHAT SCORE IT. A `want` is the difference
     between a row that DESCRIBES the table and a row that would FAIL if the table drifted, and it is spent
     only where the expectation is an INVARIANT OF THE DECISION rather than a snapshot of the arm list —
     a `want` on every row would be the change detector CLAUDE.md §Testing forbids, which is why most of
     the rows below carry none. Each of these four is one half of the owner's sentence: a child navigable the
     page's own markup named or its own code computed FIRES, and a navigation on a FORCED path does not. */
  { label: 'bridge.js navigationLoad (a child navigable)',
    want: 'FIRES',
    grep: "git grep -n 'destination: \"document\"' extension/bridge.js",
    facts: { destination: 'document', actor: 'page', pinned: 'unstated',
             provenance: 'derived', docReach: 'observed', credentialed: true, headers: null } },
  /* THE SAME CALL SITE WITH THE WITNESS MARK STATED, AND THE WHOLE VALUE OF THIS ROW IS STILL THAT IT AGREES
     WITH THE ONE ABOVE — THE AGREEMENT NOW MEANS THE OPPOSITE THING AND IS WORTH MORE. It is not a
     transcription: the mark does not travel for a navigation, so this is the HYPOTHETICAL the plumbing
     CLAUDE.md §A-REAL-NAVIGABLE calls owed would create. It used to read REFUSED together with the row
     above, which is what proved no plumbing diff could flip a navigation on. It now reads FIRES together
     with the row above, which proves the same property from the other side: the arm that admits a child
     navigable names `destination`, `actor` and `provenance` and NO witness row, so landing the mark changes
     no firing outcome at any setting. A day on which these two DISAGREE is a day some arm started reading
     the witness for a navigation, and the decision stopped being immune to an unrelated field. */
  { label: '…the same, if it STATED its witness mark (must AGREE with the row above)',
    want: 'FIRES',
    grep: 'solver/engine.c engine_pinned_of_running_path — the fact exists and does not travel',
    facts: { destination: 'document', actor: 'page', pinned: 'unpinned',
             provenance: 'derived', docReach: 'observed', credentialed: true, headers: null } },
  /* AND THE BOUND OF THE DECISION, WHICH IS THE ONE ROW THAT MUST NOT MOVE. A route that exists only past a
     forced gate is a document ONLY THIS ENGINE EVER ASKED FOR, so it is the deliberate per-origin widening
     and not the default — the asymmetry with the two destination-keyed arms is argued at the arm itself.
     The witness is stated here too, so this row also shows that the refusal is the PROVENANCE and not the
     mark: both navigation rows above fire with the mark stated and this one refuses with it stated. */
  { label: '…the same, on a FORCED path (must still REFUSE — the bound)',
    want: 'REFUSED',
    grep: 'as above; the provenance word is engine_provenance_of_running_path\'s',
    facts: { destination: 'document', actor: 'page', pinned: 'unpinned',
             provenance: 'forced', docReach: 'observed', credentialed: true, headers: null } },
  /* AND THE `actor` CONJUNCT'S OWN WORK, WHICH NO OTHER ROW HERE DEMONSTRATES. A derived navigation THIS
     TOOL composed — a discovery sweep following a route it computed itself — is identical to the
     child navigable above on destination, provenance, doc-reach and witness, and differs only in whose act
     it is. If this ever reads FIRES, the arm has stopped asking the question the owner's discriminator is
     made of and the default has quietly widened to every address this tool can build. */
  { label: '…the same at actor=tool (must REFUSE — the `actor` conjunct)',
    want: 'REFUSED',
    grep: 'no caller states this pair today; it is the arm\'s own discriminator, exercised',
    facts: { destination: 'document', actor: 'tool', pinned: 'unstated',
             provenance: 'derived', docReach: 'observed', credentialed: true, headers: null } },
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

  /* A ROW'S `want` IS ENFORCED AND NOT PRINTED BESIDE IT, WHICH IS CLAUDE.md
     §A-DESTRUCTIVE-STEP-IS-GATED-BY-THE-CHECK'S-EXIT-STATUS READ AT A VERIFICATION: an expectation
     nothing branches on is a NON-check that produces a reassuring transcript, and the transcript is the
     hazard — a later reader sees the wanted verdict sitting right there and concludes somebody compared
     them. `scored` is counted and printed so that a run which enforced NOTHING cannot read like one that
     enforced everything, which is the same reason the controls are counted above. */
  let wrong = 0, scored = 0;
  for (const s of SITES) {
    const got = ask(Z, s.facts);
    const judged = s.want !== undefined;
    const ok = !judged || got.verdict === s.want;
    if (judged) scored++;
    if (!ok) wrong++;
    console.log((judged ? (ok ? '  ok   ' : '  FAIL ') : '       ') + s.label);
    console.log('    destination=' + got.vector.destination + '  witness=' + got.vector.witness +
                '  provenance=' + got.vector.provenance + '  actor=' + got.vector.actor +
                '  =>  ' + got.verdict + (got.at ? ' at ' + got.at : '') +
                (ok ? '' : '   (wanted ' + s.want + ')'));
    console.log('    check the transcription: ' + s.grep);
  }
  console.log('');
  console.log(scored + ' of ' + SITES.length + ' rows carried an expectation and were SCORED; the rest are');
  console.log('DESCRIPTIVE, which is deliberate — a `want` on every row would be a change detector.');
  if (wrong) {
    console.log('');
    console.log(wrong + ' SCORED ROW(S) DISAGREED WITH THE POLICY. Either the arm list moved without the');
    console.log('decision moving, or the decision moved and these rows are what says so.');
    process.exitCode = 1;
  }
  console.log('');
  console.log('EVERY ROW ABOVE IS ABOUT THE DEFAULT (UNWIDENED) TABLE. A per-origin grant only ever WIDENS,');
  console.log('so a REFUSED row names work a person could permit and a FIRES row is permitted everywhere.');
}
main();
