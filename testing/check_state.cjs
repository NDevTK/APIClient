// Quick state dump: connect to the running harness, find any github
// tab, dump its DIAG_TAB + endpoint list.
"use strict";
const fs = require("fs");
const path = require("path");
const puppeteer = require("puppeteer");
const LOCK = path.resolve(__dirname, "harness.lock");
(async () => {
  const h = JSON.parse(fs.readFileSync(LOCK, "utf8"));
  const browser = await puppeteer.connect({
    browserURL: `http://127.0.0.1:${h.port}`,
    defaultViewport: null,
    targetFilter: (t) => t.type() !== "browser",
    protocolTimeout: 60000,
  });
  const p = await browser.newPage();
  await p.goto(`chrome-extension://${h.extId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 15000 });
  const allGh = await p.evaluate(async () => new Promise(r => chrome.tabs.query({}, ts => r(ts.filter(x => x.url && x.url.startsWith("https://github.com/")).map(x => ({ id: x.id, url: x.url }))))));
  console.log("github tabs:", JSON.stringify(allGh));
  for (const t of allGh) {
    const d = await p.evaluate(async (tid) => new Promise(r => chrome.runtime.sendMessage({ type: "DIAG_TAB", tabId: tid }, x => r(x))), t.id);
    console.log(`-- tab ${t.id} DIAG --`);
    console.log(JSON.stringify(d, null, 2));
    const st = await p.evaluate(async (tid) => new Promise(r => chrome.runtime.sendMessage({ type: "GET_STATE", tabId: tid }, x => r(x))), t.id);
    const eps = Array.isArray(st?.endpoints) ? st.endpoints : (st?.endpoints ? Object.values(st.endpoints) : []);
    console.log(`-- tab ${t.id} endpoints (${eps.length}) --`);
    for (const e of eps.slice(0, 20)) console.log(`  ${e.method || "?"} ${e.url || e.path || "?"}  source=${e.source}`);
    const sf = Array.isArray(st?.securityFindings) ? st.securityFindings : [];
    console.log(`-- tab ${t.id} security findings (${sf.length}) --`);
    for (const f of sf.slice(0, 5)) {
      const sinks = f.securitySinks || [];
      for (const s of sinks.slice(0, 3)) console.log(`  ${s.type} sink=${s.sink} verdict=${s.verdict}`);
    }
  }
  await p.close();
  browser.disconnect();
})();
