/* Puts the publication gate in the path of the push, so a push cannot be issued without its verdict.
 *
 *   node engine/publish.mjs <sha> --expect <n> [--remote origin] [--prose <paths>]
 *
 * `engine/pubgate.mjs` answers whether a cut is allowed and refuses a caller that has not stated how many
 * commits it believes it is publishing. This wrapper connects that verdict to the act:
 *   - The gate is spawned here with stdio inherited, so its chain is on the operator's screen. A nonzero
 *     verdict exits with that status and spawns nothing.
 *   - The count is checked: `git ls-remote` gives each destination's current sha, `old..sha` is counted here,
 *     and a disagreement with --expect refuses. Both ends are read by content; no push output is parsed.
 *   - Both refs or neither: a `main` ahead of the session branch has its extra commits undone by the next
 *     merge, so the destinations must stand at one sha before anything is pushed, and a divergence refuses.
 *   - Each push is its own spawn carrying nothing else, and the result is verified by content: the remote's
 *     sha is re-read and the tree's file count is printed, because a sha says nothing about a tree.
 *
 * It does not know which lane has reported. The publishable prefix ends at the oldest unreported commit, which
 * no artifact records, so an --expect that matches is allowed by the pair rule and the count alone. That
 * judgement becomes checkable when a lane's report is an artifact a tool can read, such as a commit whose
 * message says its author considers it final. */
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
/* The paths the publisher claims are prose-only, as an input: `prosediff.mjs` measures the claim, and it is
   spawned here so the claim has an exit status in front of the push. It is opt-in, a claim and not a scan:
   preprocessing every changed `.c` on every publish would cost minutes on a large translation unit. */
const prose = (flag("--prose") || "").split(",").map((x) => x.trim()).filter(Boolean);

if (rev === null || expect === null)
  die(2, "usage: node engine/publish.mjs <sha> --expect <n> [--remote origin]",
         "  <sha> is the CUT -- the commit to publish, and everything beneath it.",
         "  --expect <n> is how many commits you believe that publishes. It is REQUIRED, and it is required",
         "  here for the same reason engine/pubgate.mjs requires it: the count is the part a redirection hides,",
         "  so it is an input. Run the gate with no --cut first to read the chain and every allowed cut.",
         "  --prose <a.c,b.c> states that those paths' diffs across this range emit NO code. It is checked by",
         "  engine/prosediff.mjs before anything is pushed, and a nonzero verdict pushes nothing.");

/* The sha is resolved once and every later command names the resolved value, never the ref: `HEAD` moves in a
   shared checkout. */
const sha = git(["rev-parse", "--verify", `${rev}^{commit}`]).stdout.trim();

/* The destinations: `main`, and this branch, read and not spelled. A detached HEAD has no second destination. */
const here = git(["rev-parse", "--abbrev-ref", "HEAD"]).stdout.trim();
if (here === "HEAD")
  die(2, "HEAD is detached, so there is no session branch to publish to and this would push to ONE ref.",
         "CLAUDE.md: a push to `main` alone is REVERSED by the next merge of the session branch.");
const dests = here === "main" ? ["main"] : ["main", here];
if (dests.length === 1)
  say("WARNING: the current branch IS main, so this publishes to ONE ref. That is the state CLAUDE.md's " +
      "two-ref rule exists to prevent; it is allowed here only because there is no second ref to name.");

/* The hook must be armed. This file puts the gate in the path of the push, and `engine/githooks/pre-push`
   refuses a push that carries no verdict; either alone can be walked past, so this tool will not publish from
   a checkout whose `core.hooksPath` is not the tracked hook directory. The path is compared absolute, because
   git's reading of a relative `core.hooksPath` has differed by version. A frozen snapshot has no such config
   and is refused here, with the command that arms it. */
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

/* The gate, spawned with stdio inherited. A nonzero verdict spawns nothing: `if <check>; then <act>`. */
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

/* Both destinations must already stand at one sha, read from the remote and not from a tracking ref, which is
   only as fresh as the last fetch. One count then serves both. */
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

