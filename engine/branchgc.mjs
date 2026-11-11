#!/usr/bin/env node
// Lists local branches whose every commit is already on the upstream by patch identity, and deletes them on
// request. A lane's commits reach the upstream as cherry-picks, so ancestry cannot answer this; patch ids can.
// The upstream's patch ids are computed once, from the oldest fork point of any branch, and each branch's own
// commits are looked up in that set.
//
//   node engine/branchgc.mjs [--upstream <rev>] [--delete]
//
// A branch checked out in any worktree is never deleted: it is reported with its worktree path so the
// coordinator can remove the worktree of a finished lane first. A branch with commits the upstream lacks is
// kept and counted. Each deletion prints the tip it removed, which is how the branch is restored.
// The answer is a floor: a commit that landed through a conflict resolution has a new patch identity and
// keeps its branch.
import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const del = args.includes('--delete');
const upAt = args.indexOf('--upstream');
const upstream = upAt >= 0 ? args[upAt + 1] : 'origin/main';
for (const a of args) {
  if (a !== '--delete' && a !== '--upstream' && a !== upstream) {
    console.error(`[branchgc] unknown argument ${a}`);
    process.exit(2);
  }
}
const git = (...a) => execFileSync('git', ['-C', root, ...a], { encoding: 'utf8', maxBuffer: 1 << 28 });

git('rev-parse', '--verify', '--quiet', `${upstream}^{commit}`);
const held = new Map();
let path = null;
for (const line of git('worktree', 'list', '--porcelain').split('\n')) {
  if (line.startsWith('worktree ')) path = line.slice(9);
  else if (line.startsWith('branch refs/heads/')) held.set(line.slice(18), path);
}
const branches = git('for-each-ref', '--format=%(refname:short)', 'refs/heads').split('\n').filter(Boolean);
const sh = (cmd) => execFileSync('sh', ['-c', cmd], { cwd: root, encoding: 'utf8', maxBuffer: 1 << 30 });
// `git patch-id` prints one "<patch id> <commit>" line per commit that has a diff.
const patchIds = (range) => sh(`git log -p --no-merges --format=%H ${range} | git patch-id --stable`)
  .split('\n').filter(Boolean).map((l) => l.split(' '));

const own = new Map();
let oldest = null, oldestBehind = -1;
for (const b of branches) {
  const commits = git('rev-list', '--no-merges', `${upstream}..${b}`).split('\n').filter(Boolean);
  own.set(b, commits);
  if (!commits.length) continue;
  const base = git('merge-base', upstream, b).trim();
  const behind = Number(git('rev-list', '--count', `${base}..${upstream}`));
  if (behind > oldestBehind) { oldestBehind = behind; oldest = base; }
}
const upstreamIds = new Set(oldest ? patchIds(`${oldest}..${upstream}`).map(([id]) => id) : []);
const idOf = new Map();
const all = [...new Set([...own.values()].flat())];
for (let i = 0; i < all.length; i += 500) {
  for (const [id, c] of patchIds(`--no-walk=unsorted ${all.slice(i, i + 500).join(' ')}`)) idOf.set(c, id);
}
let landed = 0, kept = 0, inWorktree = 0, deleted = 0;
for (const b of branches) {
  // A commit with no patch id has an empty diff and holds nothing the upstream lacks.
  const ahead = own.get(b).filter((c) => idOf.has(c) && !upstreamIds.has(idOf.get(c))).length;
  if (ahead) { kept++; continue; }
  if (held.has(b)) {
    inWorktree++;
    console.log(`worktree  ${b}  ${held.get(b)}`);
    continue;
  }
  landed++;
  if (del) {
    process.stdout.write(`deleted   ${git('branch', '-D', b).trim()}\n`);
    deleted++;
  } else {
    console.log(`landed    ${b}  ${git('rev-parse', '--short=8', b).trim()}`);
  }
}
console.log(`[branchgc] ${branches.length} branch(es) against ${upstream}: ${landed} landed` +
  `${del ? ` (${deleted} deleted)` : ''}, ${inWorktree} landed but checked out in a worktree, ${kept} with commits the upstream lacks`);
