/* paint_predicate.mjs — DID THE PAGE'S OWN JAVASCRIPT REACH THE PIXELS?
 *
 * ONE COMPARISON, CATEGORICAL, OVER ONE RUN'S PAINT DIRECTORY: does any non-`baseline` world's raster PAYLOAD
 * differ from the `baseline` world's? It is ON or OFF and it is a byte inequality, which is what makes it
 * worth having on a host whose cooperative slice is WALL-denominated. §Testing: a reach total moves with how
 * far the box and the hour let a run get, and is not comparable across two runs or two revisions; this
 * compares two images OF ONE RUN, so the thing that varies is divided out of both sides at once.
 *
 * WHY `baseline` IS THE RIGHT OTHER SIDE AND NOT A PREVIOUS RUN. CLAUDE.md §Boot makes the COW baseline
 * PRE-boot — "the document as no flow has written it, in which none of the page's own code has run" — so the
 * difference between it and any other world is exactly what that timeline's JavaScript did to the appearance
 * of the page. A diff against another RUN would be a diff between two interleavings.
 *
 * THE DIGEST IS pamprune.py's AND IS NOT RE-IMPLEMENTED HERE, which is why this reads `INDEX.tsv` rather than
 * the images. That file seeks past `ENDHDR` and hashes the PAYLOAD, so the census comments — which carry a
 * timestamp-free but per-run URL and the walk's own numbers — are excluded from the comparison; a digest over
 * whole files would report every pair as different for reasons that are not pixels. Two implementations of
 * one hashing rule is the shape where the copy nobody runs against reality is the one that drifts, so there
 * is one, and it belongs to the file that already had it.
 * IT IS ALSO WHAT MAKES THE PREDICATE SURVIVE PRUNING. `pamprune.py` DELETES a duplicate payload and keeps a
 * row for it with `kept=0`, so on a real page most worlds' images are gone by the time anyone asks — and the
 * index is lossless about what was seen. A predicate that read the directory would report the pruned worlds
 * as absent, which is the one reading that inverts the answer: identical payloads are exactly what `OFF`
 * means, and pruning removes precisely those files.
 *
 * COLUMNS ARE READ BY NAME OUT OF THE HEADER LINE AND NEVER BY POSITION. An index written by one revision of
 * the pruner and read by another is a set that moves, and a positional read of a moving set is CLAUDE.md's
 * §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED arriving in a TSV.
 *
 * WHAT THIS DELIBERATELY DOES NOT READ, AND IT IS A FINDING RATHER THAN A PREFERENCE: `INDEX.tsv`'s column
 * headed `forced`. That column does not carry the forced grade and does not carry the walk's verdict either.
 * pamprune.py selects it with `startswith("the walk ")` and takes the FIRST match, and `abi_paint`'s header
 * now has TWO lines with that prefix — `# the walk covered N element(s)…` sorts above `# the walk FINISHED…`
 * — so the column holds the ELEMENT COUNT SENTENCE. Established by content and not by ancestry, which a
 * shallow clone cannot answer: at d9a57005, the commit that ADDED pamprune.py, `abi_paint` wrote exactly ONE
 * `# the walk ` line, so the selector was correct when written and a later commit inserted a second above it.
 * The repair is one line at that file — select on `the path that laid this ink is ` for the grade, which is
 * a prefix no other header line shares — and it is not made HERE because a pruner is a long-running poll a
 * peer lane may have running against a real paint dir this minute, and changing an instrument's columns under
 * a reader is worse than the wrong column. So the grade and the verdict are taken from the sources below.
 *
 * THE SCALARS ARE WHERE THE FALSIFYING WORK IS DONE AND THE PIXELS ARE NOT. A blank image has FOUR causes the
 * bitmap cannot separate — the walk reached no box, reached boxes that painted nothing, was STOPPED, or there
 * was no rendered region at all — and THREE of those are facts about the PAINTER while ONE is a fact about
 * the LOAD. `elements` is the only number that is not the walk's own: it is DOM §4.8's shadow-including
 * descendant count taken by the same walker at the same instant, and document_paint.h calls it the ONLY thing
 * that separates an EMPTY document from an UNREACHED one. No inequality holds between it and the offer count
 * and none is asserted here. A world whose walk did not COMPLETE is a PARTIAL picture with every mark already
 * laid still in it, and it is labelled, because publishing one as a whole page is main.c's own
 * plausible-wrong-datum defect with a PERSON as the consumer.
 *
 * usage: node testing/paint_predicate.mjs <paint-dir> [more-dirs…]
 */

import { readFileSync, existsSync, readdirSync, openSync, readSync, closeSync } from 'node:fs';
import { join, basename } from 'node:path';
import { pathToFileURL } from 'node:url';

