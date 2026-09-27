/* WHAT A PUSH WOULD PUBLISH, AND WHICH CUTS OF THE CHAIN ARE FORBIDDEN.
 *
 * CLAUDE.md §THE-POST-COMMIT-READ-IS-SCOPED-TO-ONE-COMMIT states the defect this answers: `git show --stat
 * <sha>` is scoped to ONE COMMIT while `git push <sha>:<ref>` publishes that commit AND EVERY COMMIT BENEATH
 * IT, so the one verification that file prescribes is structurally blind to what a push actually sends. The
 * cure it names is a range read, and a range read has two ends: §AND-CAPTURING-THE-SHA-CLOSES-ONLY-THE-
 * RIGHT-HAND-SIDE records that a remote-tracking ref is only as fresh as the last fetch, so an inspection
 * whose left end was not just refreshed prints FEWER commits than the push will move — silent, and in the
 * reassuring direction. THIS FILE DOES THE FETCH AND THE RANGE READ IN ONE PROCESS so a stale left end is
 * unspellable rather than merely discouraged, and it prints both ends' shas so the range it read is a fact a
 * reader can check rather than one they have to trust.
 *
 * AND THE CUT IS THE HALF NO EXISTING INSTRUMENT ANSWERS. §AND-THE-PREFIX-THAT-RULE-NAMES-IS-NOT-THEREBY-SAFE
 * records that a lane which lands TWICE — the second commit correcting a figure the first one wrote — has its
 * pair SPLIT the moment a peer commits between them, and that the literal prefix then contains the WRONG
 * number and excludes the repair. Its stated retirement condition is that a correcting commit NAME the commit
 * it corrects in its own message, "so a publication inspection can refuse to split the pair without reading
 * anybody's report". Lanes in this tree already obey that half; nothing read it. This reads it, and it REFUSES
 * — `--cut <sha>` exits nonzero when that cut splits a pair, so the push can be GATED on the exit status
 * rather than on a transcript, which is §A-DESTRUCTIVE-STEP-IS-GATED-BY-THE-CHECK'S-EXIT-STATUS owed to the
 * one irreversible outward act this project has.
 *
 * WHAT IT CANNOT SEE, STATED HERE BECAUSE §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN RATES A CONFESSION SUMMED INTO
 * A VERDICT AS THE DEFECT: it does not know which lane has REPORTED. That is a coordinator's knowledge, it
 * lives in no artifact, and a tool that guessed it would be asserting the one fact that decides the prefix. So
 * this prints the CONSTRAINTS and never the answer: the correcting edges it can prove, and the cuts those
 * edges forbid. The coordinator supplies the report state.
 *
 * LANE ATTRIBUTION IS INFERRED AND SAYS SO ON EVERY ROW. Two commits sharing a path are USUALLY one lane and
 * are not necessarily: §AND-THE-SAME-LAUNDERING-HAPPENS-WITH-A-COMMIT records a coordinator telling a lane a
 * PEER had moved its base when the commit was that lane's own, from a relative term relayed between two frames
 * of reference. So the overlap is printed as EVIDENCE with the shared paths named, never as a verdict, and the
 * author field is printed beside it because that is the field the commit already carries and nobody reads.
 *
 * A FILE LIST IS ASKED OF THE COMMIT AND NEVER REMEMBERED. Same rule, same reason: a coordinator stating a
 * commit's paths from recollection has stated the paths it dispatched rather than the paths that landed.
 *
 * RETIREMENT: this record goes when a push in this tree cannot be issued without this gate's exit status in
 * front of it — a wrapper that takes the destination and the sha and refuses to spawn `git push` on a nonzero
 * verdict — because the gate is then in the path of the act instead of being a command somebody remembers.
 */

import { execFileSync } from 'node:child_process';

const REPO = new URL('..', import.meta.url).pathname;

