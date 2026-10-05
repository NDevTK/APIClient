/* WHICH CENSUS KEYS THE ENGINE SOURCE EMITS THAT THE INSTALLED ARTIFACT DOES NOT STATE.
 *
 * THE DEFECT THIS EXISTS FOR, MEASURED AS A WHOLE-CORPUS OUTAGE RATHER THAN FEARED. The trusted zone is
 * JavaScript INTERPRETED FROM THE TREE, so it deploys ON WRITE; the engine is WASM and is live only after a
 * build. A cross-boundary pair can therefore be landed WHOLE -- both halves in one publication, which is what
 * CLAUDE.md's §A-CROSS-BOUNDARY-DIFF asks for -- and still be HALF-DEPLOYED, because publishing the engine half
 * does not BUILD it. The zone's own new assert then fires in the half that IS live. That happened to the
 * `addressRoot` key: the tree's `extension/bridge.js` named it and the installed `qjs.wasm` did not, so a
 * cross-key claim over the two partitions aborted the trusted zone on every site it was driven against --
 * `runsTotal 0`, every engine counter null, no result document at all, three runs of three with zero spread.
 *
 * SO WHOLE-LANDING IS NECESSARY AND NOT SUFFICIENT WHEN ONE ZONE IS INTERPRETED AND THE OTHER IS COMPILED, and
 * the quantity nothing was reading is the one this prints: a key the ENGINE SOURCE emits that the ARTIFACT THE
 * ZONE WILL ACTUALLY LOAD has never heard of. It is a fact about the PAIR and about neither half alone, which
 * is why no reader of either half could have had it.
 *
 * THE KEY SET IS DERIVED FROM THE EMITTER AND IS NEVER A LIST HERE. `json_buf_key(&b, "NAME")` is the construct
 * the engine composes a census row with, so the population is every file that CONTAINS that construct and every
 * name it passes -- a derivation anybody can re-run, which is the whole of why it cannot drift from what the
 * engine actually emits. A hand-kept list would be the second copy §AN-AUDITOR-DERIVES-THE-RULE forbids, and
 * this project has already paid for one of those.
 *
 * WHAT IT DOES NOT ASK, SAID HERE BECAUSE A READER WILL OTHERWISE READ MORE INTO A CLEAN RUN THAN IT MEANS:
 *   - whether any ZONE READER actually reads a tree-only key. A key absent from the artifact is only a hazard
 *     where something in `extension/` asserts on it, and that question is a parse of the zone rather than a
 *     fixed-string count. A clean run here means NO key is tree-only, which is strictly stronger than "no
 *     reader trips"; a finding here is a CANDIDATE and is reported as one.
 *   - whether the artifact is the one a given drive loaded. `HARNESS_EXT_DIR` can point a drive at another
 *     extension directory entirely, so this answers about the INSTALLED one and names which file it read.
 *   - anything about a key the engine emits through a helper rather than through this construct. That is this
 *     instrument's lexer, and a lexer is an instrument's population: the count it prints is a FLOOR and the
 *     banner says the construct it matched so the next reader knows what it could not see.
 *
 * THIS IS NOT A BUILD STAGE, AND THE CONDITION THAT SAID IT SHOULD BE IS REFUTED HERE RATHER THAN DELETED,
 * BECAUSE IT IS THE PLACEMENT A READER RE-DERIVES. It read: this goes when the build REFUSES to install an
 * artifact that does not state every census key the engine source it was built from emits. The reasoning is
 * sound and the ARITHMETIC kills it: a build INSTALLS what it just compiled, so after the install every key the
 * tree emits is in the artifact BY CONSTRUCTION and this reader answers clean on every successful build --
 * §AN-ASSERT-WHOSE-TWO-SIDES-CANNOT-DISAGREE, as a whole stage. Run BEFORE the install it reports a gap the
 * build is about to close, which is true and actionable by nobody. The gap only exists BETWEEN builds, so the
 * act whose correctness depends on it is a DRIVE and never a compile.
 *
 * NAMED RESIDUAL — the code is CORRECT for what it answers and NARROWER than the spec, because it is in the
 * path of no act at all:
 *   NOT COVERED: nothing invokes this. A lane that drives the installed artifact without running it gets the
 *     same half-deployed abort that produced it, and the only thing standing between them and that is
 *     remembering a command -- which CLAUDE.md rates as indistinguishable from a convention nobody honours.
 *   THE NEXT DIFF BUILDS the invocation into the DRIVE's own path (`testing/harness.js`'s restart, or the
 *     corpus driver), and it carries a DECISION this file must not guess: what a drive DOES on a nonzero
 *     verdict. REFUSING is wrong -- the zone's cross-key gate now reads an older wasm correctly, so driving an
 *     older artifact is legitimate and a refusal would delete a capability somebody just landed. WARNING is the
 *     non-check §A-DESTRUCTIVE-STEP-IS-GATED-BY-THE-CHECK'S-EXIT-STATUS forbids, with a reassuring transcript
 *     above the drive. The answer is likeliest to be that a drive RECORDS the tree-only set onto its census row,
 *     so a reading taken against a half-deployed pair says so in the artifact a reader later quotes -- but that
 *     is a row, a reader and a shape, which is a subproblem and not a line.
 *   ITS ABSENCE WOULD SHOW as a census row whose engine counters are null and whose `runsTotal` is 0, with the
 *     trusted zone's own assert in the log and nothing in the row saying the artifact predated the key the
 *     assert was about -- which is exactly the state three `vscode.dev` passes were in, with zero spread,
 *     before the reader that produced them was repaired.
 *
 * RETIREMENT: this goes when a DRIVE in this tree cannot be started without this reader's verdict having been
 * taken, so the tree-only set travels with the reading instead of being a command somebody remembers. MEASURED
 * ABSENT before being written, so this condition is not born met: `grep -c artifactkeys` answers 0 in each of
 * `engine/build.mjs`, `testing/harness.js` and `testing/corpus/site.mjs`, against `grep -c fieldgate
 * engine/build.mjs` answering nonzero as the armed control and an invented token answering 0. */
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { spawnSync } from 'node:child_process';