function fail(msg) { throw new Error('@WHY paint_predicate: ' + msg); }

/* THE INDEX, BY HEADER NAME. The header is `# t<TAB>file<TAB>…`; the leading `# ` is the comment marker and
   not part of the first column's name. A missing column is a loud failure rather than an `undefined` that
   would compare unequal to every digest and report every world as differing. */
function readIndex(dir) {
  const p = join(dir, 'INDEX.tsv');
  if (!existsSync(p))
    fail('no INDEX.tsv in ' + dir + '. The payload digests are pamprune.py\'s and are not re-implemented ' +
         'here — two copies of one hashing rule is the shape where the copy nobody runs against reality is ' +
         'the one that drifts. Run:  timeout 9 python3 testing/pamprune.py ' + dir + ' 3');
  const lines = readFileSync(p, 'utf8').split('\n').filter(Boolean);
  if (!lines.length || !lines[0].startsWith('#')) fail(p + ' has no header line to read column names from');
  const cols = lines[0].replace(/^#\s*/, '').split('\t').map((s) => s.trim());
  const need = ['file', 'digest', 'world', 'kept'];
  for (const c of need)
    if (!cols.includes(c)) fail(p + ' has no `' + c + '` column (it has: ' + cols.join(', ') + ')');
  const ix = Object.fromEntries(cols.map((c, i) => [c, i]));
  const rows = [];
  for (const l of lines.slice(1)) {
    const f = l.split('\t');
    if (f.length < cols.length) continue;          /* a row mid-append; the next pass rewrites it */
    rows.push({ file: f[ix.file], digest: f[ix.digest], world: f[ix.world], kept: f[ix.kept] });
  }
  return { path: p, cols, rows };
}

/* THE SCALARS. `RASTER.json` is written by testing/render_raster.mjs beside its images; a directory written
   by the NATIVE host (test_forced.c's `--paint-dir`) has none, so the PAM headers of the files that SURVIVED
   pruning are read instead. A world whose image was pruned as a duplicate has neither, and that is reported
   as UNAVAILABLE rather than filled in — an absent scalar and a zero scalar are different facts, and the
   whole point of these numbers is to separate readings a blank bitmap cannot. */
function scalarsFromRasterJson(dir) {
  const p = join(dir, 'RASTER.json');
  if (!existsSync(p)) return null;
  const j = JSON.parse(readFileSync(p, 'utf8'));
  const last = new Map();
  for (const f of j.frames || []) last.set(f.world, f);
  return { source: 'RASTER.json', stamp: j.stamp, behind: j.behind, labels: j.labels, url: j.url,
           serve: j.serve, code: j.code, pageErrors: j.pageErrors, byWorld: last };
}

function scalarsFromHeaders(dir) {
  const byWorld = new Map();
  for (const n of readdirSync(dir).filter((x) => x.endsWith('.pam'))) {
    const fd = openSync(join(dir, n), 'r');
    const buf = Buffer.alloc(8192);
    let got = 0;
    try { got = readSync(fd, buf, 0, buf.length, 0); } finally { closeSync(fd); }
    const head = buf.slice(0, got).toString('utf8');
    const k = head.indexOf('ENDHDR\n');
    if (k < 0) continue;
    const cm = head.slice(0, k).split('\n').filter((l) => l.startsWith('#')).map((l) => l.slice(1).trim());
    const pick = (re) => { for (const c of cm) { const m = c.match(re); if (m) return m; } return null; };
    const world = (pick(/^world (.+)$/) || [])[1];
    if (world === undefined) continue;
    const om = pick(/^CSS 2\.1 §E\.2 "Painting order" offered (\d+) step\(s\) and laid (\d+) mark\(s\)$/);
    const el = pick(/^the walk covered (\d+) element\(s\)/);
    const wk = pick(/^the walk (FINISHED|STOPPED)/);
    const oc = pick(/^of those offers: (.+)$/);
    const dc = pick(/^and why the offers that laid nothing laid nothing: (.+)$/);
    byWorld.set(world, {
      world, file: n,
      forced: (pick(/^the path that laid this ink is (\S+)$/) || [])[1] ?? null,
      offers: om ? Number(om[1]) : null, marks: om ? Number(om[2]) : null,
      elements: el ? Number(el[1]) : null,
      complete: wk ? wk[1] === 'FINISHED' : null,
      outcomeText: oc ? oc[1] : null, declineText: dc ? dc[1] : null,
    });
  }
  return byWorld.size ? { source: 'PAM headers', byWorld } : null;
}

function describe(w, sc, labels) {
  if (!sc) return '      scalars UNAVAILABLE (this world\'s image was pruned as a duplicate payload and this ' +
                  'directory has no RASTER.json — an absent scalar is not a zero one)';
  const outcome = sc.outcomeText ??
    (sc.outcome && labels ? sc.outcome.map((n, i) => n + ' ' + labels.outcome[i]).join(', ') : '?');
  const decl = sc.declineText ??
    (sc.decline && labels
      ? (sc.decline.map((n, i) => [n, labels.decline[i]]).filter(([n]) => n > 0)
             .map(([n, l]) => n + ' ' + l).join(', ') || 'none')
      : '?');
  const partial = sc.complete === false ? '   ** PARTIAL PICTURE — every mark already laid is in it **' : '';
  return '      offers=' + sc.offers + '  marks=' + sc.marks + '  elements=' + sc.elements +
         '  complete=' + sc.complete + partial +
         '\n      outcome: ' + outcome + '\n      declined: ' + decl +
         (sc.noRegion ? '\n      NO RENDERED REGION (CSS 2.1 §2.3.1 — a fact about the LOAD, not the painter)' : '');
}

function run(dir) {
  const idx = readIndex(dir);
  /* THE LAST ROW PER WORLD. A world's file is overwritten by every later render of that world — the NAME
     carries the world — so the index holds a row per distinct payload that name ever had, and the picture on
     disk is the last one. */
  const last = new Map();
  for (const r of idx.rows) last.set(r.world, r);
  const sc = scalarsFromRasterJson(dir) || scalarsFromHeaders(dir) || { source: 'none', byWorld: new Map() };

  const out = ['paint-predicate  ' + dir];
  if (sc.stamp)
    out.push('  artifact stamp ' + sc.stamp.head + '  treeAtBuild=' + sc.stamp.treeAtBuild +
             '  behind origin/main: ' + (sc.behind === null ? 'UNANSWERABLE' : sc.behind),
             '  url=' + sc.url + (sc.serve ? '  [--serve: PROGRAM LOADS ONLY]' : '  [no network]') +
             '  qjs_step ended ' + sc.code);
  out.push('  scalars from: ' + sc.source + '   worlds in index: ' + last.size);

  const base = last.get('baseline');
  if (!base) {
    /* NO BASELINE IS NOT AN `OFF`. The comparison has one side, so the question was not asked — and
       answering it anyway would publish the strongest available claim out of the weakest evidence. */
    out.push('  PREDICATE: UNASKED — this directory holds no `baseline` world, so there is nothing to compare ' +
             'the others against. Worlds present: ' + [...last.keys()].join(' '));
    return out.join('\n');
  }
  const others = [...last.values()].filter((r) => r.world !== 'baseline');
  const differing = others.filter((r) => r.digest !== base.digest);
  const same = others.filter((r) => r.digest === base.digest);

  out.push('  baseline payload digest ' + base.digest + '   (' + base.file + ')');
  out.push('    baseline scalars:');
  out.push(describe('baseline', sc.byWorld.get('baseline'), sc.labels));
  if (!others.length) {
    out.push('  PREDICATE: UNASKED — the run painted ONLY the baseline. No timeline of this document was ' +
             'photographed, so this is a fact about the RUN and not about the page\'s appearance.');
    return out.join('\n');
  }
  out.push('  PREDICATE: ' + (differing.length ? 'ON' : 'OFF') +
           '   — ' + differing.length + ' of ' + others.length +
           ' non-baseline world(s) differ from baseline in raster PAYLOAD');
  for (const r of others) {
    out.push('    ' + (r.digest === base.digest ? 'SAME ' : 'DIFF ') + r.world + '  ' + r.digest +
             (r.kept === '0' ? '  (payload pruned — identical to an earlier one)' : ''));
    out.push(describe(r.world, sc.byWorld.get(r.world), sc.labels));
  }
  if (!differing.length)
    out.push('  OFF means every timeline PAINTED THE SAME PIXELS as the document no flow had written. It does ' +
             'NOT mean no JavaScript ran: read the scalars above — equal `elements` across worlds is a DOM ' +
             'nothing changed, and a differing `elements` with an equal payload is a change that reached the ' +
             'tree and not the ink.');
  if (sc.pageErrors && sc.pageErrors.length)
    out.push('  pageErrors=' + JSON.stringify(sc.pageErrors).slice(0, 500));
  return out.join('\n');
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const dirs = process.argv.slice(2);
  if (!dirs.length) { console.error('usage: node testing/paint_predicate.mjs <paint-dir> [more-dirs…]'); process.exit(2); }
  try { console.log(dirs.map(run).join('\n\n')); }
  catch (e) { console.error(e.stack || e.message || e); process.exit(1); }
}

export { run, readIndex };
