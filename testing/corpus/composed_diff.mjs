#!/usr/bin/env node
/* THE CHROME-VS-ENGINE COMPOSITION DIFFERENTIAL — the one reach.mjs names and nothing computed.
 *
 *   node composed_diff.mjs <census .jsonl files>
 *
 * WHY THIS FILE EXISTS. reach.mjs's `composed / learned` is CAPPED BY THE FIXTURE: mirror.mjs fetches the
 * document and the subresources the MARKUP names, and never executes, so no lazy-chunk body is in any mirror
 * at any revision. That table's own blind-spot section says so and names the replacement -- the fixture
 * serves REAL CHROME in the same run that drives the engine, so its access log holds BOTH parties against
 * IDENTICAL BYTES and both meet the same 404. An address Chrome requested that the engine never recorded is
 * a differential no property of the corpus can flatter.
 *
 * The replacement was named in PROSE and computed by nobody. CLAUDE.md: a measurement can outlive its
 * instrument, and the tell is that you can state a number and cannot state the PATH that produces it -- not
 * the command, the path, tracked, that a fresh clone would contain. A figure of 59-to-3 was quoted across a
 * session out of ad-hoc shell in a finished lane's transcript. This is that shell, committed.
 *
 * AND THE PROSE DERIVATION WAS INCOMPLETE IN THE DIRECTION THAT OVER-STATES THE DEFICIT. reach.mjs says
 * `grep -o 'MISS .*\.m\?js' logs/<id>.serve | sort -u` against the row's siteEndpoints. Run exactly that and
 * every markup-named subresource the CAPTURE missed counts as an address Chrome COMPOSED -- which is the
 * fixture's gap reported as the engine's. The step that makes it a claim about composition is the one the
 * prose leaves out: keep only addresses ABSENT FROM THE MIRRORED BYTES, because an address written down
 * somewhere in what the site shipped is one a static reader could have found.
 *
 * WHAT IT IS A FRACTION OF, stated because a bare count here means nothing: the denominator is
 * ADDRESSES CHROME REQUESTED, THE FIXTURE COULD NOT SERVE, AND THE SHIPPED BYTES DO NOT CONTAIN. It is not
 * the number of lazy chunks a site has, and it is not a ceiling on what the engine could learn.
 *
 * BLIND SPOTS, because an instrument trusted past its evidence is worse than none:
 *  - A chunk the MIRROR DOES serve is invisible here. Chrome gets 200, the fixture logs no MISS, and the
 *    address never enters the population however well or badly the engine did on it. So this measures the
 *    UNSERVED tail only, and it SHRINKS as the capture improves -- the opposite direction from reach.mjs's
 *    cap, which is why the two are reported side by side and never summed.
 *  - Absence from the mirrored bytes is a SUBSTRING test over the whole capture. It is generous to exclusion:
 *    an address that appears in any shipped file for any reason is dropped. So the composed column is a
 *    FLOOR and can only under-state what Chrome derived at runtime.
 *  - The engine column asks whether the address is in the row's `siteEndpoints`. A row that never ran, or
 *    aborted early, contributes zeroes that are about the RUN and not about composition. The terminal event
 *    is printed beside each row for exactly that reason.
 *  - Serve logs live under logs/ and are NOT tracked by git. Every number here belongs to a RUN, never to a
 *    revision. Quote it with the pass label and the artifact, the way the census row states them.
 */
import { readFileSync, existsSync } from 'node:fs';
import { join, basename } from 'node:path';

const ROOT = new URL('.', import.meta.url).pathname;
const MIRROR = join(ROOT, 'mirror');
const LOGS = join(ROOT, 'logs');

const files = process.argv.slice(2);
if (!files.length) { console.error('usage: node composed_diff.mjs <census .jsonl files>'); process.exit(2); }

const manifest = JSON.parse(readFileSync(join(ROOT, 'provenance.json'), 'utf8'));
const byId = new Map(manifest.map((m) => [m.id, m]));

/* A ROW NAMES ITS OWN PASS. The serve log is pass-qualified (`logs/<pass>-<id>.serve`) because an unqualified
   path is owned by whichever pass ran last -- a lane lost a 37KB log to a 43-byte one that way. Guessing the
   filename here would re-open exactly that race one consumer later, so the label is READ from the row. */
const rows = [];
for (const f of files)
  for (const line of readFileSync(join(ROOT, f), 'utf8').trim().split('\n').filter(Boolean))
    rows.push(JSON.parse(line));

/* THE SITE'S OWN SHIPPED BYTES, EVERY CAPTURED RESOURCE AND NOT ONLY THE JAVASCRIPT. A chunk name can be
   written into a CSS file, a JSON manifest or the document; the question here is only whether a static
   reader could have found the string at all, so the content type decides nothing. */
function shippedBytes(site) {
  const dir = join(MIRROR, site.id);
  let text = '';
  const doc = join(dir, 'index.html');
  if (existsSync(doc)) text += readFileSync(doc, 'utf8') + '\n';
  for (const r of site.resources || []) {
    const p = r.path && join(dir, r.path);
    if (p && existsSync(p)) { try { text += readFileSync(p, 'utf8') + '\n'; } catch { /* binary */ } }
  }
  return text;
}

const MISS = /^MISS\s+(\S+)/;
const PROGRAM = /\.m?js(\?|$)/;

const out = [], notMeasured = [];
let totComposed = 0, totRecorded = 0, agreeingSites = 0;
let unqComposed = 0, unqRecorded = 0, unqRows = 0;

