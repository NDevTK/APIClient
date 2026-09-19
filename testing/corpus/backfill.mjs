/* FILL A MIRROR'S DECLINED RESOURCES FROM ITS OWN RECORD — the other half of widening `runtimeStorable`.
 *
 * WHY IT IS A SEPARATE TOOL AND NOT A RE-CAPTURE. `mirror.mjs --runtime` drives a real browser and rewrites
 * a row wholesale, so re-running it to obtain thirteen icons would re-fetch every chunk from a site that has
 * shipped new builds since — a row's bytes would move under every measurement anybody has quoted against it.
 * The record already holds what is missing: a declined resource carries its URL, its status, its
 * `contentType`, its `bytes` and its `sha256`. That is enough to fetch exactly what was seen and to PROVE it
 * is the same bytes, which re-capture cannot do.
 *
 * SO THE SHA IS THE WHOLE POINT AND NOT A PRECAUTION. A fetch today returns whatever the origin serves
 * today; writing that into a row stamped with an older `fetchedAt` produces a mirror that is internally
 * inconsistent and says nothing about it — the corpus would carry two dates and claim one. A resource whose
 * upstream has moved is REPORTED AS DRIFTED and left declined, which is a real finding about that row rather
 * than a file to write.
 *
 * THE SAVED-PATH RULE IS A FOURTH COPY AND IT IS VERIFIED RATHER THAN TRUSTED. mirror.mjs computes it at two
 * sites and serve-faithful.mjs recomputes it at a third, with mirror.mjs's own comment saying it "is shared
 * with serve-faithful.mjs … so it may not be changed here alone". A fourth spelling that drifts would write
 * files the server cannot find — a silent miss, which is the failure this corpus is least able to see. So
 * before writing anything, this tool recomputes the path for every resource that ALREADY HAS ONE and
 * requires its own answer to equal the recorded one, for all of them. A single disagreement is a refusal:
 * the rule has moved and this tool is the copy that is wrong.
 *
 *   node testing/corpus/backfill.mjs <id|--all> [--apply]
 *
 * Without `--apply` it fetches, verifies and reports, and writes NOTHING.
 */
