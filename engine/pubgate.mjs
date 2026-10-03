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
 * AND `NAMING AN EARLIER SHA` WAS THE WRONG PREDICATE FOR `CORRECTING AN EARLIER COMMIT`, WHICH THIS FILE
 * MEASURED ON ITSELF. The condition quoted above is met by a MENTION, and a message mentions a sha for every
 * reason a message has. MEASURED: the commit that landed `engine/githooks/pre-push` quoted `ed102dc..e4b1557`
 * as the OUTPUT of a `git push --dry-run` it had run, and this file reported that as a PROVEN correcting edge.
 * The edge channel is now two bands — a `Corrects: <sha>` TRAILER, which is a statement, and a bare MENTION,
 * which is a guess about prose — and BOTH still forbid a cut, because a false edge refuses a publication while
 * a missed one splits a real pair. Only the CLAIM differs, which is the banding
 * §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN asks for. RETIREMENT: this goes when every correcting pair in a range
 * this gate reads arrives DECLARED, because the inferred band is then empty by authorship rather than by luck
 * and the false positive has no population left to land in.
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
/* AND THE COUNT THE CALLER BELIEVES IT IS PUBLISHING, WHICH IS AN INPUT AND NOT A LINE IN A TRANSCRIPT.
 * MEASURED, by the author of this gate, one hour after landing it: a push was gated with
 * `node engine/pubgate.mjs --cut <sha> > /dev/null 2>&1; echo $?`. The exit status was 0 and correct -- the
 * cut split no pair -- and it published FOUR commits of which ONE had been read. Three were peers': two from
 * one lane that are a CORRECTING PAIR, and one from a lane that had not reported. The pair went out WHOLE
 * because the cut happened to sit above both, which is luck, and luck is what a gate exists to stop being
 * load-bearing: a cut one commit lower would have published a citation that lane had already withdrawn, which
 * is the exact hazard its own report named.
 * THE EXIT STATUS WAS NEVER THE THING THAT FAILED. It answers the PAIR RULE. The quantity that was ignored is
 * the DENOMINATOR -- `publishes N of M` -- and it was ignored by a redirection, which is the one way a reader
 * can obey every instruction about gating a push on a status and still not know what the push publishes.
 * CLAUDE.md's backstop is arithmetic (compare the inspection's count against what the push reports moving) and
 * it is unrunnable when the inspection's count went to /dev/null. So the count becomes an ARGUMENT: a caller
 * states how many commits it believes the cut publishes, and this refuses when it disagrees. Discarding the
 * output can no longer hide the number, because the number had to be written down to get a verdict at all.
 * IT IS REQUIRED WITH `--cut` AND REFUSED WITHOUT IT, which is the difference between a rule and a habit --
 * CLAUDE.md measured that an author who has just written a publication rule breaks it under ordinary time
 * pressure, so the mechanical form is the only one that survives being in a hurry. The EXPLORATORY read is
 * this gate with NO `--cut`: it prints the whole chain, every proven edge and every allowed cut, and judges
 * nothing. RETIREMENT: this goes when a push in this tree cannot be issued except by something that reads this
 * gate's range itself, because the count is then never a human's to transcribe. */
