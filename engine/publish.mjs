/* PUT THE PUBLICATION GATE IN THE PATH OF THE ACT, SO A PUSH CANNOT BE ISSUED WITHOUT ITS VERDICT.
 *
 * `engine/pubgate.mjs` answers whether a cut is allowed and refuses a caller that has not stated how many
 * commits it believes it is publishing. Both of those are REAL and neither is reachable from a push, because
 * nothing connects them: the gate is a command somebody remembers and the push is a separate command whose
 * argument list the gate never sees. CLAUDE.md names this residual in the gate's own words -- "a wrapper taking
 * the destination and the sha that refuses to spawn `git push` on a nonzero verdict" -- and names why prose is
 * not enough for it: its own author broke the read-the-inspection rule three times in one session, and the
 * mechanical form is the only one that survives being in a hurry.
 *
 * MEASURED, which is why this exists rather than another paragraph: a push gated with
 * `node engine/pubgate.mjs --cut <sha> > /dev/null 2>&1; echo $?` published FOUR commits of which ONE had been
 * read. The status was 0 and CORRECT the whole time -- it answers the PAIR RULE -- and the quantity that was
 * ignored went to /dev/null. Three of the four were peers': two that are a CORRECTING PAIR, and one from a lane
 * that had not reported. The pair went out whole because the cut happened to sit above both, which is luck.
 *
 * WHAT THIS CLOSES THAT THE GATE ALONE CANNOT.
 *   THE GATE IS UNBYPASSABLE because it is spawned here, with stdio INHERITED so its chain is on the operator's
 *     screen rather than in a pipe. A nonzero verdict exits with that status and spawns nothing.
 *   THE ARITHMETIC BACKSTOP IS PERFORMED. CLAUDE.md calls it "the only check that survives both ends moving" --
 *     the inspection's commit count against what the push reports moving -- and says in as many words that no
 *     instrument here does it: the gate prints its denominator, the push prints its range, and a reader joins
 *     them by eye. This asks `git ls-remote` for each destination's CURRENT sha, counts `old..sha` itself, and
 *     refuses when that disagrees with --expect. It is read by CONTENT at both ends and parses no push output.
 *   BOTH REFS OR NEITHER. A `main` that is ahead of the session branch is a `main` whose extra commits the next
 *     merge silently undoes -- measured in this project at 280 commits -- so the two destinations must already
 *     stand at ONE sha before anything is pushed, and a divergence REFUSES rather than being papered over. That
 *     is the two-ref state CLAUDE.md says nobody thinks to look at, and the refusal is how it gets looked at.
 *   EACH PUSH IS ITS OWN SPAWN, carrying nothing else, which is the shape CLAUDE.md requires of the command
 *     itself; and the verification afterwards is by CONTENT -- the remote's sha re-read, and the tree's file
 *     count printed -- because a SHA says nothing whatever about a tree and an `ls-remote` check has already
 *     passed over an empty-tree commit in this repository.
 *
 * WHAT IT DOES NOT DO, STATED SO THE GAP IS NOT INFERRED: it does not know WHICH LANE HAS REPORTED. The gate
 * says so of itself and it is still true here -- the publishable prefix ends at the oldest UNREPORTED commit,
 * which lives in no artifact, and an --expect that matches is allowed BY THE PAIR RULE AND THE COUNT ALONE.
 * RETIREMENT: this goes when the report state is an artifact a tool can read -- a lane's own commit saying in
 * its message that its author considers it final -- because the one judgement left here is then checkable too.
 */
import { spawnSync } from "node:child_process";

const NET = /could not read from remote|connection (reset|timed out)|early eof|rpc failed|unexpected disconnect|failed to connect|temporary failure in name resolution|operation timed out/i;

function git(args, { inherit = false, allowFail = false } = {}) {
  const r = spawnSync("git", args, { encoding: "utf8", stdio: inherit ? "inherit" : "pipe" });
  if (r.status !== 0 && !allowFail) {
    process.stderr.write(`[publish] git ${args.join(" ")} exited ${r.status}\n${r.stderr || ""}`);
    process.exit(3);
  }
  return r;
}
const say = (s) => process.stdout.write("[publish] " + s + "\n");
const die = (code, ...lines) => { for (const l of lines) process.stderr.write("[publish] " + l + "\n"); process.exit(code); };

