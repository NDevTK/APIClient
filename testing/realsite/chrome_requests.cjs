/* The oracle half of the real-site request diff: what real Chromium requests when it loads a page logged out.
 *   flock /tmp/apiclient-frozen/slots/bigmem node testing/realsite/chrome_requests.cjs <out-dir> <loads> <id> <url>
 * (a load of a large app holds several hundred MiB, so it runs under the machine-wide heavy-run lock).
 * Each load is a fresh browser context (no cookies, storage or cache), one navigation, then a wait for network
 * quiet: QUIET_MS without a request starting or finishing, cut at MAX_MS (a backstop, reported as `endedBy`).
 * A page that polls never goes quiet, so its window is MAX_MS; a load whose renderer died is marked `crashed`.
 * Nothing is clicked, typed or submitted. One JSON file per load, `<out-dir>/<id>/chrome-<n>.json`, holding request
 * metadata only, never a body. A request is keyed by its method and identity.cjs's identity of its URL. */
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { chromium } = require('playwright');
const { describeUrl } = require('./identity.cjs');

const [outDir, loadsArg, id, url] = process.argv.slice(2);
const loads = Number(loadsArg);
if (!outDir || !(loads > 0) || !/^[a-z0-9-]+$/.test(id || '') || !URL.canParse(url || '')) {
  console.error('usage: node testing/realsite/chrome_requests.cjs <out-dir> <loads> <id> <url>');
  process.exit(2);
}
const QUIET_MS = Number(process.env.QUIET_MS || 8000);
const MAX_MS = Number(process.env.MAX_MS || 120000);

function initiatorOf(i) {
  if (!i) return null;
  const top = i.stack && i.stack.callFrames && i.stack.callFrames[0];
  const at = top ? describeUrl(top.url).identity + ':' + top.lineNumber + ':' + top.columnNumber
           : i.url ? describeUrl(i.url).identity + (i.lineNumber !== undefined ? ':' + i.lineNumber : '') : null;
  return { type: i.type, at, fn: top ? top.functionName : null };
}

