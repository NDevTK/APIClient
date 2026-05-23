// Capture github's executable bundle (islands filtered) + raw page
// HTML for native iteration. One CDP call per script so a 6 MB
// combined doesn't ride one response. No magic-number waits — polls
// the SW for the actual conditions (extension reloaded, script
// buffer settled).
"use strict";
const fs = require("fs");
const crypto = require("crypto");
const path = require("path");
const puppeteer = require("puppeteer");
const LOCK = path.resolve(__dirname, "harness.lock");
const OUT_JS = path.resolve(__dirname, "..", "engine", "qjs", "_github_combined.js");
const OUT_HTML = path.resolve(__dirname, "..", "engine", "qjs", "_github_page.html");
const OUT_PARTS = path.resolve(__dirname, "..", "engine", "qjs", "_github_parts");

async function openPopup(browser, extId) {
  const p = await browser.newPage();
  await p.goto(`chrome-extension://${extId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 15000 });
  return p;
}

async function untilSwAlive(browser, extId) {
  // SW is alive when DIAG_TAB responds. Poll until it answers.
  for (;;) {
    try {
      const p = await openPopup(browser, extId);
      const ok = await p.evaluate(() => new Promise(r => chrome.runtime.sendMessage({ type: "DIAG_TAB", tabId: -1 }, x => r(!!x))));
      await p.close().catch(() => {});
      if (ok) return;
    } catch (_) {}
    await new Promise(r => setTimeout(r, 250));
  }
}

async function untilBufferSettled(popup, tabId) {
  // Settled = the SW reports scriptBufferDebounceFired (its own
  // _bufferScript debounce has fired and _analyzeCombinedScripts was
  // called). Deterministic SW signal — no "stable for N polls"
  // heuristic, no fixed wait. Poll interval is operational (controls
  // request frequency), not analytical.
  for (;;) {
    const d = await popup.evaluate(async (tid) => {
      return await new Promise(r => chrome.runtime.sendMessage({ type: "DIAG_TAB", tabId: tid }, x => r(x || null)));
    }, tabId);
    if (d && d.scriptBufferDebounceFired) return d;
    await new Promise(r => setTimeout(r, 500));
  }
}

(async () => {
  const h = JSON.parse(fs.readFileSync(LOCK, "utf8"));
  const browser = await puppeteer.connect({
    browserURL: `http://127.0.0.1:${h.port}`,
    defaultViewport: null,
    targetFilter: (t) => t.type() !== "browser",
    protocolTimeout: 300000,
  });

  // Reload extension via popup, then poll for SW back online.
  const p0 = await openPopup(browser, h.extId);
  await p0.evaluate(() => { try { chrome.runtime.reload(); } catch (_) {} });
  await p0.close().catch(() => {});
  await untilSwAlive(browser, h.extId);

  // Open github + wait for the script buffer to fully settle (the
  // load event has fired AND no more SCRIPT_SOURCE pending AND the
  // count is stable across 2 polls).
  const tab = await browser.newPage();
  await tab.goto("https://github.com/", { waitUntil: "load", timeout: 45000 }).catch(e => console.log("goto:", e.message));

  const popup = await openPopup(browser, h.extId);
  const allGh = await popup.evaluate(async () => {
    return await new Promise(r => chrome.tabs.query({}, ts => {
      r(ts.filter(x => x.url && x.url.startsWith("https://github.com/")).map(x => x.id));
    }));
  });
  const tabId = allGh.length ? allGh[allGh.length - 1] : null;
  if (tabId == null) { console.error("no github tab"); process.exit(2); }
  console.log("github tabId =", tabId);

  const settled = await untilBufferSettled(popup, tabId);
  console.log("settled:", JSON.stringify({ count: settled.scriptBufferCount, kb: settled.scriptBufferKB, pageHtmlLen: settled.pageHtmlLen }));

  const info = await popup.evaluate(async (tid) => {
    return await new Promise(r => chrome.runtime.sendMessage({ type: "DUMP_BUNDLE_INFO", tabId: tid }, x => r(x || null)));
  }, tabId);
  if (!info || !info.scriptCount) { console.error("no scripts:", info); process.exit(3); }
  console.log(`scripts=${info.scriptCount} islands=${info.islandCount} pageHtmlLen=${info.pageHtmlLen}`);

  fs.mkdirSync(OUT_PARTS, { recursive: true });
  for (const f of fs.readdirSync(OUT_PARTS)) fs.unlinkSync(path.join(OUT_PARTS, f));
  fs.writeFileSync(OUT_JS, "");
  const fd = fs.openSync(OUT_JS, "a");
  for (let i = 0; i < info.scriptCount; i++) {
    const part = await popup.evaluate(async (tid, idx) => {
      return await new Promise(r => chrome.runtime.sendMessage({ type: "DUMP_BUNDLE_PART", tabId: tid, index: idx }, x => r(x || null)));
    }, tabId, i);
    if (!part || part.error) { console.error("part", i, ":", part); break; }
    const urlHash = crypto.createHash("sha1").update(part.url || part.code || "").digest("hex").slice(0, 12);
    const partPath = path.join(OUT_PARTS, `${String(i).padStart(3, "0")}_${urlHash}.js`);
    fs.writeFileSync(partPath, part.code || "");
    fs.writeSync(fd, `/* script ${i} url=${part.url || "(inline)"} */\n`);
    fs.writeSync(fd, part.code || "");
    fs.writeSync(fd, "\n;\n");
  }
  fs.closeSync(fd);
  console.log(`wrote ${OUT_JS} bytes=${fs.statSync(OUT_JS).size}`);
  console.log(`wrote ${OUT_PARTS}/ files=${fs.readdirSync(OUT_PARTS).length}`);

  const ph = await popup.evaluate(async (tid) => {
    return await new Promise(r => chrome.runtime.sendMessage({ type: "DUMP_PAGE_HTML", tabId: tid }, x => r(x || null)));
  }, tabId);
  if (ph && ph.html) {
    fs.writeFileSync(OUT_HTML, ph.html);
    console.log(`wrote ${OUT_HTML} bytes=${ph.html.length}`);
  }
  await popup.close();
  await tab.close();
  browser.disconnect();
})().catch(e => { console.error("FATAL", e); process.exit(99); });