const argv = process.argv.slice(2);
const flag = (n) => { const i = argv.indexOf(n); return i < 0 ? null : argv[i + 1]; };
const rev = argv.find((a) => !a.startsWith("--") && argv[argv.indexOf(a) - 1] !== "--expect"
                                                && argv[argv.indexOf(a) - 1] !== "--remote") || null;
const expect = flag("--expect");
const remote = flag("--remote") || "origin";

if (rev === null || expect === null)
  die(2, "usage: node engine/publish.mjs <sha> --expect <n> [--remote origin]",
         "  <sha> is the CUT -- the commit to publish, and everything beneath it.",
         "  --expect <n> is how many commits you believe that publishes. It is REQUIRED, and it is required",
         "  here for the same reason engine/pubgate.mjs requires it: the count is the part a redirection hides,",
         "  so it is an input. Run the gate with no --cut first to read the chain and every allowed cut.");

/* THE SHA IS RESOLVED ONCE AND EVERY LATER COMMAND NAMES THE RESOLVED VALUE, never the ref it came from.
   CLAUDE.md: `HEAD` is a MOVING REFERENCE in a shared checkout, and two evaluations of one are two questions. */
const sha = git(["rev-parse", "--verify", `${rev}^{commit}`]).stdout.trim();

/* THE DESTINATIONS. `main` is the trunk and the other is THIS branch, read rather than spelled, because a
   hardcoded session-branch name is a coordinate and rots. A detached HEAD has no second destination to name. */
const here = git(["rev-parse", "--abbrev-ref", "HEAD"]).stdout.trim();
if (here === "HEAD")
  die(2, "HEAD is detached, so there is no session branch to publish to and this would push to ONE ref.",
         "CLAUDE.md: a push to `main` alone is REVERSED by the next merge of the session branch.");
const dests = here === "main" ? ["main"] : ["main", here];
if (dests.length === 1)
  say("WARNING: the current branch IS main, so this publishes to ONE ref. That is the state CLAUDE.md's " +
      "two-ref rule exists to prevent; it is allowed here only because there is no second ref to name.");

say(`cut ${sha.slice(0, 7)}  --expect ${expect}  destinations: ${dests.join(", ")} on ${remote}`);
say("running the gate; its whole chain is below and is not piped anywhere ──────────────────────────────────");

/* THE GATE, SPAWNED WITH stdio INHERITED. A nonzero verdict spawns nothing: this is `if <check>; then <act>`
   and never `<check>; <act>`, which CLAUDE.md calls a NON-check with a reassuring transcript above it. */
const gate = spawnSync(process.execPath,
                       [new URL("pubgate.mjs", import.meta.url).pathname, "--remote", remote,
                        "--branch", "main", "--head", sha, "--cut", sha, "--expect", String(expect)],
                       { stdio: "inherit" });
if (gate.status !== 0)
  die(gate.status === null ? 3 : gate.status,
      "──────────────────────────────────────────────────────────────────────────────────────────────────────",
      `the gate exited ${gate.status} and NOTHING was pushed. Read its verdict above: a REFUSED count means you`,
      "are publishing a DIFFERENT range than you stated and the commits between are peers' that you have not",
      "read; a split pair means cut lower.");
say("the gate allowed it ───────────────────────────────────────────────────────────────────────────────────");

/* BOTH DESTINATIONS MUST ALREADY STAND AT ONE SHA, read from the REMOTE rather than from a tracking ref, which
   is only as fresh as the last fetch. The count is then one count and serves both. */
const before = new Map();
for (const d of dests) {
  const out = git(["ls-remote", remote, `refs/heads/${d}`]).stdout.trim();
  if (!out) die(2, `${remote}/${d} does not exist. This tool publishes to refs that are already there.`);
  before.set(d, out.split(/\s+/)[0]);
}
const distinct = [...new Set(before.values())];
if (distinct.length > 1)
  die(1, "REFUSED: the destinations are at DIFFERENT commits, so one count cannot describe both pushes:",
         ...dests.map((d) => `  ${d} ${before.get(d).slice(0, 7)}`),
         "They are ONE line of history. A `main` ahead of the session branch is a `main` whose extra commits",
         "the next merge silently undoes -- 280 commits' worth, once, in this project. Reconcile them first and",
         "look at WHICH is ahead: `git rev-list --left-right --count` over the two, and read the gap.");