function git(args, { allowFail = false } = {}) {
  try {
    return execFileSync('git', args, { cwd: REPO, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
  } catch (e) {
    if (allowFail) return null;
    throw new Error(`git ${args.join(' ')} failed: ${e.stderr || e.message}`);
  }
}

function arg(name, dflt) {
  const i = process.argv.indexOf(name);
  return i < 0 ? dflt : process.argv[i + 1];
}

const remote = arg('--remote', 'origin');
const branch = arg('--branch', 'main');
const right = arg('--head', 'HEAD');
const cut = arg('--cut', null);

/* The fetch and the range read are ONE process. Nothing between them is a command a reader has to remember. */
git(['fetch', remote, branch]);
const leftRef = `${remote}/${branch}`;
const left = git(['rev-parse', leftRef]).trim();
const rightSha = git(['rev-parse', right]).trim();

/* A range is meaningless unless the left end is an ancestor: a diverged HEAD is a different question and this
 * gate does not answer it. §AND-THE-ONE-FILE-`git diff origin/main`-CANNOT-SEE is why the refusal is loud. */
const isAnc = git(['merge-base', '--is-ancestor', left, rightSha], { allowFail: true });
if (isAnc === null) {
  console.log(`REFUSED: ${leftRef} (${left.slice(0, 7)}) is NOT an ancestor of ${right} (${rightSha.slice(0, 7)}).`);
  console.log('  A push of this head would not be a fast-forward, so the commits it publishes are not this range.');
  console.log('  Establish the relationship first: git rev-list --left-right --count ' + leftRef + '...' + right);
  process.exit(2);
}

const shas = git(['rev-list', '--reverse', `${left}..${rightSha}`]).trim().split('\n').filter(Boolean);

if (shas.length === 0) {
  console.log(`NOTHING TO PUBLISH: ${leftRef} is already at ${right} (${rightSha.slice(0, 7)}).`);
  process.exit(0);
}

const US = '\x1f';
const commits = shas.map((sha) => {
  const [full, an, aI, subject, body] = git(['show', '-s', `--format=%H${US}%an${US}%aI${US}%s${US}%B`, sha])
    .split(US);
  const numstat = git(['show', '--numstat', '--format=', sha]).trim().split('\n').filter(Boolean);
  const files = numstat.map((l) => {
    const [add, del, path] = l.split('\t');
    return { add: Number(add) || 0, del: Number(del) || 0, path };
  });
  return {
    sha: full.trim(),
    short: full.trim().slice(0, 7),
    author: an,
    when: aI.slice(11, 16),
    subject,
    body: body || '',
    files,
    add: files.reduce((a, f) => a + f.add, 0),
    del: files.reduce((a, f) => a + f.del, 0),
  };
});

const byShort = new Map(commits.map((c) => [c.short, c]));
const index = new Map(commits.map((c, i) => [c.sha, i]));

/* A CORRECTING EDGE is proven only where a later commit's own message names an earlier commit IN THIS RANGE.
 * A correction that names nothing is invisible here and that is a blind spot rather than a clean bill. */
const edges = [];
for (let i = 0; i < commits.length; i += 1) {
  const c = commits[i];
  for (const m of c.body.matchAll(/\b([0-9a-f]{7,40})\b/g)) {
    const target = commits.find((o) => o.sha.startsWith(m[1]) && o.sha !== c.sha);
    if (!target) continue;
    const j = index.get(target.sha);
    if (j < i && !edges.some((e) => e.from === i && e.to === j)) edges.push({ from: i, to: j, spelling: m[1] });
  }
}

console.log(`# publication inspection — ${leftRef} ${left.slice(0, 7)} .. ${right} ${rightSha.slice(0, 7)}`);
console.log(`#   fetched in this process, so the left end is this run's; range read: git rev-list --reverse ${left.slice(0, 7)}..${rightSha.slice(0, 7)}`);
console.log(`#   ${commits.length} commit(s) would be published by a push of ${rightSha.slice(0, 7)} — that is the DENOMINATOR every`);
console.log('#   figure below is a part of, and a push that reports moving a different number moved a different range.');
console.log('');

console.log('## the chain, OLDEST FIRST — a push of any commit publishes it and everything above it in this list');
for (let i = 0; i < commits.length; i += 1) {
  const c = commits[i];
  console.log(`  [${i}] ${c.short}  ${c.when}  ${c.author}  +${c.add}/-${c.del}  ${c.files.length} file(s)`);
  console.log(`        ${c.subject}`);
  for (const f of c.files) console.log(`          ${f.path}  +${f.add}/-${f.del}`);
}
console.log('');

console.log('## correcting edges — PROVEN, from each commit\'s own message naming an earlier sha in this range');
if (edges.length === 0) {
  console.log('  none proven. THAT IS NOT `no pair exists`: a correction whose message names no sha is invisible');
  console.log('  to this check, which is the blind spot, not a clean bill.');
} else {
  for (const e of edges) {
    console.log(`  [${e.to}] ${commits[e.to].short} is corrected by [${e.from}] ${commits[e.from].short}  (named as "${e.spelling}")`);
  }
}
console.log('');

/* A cut AT index i publishes commits 0..i. It is forbidden when an edge spans it. */
const forbidden = new Set();
for (const e of edges) for (let i = e.to; i < e.from; i += 1) forbidden.add(i);

console.log('## cuts — a cut AT [i] publishes 0..i. FORBIDDEN means it splits a proven correcting pair.');
for (let i = 0; i < commits.length; i += 1) {
  const bad = forbidden.has(i);
  console.log(`  cut at [${i}] ${commits[i].short}  ->  publishes ${i + 1} commit(s)  ${bad ? 'FORBIDDEN — splits a pair' : 'allowed by the pair rule'}`);
}
console.log('');

console.log('## lane attribution — INFERRED from shared paths, never asserted. Two lanes on one file look like one.');
const seen = new Map();
for (let i = 0; i < commits.length; i += 1) {
  for (const f of commits[i].files) {
    if (!seen.has(f.path)) seen.set(f.path, []);
    seen.get(f.path).push(i);
  }
}
const groups = [...seen.entries()].filter(([, idx]) => idx.length > 1);
if (groups.length === 0) {
  console.log('  no path is touched by two commits in this range, so this channel offers no evidence either way.');
} else {
  for (const [path, idx] of groups) {
    console.log(`  ${idx.map((i) => `[${i}] ${commits[i].short}`).join(' + ')}  share  ${path}`);
  }
}
console.log('');

console.log('## what this gate does NOT know');
console.log('  WHICH LANE HAS REPORTED. CLAUDE.md makes the publishable set the prefix ending at the oldest');
console.log('  UNREPORTED commit, AND never a cut between a lane\'s commit and a later commit of its own that');
console.log('  corrects it. This gate proves the SECOND constraint and is silent on the first, which lives in no');
console.log('  artifact. Supply the report state yourself; a cut this gate calls `allowed` is allowed BY THE PAIR');
console.log('  RULE ALONE and says nothing about whether its lanes have finished reading their own commits.');
console.log('');

if (cut === null) {
  console.log('NO --cut GIVEN: nothing was judged. Pass --cut <sha> to get an exit status a push can be gated on.');
  process.exit(0);
}

const cutSha = git(['rev-parse', cut]).trim();
if (!index.has(cutSha)) {
  console.log(`REFUSED: --cut ${cut} (${cutSha.slice(0, 7)}) is not in this range.`);
  console.log('  A cut outside the range is not a smaller publication, it is a different question.');
  process.exit(2);
}
const ci = index.get(cutSha);
const spanning = edges.filter((e) => e.to <= ci && ci < e.from);
if (spanning.length > 0) {
  console.log(`VERDICT: REFUSED — a cut at [${ci}] ${cutSha.slice(0, 7)} splits ${spanning.length} proven correcting pair(s):`);
  for (const e of spanning) {
    console.log(`  ${commits[e.to].short} would be published WITHOUT ${commits[e.from].short}, which corrects it.`);
    console.log(`    ${commits[e.from].subject}`);
  }
  process.exit(1);
}
console.log(`VERDICT: ALLOWED BY THE PAIR RULE — a cut at [${ci}] ${cutSha.slice(0, 7)} publishes ${ci + 1} of ${commits.length} commit(s)`);
console.log('  and splits no proven correcting pair. It says NOTHING about report state — see above.');
process.exit(0);
