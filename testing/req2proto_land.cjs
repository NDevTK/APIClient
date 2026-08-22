// testing/req2proto_land.cjs — does what the error probe LEARNED actually land in the store?
//
// The probe returning fields is one fact and the moat holding them is another: `performProbeAndPatch` builds
// a virtual discovery document out of the rejection and merges it, while `fetchDiscoveryForService` writes a
// `not_found` for the same service when every published address fails, and the two run concurrently off one
// captured response. So this navigates once and then SAMPLES the store, printing every change — the order in
// which the two writers land is the measurement.
"use strict";
const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");

const LOCK = process.env.HARNESS_LOCK ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const FIX = "http://127.0.0.1:" + (process.env.FIX_PORT || "8791");
const DELAY = process.env.E2E_DELAY || "1500";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const lock = JSON.parse(fs.readFileSync(LOCK, "utf8"));
  const browser = await puppeteer.connect({ browserURL: "http://127.0.0.1:" + lock.port, defaultViewport: null,
                                            targetFilter: (t) => t.type() !== "browser", protocolTimeout: 120000 });
  const ourl = `chrome-extension://${lock.extId}/ast-worker.html`;
  let off = null;
  for (let i = 0; i < 40 && !off; i++) {
    const t = browser.targets().find((x) => x.url().startsWith(ourl));
    off = t ? await t.page().catch(() => null) : null;
    if (!off) await sleep(200);
  }
  if (!off) { console.log("FATAL: no offscreen"); process.exit(1); }

  const pages = await browser.pages();
  const page = pages.find((p) => !p.url().startsWith("chrome-extension://") && !p.url().startsWith("devtools://"))
            || await browser.newPage();
  const r = await page.goto(FIX + "/?land=" + Date.now() + "&delay=" + DELAY, { waitUntil: "domcontentloaded", timeout: 60000 });
  console.log("goto " + page.url() + " [" + (r && r.status()) + "]");
  await page.bringToFront();

  let prev = "";
  const t0 = Date.now();
  for (let i = 0; i < 45; i++) {
    const snap = await off.evaluate(() => JSON.stringify({
      dd: [...globalStore.discoveryDocs.entries()].map(([k, v]) => [k, v.status, v.method || null, v.isVirtual || false,
            v.doc && v.doc.resources && v.doc.resources.learned ? Object.keys(v.doc.resources.learned.methods || {}) : null]),
      probe: [...globalStore.probeResults.entries()].map(([k, v]) => [k.slice(0, 60), v && v.fieldCount, v && v.fields ? Object.keys(v.fields) : null]),
      eps: [...globalStore.endpoints.keys()].slice(0, 6),
      schemas: [...globalStore.discoveryDocs.values()].map((v) => (v.doc && v.doc.schemas) ? Object.keys(v.doc.schemas) : null),
      docs: [...state.docs.keys()].map((k) => k.slice(0, 8)),
    })).catch((e) => JSON.stringify({ evalError: String(e && e.message || e) }));
    if (snap !== prev) { console.log((Date.now() - t0) + "ms " + snap); prev = snap; }
    await sleep(500);
  }
  browser.disconnect();
})().catch((e) => { console.error(e.stack || e); process.exit(1); });