import { readFileSync, writeFileSync, mkdirSync, existsSync, renameSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';

const MANIFEST = 'testing/corpus/provenance.json';
const MIRROR = 'testing/corpus/mirror';
const sha = (b) => createHash('sha256').update(b).digest('hex');

/* The rule, spelled as mirror.mjs:240 and serve-faithful.mjs:36 spell it. Verified below, never trusted. */
const relPathOf = (urlStr) => {
  const u = new URL(urlStr);
  let rel = u.host + u.pathname + (u.search ? '__q' + sha(u.search).slice(0, 8) : '');
  if (rel.endsWith('/')) rel += 'index';
  return rel.replace(/[^A-Za-z0-9._/@%+-]/g, '_');
};

/* WHAT THE WIDENED GATE ADMITS. Kept in step with mirror.mjs's `runtimeStorable` by being the same
   expression; a row this refuses is one the capture would refuse too, so backfilling it would put a byte in
   the tree that the next capture would decline and nothing would ever reconcile. */
const storable = (ct) => /javascript|ecmascript|json|\/css|\/html|\/xml/i.test(ct || '')
                      || /^image\/|^font\/|\/font-woff/i.test(ct || '');

const arg = process.argv[2];
const APPLY = process.argv.includes('--apply');
if (!arg) { console.error('usage: node testing/corpus/backfill.mjs <id|--all> [--apply]'); process.exit(2); }

const manifest = JSON.parse(readFileSync(MANIFEST, 'utf8'));
const rows = arg === '--all' ? manifest : manifest.filter((m) => m.id === arg);
if (!rows.length) { console.error(`no such id: ${arg}`); process.exit(2); }

/* THE CONTROL, RUN FIRST AND OVER THE WHOLE MANIFEST RATHER THAN OVER THE ROWS BEING FILLED — a path rule
   that agrees on one site and not another is exactly the drift this exists to catch. */
let checked = 0;
for (const site of manifest)
  for (const r of site.resources || []) {
    if (!r.path || !r.url) continue;
    const mine = relPathOf(r.url);
    if (mine !== r.path) {
      console.error(`PATH RULE DISAGREES — this tool is the copy that is wrong, and writing would put files ` +
                    `where serve-faithful.mjs cannot find them.\n  url      ${r.url}\n  recorded ${r.path}\n  mine     ${mine}`);
      process.exit(3);
    }
    checked++;
  }
console.log(`path rule reproduces all ${checked} recorded path(s) — the control passed, so a write lands where the server looks\n`);

let filled = 0, drifted = 0, failed = 0, skipped = 0, already = 0, collided = 0;

/* ATOMIC, for mirror.mjs's own stated reason: a `writeFileSync` over this much JSON is not one operation and
   a reader landing mid-write gets a truncated manifest. */
const saveManifest = () => {
  const tmp = MANIFEST + '.tmp';
  writeFileSync(tmp, JSON.stringify(manifest, null, 1));
  renameSync(tmp, MANIFEST);
};
for (const site of rows) {
  const dirty = [];
  for (const r of site.resources || []) {
    if (r.path) { already++; continue; }
    if (r.status !== 200 || !r.sha256 || typeof r.bytes !== 'number') {
      skipped++; console.log(`NO-RECORD  ${site.id}  ${r.url} (status=${r.status})`); continue;
    }
    if (!storable(r.contentType)) { skipped++; continue; }
    let buf;
    try {
      const res = await fetch(r.url);
      if (res.status !== 200) { failed++; console.log(`HTTP-${res.status}  ${site.id}  ${r.url}`); continue; }
      buf = Buffer.from(await res.arrayBuffer());
    } catch (e) { failed++; console.log(`FETCH-FAIL ${site.id}  ${r.url}  ${e.message}`); continue; }
    const got = sha(buf);
    if (got !== r.sha256 || buf.length !== r.bytes) {
      drifted++;
      console.log(`DRIFTED    ${site.id}  ${r.url}\n             recorded ${r.sha256.slice(0, 16)}… ${r.bytes} B\n             fetched  ${got.slice(0, 16)}… ${buf.length} B`);
      continue;
    }
    if (!APPLY) { filled++; continue; }
    /* THE SAVED-PATH COLLISION IS DECLINED AND NOT THROWN, WHICH IS A LESSON THIS TOOL HAD TO LEARN TWICE.
       mirror.mjs catches exactly these three errnos at its own write and records a `declined`, and its
       comment says why in a sentence this run then demonstrated: the `mkdirSync` threw EEXIST, "which
       aborted the WHOLE capture and lost every resource that row had already stored, so the cost of not
       handling it was not one chunk but a site". This tool did not copy that handling, and the cost here was
       FIFTEEN sites — 121 files written to disk whose manifest rows were discarded with the throw, because
       the manifest is written once at the end. The address that did it is grafana's
       `…/grafanacom-api/orgs`, which is both a resource and the prefix of another, and it is the same named
       residual mirror.mjs already carries: this scheme cannot hold both.
       ONLY THESE THREE ERRNOS ARE CAUGHT and everything else still propagates — a swallow would hide a real
       filesystem failure behind a row that reads like a policy decision. */
    const rel = relPathOf(r.url);
    const f = join(MIRROR, site.id, rel);
    try {
      mkdirSync(dirname(f), { recursive: true });
      writeFileSync(f, buf);
    } catch (e) {
      if (e && (e.code === 'EEXIST' || e.code === 'EISDIR' || e.code === 'ENOTDIR')) {
        collided++;
        r.declined = `saved-path collision (${e.code}) at ${rel} — this scheme cannot hold a path that is `
          + 'both a resource and the prefix of another; not stored, the fixture 404s it';
        console.log(`COLLISION  ${site.id}  ${r.url}  (${e.code})`);
        continue;
      }
      throw e;
    }
    filled++;
    r.path = rel;
    delete r.declined;
    dirty.push(rel);
  }
  if (dirty.length) console.log(`${site.id}: wrote ${dirty.length} file(s)`);
  /* THE MANIFEST IS WRITTEN AFTER EVERY SITE, NOT ONCE AT THE END. A file on disk with no `path` row is the
     inconsistent state — the server recomputes and would serve it, while every reader of the record believes
     it was declined — and a single throw anywhere in the loop used to produce exactly that for every site
     already done. Per-site is the unit because a site is what one row covers. */
  if (APPLY && dirty.length) saveManifest();
}

console.log(`\nbackfill: verified-and-${APPLY ? 'written' : 'writable'}=${filled} drifted=${drifted} ` +
            `fetch-failed=${failed} skipped=${skipped} already-stored=${already} collided=${collided}`);
