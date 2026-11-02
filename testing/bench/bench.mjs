/* The scheduler benchmark: what a native engine produces per unit of engine work over recorded app pages, read
 * at fixed work budgets over several runs, so two engines are compared on one input with the spread beside it.
 *   record --store <dir> --snap <snapshot> [--pages a,b] [--work <units>]
 *   run --store <dir> --snap <snapshot> --out <dir> [--runs 3] [--budgets 10.8e6,27e6,54e6] [--pages a,b]
 *       [--jobs 1] [--backstop <s>] [--rss-cap <MiB>] [--bin <binary>]
 *   table <out-dir> | ab <out-A> <out-B> | manifest --store <dir>
 * `--snap` is a frozen snapshot (engine/frozen_snapshot.sh) holding engine/trusted.mjs and a built
 * engine/host/out/qjs-native-none; its zone drives its binary over a page with the wire answered by wire.mjs.
 * A run is cut at the first census sample whose summed @COLD `stepWork` passes the largest budget, and each
 * budget is read at the first sample past it, as the union of every instance's latest @RESULT. A run that ends
 * first (abort, drain, backstop) has no reading at that budget, never a zero. The documents each reading is taken
 * from, and the last sample's, are kept as logs/<page>.<run>.<budget|final>.results.json.
 * Recorded replies rather than live ones because an A/B asks what changed between two engines, and live input
 * would be a second moving subject. A recording is what the page's own sends received at a stated instant,
 * keyed by the URL each was sent to; the bytes stay outside the repository, and manifest.json names them. */

import { spawn, spawnSync } from 'node:child_process';
import { createInterface } from 'node:readline';
import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync, appendFileSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadavg } from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const WIRE = join(HERE, 'wire.mjs');

/* corpus.tsv: id, url, why. `#` lines are comments. */
export function corpus() {
  const rows = [];
  for (const line of readFileSync(join(HERE, 'corpus.tsv'), 'utf8').split('\n')) {
    if (line === '' || line.startsWith('#')) continue;
    const [id, url, why] = line.split('\t');
    if (!/^[a-z0-9-]+$/.test(id) || !URL.canParse(url) || !why)
      throw new Error(`testing/bench/corpus.tsv: a row is not <id>\\t<url>\\t<why>: ${line}`);
    rows.push({ id, url, why });
  }
  return rows;
}

function args(argv) {
  const o = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    if (!argv[i].startsWith('--')) { o._.push(argv[i]); continue; }
    const k = argv[i].slice(2), v = argv[i + 1];
    if (v === undefined || v.startsWith('--')) throw new Error(`--${k} takes a value`);
    o[k] = v; i++;
  }
  return o;
}

/* The zone of snapshot `snap` and the binary it drives: the snapshot's own build, or `binOverride` (a native
   binary with its `.build.json` stamp beside it) when an A/B holds the zone fixed and varies only the engine. */
function snapshotOf(snap, binOverride) {
  const zone = join(snap, 'engine', 'trusted.mjs');
  const bin = binOverride ?? join(snap, 'engine', 'host', 'out', 'qjs-native-none');
  if (!existsSync(zone) || !existsSync(bin))
    throw new Error(`${snap} holds no engine/trusted.mjs, or ${bin} does not exist — build a snapshot with ` +
                    '`engine/frozen_snapshot.sh <sha> <lane> node engine/build.mjs native`');
  const stamp = bin + '.build.json';
  const sha = createHash('sha256').update(readFileSync(bin)).digest('hex');
  let revision = null;
  try { revision = JSON.parse(readFileSync(stamp, "utf8")).head ?? null; } catch { revision = null; }
  return { zone, bin, binSha256: sha, revision };
}

/* ── One reading: the union of every instance's latest result document ────────────────────────────────────── */

const key = (r) => `${r.method} ${r.url}`;

/* Throws naming the field when the engine's result document lacks one this reader counts; extension/bridge.js
   asserts the same document, so a miss is a reader no longer paired with its producer. */
function need(ok, what) { if (!ok) throw new Error(`a result document breaks its contract: ${what}`); }

