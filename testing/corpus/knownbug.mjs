// The known-vulnerability driver: every row of knownbugs.tsv, run through the native engine in its vulnerable
// and its first fixed version, scored on what the engine's own result document says it reached, searched and
// fired.
//
//   node testing/corpus/knownbug.mjs pin [case...]                  fetch every asset and print its digest
//   node testing/corpus/knownbug.mjs stage <case> <variant>         fetch, verify and stage one page; print dir
//   node testing/corpus/knownbug.mjs run --bin <qjs-native-none> [--budget <s>] [--jobs <n>] [case[:variant]...]
//
// Run with NODE_USE_ENV_PROXY=1 wherever the network is behind a proxy. The repository carries none of the
// libraries: each asset is fetched by exact version at run time and refused unless its sha256 is the one the
// case list records. Everything fetched or staged lives under engine/.work/knownbug/, which is ignored.
//
// `stage` writes a variant's files for two origins (the page's site and the attacker's) and `served.json`, the
// data another host needs to serve the same case: each file's path, sha256 and response headers, resolved from
// knownbug/headers.tsv. `run` serves that manifest from two loopback ports and drives the page with the
// engine/trusted.mjs that sits beside the binary (so the zone and the engine are one frozen revision). The run ends when the
// zone exits or at the wall-clock backstop, which is not a bound on the engine: it is this driver handing the
// machine back, and the row says which of the two ended it.
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createServer as createHttpServer } from 'node:http';
import { createWriteStream, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve, posix } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { gunzipSync } from 'node:zlib';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..', '..');
const WORK = join(REPO, 'engine', '.work', 'knownbug');
const CACHE = join(WORK, 'cache');
const VARIANTS = ['vulnerable', 'fixed'];
const COLUMNS = ['case', 'advisory', 'cve', 'package', 'vulnerable', 'fixed', 'source', 'sink', 'advisory_url',
                 'read_from', 'fetched', 'entry', 'harness', 'precondition', 'expected', 'assets_vulnerable',
                 'assets_fixed'];

function fail(msg) { throw new Error(msg); }
const sha256 = (b) => createHash('sha256').update(b).digest('hex');

/* ---- the case list ------------------------------------------------------------------------------------- */

/* One header row naming COLUMNS in order, then one row per case. A short row or an unknown column is fatal:
   a skipped row is a case silently absent from a table whose totals still read as complete. */
export function caseList(path = join(HERE, 'knownbugs.tsv')) {
  const lines = readFileSync(path, 'utf8').split('\n');
  let header = null;
  const rows = [];
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (!line.trim() || line.startsWith('#')) continue;
    const cols = line.split('\t');
    if (header === null) {
      if (cols.join('\t') !== COLUMNS.join('\t'))
        fail(`${path}:${i + 1}: the header row must be exactly ${COLUMNS.join(' ')}; it is ${cols.join(' ')}`);
      header = cols;
      continue;
    }
    if (cols.length !== COLUMNS.length || cols.some((c) => !c.trim()))
      fail(`${path}:${i + 1}: a case row has ${COLUMNS.length} non-empty columns and this one has ` +
           `${cols.filter((c) => c.trim()).length}`);
    const row = Object.fromEntries(COLUMNS.map((k, j) => [k, cols[j].trim()]));
    row.assets = { vulnerable: parseAssets(row.assets_vulnerable, path, i + 1),
                   fixed: parseAssets(row.assets_fixed, path, i + 1) };
    if (rows.some((r) => r.case === row.case)) fail(`${path}:${i + 1}: case ${row.case} is named twice`);
    rows.push(row);
  }
  if (!rows.length) fail(`${path} holds no case rows`);
  return rows;
}

/* An asset is `<served path>=<spec>@sha256:<hex>`, items separated by spaces. A served path ending in `/`
   is a directory; its digest is the sha256 of its sha256sum-format manifest, sorted by path. */
