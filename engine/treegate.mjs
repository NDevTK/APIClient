#!/usr/bin/env node
/* The HTML tree-construction corpus: html5lib's tree-construction cases (testing/tree_construction/html5lib, the
 * source commit in its SOURCE file) parsed by the Lexbor this tree compiles, each case's verdict compared with
 * testing/tree_construction/expected.txt. A case whose verdict differs from the recorded one, a recorded case
 * the corpus no longer holds and a case with no record are each named; a known failure stays FAIL in the
 * expected file until Lexbor is fixed, and the fix moves it to PASS with the file regenerated.
 *
 *   node engine/treegate.mjs           exit 0: every verdict is the recorded one. 1: some differ, each named.
 *                                      2: the runner could not be built or run, or its own control failed.
 *   node engine/treegate.mjs --write   records the current verdicts as expected.txt */

import { execFileSync, spawnSync } from 'node:child_process';
import { mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { lexborNativeArchive } from './lexbor_source.mjs';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const CORPUS = join(ENGINE, '..', 'testing', 'tree_construction');
const EXPECTED = join(CORPUS, 'expected.txt');
const write = process.argv.includes('--write');
const die = (msg) => { console.error('[treegate] ' + msg); process.exit(2); };

const work = join(ENGINE, '.work', 'treegate-' + process.pid);
rmSync(work, { recursive: true, force: true });
mkdirSync(work, { recursive: true });
const archive = lexborNativeArchive(ENGINE, 'treegate');
const bin = join(work, 'tc');
try {
  execFileSync('clang', ['-O1', '-g', '-Wall', '-Werror', '-I', join(ENGINE, 'lexbor', 'source'),
                         join(CORPUS, 'tc.c'), archive, '-lm', '-o', bin], { stdio: 'inherit' });
} catch (e) { die(`the runner did not compile against ${archive}: ${e.message}`); }

/* `tc` over `files`: a Map of case key to { verdict, tree } for a FAIL's dump. */
function run(files) {
  const r = spawnSync(bin, files, { encoding: 'utf8', maxBuffer: 1 << 28 });
  if (r.status !== 0) die(`the runner exited ${r.status}${r.signal ? ' on ' + r.signal : ''}: ${r.stderr.trim()}`);
  const out = new Map();
  let last = null;
  for (const line of r.stdout.split('\n')) {
    const m = /^(\S+#\d+@[0-9a-f]{8}\/script-(?:on|off)) (PASS|FAIL)$/.exec(line);
    if (m) {
      if (out.has(m[1])) die(`the runner printed case ${m[1]} twice`);
      out.set(m[1], { verdict: m[2], tree: [] });
      last = out.get(m[1]);
    } else if (line !== '') {
      if (last === null || last.verdict !== 'FAIL') die(`the runner printed a line outside a failing case: ${line}`);
      last.tree.push(line);
    }
  }
  return out;
}

/* The control: one invented case whose expected tree is the one HTML builds and one whose expected tree is not,
   so a runner that cannot tell a right tree from a wrong one cannot pass. */
const control = join(work, 'control.dat');
const tree = (text) => `| <html>\n|   <head>\n|   <body>\n|     <p>\n|       "${text}"\n`;
writeFileSync(control, `#data\n<p>x\n#errors\n#document\n${tree('x')}\n#data\n<p>x\n#errors\n#document\n${tree('y')}`);
const ctl = [...run([control]).values()].map((c) => c.verdict).join(' ');
if (ctl !== 'PASS PASS FAIL FAIL') die(`its own control read "${ctl}" where "PASS PASS FAIL FAIL" is right`);

const files = readdirSync(join(CORPUS, 'html5lib')).filter((f) => f.endsWith('.dat')).sort()
  .map((f) => join(CORPUS, 'html5lib', f));
if (files.length === 0) die(`no .dat file under ${join(CORPUS, 'html5lib')}`);
const got = run(files);
rmSync(work, { recursive: true, force: true });
const fails = [...got.values()].filter((c) => c.verdict === 'FAIL').length;

if (write) {
  writeFileSync(EXPECTED, '# The verdict of each html5lib tree-construction case and scripting mode, written by ' +
                          '`node engine/treegate.mjs --write`.\n# A FAIL is a known Lexbor divergence from the ' +
                          'expected tree.\n' + [...got].map(([k, c]) => `${k} ${c.verdict}\n`).join(''));
  console.log(`[treegate] recorded ${got.size} verdict(s), ${fails} FAIL, in ${EXPECTED}`);
  process.exit(0);
}

const want = new Map();
for (const line of readFileSync(EXPECTED, 'utf8').split('\n')) {
  if (line === '' || line.startsWith('#')) continue;
  const m = /^(\S+) (PASS|FAIL)$/.exec(line);
  if (!m) die(`expected.txt holds a line that is not "<case> PASS|FAIL": ${line}`);
  want.set(m[1], m[2]);
}
let moved = 0;
for (const [key, c] of got) {
  const w = want.get(key);
  if (w === c.verdict) continue;
  moved++;
  console.log(`[treegate] ${key}: ${w === undefined ? 'no recorded verdict' : 'recorded ' + w}, now ${c.verdict}`);
  if (c.verdict === 'FAIL') for (const l of c.tree) console.log('[treegate]   ' + l);
}
for (const key of want.keys())
  if (!got.has(key)) { moved++; console.log(`[treegate] ${key}: recorded ${want.get(key)}, and the corpus no longer holds it`); }
console.log(`[treegate] ${got.size} case-mode(s), ${fails} FAIL; ${moved} differ from expected.txt`);
process.exit(moved ? 1 : 0);