async function oneLoad(browser, n) {
  const context = await browser.newContext();
  const page = await context.newPage();
  const cdp = await context.newCDPSession(page);
  await cdp.send('Network.enable');
  /* The main-frame target's own account of who asked: keyed by method and full URL, in arrival order. */
  const initiators = new Map();
  cdp.on('Network.requestWillBeSent', (e) => {
    const k = e.request.method + ' ' + e.request.url;
    if (!initiators.has(k)) initiators.set(k, []);
    initiators.get(k).push(initiatorOf(e.initiator));
  });

  const t0 = Date.now();
  const requests = [];
  const errors = [];
  const sockets = [];
  let last = t0, inflight = 0;
  context.on('request', (r) => {
    last = Date.now(); inflight++;
    let frameUrl = null, mainFrame = null;
    try { const f = r.frame(); frameUrl = f.url(); mainFrame = f === page.mainFrame(); } catch { /* a worker's request has no frame */ }
    requests.push({ order: requests.length, atMs: last - t0, method: r.method(), raw: r.url(),
                    resourceType: r.resourceType(), navigation: r.isNavigationRequest(),
                    serviceWorker: r.serviceWorker() !== null, frameUrl, mainFrame,
                    redirectedFrom: r.redirectedFrom() ? r.redirectedFrom().url() : null,
                    hasBody: r.postDataBuffer() !== null, _r: r });
  });
  const settle = (r, how) => {
    last = Date.now(); inflight--;
    const row = requests.find((x) => x._r === r);
    if (row) row.outcome = how;
  };
  context.on('requestfinished', (r) => settle(r, 'finished'));
  context.on('requestfailed', (r) => settle(r, 'failed: ' + (r.failure() ? r.failure().errorText : '?')));
  context.on('response', (resp) => {
    const row = requests.find((x) => x._r === resp.request());
    if (row) { row.status = resp.status(); row.contentType = resp.headers()['content-type'] || null; }
  });
  page.on('pageerror', (e) => errors.push({ kind: 'pageerror', atMs: Date.now() - t0, text: String(e && e.stack || e).slice(0, 600) }));
  page.on('console', (m) => { if (m.type() === 'error') errors.push({ kind: 'console.error', atMs: Date.now() - t0, text: m.text().slice(0, 600) }); });
  let crashed = false;
  page.on('crash', () => { crashed = true; });
  page.on('websocket', (ws) => { last = Date.now(); sockets.push({ atMs: last - t0, url: describeUrl(ws.url()) }); });

  let nav;
  try {
    const resp = await page.goto(url, { waitUntil: 'load', timeout: MAX_MS });
    nav = resp ? { status: resp.status(), finalUrl: describeUrl(page.url()).identity } : { status: null };
  } catch (e) { nav = { error: String(e && e.message || e).split('\n')[0] }; }
  const loadMs = Date.now() - t0;
  let endedBy = 'quiet';
  for (;;) {
    const now = Date.now();
    if (crashed) { endedBy = 'crash'; break; }
    if (now - last >= QUIET_MS) break;
    if (now - t0 >= MAX_MS) { endedBy = 'backstop'; break; }
    await new Promise((ok) => setTimeout(ok, 250));
  }
  const docOrigin = (() => { try { return new URL(page.url()).origin; } catch { return null; } })();
  const scripts = await page.evaluate(() => ({
    title: document.title.slice(0, 80),
    scriptSrc: Array.from(document.querySelectorAll('script[src]')).map((s) => s.src),
    inlineScripts: document.querySelectorAll('script:not([src])').length,
    frames: Array.from(document.querySelectorAll('iframe')).map((f) => f.src),
    serviceWorker: !!(navigator.serviceWorker && navigator.serviceWorker.controller),
  })).catch((e) => ({ evalThrew: String(e && e.message || e).split('\n')[0] }));
  const cursor = new Map();
  const rows = requests.map((x) => {
    const d = describeUrl(x.raw);
    const k = x.method + ' ' + x.raw;
    const list = initiators.get(k);
    const ix = cursor.get(k) || 0; cursor.set(k, ix + 1);
    return { order: x.order, atMs: x.atMs, method: x.method, identity: d.identity, origin: d.origin, path: d.path,
             query: d.query, resourceType: x.resourceType, navigation: x.navigation, serviceWorker: x.serviceWorker,
             mainFrame: x.mainFrame, frame: x.frameUrl ? describeUrl(x.frameUrl).identity : null,
             sameOriginToDocument: d.origin !== null && d.origin === docOrigin,
             redirectedFrom: x.redirectedFrom ? describeUrl(x.redirectedFrom).identity : null,
             /* Absent when the main-frame target did not see the request (another frame's process, a worker). */
             initiator: list && list[ix] ? list[ix] : null,
             hasBody: x.hasBody, status: x.status === undefined ? null : x.status,
             contentType: x.contentType === undefined ? null : x.contentType,
             outcome: x.outcome === undefined ? 'pending at the cut' : x.outcome };
  });
  await context.close();
  return { id, url, load: n, started: new Date(t0).toISOString(), nav, loadMs, endedBy, crashed, quietMs: QUIET_MS,
           maxMs: MAX_MS, windowMs: Date.now() - t0, inflightAtCut: inflight, docOrigin,
           load1: os.loadavg()[0], document: scripts, sockets, errors, requests: rows };
}

(async () => {
  const dir = path.join(outDir, id);
  fs.mkdirSync(dir, { recursive: true });
  const browser = await chromium.launch();
  const version = browser.version();
  try {
    for (let n = 0; n < loads; n++) {
      const r = await oneLoad(browser, n);
      r.chromium = version;
      fs.writeFileSync(path.join(dir, `chrome-${n}.json`), JSON.stringify(r, null, 1));
      console.log(`${id} load ${n}: nav=${JSON.stringify(r.nav)} requests=${r.requests.length} sockets=${r.sockets.length} ` +
                  `errors=${r.errors.length} ended=${r.endedBy} window=${r.windowMs}ms load1=${r.load1.toFixed(2)}`);
    }
  } finally { await browser.close(); }
})().catch((e) => { console.error(e && e.stack || e); process.exit(1); });
