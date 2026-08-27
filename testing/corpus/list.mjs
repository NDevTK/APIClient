// THE SITE LIST, READ IN ONE PLACE — because a census is a measurement OF a list, and every instrument in
// this directory had its own copy of the parse with its own idea of which list that was.
//
// `run.sh` alone took the list as a parameter (`SITES`); `mirror.mjs` and `report.mjs` each hard-coded
// `sites.tsv`. That is not a tidiness complaint, it is how a report came to describe a corpus it had never
// been handed: the app-page census walked twelve ids that are in no line of `sites.tsv`, and `report.mjs`
// looked EVERY ONE of them up in that file, matched none, and filled the miss with `|| ''` — so the table
// printed twelve complete-looking rows whose stack column was blank for a reason nothing in the output
// stated. A consumer that defaults a producer's field is the defect CLAUDE.md §Architecture names, and here
// the "producer" is the list itself: a row whose id the list does not contain means the report was handed
// the WRONG LIST, which makes every column beside it a claim about a corpus nobody selected.
//
// So: one reader, one env var (`SITES`, the spelling run.sh already used), and the resolved path is
// RETURNED so a report can NAME the list it measured against instead of leaving a reader to assume.
import { readFileSync } from 'node:fs';
import { isAbsolute, join } from 'node:path';

const ROOT = new URL('.', import.meta.url).pathname;

/* A ROW IS THREE COLUMNS AND A SHORT ONE IS FATAL, never skipped. A skipped row is a site silently absent
   from a census whose totals still read as complete — the excluded-test defect at corpus scale. Blank lines
   and `#` comments are not rows and are dropped before the shape is checked; everything else must be one. */
export function siteList(which) {
  const rel = which || process.env.SITES || 'sites.tsv';
  const path = isAbsolute(rel) ? rel : join(ROOT, rel);
  const rows = [];
  const lines = readFileSync(path, 'utf8').split('\n');
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (!line.trim() || line.startsWith('#')) continue;
    const cols = line.split('\t');
    if (cols.length < 3 || !cols[0].trim() || !cols[1].trim())
      throw new Error(`${path}:${i + 1} is not a site row — a row is \`id<TAB>url<TAB>observed stack\` and this\n` +
        `  one is \`${line.slice(0, 120)}\`. It is fatal rather than skipped because a skipped row is a site\n` +
        `  missing from a census whose totals still print as complete.`);
    rows.push({ id: cols[0].trim(), url: cols[1].trim(), stack: cols.slice(2).join('\t').trim() });
  }
  if (!rows.length) throw new Error(`${path} holds no site rows.`);
  const byId = new Map();
  for (const r of rows) {
    if (byId.has(r.id)) throw new Error(`${path} names \`${r.id}\` twice — one id is one site, and a census ` +
      `keyed on a duplicate would overwrite one measurement with the other.`);
    byId.set(r.id, r);
  }
  return { path, rel, rows, byId };
}
