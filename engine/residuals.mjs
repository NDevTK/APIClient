#!/usr/bin/env node
// Lists the tree's named residuals: code that is correct but narrower than the spec, stated at its site.
// CLAUDE.md says what the engine must do; this list says where the tree does not yet do it.
//
//   node engine/residuals.mjs [--count] [path-prefix...]
//
// A residual is a comment containing "named residual" that does not say it is retired. Each is printed as
// `file:line  first sentence`. With --count, one line per file and a total. The scan is textual, so it is a
// floor: a residual spelled another way is not seen.
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const countOnly = args.includes('--count');
const prefixes = args.filter((a) => !a.startsWith('--'));
const scopes = prefixes.length ? prefixes : ['engine/host', 'engine/qjs', 'engine/lexbor', 'extension'];

const files = execFileSync('git', ['-C', root, 'ls-files', '--', ...scopes], { encoding: 'utf8', maxBuffer: 1 << 26 })
  .split('\n')
  .filter((f) => /\.(c|h|js|mjs)$/.test(f));

const MARK = /named residual/i;
const RETIRED = /retired|that stood here|is built|was built/i;
let total = 0;
const perFile = [];
for (const f of files) {
  let text;
  try {
    text = readFileSync(join(root, f), 'utf8');
  } catch {
    continue; // a tracked path that is a gitlink or was removed in the working tree
  }
  if (!MARK.test(text)) continue;
  const lines = text.split('\n');
  let n = 0;
  for (let i = 0; i < lines.length; i++) {
    if (!MARK.test(lines[i])) continue;
    const sentence = lines
      .slice(i, i + 4)
      .join(' ')
      .replace(/\s+/g, ' ')
      .replace(/^[\s/*]+/, '');
    const head = sentence.slice(sentence.search(MARK));
    if (RETIRED.test(head.slice(0, 120))) continue;
    n++;
    if (!countOnly) console.log(`${f}:${i + 1}  ${head.slice(0, 200)}`);
  }
  if (n) perFile.push([f, n]);
  total += n;
}
if (countOnly) for (const [f, n] of perFile) console.log(`${n}\t${f}`);
console.log(`[residuals] ${total} named residual(s) in ${perFile.length} file(s) under ${scopes.join(', ')} (textual scan, a floor)`);