/* The population a reading counts, from result documents `docs` (one per instance). An address is runtime-only
   when any row of it states that razor class (endpoint.c's `razorClass`, per address); its keys and values are
   the distinct (location, name) and (location, name, example) of those rows' params. A finding is a
   securitySinks entry carrying a `poc` (fire-verified); a sink search is any entry, by (sink, source).
   `noveltyPaid` sums `_novelty.kinds[].paid` (asks the product had not seen) and `landedValue` sums
   `_novelty.landed` (the paid value Level 1 ranks: 1.0 a razor identity, 0.25 a statable one). */
export function readingOf(docs) {
  const endpoints = new Map();
  const sinks = new Set(), findings = new Set();
  const drives = new Set();
  const paidByKind = {};
  let pageErrors = 0, landed = 0, attackerRooted = 0;
  const sec = { sourceReads: 0, sinkReached: 0, sinkTainted: 0, candidates: 0 };
  for (const d of docs) {
    for (const [f, k] of [['_sourceReads', 'sourceReads'], ['_sinkReached', 'sinkReached'],
                          ['_sinkTainted', 'sinkTainted'], ['_candidates', 'candidates']]) {
      need(Number.isInteger(d[f]), `${f} is an integer`);
      sec[k] += d[f];
    }
    need(Array.isArray(d.fetchCallSites) && Array.isArray(d.securitySinks) && Array.isArray(d.pageErrors),
         'fetchCallSites, securitySinks and pageErrors are arrays');
    for (const r of d.fetchCallSites) {
      need(typeof r.method === 'string' && typeof r.url === 'string' && typeof r.razorClass === 'string' &&
           typeof r.provenance === 'string' && Array.isArray(r.params),
           `a fetchCallSites row has method, url, razorClass, provenance and params: ${JSON.stringify(r).slice(0, 200)}`);
      const k = `${r.method} ${r.url}`;
      const row = endpoints.get(k) || { razor: new Set(), provenance: new Set(), params: new Set(), values: new Set(),
                                        attackerRooted: false };
      /* `false`, or the record of the attacker source rooting the address (endpoint.c's `attackerRooted`). */
      need(r.attackerRooted === false || (typeof r.attackerRooted === 'object' && r.attackerRooted !== null),
           `a fetchCallSites row's attackerRooted is false or a record: ${JSON.stringify(r.attackerRooted)}`);
      row.attackerRooted ||= r.attackerRooted !== false;
      row.razor.add(r.razorClass);
      row.provenance.add(r.provenance);
      for (const p of r.params) {
        need(typeof p.name === 'string' && typeof p.location === 'string' && Array.isArray(p.validValues),
             `a param has name, location and validValues: ${JSON.stringify(p).slice(0, 200)}`);
        row.params.add(`${p.location}:${p.name}`);
        for (const v of p.validValues) row.values.add(`${p.location}:${p.name}=${JSON.stringify(v)}`);
      }
      endpoints.set(k, row);
    }
    for (const s of d.securitySinks) {
      need(typeof s.sink === 'string' && typeof s.source === 'string', 'a securitySinks entry has sink and source');
      const id = `${s.sink} ${s.source}`;
      sinks.add(id);
      if (typeof s.poc === 'string' && s.poc !== '') findings.add(id);
    }
    pageErrors += d.pageErrors.length;
    const nov = d._novelty;
    need(nov && Array.isArray(nov.kinds) && Array.isArray(nov.identities) && typeof nov.landed === 'number',
         '_novelty has kinds, identities and landed');
    for (const k of nov.kinds) {
      need(typeof k.kind === 'string' && Number.isInteger(k.paid), '_novelty.kinds rows have kind and paid');
      paidByKind[k.kind] = (paidByKind[k.kind] || 0) + k.paid;
    }
    landed += nov.landed;
    for (const line of nov.identities) {
      const m = /^drive ([0-9a-f]{32}) [1-9][0-9]*$/.exec(line);
      if (m !== null) drives.add(m[1]);
    }
  }
  const rt = [...endpoints.entries()].filter(([, r]) => r.razor.has('runtime-only'));
  const provenance = {}, razor = {};
  for (const [, r] of endpoints) {
    if (r.attackerRooted) attackerRooted++;
    for (const p of r.provenance) provenance[p] = (provenance[p] || 0) + 1;
    for (const c of r.razor) razor[c] = (razor[c] || 0) + 1;
  }
  return {
    addresses: endpoints.size,
    runtimeOnlyAddresses: rt.length,
    runtimeOnlyKeys: rt.reduce((n, [, r]) => n + r.params.size, 0),
    runtimeOnlyValues: rt.reduce((n, [, r]) => n + r.values.size, 0),
    runtimeOnly: rt.map(([k]) => k).sort(),
    addressesByProvenance: provenance,
    addressesByRazor: razor,
    sinkSearches: sinks.size,
    findingsFired: findings.size,
    attackerRootedAddresses: attackerRooted,
    ...sec,
    noveltyPaid: Object.values(paidByKind).reduce((a, b) => a + b, 0),
    noveltyPaidByKind: paidByKind,
    landedValue: +landed.toFixed(3),
    drivesUncalled: drives.size,
    pageErrors,
  };
}

