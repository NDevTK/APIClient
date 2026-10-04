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
import { accessSync, constants } from "node:fs";
import { join, resolve } from "node:path";

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
                                                && argv[argv.indexOf(a) - 1] !== "--remote"
                                                && argv[argv.indexOf(a) - 1] !== "--prose") || null;
const expect = flag("--expect");
const remote = flag("--remote") || "origin";
/* THE PATHS THE PUBLISHER CLAIMS ARE PROSE-ONLY, AS AN INPUT RATHER THAN AS A SENTENCE IN A MESSAGE. CLAUDE.md
   requires a large share of this project's diffs to be comment-only — a retired argument is REWRITTEN rather
   than deleted, an incident is kept at its site — and the evidence offered for one is always the same sentence,
   `I only added comments`, which is a claim about the EMITTED PROGRAM that no gate here measures. `prosediff.mjs`
   measures it, and the reason it is spawned HERE is the reason `--expect` is required here: nothing connects a
   command somebody remembers to the act it was supposed to gate. A claim stated at the publish is a claim with
   an exit status in front of it.
   IT IS A CLAIM AND NEVER A SCAN, which is the whole of why it is opt-in. Preprocessing every changed `.c` on
   every publish costs minutes on a large translation unit and would make a prose fix unlandable, which CLAUDE.md
   refuses by name: a citation is prose and a build that cannot land a spelling fix stops every lane. So this
   asks nothing of a publisher who claims nothing, and refuses the one who claims wrongly. */
const prose = (flag("--prose") || "").split(",").map((x) => x.trim()).filter(Boolean);

if (rev === null || expect === null)
  die(2, "usage: node engine/publish.mjs <sha> --expect <n> [--remote origin]",
         "  <sha> is the CUT -- the commit to publish, and everything beneath it.",
         "  --expect <n> is how many commits you believe that publishes. It is REQUIRED, and it is required",
         "  here for the same reason engine/pubgate.mjs requires it: the count is the part a redirection hides,",
         "  so it is an input. Run the gate with no --cut first to read the chain and every allowed cut.",
         "  --prose <a.c,b.c> states that those paths' diffs across this range emit NO code. It is checked by",
         "  engine/prosediff.mjs before anything is pushed, and a nonzero verdict pushes nothing.");

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

/* THE OTHER HALF OF THE CONSTRUCTION IS ASSERTED HERE, AND THE TWO LOCK EACH OTHER. This file puts the gate in
   the PATH of the push; `engine/githooks/pre-push` makes a push that carries no verdict REFUSE. Either alone is
   a convention somebody has to remember -- a bare `git push` walks past this file, and a hook nobody armed is
   the check-shaped version `engine/lexbor_source.mjs`'s header condemns, "a check every caller must remember"
   rather than an impossible state. So this tool will not publish from a checkout where the hook is not armed:
   you cannot use the wrapper without the hook, and with the hook you cannot bypass the wrapper by forgetting.
   THE PATH IS COMPARED ABSOLUTE, DELIBERATELY. git's own interpretation of a RELATIVE `core.hooksPath` has
   differed by version about what it is relative to, and a check that cannot say which directory it means is a
   check whose zero means nothing. An absolute path is a fact about WHERE this tree is checked out, which
   `engine/lexbor_source.mjs` rightly refuses for an IDENTITY -- and this is not one: it is a local config in one
   working copy, and a frozen snapshot legitimately has none, so a publish attempted from a snapshot refuses
   here and names the command, which is the correct answer rather than a false alarm. */
const TOP = git(["rev-parse", "--show-toplevel"]).stdout.trim();
const HOOKS = join(TOP, "engine", "githooks");
const havePath = git(["config", "--get", "core.hooksPath"], { allowFail: true }).stdout.trim();
if (!havePath || resolve(havePath) !== HOOKS)
  die(2, "REFUSED: core.hooksPath does not name this repository's tracked hook directory, so a bare `git push`",
         "from this checkout would publish an ancestry with no verdict in front of it -- which is the incident",
         "this tool's own header records. Arm it once, in this checkout:",
         `      git config core.hooksPath ${HOOKS}`,
         `  it currently reads ${havePath ? "'" + havePath + "'" : "(unset)"}.`);
try { accessSync(join(HOOKS, "pre-push"), constants.X_OK); }
catch (e) {
  die(2, `REFUSED: ${join(HOOKS, "pre-push")} is not an executable file (${e.code || e.message}).`,
         "git runs a hook only if it is executable AND SAYS NOTHING WHEN IT IS NOT, so an un-executable hook is",
         "the absence of a question rather than a failing check -- every push would succeed unverified and the",
         "transcript would look identical to one that was verified. Restore the mode: chmod +x on that path.");
}

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

/* THE PROSE-ONLY CLAIM, CHECKED AGAINST THE REMOTE'S OWN TIP AND BEFORE ANY PUSH. The base is `old` and not a
   tracking ref: it is what this push's range is defined against, read from `ls-remote` above, so the residue is
   over exactly the commits about to be published rather than over whatever a local ref last fetched.
   SPAWNED WITH stdio INHERITED AND A NONZERO VERDICT PUSHES NOTHING, which is the gate's shape for the gate's
   reason — `if <check>; then <act>` and never `<check>; <act>`, the second being a NON-check with a reassuring
   transcript above an unconditional irreversible act. */
if (prose.length) {
  say(`checking the --prose claim over ${prose.length} path(s) against ${old.slice(0, 7)} ────────────────────`);
  const pd = spawnSync(process.execPath,
                       [new URL("prosediff.mjs", import.meta.url).pathname, ...prose, "--base", old],
                       { stdio: "inherit" });
  if (pd.status !== 0)
    die(pd.status === null ? 3 : pd.status,
        "──────────────────────────────────────────────────────────────────────────────────────────────────────",
        `the prose-only claim FAILED (prosediff exited ${pd.status}) and NOTHING was pushed. Read its verdict`,
        "above: a NON-NUMERIC difference is a statement, an expression or a datum your diff emits; an",
        "UNEXPLAINED numeric delta is one no hunk boundary accounts for; a header is DECLINED rather than",
        "cleared. Either the claim is wrong, or drop --prose and say in the message what the diff emits.");
  say("the prose-only claim HELD ─────────────────────────────────────────────────────────────────────────────");
}

/* EACH PUSH IS ITS OWN SPAWN AND CARRIES NOTHING ELSE. A retry is only ever for a NETWORK failure: a refusal
   is a verdict and retrying one is arguing with it. */
const moved = [];
for (const d of dests) {
  let ok = false;
  for (let attempt = 0, wait = 2000; attempt < 5 && !ok; attempt++, wait *= 2) {
    if (attempt) { say(`retrying ${d} after ${wait / 2000}s (network)`); spawnSync("sleep", [String(wait / 2000)]); }
    /* THE VERDICT TRAVELS WITH THE PUSH AND IS A CLAIM THE HOOK FALSIFIES, never a password. It carries the
       SHA and the COUNT, and `engine/githooks/pre-push` checks both against what git hands it on stdin -- the
       local sha it is about to send and the REMOTE'S CURRENT sha for that ref, which is the freshest left end
       there is. So the arithmetic backstop is performed a SECOND time, by something that cannot be skipped,
       against a left end that cannot be stale. */
    const r = spawnSync("git", ["push", remote, `${sha}:refs/heads/${d}`],
                        { encoding: "utf8", env: { ...process.env, APICLIENT_PUBGATE: `${sha}:${n}` } });
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
