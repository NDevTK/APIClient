// testing/popup_handlers.cjs — the four on-demand commands, driven from the POPUP, against a LIVE document.
//
// They are posted with chrome.runtime.sendMessage from popup.html's own context because that is the surface a
// user reaches them through — and no button in popup.html sends any of them, so this is currently the ONLY
// way they can be reached at all.
//
// The document must be REGISTERED when the message arrives: each handler resolves `_docFromMsg` and answers
// null without it, so a run that asks too early or too late measures the clock rather than the handler. The
// popup is opened first, the page is navigated second, and the offscreen is polled for the document this run
// navigated (by its own url) before anything is posted.
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

  const popupUrl = `chrome-extension://${lock.extId}/popup.html`;
  let popup = (await browser.pages()).find((p) => p.url().startsWith(popupUrl));
  if (!popup) { popup = await browser.newPage(); await popup.goto(popupUrl, { waitUntil: "domcontentloaded", timeout: 20000 }); await sleep(1000); }
  const popErrs = [];
  popup.on("pageerror", (e) => popErrs.push(String(e && e.message || e)));

  const tag = "ph" + Date.now();
  const armed = off.evaluate(async (want) => {
    let d = null;
    for (let i = 0; i < 400 && !d; i++) {
      d = [...state.docs.values()].find((x) => x && typeof x.url === "string" && x.url.indexOf(want) >= 0) || null;
      if (!d) await new Promise((r) => setTimeout(r, 100));
    }
    if (!d) return { err: "the navigated document was never registered within 40s" };
    return { documentId: d.documentId, tabId: d.tabId, url: d.url,
             endpoints: [...d.endpoints.entries()].map(([k, v]) => [k, v.method, v.url]) };
  }, tag);

  const pages = await browser.pages();
  const page = pages.find((p) => !p.url().startsWith("chrome-extension://") && !p.url().startsWith("devtools://"))
            || await browser.newPage();
  await page.bringToFront();
  const r = await page.goto(FIX + "/?" + tag + "=1&delay=1500", { waitUntil: "domcontentloaded", timeout: 60000 });
  console.log("goto " + page.url() + " [" + (r && r.status()) + "]");
  const ctx = await armed;
  console.log("live document: " + JSON.stringify(ctx));
  if (ctx.err) { browser.disconnect(); return; }

  const send = (m) => popup.evaluate((msg) => new Promise((res) => {
    const to = setTimeout(() => res({ __timeout_40s: true }), 40000);
    try { chrome.runtime.sendMessage(msg, (rr) => { clearTimeout(to); res({ r: rr === undefined ? "(undefined)" : rr, lastError: chrome.runtime.lastError ? chrome.runtime.lastError.message : null }); }); }
    catch (e) { clearTimeout(to); res({ threw: String(e && e.message || e) }); }
  }), m);

  const epKey = (ctx.endpoints[0] || [])[0] || null;
  const msgs = [
    { type: "GET_DISCOVERY_CHANGES", tabId: ctx.tabId, documentId: ctx.documentId },
    // Hypothesis 1: no hostname in the message and no endpoint to take one from ⇒ null, never an invented
    // `${service}.googleapis.com`.
    { type: "FETCH_DISCOVERY", tabId: ctx.tabId, documentId: ctx.documentId, service: "nosuchservice" },
    { type: "FETCH_DISCOVERY", tabId: ctx.tabId, documentId: ctx.documentId, service: "127.0.0.1", hostname: "127.0.0.1" },
    { type: "PROBE_ENDPOINT", tabId: ctx.tabId, documentId: ctx.documentId, endpointKey: epKey },
    { type: "DISCOVER_SERVICE", tabId: ctx.tabId, documentId: ctx.documentId, endpointKey: epKey },
  ];
  for (const m of msgs) {
    const t0 = Date.now();
    const res = await send(m);
    const s = JSON.stringify(res);
    console.log(`${m.type}${m.hostname ? " hostname=" + m.hostname : (m.type === "FETCH_DISCOVERY" ? " (no hostname)" : "")}` +
                `${m.type.indexOf("PROBE") === 0 || m.type === "DISCOVER_SERVICE" ? " endpointKey=" + epKey : ""}` +
                ` [${Date.now() - t0}ms] -> ` + (s.length > 500 ? s.slice(0, 500) + "…" : s));
  }
  /* PROBE_ENDPOINT and DISCOVER_SERVICE BOTH TAKE AN ENDPOINT KEY OUT OF `tab.endpoints`, and that map is
     filled from ENGINE results alone (lib/merge.js, `source: "ast_analysis"` / `"ast_dynamic"`) — never from
     live traffic. So on a page whose analysis produced nothing they answer null having done nothing, and the
     null says nothing about them. One endpoint record is SEEDED here, stated rather than hidden, so the two
     handlers run their real bodies; everything after the lookup — the key placement, the relay, the probe and
     the merge — is the code under test. */
  const seeded = await off.evaluate((docId) => {
    const d = state.docs.get(docId);
    if (!d) return { err: "document already dropped from state.docs — cannot seed" };
    const key = "SEEDED POST 127.0.0.1/v1/things:list";
    d.endpoints.set(key, {
      url: "http://127.0.0.1:8791/v1/things:list?key=AIzaSyFIXTUREKEY0000000000000000000000",
      method: "POST", host: "127.0.0.1", path: "/v1/things:list", service: "127.0.0.1",
      source: "harness_seed", apiKey: "AIzaSyFIXTUREKEY0000000000000000000000", apiKeySource: "url",
    });
    return { key };
  }, ctx.documentId);
  console.log("seeded endpoint: " + JSON.stringify(seeded));
  if (seeded.key) {
    for (const type of ["PROBE_ENDPOINT", "DISCOVER_SERVICE"]) {
      const t0 = Date.now();
      const res = await send({ type, tabId: ctx.tabId, documentId: ctx.documentId, endpointKey: seeded.key });
      const s = JSON.stringify(res);
      console.log(`${type} (seeded) [${Date.now() - t0}ms] -> ` + (s.length > 900 ? s.slice(0, 900) + "…" : s));
    }
  }

  if (popErrs.length) console.log("POPUP ERRORS: " + JSON.stringify(popErrs));
  browser.disconnect();
})().catch((e) => { console.error(e.stack || e); process.exit(1); });