/* ── One run ──────────────────────────────────────────────────────────────────────────────────────────────── */

/* The frontier's membership at the sample, summed over instances, from @WFQ (solver/flow.c's census): every
   member, the members resting on assumed application state, and the observed world's. */
function populationOf(wfqs) {
  const o = { members: 0, assumedMembers: 0, observedMembers: 0 };
  for (const w of wfqs)
    for (const k of Object.keys(o)) {
      need(Number.isInteger(w[k]), `@WFQ carries an integer ${k}`);
      o[k] += w[k];
    }
  return o;
}

const LINE = /^\[trusted\] \[([^\]]+)\] (@COLD|@WFQ|@RESULT)#(\d+) \2 (.*)$/;
const ABORT = /^@(?:WHY|E) (?:\{"phase":"assert"|.*\([^()]*:\d+\)$)/;

function runOne({ page, store, snap, budgets, backstopS, rssCapMiB, logDir, runIx, wire = 'replay' }) {
  return new Promise((resolveRun) => {
    const tag = `${page.id}.${runIx}`;
    const wireLog = join(logDir, `${tag}.wire.jsonl`);
    const errLog = join(logDir, `${tag}.stderr.tail`);
    writeFileSync(wireLog, '');
    const load0 = loadavg()[0];
    const t0 = Date.now();
    const child = spawn(process.execPath, ['--import', WIRE, snap.zone, page.url, snap.bin], {
      cwd: dirname(dirname(snap.zone)), detached: true, stdio: ['ignore', 'ignore', 'pipe'],
      env: { ...process.env, ...(wire === 'record' ? { NODE_USE_ENV_PROXY: '1' } : {}), BENCH_WIRE: wire,
             BENCH_STORE: store, BENCH_PAGE: page.id, BENCH_WIRE_LOG: wireLog },
    });
    const inst = new Map();                 /* tag -> {work, result text} */
    const readings = [];
    let next = 0, cut = false, abort = null, results = 0;
    const tail = [];
    const kill = (why) => {
      if (cut) return;
      cut = why;
      try { process.kill(-child.pid, 'SIGKILL'); } catch { /* the group has already exited */ }
    };
    const backstop = setTimeout(() => kill('backstop'), backstopS * 1000);
    /* The session's resident memory, summed over its process group, polled; past `rssCapMiB` the run is ended
       with its own signal, because the machine is shared and the kernel's OOM killer would pick any lane's
       process. */
    let rssPeakMiB = 0;
    const rssPoll = setInterval(() => {
      const ps = spawnSync('ps', ['-o', 'rss=', '-g', String(child.pid)], { encoding: 'utf8' });
      const mib = ps.stdout.split('\n').filter(Boolean).reduce((n, x) => n + Number(x), 0) / 1024;
      rssPeakMiB = Math.max(rssPeakMiB, Math.round(mib));
      if (mib > rssCapMiB) kill('rss-backstop');
    }, 2000);
    const progress = join(logDir, `${tag}.progress.jsonl`);
    writeFileSync(progress, '');
    const sample = () => {
      let work = 0;
      for (const i of inst.values()) work += i.work;
      appendFileSync(progress, JSON.stringify({ stepWork: work, wallS: (Date.now() - t0) / 1000, rssPeakMiB,
                                                load1: loadavg()[0] }) + '\n');
      while (next < budgets.length && work >= budgets[next]) {
        const docs = [...inst.values()].filter((i) => i.result !== null).map((i) => JSON.parse(i.result));
        writeFileSync(join(logDir, `${tag}.${budgets[next]}.results.json`), JSON.stringify(docs));
        readings.push({ budget: budgets[next], stepWork: work, instances: inst.size, ...readingOf(docs),
                        ...populationOf([...inst.values()].filter((i) => i.wfq !== null).map((i) => JSON.parse(i.wfq))) });
        next++;
      }
      if (next === budgets.length) kill('budget');
    };
    /* A census sample is written as @SWAP, @COLD, …, then @RESULT, so a sample's work is known when its
       result arrives, and the reading is taken there. */
    const rl = createInterface({ input: child.stderr, crlfDelay: Infinity });
    rl.on('line', (line) => {
      if (abort === null && ABORT.test(line)) abort = line.slice(0, 400);
      tail.push(line.slice(0, 400)); if (tail.length > 40) tail.shift();
      const m = LINE.exec(line);
      if (m === null) return;
      const [, itag, marker, , payload] = m;
      if (!inst.has(itag)) inst.set(itag, { work: 0, result: null, wfq: null });
      const i = inst.get(itag);
      if (marker === '@RESULT') { i.result = payload; results++; sample(); return; }
      if (marker === '@WFQ') { i.wfq = payload; return; }
      const c = JSON.parse(payload);
      if (!Number.isSafeInteger(c.stepWork))
        throw new Error(`[${itag}] @COLD carries no integer stepWork: ${payload.slice(0, 200)}`);
      i.work = c.stepWork;
    });
    child.on('exit', (code, signal) => {
      clearTimeout(backstop);
      rl.close();
      clearInterval(rssPoll);
      const endedBy = cut ? cut
        : abort !== null ? 'abort' : code === 0 ? 'drained' : `exit ${code}${signal ? ' ' + signal : ''}`;
      let work = 0;
      for (const i of inst.values()) work += i.work;
      /* A drained frontier's last document is its answer at every larger budget. */
      if (endedBy === 'drained') {
        const docs = [...inst.values()].filter((i) => i.result !== null).map((i) => JSON.parse(i.result));
        while (next < budgets.length) readings.push({ budget: budgets[next++], stepWork: work, instances: inst.size,
                                                      drained: true, ...readingOf(docs),
                                                      ...populationOf([...inst.values()].filter((i) => i.wfq !== null)
                                                                        .map((i) => JSON.parse(i.wfq))) });
      }
      /* The last sample the run produced, whatever ended it, kept apart from the budget readings. */
      const lastDocs = [...inst.values()].filter((i) => i.result !== null).map((i) => JSON.parse(i.result));
      writeFileSync(join(logDir, `${tag}.final.results.json`), JSON.stringify(lastDocs));
      const final = lastDocs.length === 0 ? null : { stepWork: work, instances: inst.size, ...readingOf(lastDocs),
        ...populationOf([...inst.values()].filter((i) => i.wfq !== null).map((i) => JSON.parse(i.wfq))) };
      const wire = readFileSync(wireLog, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
      writeFileSync(errLog, tail.join('\n') + '\n');
      resolveRun({
        page: page.id, run: runIx, endedBy, abort, stepWorkReached: work, results,
        instances: inst.size, readings, final,
        wireSends: wire.length, wireMisses: wire.filter((w) => !w.hit).length,
        wireMissed: [...new Set(wire.filter((w) => !w.hit).map((w) => `${w.method} ${w.url}`))].slice(0, 20),
        load1: [load0, loadavg()[0]], wallS: (Date.now() - t0) / 1000, rssPeakMiB,
      });
    });
  });
}

/* ── Statistics ───────────────────────────────────────────────────────────────────────────────────────────── */

const METRICS = ['runtimeOnlyAddresses', 'runtimeOnlyKeys', 'runtimeOnlyValues', 'findingsFired', 'landedValue',
                 'noveltyPaid', 'addresses', 'attackerRootedAddresses', 'sinkSearches', 'sinkTainted', 'candidates',
                 'drivesUncalled', 'observedMembers', 'assumedMembers', 'members'];
const SHORT = { runtimeOnlyAddresses: 'rtAddr', runtimeOnlyKeys: 'rtKeys', runtimeOnlyValues: 'rtVals',
                findingsFired: 'fired', landedValue: 'landed', noveltyPaid: 'paid', addresses: 'addr', attackerRootedAddresses: 'atkAddr',
                sinkSearches: 'sinks', sinkTainted: 'tainted', candidates: 'cands', drivesUncalled: 'drives',
                observedMembers: 'obsMem', assumedMembers: 'asmMem', members: 'mem' };

function spread(xs) {
  if (xs.length === 0) return null;
  const s = [...xs].sort((a, b) => a - b);
  const med = s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
  return { n: s.length, min: s[0], med, max: s[s.length - 1] };
}
const fmt = (sp) => sp === null ? '—' : sp.min === sp.max ? `${sp.min}` : `${sp.min}/${sp.med}/${sp.max}`;

function loadRuns(dir) {
  const f = join(dir, 'runs.jsonl');
  if (!existsSync(f)) throw new Error(`${dir} holds no runs.jsonl`);
  return readFileSync(f, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
}

/* page -> budget -> metric -> values over the runs that reached that budget, and the runs that did not. */
function cells(runs) {
  const out = new Map();
  for (const r of runs) {
    if (!out.has(r.page)) out.set(r.page, { runs: [], budgets: new Map() });
    const p = out.get(r.page);
    p.runs.push(r);
    for (const g of r.readings) {
      if (!p.budgets.has(g.budget)) p.budgets.set(g.budget, []);
      p.budgets.get(g.budget).push(g);
    }
  }
  return out;
}

function table(dir) {
  const meta = JSON.parse(readFileSync(join(dir, 'meta.json'), 'utf8'));
  const runs = loadRuns(dir);
  const c = cells(runs);
  const lines = [];
  lines.push(`# ${dir}`);
  lines.push(`revision ${meta.revision} binary sha256 ${meta.binSha256.slice(0, 16)}… runs/page ${meta.runs} ` +
             `budgets ${meta.budgets.map((b) => b / 1e6 + 'M').join(',')} store ${meta.storeDigest.slice(0, 16)}…`);
  const loads = runs.flatMap((r) => r.load1);
  lines.push(`load1 over the runs: ${fmt(spread(loads.map((x) => Math.round(x * 100) / 100)))} (min/median/max) on ${meta.cores} cores`);
  lines.push('cells are min/median/max over the runs that reached the budget; n is that count of runs');
  for (const b of meta.budgets) {
    lines.push('');
    lines.push(`## budget ${b / 1e6}M stepWork`);
    lines.push(['page', 'n', 'ended', ...METRICS.map((m) => SHORT[m]), 'miss'].join('\t'));
    const agg = Object.fromEntries(METRICS.map((m) => [m, []]));
    for (const [page, p] of c) {
      const rs = p.budgets.get(b) || [];
      const ended = p.runs.map((r) => r.endedBy).reduce((o, e) => (o[e] = (o[e] || 0) + 1, o), {});
      lines.push([page, rs.length, Object.entries(ended).map(([k, v]) => `${k}:${v}`).join(','),
                  ...METRICS.map((m) => fmt(spread(rs.map((g) => g[m])))),
                  fmt(spread(p.runs.map((r) => r.wireMisses)))].join('\t'));
      for (const m of METRICS) for (const g of rs) agg[m].push(g[m] / (g.stepWork / 1e6));
    }
    lines.push(`per M stepWork, over every (page, run) reading at this budget: ` +
               METRICS.map((m) => `${SHORT[m]} ${fmt(spread(agg[m].map((x) => +x.toFixed(3))))}`).join('  '));
  }
  lines.push('', '## the last sample of each run, whatever ended it (not a budget reading)',
             ['page', 'run', 'ended', 'work', ...METRICS.map((m) => SHORT[m])].join('\t'));
  for (const r of runs)
    lines.push([r.page, r.run, r.endedBy, r.stepWorkReached,
                ...METRICS.map((m) => (r.final ? r.final[m] : '—'))].join('\t'));
  const aborts = runs.filter((r) => r.abort !== null);
  if (aborts.length) {
    lines.push('');
    lines.push('## aborts');
    for (const r of aborts) lines.push(`${r.page}.${r.run} at ${r.stepWorkReached} work: ${r.abort.slice(0, 240)}`);
  }
  return lines.join('\n');
}

/* A delta is called only when the two runs' ranges do not overlap; otherwise it is within the spread. */
function ab(dirA, dirB) {
  const [ma, mb] = [dirA, dirB].map((d) => JSON.parse(readFileSync(join(d, 'meta.json'), 'utf8')));
  /* A page is compared only when both arms were answered from the same recording of it. */
  const sameInput = (page) => ma.pageDigests[page] !== undefined && ma.pageDigests[page] === mb.pageDigests[page];
  const [ca, cb] = [loadRuns(dirA), loadRuns(dirB)].map(cells);
  const lines = [`# A ${ma.revision} (${dirA})  vs  B ${mb.revision} (${dirB})`,
                 'cell: A min/med/max -> B min/med/max, then "+"/"-" when B\'s range lies wholly above/below A\'s, ' +
                 '"~" when the ranges overlap'];
  const budgets = ma.budgets.filter((b) => mb.budgets.includes(b));
  for (const b of budgets) {
    lines.push('', `## budget ${b / 1e6}M stepWork`, ['page', 'nA', 'nB', ...METRICS.map((m) => SHORT[m])].join('\t'));
    for (const page of new Set([...ca.keys(), ...cb.keys()])) {
      if (!sameInput(page)) { lines.push(`${page}\tthe two arms were answered from different recordings`); continue; }
      const ra = ca.get(page)?.budgets.get(b) || [], rb = cb.get(page)?.budgets.get(b) || [];
      lines.push([page, ra.length, rb.length, ...METRICS.map((m) => {
        const sa = spread(ra.map((g) => g[m])), sb = spread(rb.map((g) => g[m]));
        if (sa === null || sb === null) return `${fmt(sa)}->${fmt(sb)}`;
        const v = sb.min > sa.max ? '+' : sb.max < sa.min ? '-' : '~';
        return `${fmt(sa)}->${fmt(sb)}${v}`;
      })].join('\t'));
    }
  }
  return lines.join('\n');
}

/* ── The store's manifest ─────────────────────────────────────────────────────────────────────────────────── */

export function manifest(store) {
  const rows = [];
  for (const page of corpus()) {
    const dir = join(store, 'pages', page.id);
    if (!existsSync(dir)) { rows.push({ id: page.id, url: page.url, recorded: false }); continue; }
    const entries = readdirSync(dir).filter((f) => f.endsWith('.json'))
      .map((f) => JSON.parse(readFileSync(join(dir, f), 'utf8')))
      .sort((a, b) => (a.key.method + a.key.url).localeCompare(b.key.method + b.key.url));
    const digest = createHash('sha256').update(JSON.stringify(entries.map((e) =>
      [e.key, e.status ?? null, e.error ?? null, e.body ?? null]))).digest('hex');
    const dates = entries.map((e) => e.recordedAt).sort();
    rows.push({ id: page.id, url: page.url, recorded: true, from: dates[0], to: dates[dates.length - 1],
                replies: entries.length, errors: entries.filter((e) => e.error !== undefined).length,
                bodyBytes: entries.reduce((n, e) => n + (e.bytes || 0), 0), sha256: digest });
  }
  return rows;
}
const storeDigest = (rows) => createHash('sha256').update(JSON.stringify(rows.map((r) => [r.id, r.sha256 ?? null])))
  .digest('hex');

/* ── Commands ─────────────────────────────────────────────────────────────────────────────────────────────── */

function pagesOf(o) {
  const all = corpus();
  if (!o.pages) return all;
  const want = o.pages.split(',');
  for (const w of want) if (!all.some((p) => p.id === w)) throw new Error(`no page ${w} in corpus.tsv`);
  return all.filter((p) => want.includes(p.id));
}

/* Records each page by running the snapshot's zone over it with the recording wire until `--work` units or a
   backstop. Sends already recorded are answered from the store, so a second pass adds only what earlier passes
   did not reach. */
async function record(o) {
  const store = resolve(o.store), snap = snapshotOf(resolve(o.snap), o.bin ? resolve(o.bin) : undefined);
  const work = Number(o.work || 54e6), backstopS = Number(o.backstop || 1800), rssCapMiB = Number(o['rss-cap'] || 4096);
  const logDir = join(store, 'logs');
  mkdirSync(logDir, { recursive: true });
  for (const page of pagesOf(o)) {
    const runIx = 'rec' + new Date().toISOString().replace(/[:.]/g, '');
    const r = await runOne({ page, store, snap, budgets: [work], backstopS, rssCapMiB, logDir, runIx, wire: 'record' });
    appendFileSync(join(logDir, 'record.jsonl'), JSON.stringify({ ...r, readings: undefined }) + '\n');
    console.log(`${page.id} ${r.endedBy} work=${r.stepWorkReached} sends=${r.wireSends} new=${r.wireMisses} ` +
                `rss=${r.rssPeakMiB}MiB wall=${r.wallS.toFixed(0)}s${r.abort ? ' ABORT ' + r.abort.slice(0, 200) : ''}`);
  }
}

async function run(o) {
  const store = resolve(o.store), snap = snapshotOf(resolve(o.snap), o.bin ? resolve(o.bin) : undefined), out = resolve(o.out);
  const budgets = (o.budgets || '10.8e6,54e6,216e6').split(',').map(Number).sort((a, b) => a - b);
  const runs = Number(o.runs || 3), jobs = Number(o.jobs || 1), backstopS = Number(o.backstop || 3600);
  const rssCapMiB = Number(o['rss-cap'] || 4096);
  const pages = pagesOf(o);
  const rows = manifest(store).filter((r) => pages.some((p) => p.id === r.id));
  for (const r of rows) if (!r.recorded) throw new Error(`${r.id} has no recording in ${store}`);
  mkdirSync(join(out, 'logs'), { recursive: true });
  const meta = { revision: snap.revision, binSha256: snap.binSha256, snap: resolve(o.snap), bin: snap.bin, budgets, runs,
                 store, storeDigest: storeDigest(manifest(store)), pages: pages.map((p) => p.id),
                 pageDigests: Object.fromEntries(rows.map((r) => [r.id, r.sha256])),
                 cores: (await import('node:os')).cpus().length, started: new Date().toISOString() };
  writeFileSync(join(out, 'meta.json'), JSON.stringify(meta, null, 1));
  /* Runs interleave pages (run 0 of every page, then run 1 …) so a load change over the session spreads across
     pages instead of landing on one. */
  const queue = [];
  for (let k = 0; k < runs; k++) for (const page of pages) queue.push({ page, runIx: k });
  const worker = async () => {
    while (queue.length) {
      const { page, runIx } = queue.shift();
      const r = await runOne({ page, store, snap, budgets, backstopS, rssCapMiB, logDir: join(out, 'logs'), runIx });
      appendFileSync(join(out, 'runs.jsonl'), JSON.stringify(r) + '\n');
      const last = r.readings[r.readings.length - 1];
      console.log(`${page.id}.${runIx} ${r.endedBy} work=${r.stepWorkReached} readings=${r.readings.length} ` +
                  (last ? `rtAddr=${last.runtimeOnlyAddresses} addr=${last.addresses} fired=${last.findingsFired} ` +
                          `landed=${last.landedValue} paid=${last.noveltyPaid} ` : '') +
                  `miss=${r.wireMisses}/${r.wireSends} load=${r.load1.map((x) => x.toFixed(2)).join('->')} ` +
                  `wall=${r.wallS.toFixed(0)}s${r.abort ? ' ABORT ' + r.abort.slice(0, 160) : ''}`);
    }
  };
  await Promise.all(Array.from({ length: jobs }, worker));
  console.log(table(out));
}

const [cmd, ...rest] = process.argv.slice(2);
const o = args(rest);
if (cmd === 'record') await record(o);
else if (cmd === 'run') await run(o);
else if (cmd === 'table') console.log(table(o._[0]));
else if (cmd === 'ab') console.log(ab(o._[0], o._[1]));
else if (cmd === 'manifest') console.log(JSON.stringify(manifest(resolve(o.store)), null, 1));
else {
  console.error('usage: node testing/bench/bench.mjs record|run|table|ab|manifest … (see the header)');
  process.exit(2);
}