const ENGINE = dirname(new URL(import.meta.url).pathname);
const ROOT = dirname(ENGINE);
const ARTIFACT = join(ROOT, 'extension', 'lib', 'qjs', 'qjs.wasm');
const SIDECAR = join(ROOT, 'extension', 'lib', 'qjs', 'qjs.mjs.build.json');
const KEY_CALL = /json_buf_key\(&b, "([A-Za-z][A-Za-z0-9_]*)"\)/g;

/* EVERY `.c` AND `.h` UNDER THE ENGINE THAT CONTAINS THE CONSTRUCT, WALKED RATHER THAN NAMED. A path typed here
   would be a scope choice wearing a derivation's clothes, which is the defect §AND-THE-COMMONEST-WAY-A-
   POPULATION-COMES-OUT-SHORT is about -- and the file that emits a key is free to move. */
const walk = (d, out = []) => {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    const p = join(d, e.name);
    if (e.isDirectory()) { if (e.name !== 'qjs' && e.name !== 'lexbor' && e.name !== 'out') walk(p, out); }
    else if (/\.[ch]$/.test(e.name)) out.push(p);
  }
  return out;
};

const emitters = new Map();   /* key -> Set of files that emit it */
for (const f of walk(join(ROOT, 'engine'))) {
  const txt = readFileSync(f, 'utf8');
  if (!txt.includes('json_buf_key')) continue;
  for (const m of txt.matchAll(KEY_CALL)) {
    if (!emitters.has(m[1])) emitters.set(m[1], new Set());
    emitters.get(m[1]).add(f.slice(ROOT.length + 1));
  }
}
const keys = [...emitters.keys()].sort();

if (!existsSync(ARTIFACT)) {
  console.log('artifactkeys: CANNOT ASK — no installed artifact at ' + ARTIFACT.slice(ROOT.length + 1) +
              '. ' + keys.length + ' census key(s) derived from ' +
              new Set([...emitters.values()].flatMap((s) => [...s])).size + ' emitting file(s), and nothing to ' +
              'compare them against. An ABSENT artifact and an artifact MISSING A KEY are different facts and ' +
              'this is the first; build and install, then ask again.');
  process.exit(0);
}

