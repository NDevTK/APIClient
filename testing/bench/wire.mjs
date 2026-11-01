/* The benchmark's wire: a recorded stand-in for the platform fetch engine/trusted.mjs hands its zone.
 *   BENCH_WIRE=record|replay BENCH_STORE=<dir> BENCH_PAGE=<id> [BENCH_WIRE_LOG=<file>]
 *     node --import testing/bench/wire.mjs engine/trusted.mjs <url> <binary>
 * It replaces the main realm's `fetch`, the one call speakAsHost makes for every send extension/lib/safe-fetch.js
 * lets out, so the policy, redirects (each hop is its own send), CORS, cache and coalescing all still run.
 * A send is keyed by (method, URL without fragment, sha256 of its body). `record` answers a recorded key from
 * the store and fetches and stores any other. `replay` never reaches the network: an unrecorded key is answered
 * as Node's fetch answers a network failure (TypeError `fetch failed` with a cause), so the zone meets it on its
 * real path, and it is logged as a miss. A network error met while recording is replayed the same way.
 * Store: `<store>/blobs/<sha256>.gz` holds each body once; `<store>/pages/<page>/<sha256(key)>.json` one
 * reply's head and body digest. The log gets one JSON line per send: {wire, hit, method, url, status|error}. */

import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync, appendFileSync, renameSync } from 'node:fs';
import { gzipSync, gunzipSync } from 'node:zlib';
import { join } from 'node:path';

const MODE = process.env.BENCH_WIRE;
const STORE = process.env.BENCH_STORE;
const PAGE = process.env.BENCH_PAGE;
const LOG = process.env.BENCH_WIRE_LOG || null;
if (MODE !== 'record' && MODE !== 'replay')
  throw new Error(`testing/bench/wire.mjs: BENCH_WIRE must be record or replay, got ${JSON.stringify(MODE)}`);
if (!STORE || !PAGE || !/^[a-z0-9-]+$/.test(PAGE))
  throw new Error('testing/bench/wire.mjs: BENCH_STORE (a directory) and BENCH_PAGE ([a-z0-9-]+) are required');

const BLOBS = join(STORE, 'blobs');
const ENTRIES = join(STORE, 'pages', PAGE);
if (MODE === 'record') { mkdirSync(BLOBS, { recursive: true }); mkdirSync(ENTRIES, { recursive: true }); }
else if (!existsSync(ENTRIES)) throw new Error(`testing/bench/wire.mjs: no recording of ${PAGE} under ${STORE}`);

const sha256 = (b) => createHash('sha256').update(b).digest('hex');
const platformFetch = globalThis.fetch;

function bodyBytes(body) {
  if (body === undefined || body === null) return null;
  if (typeof body === 'string') return Buffer.from(body, 'utf8');
  if (ArrayBuffer.isView(body)) return Buffer.from(body.buffer, body.byteOffset, body.byteLength);
  if (body instanceof ArrayBuffer) return Buffer.from(body);
  throw new Error(`testing/bench/wire.mjs: a send carried a body of a kind the zone does not send: ${typeof body}`);
}

function keyOf(input, init) {
  const u = new URL(String(input));
  u.hash = '';
  const method = (init && typeof init.method === 'string') ? init.method.toUpperCase() : 'GET';
  const b = bodyBytes(init ? init.body : undefined);
  return { method, url: u.href, bodySha: b === null ? null : sha256(b) };
}
const entryPath = (k) => join(ENTRIES, sha256(JSON.stringify([k.method, k.url, k.bodySha])) + '.json');

function writeAtomic(path, data) {
  const tmp = path + '.' + process.pid + '.tmp';
  writeFileSync(tmp, data);
  renameSync(tmp, path);
}

function log(row) { if (LOG !== null) appendFileSync(LOG, JSON.stringify(row) + '\n'); }

/* Fetch §2.2.3 "Statuses"' null body statuses, whose Response has no body. */
const NULL_BODY = new Set([101, 103, 204, 205, 304]);

function answer(entry) {
  if (entry.error !== undefined) throw new TypeError('fetch failed', { cause: new Error(entry.error) });
  const body = entry.body === null || NULL_BODY.has(entry.status) ? null
    : gunzipSync(readFileSync(join(BLOBS, entry.body + '.gz')));
  if (body !== null && sha256(body) !== entry.body)
    throw new Error(`testing/bench/wire.mjs: blob ${entry.body} does not hash to its name — the store is damaged`);
  return new Response(body, { status: entry.status, statusText: entry.statusText, headers: entry.headers });
}

async function recordLive(k, input, init) {
  const recordedAt = new Date().toISOString();
  let entry;
  try {
    const resp = await platformFetch(input, init);
    const bytes = NULL_BODY.has(resp.status) || resp.body === null ? null : Buffer.from(await resp.arrayBuffer());
    let body = null;
    if (bytes !== null) {
      body = sha256(bytes);
      const p = join(BLOBS, body + '.gz');
      if (!existsSync(p)) writeAtomic(p, gzipSync(bytes, { level: 9 }));
    }
    entry = { key: k, recordedAt, status: resp.status, statusText: resp.statusText, headers: [...resp.headers],
              body, bytes: bytes === null ? 0 : bytes.length };
  } catch (e) {
    if (e && e.name === 'AbortError') throw e;
    entry = { key: k, recordedAt, error: String(e && e.cause ? e.cause.message || e.cause : e) };
  }
  writeAtomic(entryPath(k), JSON.stringify(entry));
  return entry;
}

const inflight = new Map();
globalThis.fetch = async function benchWire(input, init) {
  if (init && init.signal && init.signal.aborted) throw new DOMException('This operation was aborted', 'AbortError');
  const k = keyOf(input, init);
  const p = entryPath(k);
  if (existsSync(p)) {
    const entry = JSON.parse(readFileSync(p, 'utf8'));
    log({ wire: MODE, hit: true, method: k.method, url: k.url,
          ...(entry.error !== undefined ? { error: entry.error } : { status: entry.status }) });
    return answer(entry);
  }
  if (MODE === 'replay') {
    log({ wire: MODE, hit: false, method: k.method, url: k.url, error: 'not recorded' });
    throw new TypeError('fetch failed', { cause: new Error('testing/bench/wire.mjs: not recorded') });
  }
  const id = p;
  if (!inflight.has(id)) inflight.set(id, recordLive(k, input, init).finally(() => inflight.delete(id)));
  const entry = await inflight.get(id);
  log({ wire: MODE, hit: false, method: k.method, url: k.url,
        ...(entry.error !== undefined ? { error: entry.error } : { status: entry.status }) });
  return answer(entry);
};