const old = distinct[0];
if (old === sha) { say(`NOTHING TO PUBLISH: every destination is already at ${sha.slice(0, 7)}.`); process.exit(0); }
if (git(["merge-base", "--is-ancestor", old, sha], { allowFail: true }).status !== 0)
  die(1, `REFUSED: ${old.slice(0, 7)} is not an ancestor of ${sha.slice(0, 7)}, so this is not a fast-forward.`,
         "What such a push publishes is not this range, and this tool will not force one: history is never",
         "rewritten here to repair anything.");

/* THE ARITHMETIC BACKSTOP, PERFORMED RATHER THAN LEFT TO AN EYE. */
const n = Number(git(["rev-list", "--count", `${old}..${sha}`]).stdout.trim());
if (String(n) !== String(expect).trim())
  die(1, `REFUSED: ${remote}'s own tip is ${old.slice(0, 7)} and ${old.slice(0, 7)}..${sha.slice(0, 7)} is ${n}`,
         `commit(s), not the --expect ${expect} you stated.`,
         "This is the backstop CLAUDE.md calls the only check that survives both ends moving, and the gap means",
         "the remote moved after the gate read it or you stated the wrong number. Re-read the chain:",
         `  git log --oneline ${old.slice(0, 7)}..${sha.slice(0, 7)}`);
say(`${n} commit(s) to publish, which is the --expect you stated and the remote's own tip says so: ` +
    `${old.slice(0, 7)}..${sha.slice(0, 7)}`);

/* EACH PUSH IS ITS OWN SPAWN AND CARRIES NOTHING ELSE. A retry is only ever for a NETWORK failure: a refusal
   is a verdict and retrying one is arguing with it. */
const moved = [];
for (const d of dests) {
  let ok = false;
  for (let attempt = 0, wait = 2000; attempt < 5 && !ok; attempt++, wait *= 2) {
    if (attempt) { say(`retrying ${d} after ${wait / 2000}s (network)`); spawnSync("sleep", [String(wait / 2000)]); }
    const r = spawnSync("git", ["push", remote, `${sha}:refs/heads/${d}`], { encoding: "utf8" });
    process.stderr.write(r.stderr || "");
    if (r.status === 0) { ok = true; break; }
    if (!NET.test(r.stderr || "")) break;
  }
  if (!ok)
    die(1, `PUSH TO ${d} FAILED. Refs moved so far: ${moved.length ? moved.join(", ") : "none"}.`,
           moved.length ? "THE DESTINATIONS ARE NOW DIVERGENT and that is the state to fix before anything else."
                        : "Nothing was published.");
  moved.push(d);
}

/* VERIFIED BY CONTENT. A SHA says nothing about a tree: an empty-tree commit has passed an `ls-remote` check in
   this repository and was reported as proof. So the remote's sha is re-read AND the tree is counted. */
let bad = 0;
for (const d of dests) {
  const now = git(["ls-remote", remote, `refs/heads/${d}`]).stdout.trim().split(/\s+/)[0];
  const agree = now === sha;
  if (!agree) bad++;
  say(`  ${d.padEnd(44)} ${now.slice(0, 7)} ${agree ? "= the sha pushed" : "DOES NOT MATCH " + sha.slice(0, 7)}`);
}
const files = git(["ls-tree", "-r", sha, "--name-only"]).stdout.trim().split("\n").filter(Boolean).length;
say(`tree at ${sha.slice(0, 7)} holds ${files} file(s) -- the pointer check above says WHICH commit, this says ` +
    "there is a tree under it");
if (bad) die(1, `${bad} destination(s) do not hold ${sha.slice(0, 7)}. The publication is INCOMPLETE.`);
say(`published ${n} commit(s) to ${dests.length} ref(s), verified by content.`);