/* A FIXED STRING AND NEVER A PATTERN, over the BYTES. A key is an identifier, so a regex would be answering a
   question about escaping rather than about the artifact -- §AND-A-CONTROL-CANNOT-CALIBRATE-A-PATTERN's defect
   with the metacharacter supplied by the subject instead of by the reader. */
const bytes = readFileSync(ARTIFACT);
const states = (s) => bytes.includes(Buffer.from(s, 'utf8'));

/* THE TWO CONTROLS, AND THE VERDICT IS REFUSED WITHOUT BOTH. An invented key must read ABSENT, or this is
   matching something other than what it thinks; and at least one DERIVED key must read PRESENT, or the probe
   never reached the artifact at all and every `absent` is the unarmed-probe silence §A-CONTROL-ARMS-ONLY-ON-A-
   SITE-THE-INSTRUMENT-CAN-JUDGE says manufactures a blind spot that is not there. */
const INVENTED = 'zzQqNeverEmittedByAnyComposer';
const present = keys.filter(states);
if (states(INVENTED) || (keys.length && !present.length)) {
  console.log('artifactkeys: REFUSED — the controls did not separate. invented key present: ' +
              states(INVENTED) + '; derived keys present: ' + present.length + ' of ' + keys.length + '. ' +
              'A verdict from an unarmed probe is a statement about the probe, so none is printed.');
  process.exit(2);
}

const treeOnly = keys.filter((k) => !states(k));

/* THE DISTANCE, BESIDE THE FINDING, BECAUSE IT IS THE QUANTITY THAT EXPLAINS IT. A tree-only key means the
   artifact predates the commit that added it, and how far it predates the tip is what says whether the answer
   is "rebuild" or "this artifact is an oracle for nothing". A stamp names a revision where a reader needs a
   distance, and a distance is one command. */
let stampLine = 'stamp: none beside the artifact — so how far behind the tip it is cannot be stated';
if (existsSync(SIDECAR)) {
  try {
    const s = JSON.parse(readFileSync(SIDECAR, 'utf8'));
    const n = spawnSync('git', ['rev-list', '--count', s.head + '..origin/main'],
                        { cwd: ROOT, encoding: 'utf8' });
    stampLine = 'stamp: ' + String(s.head).slice(0, 7) + ' (' + s.assertRegime + ', dirty ' +
                (Array.isArray(s.dirty) ? s.dirty.length : '?') + ') — ' +
                (n.status === 0 ? n.stdout.trim() + ' commit(s) behind origin/main' :
                 'its distance from origin/main is not resolvable in this shallow clone');
  } catch { stampLine = 'stamp: the sidecar beside the artifact did not parse'; }
}

console.log('artifactkeys: ' + keys.length + ' census key(s) derived from the construct `json_buf_key(&b, "…")` ' +
            'over ' + new Set([...emitters.values()].flatMap((s) => [...s])).size + ' emitting file(s); ' +
            present.length + ' stated by the installed artifact.');
console.log('  artifact: extension/lib/qjs/qjs.wasm (' + statSync(ARTIFACT).size + ' bytes)');
console.log('  ' + stampLine);
console.log('  controls ARMED: the invented key reads absent and ' + present.length + ' derived key(s) read present.');
if (!treeOnly.length) {
  console.log('  NO key the engine source emits is absent from the artifact. That is stronger than "no zone ' +
              'reader trips": it says the pair cannot be half-deployed on any of these keys at all.');
  process.exit(0);
}
console.log('\n  ' + treeOnly.length + ' KEY(S) THE ENGINE SOURCE EMITS AND THIS ARTIFACT HAS NEVER HEARD OF — ' +
            'each is a CANDIDATE for the half-deployed seam, and a hazard exactly where something in ' +
            '`extension/` asserts on it:');
for (const k of treeOnly)
  console.log('    ' + k + '   emitted by ' + [...emitters.get(k)].join(' '));
console.log('\n  The zone deploys ON WRITE and the engine needs a BUILD, so a key here is not a defect in either ' +
            'half — it is the PAIR being half-live. Build and install to close it, or confirm no reader in ' +
            '`extension/` asserts on these and say so at the reader. Whole-landing did not prevent this and ' +
            'cannot: publishing both halves of a cross-boundary diff does not compile one of them.');
process.exit(1);