function parseAssets(text, path, line) {
  return text.split(' ').filter(Boolean).map((item) => {
    const eq = item.indexOf('='), at = item.lastIndexOf('@sha256:');
    if (eq <= 0 || at < eq) fail(`${path}:${line}: asset \`${item}\` is not <served>=<spec>@sha256:<hex>`);
    const served = item.slice(0, eq), spec = item.slice(eq + 1, at), digest = item.slice(at + 8);
    if (!/^[0-9a-f]{64}$/.test(digest) && digest !== 'unpinned')
      fail(`${path}:${line}: asset \`${item}\` carries no sha256`);
    if (served.startsWith('/') || served.split('/').includes('..'))
      fail(`${path}:${line}: asset \`${item}\` is served outside the page's directory`);
    return { served, spec, digest };
  });
}

/* ---- fetching ------------------------------------------------------------------------------------------ */

async function get(url) {
  const r = await fetch(url);
  if (!r.ok) fail(`GET ${url} answered ${r.status}`);
  return Buffer.from(await r.arrayBuffer());
}

/* The members of a ustar/pax tarball, as {path, data}. npm packs every member under `package/`. */
function untar(buf) {
  const out = [];
  let off = 0, paxPath = null;
  const str = (a, b) => buf.toString('utf8', a, b).replace(/\0.*$/s, '');
  while (off + 512 <= buf.length) {
    if (buf[off] === 0) break;
    const size = parseInt(str(off + 124, off + 136).trim() || '0', 8);
    const type = String.fromCharCode(buf[off + 156] || 48);
    const prefix = str(off + 345, off + 500);
    let name = str(off, off + 100);
    if (prefix) name = prefix + '/' + name;
    const data = buf.subarray(off + 512, off + 512 + size);
    if (type === 'x') {
      const m = data.toString('utf8').match(/\d+ path=([^\n]*)\n/);
      paxPath = m ? m[1] : null;
    } else if (type === '0' || type === '\0' || type === '7') {
      out.push({ path: paxPath ?? name, data });
      paxPath = null;
    } else {
      paxPath = null;
    }
    off += 512 + Math.ceil(size / 512) * 512;
  }
  return out;
}

function npmSpec(spec) {
  const body = spec.slice('npm:'.length);
  const at = body.indexOf('@', body.startsWith('@') ? 1 : 0);
  const slash = body.indexOf('/', at);
  if (at < 0 || slash < 0) fail(`\`${spec}\` is not npm:<name>@<version>/<path>`);
  return { name: body.slice(0, at), version: body.slice(at + 1, slash), member: body.slice(slash + 1) };
}

