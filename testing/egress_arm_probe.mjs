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
// (`witness=pinned` and `actor=tool` must REFUSE) are the BOUND of that decision, so a future owner
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
import { createContext } from 'node:vm';
import { loadZone, processSiteRpc } from '../engine/zone.mjs';


/* The chokepoint, loaded by engine/zone.mjs in ast-worker.html's order, as every process host loads it.
   Nothing below opens a socket; a site question is answered from the engine's own list. */
function zone() {
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder, Headers, Response, swRpc: processSiteRpc };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  createContext(sandbox);
  loadZone(sandbox);
  /* This probe keeps no store between runs, so the HTTP cache restores nothing. */
  sandbox.httpCacheStated(null, null);
  for (const n of ['safeFetchEgressStated', 'safeFetchFiringRefusal', 'safeFetchSignalVector',
                   'safeFetchDefaultArms', 'safeFetchCrossDocumentRefusal'])
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
  /* THE OWNER'S NAVIGATION DECISION, AND THESE FOUR ROWS ARE WHAT SCORE IT. A `want` is spent only where the
     expectation is an invariant of the decision rather than a snapshot of the arm list. Each row is one
     conjunct of the sentence: the page's own child navigable is judged like its `fetch()`, so it fires on any
     path that chose no witness, forced included, and is refused on a path that pinned one or as this tool's act. The
     witness mark is the engine's, relayed off the create notice, the load job and the route declaration. */
  { label: 'bridge.js navigationLoad (a child navigable)',
    want: 'FIRES',
    grep: "git grep -n 'destination: \"document\"' extension/bridge.js",
    facts: { destination: 'document', actor: 'page', driven: 'undriven', pinned: 'unpinned',
             provenance: 'derived',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
  { label: '…the same, on a path that PINNED a witness (must REFUSE — the witness conjunct)',
    want: 'REFUSED',
    grep: 'solver/engine.c engine_navigation_pinned_token — the mark rides the navigation records',
    facts: { destination: 'document', actor: 'page', driven: 'undriven', pinned: 'pinned',
             provenance: 'derived',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
  { label: '…the same, on a FORCED path that pinned nothing (FIRES — judged like a fetch, whose bound is the witness)',
    want: 'FIRES',
    grep: 'as above; the provenance word is engine_provenance_of_running_path\'s',
    facts: { destination: 'document', actor: 'page', driven: 'undriven', pinned: 'unpinned',
             provenance: 'forced',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
  { label: '…the same at actor=tool (must REFUSE — the `actor` conjunct)',
    want: 'REFUSED',
    grep: 'no caller states this pair today; it is the arm\'s own discriminator, exercised',
    facts: { destination: 'document', actor: 'tool', driven: 'unstated', pinned: 'unpinned',
             provenance: 'derived',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
  /* THE AMBIENT SEED AT BOTH ACTOR WORDS, WHICH IS THE PAIR THAT MAKES ONE REPAIR AUDITABLE. The loader used
     to answer `page` for all three of its callers out of one literal, and the seed is the one that is this
     TOOL's act — safe-fetch.js's `_actorOf` says so in its own words. The two rows are kept together because
     the whole claim the repair rests on is that they AGREE: the arm that admits a seed is keyed on the PATH
     and names no actor, so the row a person reads changes and nothing fires differently. A day on which these
     two disagree is a day the repair stopped being free, and nothing else in this tree would say so. */
  { label: 'bridge.js AMBIENT SEED, actor=tool (what it states)',
    grep: "git grep -n 'PROVENANCE_OBSERVED, PROVENANCE_OBSERVED' extension/bridge.js",
    facts: { destination: 'document', actor: 'tool', driven: 'unstated', pinned: 'unstated',
             provenance: 'observed',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
  { label: '…the same at actor=page (the retired literal)',
    grep: 'must agree with the row above, or the repair is no longer outcome-free',
    facts: { destination: 'document', actor: 'page', driven: 'undriven', pinned: 'unstated',
             provenance: 'observed',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
  { label: "bridge.js page fetch() relay, unpinned address",
    grep: "git grep -n 'doc: eng.doc, credentialed: false' extension/bridge.js",
    facts: { destination: '', actor: 'page', driven: 'undriven', pinned: 'unpinned',
             provenance: 'derived',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: false, headers: null } },
  { label: "bridge.js page fetch() relay, PINNED address",
    grep: 'as above; the mark comes off the engine pending line',
    facts: { destination: '', actor: 'page', driven: 'undriven', pinned: 'pinned',
             provenance: 'forced',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: false, headers: null } },
  { label: 'a program load (a lazy chunk)',
    grep: 'Fetch §2.2.5 destination `script` — _isScriptLike',
    facts: { destination: 'script', actor: 'page', driven: 'undriven', pinned: 'unstated',
             provenance: 'forced',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
];

/* THE TWO CONTROLS, ONE EACH WAY. A probe whose every row answers FIRES is indistinguishable from one that
   never reached the walk, and a probe whose every row answers REFUSED is indistinguishable from one whose
   table was never stated — so both directions are demonstrated before any row above is worth reading. */
const CONTROLS = [
  { label: 'CONTROL must REFUSE: this tool composing a data request',
    want: 'REFUSED',
    facts: { destination: '', actor: 'tool', driven: 'unstated', pinned: 'unpinned',
             provenance: 'derived',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: false, headers: null } },
  { label: 'CONTROL must FIRE: the page loading its own code',
    want: 'FIRES',
    facts: { destination: 'script', actor: 'page', driven: 'undriven', pinned: 'unstated',
             provenance: 'forced',
             doc: ['observed', 'undriven', 'unpinned', 'none'], credentialed: true, headers: null } },
  /* THE ZONE'S DOCUMENT RECORD IS A FLOOR UNDER THE ENGINE'S WORDS (lib/document-record.js). The page's own
     unpinned `fetch()` with the engine stating `undriven` and `unpinned`, from three documents: one the zone
     recorded with no mark (the control, which fires), one it recorded as reached by a drive, and one it
     recorded as reached on a pin. The engine's words are identical in all three, so only the record can
     refuse the last two. */
  { label: 'page fetch(), document recorded unmarked (CONTROL: fires)',
    want: 'FIRES', grep: "git grep -n 'doc: eng.doc, credentialed: false' extension/bridge.js",
    facts: { destination: '', actor: 'page', driven: 'undriven', pinned: 'unpinned',
             provenance: 'derived',
             doc: ['derived', 'undriven', 'unpinned', 'none'], credentialed: false,
             headers: null } },
  { label: '…from a document the zone recorded as DRIVEN (must REFUSE at actor)',
    want: 'REFUSED', grep: "git grep -n 'f.doc.driven' extension/lib/safe-fetch.js",
    facts: { destination: '', actor: 'page', driven: 'undriven', pinned: 'unpinned',
             provenance: 'derived',
             doc: ['derived', 'driven', 'unpinned', 'none'], credentialed: false,
             headers: null } },
  { label: '…from a document the zone recorded as PINNED (must REFUSE at witness)',
    want: 'REFUSED', grep: "git grep -n 'f.doc.pinned' extension/lib/safe-fetch.js",
    facts: { destination: '', actor: 'page', driven: 'undriven', pinned: 'unpinned',
             provenance: 'derived',
             doc: ['derived', 'undriven', 'pinned', 'none'], credentialed: false,
             headers: null } },
];

const URL_UNDER_TEST = 'https://example.test/x';

/* A CROSS-DOCUMENT ACT, DECIDED AS A FETCH WITH THE SAME SIGNALS. Each row is a record as the engine writes it
   (engine_record_head's `<verb>\t<doc>\t<world>\t<marks>` and the verb's own fields) with the asking or sending
   document's record, and its marks are taken from field 3 as engine/trusted.mjs and bridge.js take them, then
   handed to safeFetchCrossDocumentRefusal. It is scored against the fetch that states the same signals at the
   receiving origin: a page `fetch()` from the same record, with no cookie jar and no header list. Every row
   must agree with its fetch. `differs` is the armed control: the act beside a fetch whose destination differs,
   which must disagree, so agreement below is not a comparison that cannot fail. */
const POST_ORIGIN = 'https://example.test';
const W = 'root:1:1';
const POSTS = [
  { label: 'post, derived path, nothing pinned (fires, as the page fetch() does)',
    record: `windowproxy.post\tpeer\t${W}\tderived,undriven,unpinned\t*\teA==`,
    doc: ['observed', 'undriven', 'unpinned', 'none'] },
  { label: 'post, forced path, nothing pinned (fires: the bound is the witness)',
    record: `windowproxy.post\tpeer\t${W}\tforced,undriven,unpinned\t*\teA==`,
    doc: ['observed', 'undriven', 'unpinned', 'none'] },
  { label: 'post on a path that PINNED a witness (refused: the witness conjunct)',
    record: `windowproxy.post\tpeer\t${W}\tderived,undriven,pinned\t*\teA==`,
    doc: ['observed', 'undriven', 'unpinned', 'none'] },
  { label: 'post from a DRIVEN timeline (refused: the actor conjunct, this tool\'s act)',
    record: `windowproxy.post\tpeer\t${W}\tderived,driven,unpinned\t*\teA==`,
    doc: ['observed', 'undriven', 'unpinned', 'none'] },
  { label: 'post from a document the zone recorded as PINNED (refused: the record\'s witness)',
    record: `windowproxy.post\tpeer\t${W}\tderived,undriven,unpinned\t*\teA==`,
    doc: ['derived', 'undriven', 'pinned', 'none'] },
  { label: 'windowproxy.get of `length`, derived path (fires, as the page fetch() does)',
    record: `windowproxy.get\tpeer\t${W}\tderived,undriven,unpinned\t-\tlength`,
    doc: ['observed', 'undriven', 'unpinned', 'none'] },
  { label: 'windowproxy.get on a path that PINNED a witness (refused: the witness conjunct)',
    record: `windowproxy.get\tpeer\t${W}\tderived,undriven,pinned\t-\tlength`,
    doc: ['observed', 'undriven', 'unpinned', 'none'] },
  { label: 'windowproxy.close from a DRIVEN timeline (refused: the actor conjunct)',
    record: `windowproxy.close\tpeer\t${W}\tderived,driven,unpinned\t-`,
    doc: ['observed', 'undriven', 'unpinned', 'none'] },
  { label: 'object.set from a document the zone recorded as DRIVEN (refused: the record\'s actor)',
    record: `object.set\tpeer\t${W}\tderived,undriven,unpinned\t-\t7\tk\tv`,
    doc: ['derived', 'driven', 'unpinned', 'none'] },
  { label: 'CONTROL: the pinned post beside a program load, which must DISAGREE',
    record: `windowproxy.post\tpeer\t${W}\tderived,undriven,pinned\t*\teA==`,
    doc: ['observed', 'undriven', 'unpinned', 'none'], differs: 'script' },
  { label: 'CONTROL: the pinned windowproxy.get beside a program load, which must DISAGREE',
    record: `windowproxy.get\tpeer\t${W}\tderived,undriven,pinned\t-\tlength`,
    doc: ['observed', 'undriven', 'unpinned', 'none'], differs: 'script' },
];
/* The sending document a record's world names, read as both hosts read it: the head's name, before its last two
   colons. A root name holding a colon is the case the last-two rule exists for. */
const WORLD_HEADS = [
  { world: 'root:1:1', doc: 'root' },
  { world: 'root.2-00000000000000a1:1:7,root.2-00000000000000a1:1:3', doc: 'root.2-00000000000000a1' },
  { world: 'https://a.test/:3:9', doc: 'https://a.test/' },
];
function askPost(Z, row) {
  const marks = row.record.split('\t')[3];
  const m = marks.split(',');
  const doc = Z.docRecord(...row.doc, 'public');
  const post = Z.safeFetchCrossDocumentRefusal({ targetOrigin: POST_ORIGIN, marks, doc });
  const fetch = Z.safeFetchFiringRefusal({ url: POST_ORIGIN + '/', destination: row.differs || '', provenance: m[0],
                                           pinned: m[2], doc, actor: 'page', driven: m[1], credentialed: false,
                                           headers: null });
  return { post, fetch };
}

/* A site states its document record as lib/document-record.js's first four words, minted here through the
   zone's own docRecord, since the chokepoint accepts only a record that function made. The address space is
   `public`: the firing decision reads none, so every row states the one a new container has. */
function ask(Z, facts) {
  if (!Array.isArray(facts.doc) || facts.doc.length !== 4)
    throw new Error(`a probe row states the document record ${JSON.stringify(facts.doc)} — four words`);
  const f = Object.assign({ url: URL_UNDER_TEST }, facts, { doc: Z.docRecord(...facts.doc, 'public') });
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
  console.log('THE SENDING DOCUMENT a record\'s world names (docRecordWorldDocument):');
  let headsWrong = 0;
  for (const h of WORLD_HEADS) {
    const got = Z.docRecordWorldDocument(h.world);
    if (got !== h.doc) headsWrong++;
    console.log((got === h.doc ? '  ok   ' : '  FAIL ') + JSON.stringify(h.world) + ' => ' + JSON.stringify(got));
  }
  if (headsWrong) process.exitCode = 1;
  console.log('A CROSS-DOCUMENT ACT against the fetch with the same signals (safeFetchCrossDocumentRefusal):');
  let postsWrong = 0;
  for (const r of POSTS) {
    const got = askPost(Z, r);
    const agree = got.post === got.fetch;
    const ok = r.differs ? !agree : agree;
    if (!ok) postsWrong++;
    console.log((ok ? '  ok   ' : '  FAIL ') + r.label);
    console.log('    post => ' + (got.post === null ? 'FIRES' : 'REFUSED at ' + got.post) + '   fetch => ' +
                (got.fetch === null ? 'FIRES' : 'REFUSED at ' + got.fetch));
  }
  if (postsWrong) {
    console.log(postsWrong + ' CROSS-DOCUMENT ROW(S) WERE NOT DECIDED AS THEIR FETCH (or a control agreed): ' +
                'a cross-document act has an arm of its own, or the comparison cannot fail.');
    process.exitCode = 1;
  }
  console.log('');
  console.log('EVERY ROW ABOVE IS ABOUT THE DEFAULT (UNWIDENED) TABLE. A per-origin grant only ever WIDENS,');
  console.log('so a REFUSED row names work a person could permit and a FIRES row is permitted everywhere.');
}
main();
