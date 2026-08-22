// testing/discovery_diff.cjs — the discovery DIFF path, which nothing else can reach here.
//
// `buildDiscoveryUrls` (lib/discovery.js) emits `https://<hostname><path>` — scheme fixed, port dropped —
// which is right for the Google APIs it names and means a fixture on http://127.0.0.1:8791 can never be a
// candidate. So this SUBSTITUTES that one function for the duration of the run, and nothing else: the
// substitution is stated here rather than hidden, because everything downstream of it — the fetch through the
// page relay, the JSON parse, `convertOpenApiToDiscovery`, `_diffDiscoveryDocs`, the `globalStore
// .discoveryChanges` record and the IndexedDB round trip — is the code under test, unmodified.
//
// The fixture serves a DIFFERENT document on its second GET (one method added), so the second fetch has a
// real difference to report. Without two servings there is nothing for the differ to find and a green result
// would mean only that it ran.
"use strict";
const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");

const LOCK = process.env.HARNESS_LOCK ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const FIX = "http://127.0.0.1:" + (process.env.FIX_PORT || "8791");
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

  // Arm first, navigate second — the document must be registered before fetchDiscoveryForService can derive
  // a tab id from it, and it is dropped again a few seconds after its analysis is reviewed.
  const armed = off.evaluate(async (fix) => {
    let version = 0;   // the candidate list this run substitutes; the second fetch asks for the SECOND document
    self.buildDiscoveryUrls = () => [{ url: fix + "/openapi.json" + (version ? "?v2" : ""), headers: {} }];
    /* THE DOCUMENT MUST BE THE ONE THIS RUN NAVIGATED, matched by its own url. Taking the last entry took a
       document from an earlier navigation that state.docs still held: its tab id was real, its documentId
       named a document the browser no longer has, and every relayed fetch failed before reaching the network
       — a false red that looked exactly like a broken candidate list. */
    let d = null;
    for (let i = 0; i < 300 && !d; i++) {
      d = [...state.docs.values()].find((x) => x && typeof x.url === "string" && x.url.indexOf("diff=") >= 0) || null;
      if (!d) await new Promise((r) => setTimeout(r, 100));
    }
    if (!d) return { err: "the navigated document was never registered in state.docs within 30s" };
    const svc = "difffixture";
    const readback = () => ({
      status: (globalStore.discoveryDocs.get(svc) || {}).status || null,
      methods: Object.keys(((globalStore.discoveryDocs.get(svc) || {}).doc || {}).resources
                 ? Object.assign({}, ...Object.values((globalStore.discoveryDocs.get(svc).doc.resources)).map((b) => b.methods || {}))
                 : {}),
      changes: (globalStore.discoveryChanges.get(svc) || []).map((c) => ({ at: c.timestamp, url: c.fetchUrl, changes: c.changes })),
    });
    await fetchDiscoveryForService(d.documentId, svc, "127.0.0.1", []);
    const first = readback();
    version = 1;
    await fetchDiscoveryForService(d.documentId, svc, "127.0.0.1", []);
    const second = readback();
    // …and through IndexedDB, the way a browser restart reads it.
    await saveGlobalStore();
    globalStore.discoveryChanges.clear();
    await loadGlobalStore();
    const afterReload = (globalStore.discoveryChanges.get(svc) || []).map((c) => c.changes);
    return { documentId: d.documentId, tabId: d.tabId, first, second, afterReload };
  }, FIX);

  const pages = await browser.pages();
  const page = pages.find((p) => !p.url().startsWith("chrome-extension://") && !p.url().startsWith("devtools://"))
            || await browser.newPage();
  const r = await page.goto(FIX + "/?diff=" + Date.now() + "&delay=600000", { waitUntil: "domcontentloaded", timeout: 60000 });
  console.log("goto " + page.url() + " [" + (r && r.status()) + "]");
  await page.bringToFront();

  console.log(JSON.stringify(await armed, null, 1));
  browser.disconnect();
})().catch((e) => { console.error(e.stack || e); process.exit(1); });