const tarballs = new Map();
async function npmMembers(name, version) {
  const key = `${name}@${version}`;
  if (!tarballs.has(key)) {
    const base = name.split('/').pop();
    const url = `https://registry.npmjs.org/${name}/-/${base}-${version}.tgz`;
    const members = (async () => untar(gunzipSync(await get(url)))
      .map((m) => ({ path: m.path.replace(/^[^/]+\//, ''), data: m.data })))();
    tarballs.set(key, members);
  }
  return tarballs.get(key);
}

/* The pdf.js generic viewer, as its unbuilt source tree at a release tag, in the development mode its own
   `gulp server` serves: web/viewer.html with its import map, every module reachable from it, the stylesheets
   and images they name, and the en-US strings the l10n fallback reads. Bare specifiers resolve through the
   viewer's import map; `node_modules/` and `build/` are not in the tree and come from npm assets. */
async function pdfjsTree(tag) {
  const base = `https://raw.githubusercontent.com/mozilla/pdf.js/${tag}/`;
  const files = new Map();
  const queue = ['web/viewer.html', 'l10n/en-US/viewer.ftl'];
  let imports = null;
  const want = (from, ref, module) => {
    // A template literal composing code at run time (`import("${url}")` inside a string) names no file.
    if (/^(data:|https?:|#|blob:|resource:)/.test(ref) || ref.includes('${')) return;
    let p;
    if (module && !ref.startsWith('.') && !ref.startsWith('/')) {
      if (!imports) fail(`pdf.js ${tag}: ${from} imports \`${ref}\` before the viewer's import map was read`);
      const key = Object.keys(imports).filter((k) => k === ref || (k.endsWith('/') && ref.startsWith(k)))
        .sort((a, b) => b.length - a.length)[0];
      if (!key) fail(`pdf.js ${tag}: ${from} imports \`${ref}\`, which the viewer's import map does not name`);
      p = posix.normalize(posix.join('web', imports[key] + ref.slice(key.length)));
    } else {
      p = posix.normalize(posix.join(posix.dirname(from), ref.replace(/[?#].*$/, '')));
    }
    if (p.startsWith('node_modules/') || p.startsWith('build/')) return;
    if (!files.has(p) && !queue.includes(p)) queue.push(p);
  };
  while (queue.length) {
    const batch = queue.splice(0, 8);
    const got = await Promise.all(batch.map(async (p) => [p, await get(base + p)]));
    for (const [p, data] of got) {
      files.set(p, data);
      const text = data.toString('utf8');
      if (p.endsWith('.html')) {
        /* The other builds' tags stand in comments (the preprocessor's #if arms); development mode is what is
           left outside them. */
        const live = text.replace(/<!--[\s\S]*?-->/g, '');
        const m = live.match(/<script type="importmap">([\s\S]*?)<\/script>/);
        if (m) imports = JSON.parse(m[1]).imports;
        for (const s of live.matchAll(/<(?:script|link)\b[^>]*?\b(?:src|href)="([^"]+)"/g)) {
          if (/<link[^>]*rel="resource"/.test(s[0])) continue;   // locale.json: a build product, absent here
          want(p, s[1]);
        }
      } else if (/\.m?js$/.test(p)) {
        /* JSDoc names types with import("...") inside comments; only code loads modules. */
        const code = text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/.*$/gm, '');
        for (const s of code.matchAll(/^\s*(?:import|export)\b[^;]*?\bfrom\s*["']([^"'\s]+)["']/gm)) want(p, s[1], true);
        for (const s of code.matchAll(/^\s*import\s*["']([^"'\s]+)["']/gm)) want(p, s[1], true);
        for (const s of code.matchAll(/\bimport\(\s*["']([^"'\s]+)["']\s*\)/g)) want(p, s[1], true);
      } else if (p.endsWith('.css')) {
        for (const s of text.matchAll(/@import\s+(?:url\()?["']?([^"')\s;]+)/g)) want(p, s[1]);
        for (const s of text.matchAll(/url\(\s*["']?([^"')]+)["']?\s*\)/g)) want(p, s[1]);
      }
    }
  }
  return [...files].map(([path, data]) => ({ path, data }));
}

function manifestDigest(members) {
  return sha256([...members].sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0))
    .map((m) => `${sha256(m.data)}  ${m.path}\n`).join(''));
}

/* An asset's bytes, as members relative to its served path (one member with path '' for a single file). */
async function resolveAsset(spec, served) {
  const dir = served.endsWith('/');
  if (spec.startsWith('npm:')) {
    const { name, version, member } = npmSpec(spec);
    const all = await npmMembers(name, version);
    if (dir !== member.endsWith('/')) fail(`\`${served}=${spec}\`: a directory is served from a directory`);
    const picked = dir ? all.filter((m) => m.path.startsWith(member)).map((m) => ({
      path: m.path.slice(member.length), data: m.data })) : all.filter((m) => m.path === member)
      .map((m) => ({ path: '', data: m.data }));
    if (!picked.length) fail(`npm ${name}@${version} has no member \`${member}\``);
    return picked;
  }
  if (spec.startsWith('pdfjs-src:')) {
    if (!dir) fail(`\`${served}=${spec}\`: a source tree is served at a directory`);
    return pdfjsTree(spec.slice('pdfjs-src:'.length));
  }
  if (spec.startsWith('https://')) {
    if (dir) fail(`\`${served}=${spec}\`: a URL is one file`);
    return [{ path: '', data: await get(spec) }];
  }
  fail(`asset spec \`${spec}\` names no known kind (npm:, pdfjs-src:, https://)`);
}

const digestOf = (members, served) => (served.endsWith('/') ? manifestDigest(members) : sha256(members[0].data));

/* Fetched once per spec into the cache, and re-hashed from the cache on every use, so a cache somebody
   edited is refused exactly as a changed upstream is. */
async function assetMembers(a) {
  const key = createHash('sha256').update(a.served.endsWith('/') + a.spec).digest('hex').slice(0, 24);
  const dir = join(CACHE, key);
  let members;
  if (existsSync(join(dir, '.complete'))) {
    members = JSON.parse(readFileSync(join(dir, '.complete'), 'utf8'))
      .map((p) => ({ path: p, data: readFileSync(join(dir, 'm', p === '' ? '.file' : p)) }));
  } else {
    members = await resolveAsset(a.spec, a.served);
    rmSync(dir, { recursive: true, force: true });
    for (const m of members) {
      const to = join(dir, 'm', m.path === '' ? '.file' : m.path);
      mkdirSync(dirname(to), { recursive: true });
      writeFileSync(to, m.data);
    }
    writeFileSync(join(dir, '.complete'), JSON.stringify(members.map((m) => m.path)));
  }
  return members;
}

async function verifiedMembers(a) {
  const members = await assetMembers(a);
  const got = digestOf(members, a.served);
  if (got !== a.digest)
    fail(`asset \`${a.served}=${a.spec}\` hashes to sha256:${got}, and the case list records ` +
         `sha256:${a.digest} — the bytes are not the version this case is about, so nothing is served`);
  return members;
}

/* ---- staging ------------------------------------------------------------------------------------------- */

const ORIGINS = ['page', 'attacker'];

/* knownbug/headers.tsv: case, origin, path pattern, header, value. */
function headerRules(path = join(HERE, 'knownbug', 'headers.tsv')) {
  const rules = [];
  let header = false;
  readFileSync(path, 'utf8').split('\n').forEach((line, i) => {
    if (!line.trim() || line.startsWith('#')) return;
    const cols = line.split('\t');
    if (!header) {
      if (cols.join(' ') !== 'case origin path header value') fail(`${path}:${i + 1}: the header row is wrong`);
      header = true;
      return;
    }
    if (cols.length !== 5 || cols.some((c) => !c)) fail(`${path}:${i + 1}: a rule has five non-empty columns`);
    if (cols[1] !== '*' && !ORIGINS.includes(cols[1])) fail(`${path}:${i + 1}: origin \`${cols[1]}\` is not one of ${ORIGINS}`);
    rules.push({ case: cols[0], origin: cols[1], path: cols[2], header: cols[3].toLowerCase(), value: cols[4] });
  });
  return rules;
}

function headersFor(rules, kase, origin, path) {
  const out = {};
  for (const r of rules) {
    if (r.case !== '*' && r.case !== kase) continue;
    if (r.origin !== '*' && r.origin !== origin) continue;
    const hit = r.path === '*' || (r.path.startsWith('*.') ? path.endsWith(r.path.slice(1)) : r.path === path);
    if (hit) out[r.header] = r.value;
  }
  return out;
}

/* A variant is staged as two origins' files: `page`, the document's own site, and `attacker`, a second
   origin for an asset whose served path carries the `attacker:` prefix. `served.json` beside them is the whole
   of what is served: each file's path, sha256 and response headers. */
async function stage(row, variant) {
  const base = join(WORK, 'stage', row.case, variant);
  rmSync(base, { recursive: true, force: true });
  const files = { page: new Map(), attacker: new Map() };
  if (row.harness !== '-') files.page.set('index.html', readFileSync(join(HERE, 'knownbug', row.harness)));
  for (const a of row.assets[variant]) {
    const origin = a.served.startsWith('attacker:') ? 'attacker' : 'page';
    const served = origin === 'attacker' ? a.served.slice('attacker:'.length) : a.served;
    for (const m of await verifiedMembers(a)) {
      const p = posix.normalize(served.endsWith('/') ? posix.join(served, m.path) : served);
      if (files[origin].has(p)) fail(`${row.case}:${variant} serves ${origin} path ${p} from two assets`);
      files[origin].set(p, m.data);
    }
  }
  const rules = headerRules();
  const manifest = {};
  for (const origin of ORIGINS) {
    manifest[origin] = [];
    for (const [p, data] of [...files[origin]].sort()) {
      const to = join(base, origin, p);
      mkdirSync(dirname(to), { recursive: true });
      writeFileSync(to, data);
      manifest[origin].push({ path: p, sha256: sha256(data), headers: headersFor(rules, row.case, origin, p) });
    }
  }
  writeFileSync(join(base, 'served.json'), JSON.stringify(manifest, null, 1));
  return { base, manifest };
}

/* ---- running ------------------------------------------------------------------------------------------- */

/* One origin on loopback, serving exactly the staged manifest: a listed path answers 200 with its listed
   headers, anything else 404. Every request is logged with the request headers a CORS reading needs. */
async function serveOrigin(base, origin, entries) {
  const byPath = new Map(entries.map((e) => [e.path, e]));
  const log = [];
  const srv = createHttpServer((req, res) => {
    const path = posix.normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^\/+/, '');
    const e = byPath.get(path);
    log.push(`${req.method} ${req.url} ${e ? 200 : 404} origin=${req.headers.origin ?? '-'}` +
             ` range=${req.headers.range ?? '-'}`);
    if (!e) { res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' }); res.end('not found'); return; }
    res.writeHead(200, e.headers);
    res.end(req.method === 'HEAD' ? undefined : readFileSync(join(base, origin, path)));
  });
  await new Promise((r) => srv.listen(0, '127.0.0.1', r));
  return { port: srv.address().port, log: () => log, stop: () => new Promise((r) => srv.close(r)) };
}

/* The zone's stderr, read as lines as it arrives: a productive run echoes a growing @RESULT document per
   census sample, which is far too much to hold. */
function lineReader(onLine) {
  let tail = '';
  return {
    push(chunk) {
      tail += chunk;
      let nl;
      while ((nl = tail.indexOf('\n')) >= 0) { onLine(tail.slice(0, nl)); tail = tail.slice(nl + 1); }
    },
    end() { if (tail) onLine(tail); tail = ''; },
  };
}

const fired = (s) => !('search' in s);

async function runOne(row, variant, opts) {
  const { base, manifest } = await stage(row, variant);
  const page = await serveOrigin(base, 'page', manifest.page);
  const attacker = await serveOrigin(base, 'attacker', manifest.attacker);
  const url = `http://127.0.0.1:${page.port}/` +
              row.entry.replaceAll('{attacker}', `http://127.0.0.1:${attacker.port}`);
  const zone = join(opts.root, 'engine', 'trusted.mjs');
  const stem = join(WORK, 'logs', `${row.case}.${variant}`);
  mkdirSync(dirname(stem), { recursive: true });
  /* The log keeps every line verbatim except @RESULT, which it records by size; the last @RESULT per
     instance is kept whole and written beside the log. */
  const log = createWriteStream(`${stem}.log`);
  log.write(`# ${url}\n# zone ${zone}\n# bin ${opts.bin}\n`);
  const last = new Map();
  const aborts = [];
  const reader = lineReader((line) => {
    const m = line.match(/^\[trusted\] \[([^\]]+)\] @RESULT#(\d+) @RESULT (.*)$/);
    if (m) {
      last.set(m[1], { n: +m[2], text: m[3] });
      log.write(`[trusted] [${m[1]}] @RESULT#${m[2]} (${m[3].length} bytes)\n`);
      return;
    }
    if (opts.abortRecord(line) !== null) aborts.push(line);
    log.write(line + '\n');
  });
  const started = Date.now();
  const child = spawn(process.execPath, [zone, url, opts.bin], { cwd: opts.root, detached: true,
                                                                stdio: ['ignore', 'ignore', 'pipe'] });
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (d) => reader.push(d));
  let backstop = false;
  /* The child leads its own process group, so the zone and every engine it spawned stop together. */
  const timer = setTimeout(() => { backstop = true; try { process.kill(-child.pid, 'SIGTERM'); } catch {} },
                           opts.budget * 1000);
  const end = await new Promise((res) => child.on('close', (code, signal) => res({ code, signal })));
  clearTimeout(timer);
  reader.end();
  try { process.kill(-child.pid, 'SIGKILL'); } catch {}
  await page.stop();
  await attacker.stop();
  log.write(`# ---- page origin requests\n${page.log().join('\n')}\n# ---- attacker origin requests\n` +
            `${attacker.log().join('\n')}\n`);
  await new Promise((r) => log.end(r));
  const rootEntry = last.size ? [...last.values()][0] : null;
  if (rootEntry) writeFileSync(`${stem}.result.json`, rootEntry.text);
  const root = rootEntry ? JSON.parse(rootEntry.text) : null;
  const sinks = root ? root.securitySinks : null;
  const num = (k) => (root && Number.isInteger(root[k]) ? root[k] : null);
  return {
    case: row.case, variant, version: row[variant], advisory: row.advisory, source: row.source, sink: row.sink,
    expected: variant === 'vulnerable' ? row.expected : 'not fired',
    /* A signal this driver did not send (the OOM killer, say) ended the zone from outside. */
    url, ended: backstop ? 'backstop' : end.signal ? `signal ${end.signal}` : 'zone-exit', exit: end.code, signal: end.signal,
    seconds: Math.round((Date.now() - started) / 1000), budget: opts.budget,
    results: rootEntry ? rootEntry.n : 0,
    sourceReads: num('_sourceReads'), sinkReached: num('_sinkReached'), sinkTainted: num('_sinkTainted'),
    candidates: num('_candidates'), flows: num('_flows'),
    reached: root ? num('_sinkReached') > 0 : null,
    tainted: root ? num('_sinkTainted') > 0 : null,
    candidate: root ? (num('_candidates') > 0 || sinks.some((s) => fired(s) || s.tried > 0)) : null,
    fired: root ? sinks.some(fired) : null,
    pocs: sinks ? sinks.filter(fired).map((s) => ({ sink: s.sink, source: s.source, poc: s.poc })) : null,
    parked: sinks ? sinks.filter((s) => !fired(s)).map((s) => ({ sink: s.sink, source: s.source, tried: s.tried,
      reached: s.reached, substituted: s.substituted, sinkStrings: s.sinkStrings })) : null,
    pageErrors: root ? root.pageErrors : null,
    attackerServed: manifest.attacker, attackerRequests: attacker.log(),
    /* The backstop's own SIGTERM is this driver's act, not a crash; an abort record still is one. */
    abort: aborts[0] ?? null,
    crash: opts.crashIdentity(aborts.join('\n'), backstop && end.signal === 'SIGTERM' ? null : end.signal),
    log: relative(REPO, `${stem}.log`), served: relative(REPO, join(base, 'served.json')),
  };
}

function verdict(r) {
  if (r.fired === null) return 'no-result';
  return r.fired ? 'FIRED' : 'not fired';
}

function table(rows) {
  const head = ['case', 'variant', 'version', 'ended', 'reached', 'tainted', 'candidate', 'verdict', 'expected',
                'abort'];
  const cell = (v) => (v === null || v === undefined ? '-' : v === true ? 'yes' : v === false ? 'no' : String(v));
  const body = rows.map((r) => [r.case, r.variant, r.version, r.ended, r.reached, r.tainted, r.candidate,
                                verdict(r), r.expected, r.crash ?? ''].map(cell));
  const w = head.map((h, i) => Math.max(h.length, ...body.map((b) => b[i].length)));
  const fmt = (cols) => '| ' + cols.map((c, i) => c.padEnd(w[i])).join(' | ') + ' |';
  return [fmt(head), '|' + w.map((n) => '-'.repeat(n + 2)).join('|') + '|', ...body.map(fmt)].join('\n');
}

/* ---- commands ------------------------------------------------------------------------------------------ */

function pick(rows, args) {
  if (!args.length) return rows.flatMap((r) => VARIANTS.map((v) => [r, v]));
  return args.flatMap((a) => {
    const [c, v] = a.split(':');
    const row = rows.find((r) => r.case === c) ?? fail(`no case \`${c}\` in knownbugs.tsv`);
    if (v && !VARIANTS.includes(v)) fail(`variant \`${v}\` is not one of ${VARIANTS.join(', ')}`);
    return (v ? [v] : VARIANTS).map((x) => [row, x]);
  });
}

async function main(argv) {
  const [cmd, ...rest] = argv;
  const rows = caseList();
  if (cmd === 'pin') {
    for (const [row, v] of pick(rows, rest)) {
      for (const a of row.assets[v]) {
        const got = digestOf(await assetMembers(a), a.served);
        console.log(`${got === a.digest ? 'ok  ' : 'DIFF'} ${row.case}:${v} ${a.served}=${a.spec}@sha256:${got}`);
      }
    }
    return;
  }
  if (cmd === 'stage') {
    const [[row, v]] = pick(rows, [`${rest[0]}:${rest[1]}`]);
    console.log((await stage(row, v)).base);
    return;
  }
  if (cmd === 'run') {
    const opts = { budget: 300, jobs: 1, bin: null };
    const names = [];
    for (let i = 0; i < rest.length; i++) {
      if (rest[i] === '--bin') opts.bin = resolve(rest[++i]);
      else if (rest[i] === '--budget') opts.budget = +rest[++i];
      else if (rest[i] === '--jobs') opts.jobs = +rest[++i];
      else names.push(rest[i]);
    }
    if (!opts.bin || !existsSync(opts.bin)) fail('`run` needs --bin <path to qjs-native-none>');
    if (!(opts.budget > 0) || !(opts.jobs >= 1)) fail('--budget and --jobs take positive numbers');
    /* The zone that drives the binary is the one in the same tree: <root>/engine/host/out/qjs-native-none. */
    opts.root = resolve(dirname(opts.bin), '..', '..', '..');
    if (!existsSync(join(opts.root, 'engine', 'trusted.mjs')))
      fail(`${opts.bin} does not sit in a tree with engine/trusted.mjs beside it`);
    const ar = await import(pathToFileURL(join(opts.root, 'engine', 'abort_record.mjs')).href);
    opts.abortRecord = ar.abortRecord;
    opts.crashIdentity = ar.crashIdentity;
    const todo = pick(rows, names);
    const out = new Array(todo.length);
    const resultsFile = join(WORK, 'results.jsonl');
    mkdirSync(WORK, { recursive: true });
    let next = 0;
    await Promise.all(Array.from({ length: Math.min(opts.jobs, todo.length) }, async () => {
      while (next < todo.length) {
        const i = next++;
        const [row, v] = todo[i];
        out[i] = await runOne(row, v, opts);
        const line = JSON.stringify(out[i]);
        console.log(line);
        writeFileSync(resultsFile, line + '\n', { flag: 'a' });
      }
    }));
    console.log('\n' + table(out));
    return;
  }
  fail('usage: knownbug.mjs pin [case...] | stage <case> <variant> | run --bin <path> [--budget s] [--jobs n] [case[:variant]...]');
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  await main(process.argv.slice(2));