/* The count: the range the remote's own tip defines, against the --expect the caller stated. */
const n = Number(git(["rev-list", "--count", `${old}..${sha}`]).stdout.trim());
if (String(n) !== String(expect).trim())
  die(1, `REFUSED: ${remote}'s own tip is ${old.slice(0, 7)} and ${old.slice(0, 7)}..${sha.slice(0, 7)} is ${n}`,
         `commit(s), not the --expect ${expect} you stated.`,
         "This is the backstop CLAUDE.md calls the only check that survives both ends moving, and the gap means",
         "the remote moved after the gate read it or you stated the wrong number. Re-read the chain:",
         `  git log --oneline ${old.slice(0, 7)}..${sha.slice(0, 7)}`);
say(`${n} commit(s) to publish, which is the --expect you stated and the remote's own tip says so: ` +
    `${old.slice(0, 7)}..${sha.slice(0, 7)}`);

/* The prose-only claim, checked before any push. The base is `old`, the remote's own tip read by `ls-remote`
   above, so the check covers exactly the commits about to be published. A nonzero verdict pushes nothing. */
if (prose.length) {
  say(`checking the --prose claim over ${prose.length} path(s) against ${old.slice(0, 7)} ────────────────────`);
  const pd = spawnSync(process.execPath,
                       [new URL("prosediff.mjs", import.meta.url).pathname, ...prose, "--base", old, "--head", sha],
                       { stdio: "inherit" });
  /* The message is chosen by which of prosediff's bands decided: a declined path was not judged and a void
     one had no armed control, and neither is a failed claim. Every band refuses the push. */
  if (pd.status !== 0) {
    const bar = "──────────────────────────────────────────────────────────────────────────────────────────────────────";
    if (pd.status === 6)
      die(6, bar,
          "the --prose claim was NOT JUDGED and NOTHING was pushed. Every path you named was DECLINED: this",
          "instrument measures what a C PREPROCESSOR emits, so it answers about a `.c` and about nothing else —",
          "a header has no translation unit, and a `.md`, `.mjs` or `.js` has no preprocessor at all.",
          "That is a BLIND SPOT and not a finding about your diff, so there is no statement to go looking for.",
          "Name the `.c` path(s) you are claiming about, or drop --prose: for a file the build does not compile",
          "the claim is not one this check can hold, and the push needs no claim it cannot verify.");
    if (pd.status === 5)
      die(5, bar,
          "the --prose claim is VOID and NOTHING was pushed. A path's CONTROL DID NOT SPEAK — an injected",
          "statement produced no masked difference — so a clean answer for it would mean `my input never reached",
          "this check`, which is indistinguishable from `this check cannot see this`. Nothing is published off an",
          "unarmed control. Read which path above; the include roots or the base revision are where to look.");
    die(pd.status === null ? 3 : pd.status, bar,
        `the prose-only claim FAILED (prosediff exited ${pd.status}) and NOTHING was pushed. Read its verdict`,
        "above: a NON-NUMERIC difference is a statement, an expression or a datum your diff emits; an",
        "UNEXPLAINED numeric delta is one no hunk boundary accounts for. Either the claim is wrong, or drop",
        "--prose and say in the message what the diff emits.");
  }
  say("the prose-only claim HELD ─────────────────────────────────────────────────────────────────────────────");
}

/* Comment size, over exactly the commits this push publishes (CLAUDE.md "Comments"). Nonzero pushes nothing. */
{
  const cg = spawnSync(process.execPath,
                       [new URL("commentgate.mjs", import.meta.url).pathname, "--base", old, "--head", sha],
                       { stdio: "inherit" });
  if (cg.status !== 0)
    die(1, `REFUSED by engine/commentgate.mjs (exit ${cg.status}) and NOTHING was pushed. Shorten the comment`,
           "blocks it names; history belongs in the commit message.");
}

/* Each push is its own spawn and carries nothing else. A retry is only for a network failure; a refusal is a
   verdict. */
const moved = [];
for (const d of dests) {
  let ok = false;
  for (let attempt = 0, wait = 2000; attempt < 5 && !ok; attempt++, wait *= 2) {
    if (attempt) { say(`retrying ${d} after ${wait / 2000}s (network)`); spawnSync("sleep", [String(wait / 2000)]); }
    /* The verdict travels with the push as a claim the hook checks: `engine/githooks/pre-push` compares the
       sha and the count against what git hands it on stdin, the local sha and the remote's current sha for
       the ref, so the count is checked a second time by something that cannot be skipped. */
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

/* Verified by content: the remote's sha is re-read and the tree is counted, because a sha says nothing about
   a tree. */
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