for (const r of rows) {
  const site = byId.get(r.id);
  if (!site) { notMeasured.push(`${r.id}: census row exists but provenance.json records no frozen capture`); continue; }
  if (!r.pass) { notMeasured.push(`${r.id}: census row states no \`pass\`, so its serve log cannot be named`); continue; }

  /* A PASS-QUALIFIED LOG IS A FACT ABOUT THIS ROW'S RUN; AN UNQUALIFIED ONE IS A FACT ABOUT WHICHEVER PASS
     RAN LAST, AND THE TWO MAY NOT BE SUMMED. Qualification landed after the passes this corpus already holds,
     so `logs/<id>.serve` still exists and is exactly the shape that once made a run read as an AFTER when its
     bytes were a second BEFORE. The fallback is taken -- refusing every historical row would throw away the
     only evidence there is -- and it is MARKED, and its rows are totalled separately, because a number whose
     provenance a reader cannot see is the one they will quote. */
  let logPath = join(LOGS, `${r.pass}-${r.id}.serve`), qualified = true;
  if (!existsSync(logPath)) { logPath = join(LOGS, `${r.id}.serve`); qualified = false; }
  /* AN ABSENT LOG IS NOT A ZERO. CLAUDE.md: an absent count and a zero count are different facts and must
     never be averaged. A site whose server log was never written, or was lost to a race, has not been
     measured at zero -- it has not been measured. */
  if (!existsSync(logPath)) { notMeasured.push(`${r.id} (${r.pass}): no serve log, qualified or bare`); continue; }

  const missed = new Set();
  for (const line of readFileSync(logPath, 'utf8').split('\n')) {
    const m = MISS.exec(line);
    if (m && PROGRAM.test(m[1])) missed.add(m[1]);
  }

  const bytes = shippedBytes(site);
  const composed = [...missed].filter((p) => {
    const b = basename(p.split('?')[0]);
    return !bytes.includes(b) && !bytes.includes(p);
  });

  /* THE ENGINE'S SIDE IS ITS OWN RECORD, MATCHED ON THE BASENAME. The engine records an absolute URL against
     the fixture's own origin and the serve log records the served path, so the two spellings differ by a
     prefix neither party chose. Matching on the basename is what makes them comparable; it can only
     OVER-credit the engine, which is the safe direction for a claim that the engine recorded nothing. */
  const learned = (r.siteEndpoints || []).join('\n');
  const recorded = composed.filter((p) => learned.includes(basename(p.split('?')[0])));

  if (qualified) { totComposed += composed.length; totRecorded += recorded.length; }
  else { unqComposed += composed.length; unqRecorded += recorded.length; unqRows++; }
  if (recorded.length) agreeingSites++;
  /* THE TERMINAL EVENT, NAMED AND NOT QUOTED. A row's `why` carries a whole @WHY envelope -- the condition,
     the file:line and a paragraph of remedy -- and printing it whole buries the table it annotates. What a
     reader needs here is only WHETHER this row's zeros are about composition or about a run that died, so
     the coordinate is enough and the paragraph is one `grep` away in the census itself. */
  const at = /"at"\s*:\s*"([^"]+)"/.exec(r.why || '');
  const why = at ? at[1] : (r.why ? String(r.why).slice(0, 28) : (r.atE ? 'atE' : 'ok'));
  out.push({ id: r.id, pass: r.pass, qualified, why, composed: composed.length,
             recorded: recorded.length, missTotal: missed.size, sample: composed.slice(0, 2) });
}

out.sort((a, b) => b.composed - a.composed || a.id.localeCompare(b.id));
console.log('site            pass            log  composed  recorded  (of MISS .js)  terminal');
for (const o of out)
  console.log(`${o.id.padEnd(15)} ${o.pass.padEnd(15)} ${o.qualified ? 'PASS' : 'BARE'} ${String(o.composed).padStart(8)}  ${String(o.recorded).padStart(8)}  ${String(o.missTotal).padStart(13)}  ${o.why}`);

if (out.length - unqRows)
  console.log(`\nPASS-QUALIFIED   composed ${totComposed}   recorded ${totRecorded}   over ${out.length - unqRows} rows`);
if (unqRows)
  console.log(`BARE LOG         composed ${unqComposed}   recorded ${unqRecorded}   over ${unqRows} rows` +
    ` -- these read logs/<id>.serve, which belongs to WHICHEVER PASS RAN LAST and may not be this row's run.` +
    ` Not summed with the line above.`);

/* THE ARMED CONTROL, AND IT REFUSES RATHER THAN REPORTING. A table of zeroes is consistent with "the engine
   composed nothing" AND with "this instrument cannot match the two spellings", and those take opposite work.
   CLAUDE.md: a control that has never produced a finding is not a control, and reporting a blind spot from
   one is reporting on your own probe. So a run in which NO site shows agreement is not a result. */
if (out.length && !agreeingSites) {
  console.error(`\nREFUSING TO REPORT: not one measured row shows the engine recording a composed address.\n` +
    `  That is indistinguishable from this file failing to match the two spellings, so it is not evidence\n` +
    `  about the engine. Establish agreement on at least one site before quoting any zero above.`);
  process.exit(1);
}
console.log(`ARMED: ${agreeingSites} of ${out.length} rows show agreement, so the zeros are a result.`);

if (notMeasured.length) {
  console.log('\nNOT MEASURED (named rather than counted as zero):');
  for (const n of notMeasured) console.log('  ' + n);
}