const expect = arg('--expect', null);

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
  /* TWO BANDS, BECAUSE `NAMES AN EARLIER SHA` AND `CORRECTS AN EARLIER COMMIT` ARE TWO QUESTIONS AND THIS USED
     TO ANSWER THE SECOND WITH THE FIRST. A message mentions a sha for every reason a message has: an incident
     it is recording, a range it is quoting, a command's OUTPUT it is pasting. MEASURED, by this gate on the
     commit that landed `engine/githooks/pre-push`: that message quoted `ed102dc..e4b1557` as the output of a
     `git push --dry-run` it had run, and this file reported `[1] e4b1557 is corrected by [2] 27fad84` as
     PROVEN. Nothing was corrected and nothing in the prose said it was.
     THE DIRECTION IS CONSERVATIVE AND THAT IS WHY BOTH BANDS STILL FORBID A CUT. A FALSE edge refuses a
     publication; a MISSED one splits a real pair, and §AND-THE-PREFIX-THAT-RULE-NAMES-IS-NOT-THEREBY-SAFE
     prices that at a known-wrong figure published while its correction sits unpublished on a branch nobody
     merges from, read by exactly one person at the moment they have already decided to do the work. So the
     bands differ in what this file CLAIMS and not in what it allows — which is the banding
     §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN asks for, a finding and a confession never summed.
     WHAT A FALSE EDGE COSTS IS NOT NOTHING AND IS STATED SO NOBODY DISCOVERS IT AT A PUSH: it can forbid the
     one cut the report state permits. On that same chain, had [1]'s lane reported and [2]'s not, the allowed
     cuts would have been [0] alone — one of two reported commits — because the cut at [1] was forbidden by an
     edge that was a quotation. The remedy is the DECLARED band: a trailer is read rather than inferred.
     A `Corrects:` TRAILER IS AN ARTIFACT AND THE PROSE IS A GUESS ABOUT ONE, which is §AN-AUDITOR-DERIVES-THE-
     RULE's own standard — derive the rule from the thing that states it, never restate it. */
  for (const m of c.body.matchAll(/^\s*Corrects:\s*([0-9a-f]{7,40})\b/gim)) {
    const target = commits.find((o) => o.sha.startsWith(m[1]) && o.sha !== c.sha);
    if (!target) continue;
    const j = index.get(target.sha);
    if (j < i && !edges.some((e) => e.from === i && e.to === j))
      edges.push({ from: i, to: j, spelling: m[1], band: "DECLARED", inRange: false });
  }
  for (const m of c.body.matchAll(/\b([0-9a-f]{7,40})\b/g)) {
    const target = commits.find((o) => o.sha.startsWith(m[1]) && o.sha !== c.sha);
    if (!target) continue;
    const j = index.get(target.sha);
    if (j >= i || edges.some((e) => e.from === i && e.to === j)) continue;
    /* THE RANGE NOTE IS INFORMATION AND NEVER AN EXCLUSION. `A..B` is the shape the measured false positive
       had, and reporting it tells a reader WHERE to look; excluding on it would miss a correction whose only
       mention of its target is inside a range, which is the expensive direction. */
    const around = c.body.slice(Math.max(0, m.index - 2), m.index + m[1].length + 2);
    edges.push({ from: i, to: j, spelling: m[1], band: "MENTIONED", inRange: around.includes("..") });
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

console.log('## correcting edges — DECLARED by a `Corrects:` trailer, or INFERRED from a bare mention. Both forbid a cut.');
if (edges.length === 0) {
  console.log('  none. THAT IS NOT `no pair exists`: a correction whose message names its target nowhere at all');
  console.log('  is invisible to this check, which is the blind spot, not a clean bill.');
} else {
  for (const e of edges) {
    const how = e.band === "DECLARED"
      ? 'DECLARED — the message carries `Corrects: ' + e.spelling + '`, which is a statement and not a reading of prose'
      : 'INFERRED from a bare mention of "' + e.spelling + '"' +
        (e.inRange ? ' INSIDE A RANGE (`A..B`), which is the shape of this check\'s one MEASURED false positive —'
                   + ' OPEN THE MESSAGE before cutting lower on account of it'
                   : ' — a message names a sha for every reason a message has, so READ IT');
    console.log(`  [${e.to}] ${commits[e.to].short} <- [${e.from}] ${commits[e.from].short}  ${how}`);
  }
  const inferred = edges.filter((e) => e.band === "MENTIONED").length;
  if (inferred)
    console.log(`  ${inferred} of ${edges.length} edge(s) are INFERRED. A lane that writes \`Corrects: <sha>\` moves its own` +
                ' pair into the declared band and out of this sentence.');
}
console.log('');

/* A cut AT index i publishes commits 0..i. It is forbidden when an edge spans it. */
const forbidden = new Set();
for (const e of edges) for (let i = e.to; i < e.from; i += 1) forbidden.add(i);

console.log('## cuts — a cut AT [i] publishes 0..i. FORBIDDEN means it splits a correcting pair, declared or inferred.');
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

if (expect === null) {
  console.log('REFUSED: --cut was given without --expect <n>.');
  console.log('  A cut is a decision about HOW MANY commits go out, and this gate will not hand a push an exit');
  console.log('  status for one unless the caller states that number. The chain and every allowed cut are');
  console.log('  printed above: read the `publishes N of M` you intend and pass it back as --expect N.');
  console.log('  WHY IT IS REQUIRED: a push gated with `pubgate --cut <sha> > /dev/null; echo $?` published FOUR');
  console.log('  commits of which one had been read, and the status was 0 and correct the whole time. The number');
  console.log('  is the part a redirection hides, so the number is an input.');
  process.exit(2);
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
  console.log(`VERDICT: REFUSED — a cut at [${ci}] ${cutSha.slice(0, 7)} splits ${spanning.length} correcting pair(s), declared or inferred:`);
  for (const e of spanning) {
    console.log(`  ${commits[e.to].short} would be published WITHOUT ${commits[e.from].short}, which corrects it.`);
    console.log(`    ${commits[e.from].subject}`);
  }
  process.exit(1);
}
/* THE COUNT IS CHECKED BEFORE THE PAIR VERDICT IS PRINTED, because a caller that disagrees with this gate
 * about what it is publishing has not made a smaller decision -- it has made a decision about a different
 * range, and an ALLOWED line above a wrong number is the reassuring transcript CLAUDE.md names. */
const n = ci + 1;
if (String(expect).trim() !== String(n)) {
  console.log(`VERDICT: REFUSED — you passed --expect ${JSON.stringify(expect)} and a cut at [${ci}] ` +
              `${cutSha.slice(0, 7)} publishes ${n} of ${commits.length} commit(s).`);
  console.log('  This is not a smaller publication than you asked for, it is a DIFFERENT one: the commits');
  console.log('  between them are peers\' and you have not read them. The chain is printed above, oldest first.');
  console.log('  If the extra commits are ones you mean to publish, pass --expect ' + n + ' and say so in your');
  console.log('  report; if they are not, cut lower and check the pair rule again at that cut.');
  process.exit(1);
}
console.log(`VERDICT: ALLOWED BY THE PAIR RULE — a cut at [${ci}] ${cutSha.slice(0, 7)} publishes ${n} of ${commits.length} commit(s)`);
console.log(`  which is the --expect ${n} you stated, and splits no correcting pair this gate can see. It says NOTHING`);
console.log('  about report state — see above.');
process.exit(0);
