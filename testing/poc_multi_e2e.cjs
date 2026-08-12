// End-to-end: drive the popup's actual "Run multi-step PoC" button
// against the persistent fixtures server. No profile wipe, no chrome
// kill — uses CLEAR_TAB to drop stale findings + scriptCache so the
// next analysis runs against the current worker bytes.
"use strict";
const http = require("http");
const fs = require("fs");
const path = require("path");
const puppeteer = require("puppeteer");

const HARNESS_LOCK = path.resolve(__dirname, "harness.lock");
const FIX_LOCK = path.resolve(__dirname, "fixtures.lock");

(async () => {
  const harness = JSON.parse(fs.readFileSync(HARNESS_LOCK, "utf8"));
  const fix = JSON.parse(fs.readFileSync(FIX_LOCK, "utf8"));
  const pageUrl = `http://127.0.0.1:${fix.port}/poc_multi.html`;

  const browser = await puppeteer.connect({
    browserURL: `http://127.0.0.1:${harness.port}`,
    defaultViewport: null,
    targetFilter: () => true,
    protocolTimeout: 300000,
  });

  // Attach to ANY new target opened during the run. Probe tabs may
  // start at about:blank and later navigate; rebind console handlers
  // on each framenavigated.
  const tabCreatedLog = [];
  browser.on("targetcreated", async (t) => {
    if (t.type() !== "page") return;
    const u0 = t.url();
    tabCreatedLog.push("CREATED " + (u0 || "(none)"));
    try {
      const p = await t.page();
      if (!p) return;
      p.on("console", msg => {
        const cu = p.url();
        if (cu.includes("/popup.html") || cu === "about:blank") return;
        console.log(`[probe-tab:${cu.slice(0, 70)}]`, msg.text());
      });
      p.on("pageerror", e => console.log(`[probe-tab.pageerror:${p.url().slice(0, 70)}]`, e.message));
      p.on("framenavigated", f => {
        if (f === p.mainFrame()) tabCreatedLog.push("NAV " + f.url());
      });
    } catch (_) {}
  });

  // Attach to SW console so we see orchestrator console.log/.error.
  const swTarget = (await browser.targets()).find(t => t.type() === "service_worker" && t.url().includes(harness.extId));
  if (swTarget) {
    try {
      const cdp = await swTarget.createCDPSession();
      await cdp.send("Runtime.enable");
      cdp.on("Runtime.consoleAPICalled", ev => {
        const args = ev.args.map(a => a.value || a.description || JSON.stringify(a)).join(" ");
        if (/probe|EXPLOIT|apiclientsink|__apisec|targetUrl|StructuredPlan/i.test(args)) {
          console.log(`[SW.${ev.type}]`, args);
        }
      });
    } catch (_) {}
  }

  // 1. Open popup. Trigger chrome.runtime.reload() so the SW picks up
  //    any background.js edits, then re-open the popup and click the
  //    Clear button. This is the workflow a user follows after editing
  //    extension code: reload extension via chrome://extensions, then
  //    Clear to drop stale findings.
  const popupReload = await browser.newPage();
  await popupReload.goto(`chrome-extension://${harness.extId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 15000 });
  await popupReload.evaluate(() => { try { chrome.runtime.reload(); } catch (_) {} });
  await popupReload.close().catch(() => {});
  await new Promise(r => setTimeout(r, 2500));

  const popupClear = await browser.newPage();
  await popupClear.goto(`chrome-extension://${harness.extId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 15000 });
  await popupClear.evaluate(() => {
    const b = document.getElementById("btn-clear");
    if (b) b.click();
  });
  await new Promise(r => setTimeout(r, 500));
  await popupClear.close();

  // 2. Open the target — content_script attaches → SW pipes scripts
  //    into the offscreen worker for forced-execution analysis.
  const target = await browser.newPage();
  target.on("console", msg => console.log("[target.console]", msg.text()));
  target.on("pageerror", e => console.log("[target.pageerror]", e.message));
  await target.goto(pageUrl, { waitUntil: "load", timeout: 30000 });

  // 3. Open popup; from inside, find the tabId for our target via
  //    chrome.tabs (popup has the API). Poll the SW state until a
  //    finding for THIS pageUrl shows up — that's when the worker
  //    has emitted @S/@Z/@P and the popup has the verify button.
  const popup = await browser.newPage();
  await popup.goto(`chrome-extension://${harness.extId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 15000 });
  popup.on("console", msg => console.log("[popup.console]", msg.text()));
  popup.on("pageerror", e => console.log("[popup.pageerror]", e.message));

  const tabId = await popup.evaluate(async (u) => {
    return await new Promise(resolve => chrome.tabs.query({}, ts => {
      const t = ts.find(x => x.url === u);
      resolve(t ? t.id : null);
    }));
  }, pageUrl);
  if (tabId == null) { console.error("FAIL: target tab not found"); process.exit(2); }
  console.log("target tabId =", tabId);

  // 4. Poll for the finding matching this pageUrl.
  const wantSourcePrefix = pageUrl.split("#")[0];
  const deadline = Date.now() + 120000;
  let finding = null;
  while (Date.now() < deadline) {
    const td = await popup.evaluate(async (tid) => {
      return await new Promise(resolve => chrome.runtime.sendMessage({ type: "GET_STATE", tabId: tid }, r => resolve(r)));
    }, tabId);
    const findings = Array.isArray(td?.securityFindings) ? td.securityFindings : [];
    const mine = findings.find(x => x.sourceUrl && x.sourceUrl.startsWith(wantSourcePrefix));
    if (mine && mine.securitySinks && mine.securitySinks.length) {
      finding = mine;
      break;
    }
    await new Promise(r => setTimeout(r, 2000));
  }
  if (!finding) { console.error("FAIL: no finding for", wantSourcePrefix); process.exit(3); }

  const sink = finding.securitySinks[0];
  console.log("---- Z3 witness (the model bindings) ----");
  console.log(sink.witness);
  console.log("---- Φ (path constraints) ----");
  console.log(sink.phi);
  console.log("---- verdict:", sink.verdict);
  console.log("---- poc.url:", JSON.stringify(sink.poc && sink.poc.url));
  const events = (sink.poc && sink.poc.events) || [];
  console.log("---- poc.events count:", events.length);
  for (let i = 0; i < events.length; i++) {
    console.log(`  event[${i}] leafId=${events[i].leafId} carriesPayload=${!!events[i].carriesPayload} payloadField=${events[i].payloadField || ""}`);
    console.log("    payload =", JSON.stringify(events[i].payload));
  }

  // 5. Re-render the popup so the verify button shows up, then CLICK it.
  await popup.reload({ waitUntil: "domcontentloaded" });
  // Wait until the popup has rendered the security panel + verify button.
  let clicked = false;
  for (let attempt = 0; attempt < 30 && !clicked; attempt++) {
    clicked = await popup.evaluate(() => {
      const btn = document.querySelector(".poc-run-btn");
      if (btn) { btn.scrollIntoView(); btn.click(); return true; }
      return false;
    });
    if (clicked) break;
    await new Promise(r => setTimeout(r, 1000));
  }
  if (!clicked) { console.error("FAIL: poc-run-btn never appeared in popup"); process.exit(4); }
  console.log("---- clicked .poc-run-btn ----");

  // 6. Wait for the probe to land a verdict. The popup writes the
  //    result into .probe-result; we read its className + textContent.
  const probeDeadline = Date.now() + 90000;
  let verdict = null;
  while (Date.now() < probeDeadline) {
    const state = await popup.evaluate(() => {
      const r = document.querySelector(".probe-result");
      if (!r) return null;
      const pre = r.querySelector("pre");
      return { cls: r.className, text: r.textContent, recipe: pre ? pre.textContent : null };
    });
    if (state && state.text && !/Starting|Verifying|Verifying…/.test(state.text) && state.cls && !state.cls.includes("probe-running")) {
      verdict = state;
      break;
    }
    await new Promise(r => setTimeout(r, 1000));
  }
  console.log("---- probe verdict ----");
  console.log(JSON.stringify(verdict, null, 2));
  console.log("---- tabs opened during run ----");
  for (const u of tabCreatedLog) console.log("  ", u);

  await popup.close();
  await target.close();
  browser.disconnect();
})().catch(e => { console.error("FATAL", e); process.exit(99); });
