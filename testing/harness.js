// Live harness for driving Chrome + the extension interactively.
//
// Three commands. Real interactive UI testing means driving the
// popup as a user would and observing the page DOM; nothing
// shortcuts into the service worker. Any SW behaviour the harness
// needs to verify gets driven via the popup's own chrome.runtime
// .sendMessage surface (or via popup clicks that trigger it).
//
//   start [port]              launch Chrome with the extension loaded;
//                             detached so it survives this shell.
//   page <js-expression>      run JS inside the active web page
//                             (MAIN world). Cannot read chrome.* —
//                             the page DOM is what matters.
//   popup <js-expression>     open the extension popup (if not already
//                             open) and run JS inside its document
//                             context. From the popup you can click
//                             buttons, read rendered state, and the
//                             popup itself talks to the SW.
//
// Examples:
//   node testing/harness.js start
//   node testing/harness.js page  "location.href"
//   node testing/harness.js page  "document.querySelector('h1')?.textContent"
//   node testing/harness.js popup "document.querySelectorAll('.poc-run-btn').length"
//   node testing/harness.js popup "document.querySelector('.poc-run-btn')?.click()"
//
// To stop Chrome: it survives the shell. Kill the pid from
// `testing/harness.lock` or close the window.

"use strict";

const path = require("path");
const fs = require("fs");
const fsp = fs.promises;
const http = require("http");
const crypto = require("crypto");
const { spawn, execSync } = require("child_process");
const puppeteer = require("puppeteer");

// Deterministic Chrome extension ID for an unpacked extension at the
// given absolute path. Algorithm: SHA-256 of the path bytes (UTF-16LE
// on Windows, UTF-8 on POSIX), first 32 hex chars, each hex digit
// remapped 0→a..f→p. Matches chromium/src/components/crx_file/
// id_util.cc — the SAME ID Chrome computes when --load-extension
// points at this path, so we don't have to guess which target the
// probe finds.
function computeExtensionId(absPath) {
  const buf = Buffer.from(absPath, process.platform === "win32" ? "utf16le" : "utf8");
  const hex = crypto.createHash("sha256").update(buf).digest("hex").slice(0, 32);
  let id = "";
  for (const c of hex) id += String.fromCharCode("a".charCodeAt(0) + parseInt(c, 16));
  return id;
}

const ROOT = path.resolve(__dirname, "..");
const EXT_DIR = path.join(ROOT, "extension");
const PROFILE_DIR = path.join(__dirname, "profile");
const LOCK_FILE = path.join(__dirname, "harness.lock");
const DEFAULT_PORT = 9337;

function log(...args) { console.log(...args); }
function err(...args) { console.error(...args); }
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ─── port + lock helpers ───────────────────────────────────────────────────

function pingPort(port) {
  return new Promise(resolve => {
    const req = http.get({ host: "127.0.0.1", port, path: "/json/version", timeout: 1500 }, res => {
      let body = "";
      res.on("data", c => { body += c; });
      res.on("end", () => {
        try { resolve(JSON.parse(body)); } catch { resolve(null); }
      });
    });
    req.on("error", () => resolve(null));
    req.on("timeout", () => { req.destroy(); resolve(null); });
  });
}

async function readLock() {
  try { return JSON.parse(await fsp.readFile(LOCK_FILE, "utf8")); }
  catch { return null; }
}
async function writeLock(data) { await fsp.writeFile(LOCK_FILE, JSON.stringify(data, null, 2), "utf8"); }
async function clearLock() { try { await fsp.unlink(LOCK_FILE); } catch {} }

// ─── browser attach ────────────────────────────────────────────────────────

async function connect(port) {
  const ver = await pingPort(port);
  if (!ver) throw new Error(`no browser on port ${port} — run: node testing/harness.js start`);
  return puppeteer.connect({
    browserURL: `http://127.0.0.1:${port}`,
    defaultViewport: null,
    targetFilter: (t) => t.type() !== "browser",
    protocolTimeout: 300000,
  });
}

async function withBrowser(fn) {
  const lock = await readLock();
  const port = lock?.port || DEFAULT_PORT;
  const browser = await connect(port);
  try { return await fn(browser); }
  finally { browser.disconnect(); }
}

async function getActivePage(browser) {
  const pages = await browser.pages();
  const nonExt = pages.filter(p => !p.url().startsWith("chrome-extension://") && !p.url().startsWith("devtools://"));
  if (nonExt.length === 0) throw new Error("no active web page — navigate a tab first via the browser window");
  return nonExt[nonExt.length - 1];
}

// Open the extension popup as a regular tab so puppeteer can attach
// (the real popup window in the toolbar is short-lived; opening
// popup.html in a tab gives a stable page object with the same JS +
// chrome.runtime context). Reuses an existing popup tab if present.
async function getPopupPage(browser) {
  const lock = await readLock();
  const extId = lock?.extId;
  if (!extId) throw new Error("no extId in lock file — run `start` again");
  const popupUrl = `chrome-extension://${extId}/popup.html`;
  const pages = await browser.pages();
  let popup = pages.find(p => p.url().startsWith(popupUrl));
  if (!popup) {
    popup = await browser.newPage();
    await popup.goto(popupUrl, { waitUntil: "domcontentloaded", timeout: 10000 });
    // Give the popup's own init pass (chrome.runtime.sendMessage for
    // initial state, renderSecurityPanel, etc.) a moment to settle so
    // a subsequent `popup "document.querySelector(…)"` actually finds
    // what the user would see.
    await sleep(800);
  }
  return popup;
}

// ─── commands ──────────────────────────────────────────────────────────────

// restart [port] — kill the harness Chrome and relaunch CLEAN. A running
// analysis worker re-imports `lib/qjs/qjs_worker.js` (the SINGLE_FILE wasm) and
// Chrome caches it in worker memory; chrome.runtime.reload() + bin/Clear (worker
// respawn) do NOT bust that in-memory cache, so a freshly-BUILT engine isn't
// picked up live (verified: the grind stuck at the SAME emission count pre/post
// reload = stale wasm). A new Chrome PROCESS has no in-memory cache and re-reads
// the rebuilt qjs_worker.js from disk (V8's on-disk code cache keys on file
// content, so a rebuilt file invalidates automatically — no need to wipe it).
// Also clears IndexedDB (learned-state reset). PRESERVES cookies / Login Data /
// the rest of the profile so authenticated sites stay logged in. DO NOT wipe the
// "Service Worker" or "Code Cache" dirs — deleting the SW dir breaks the
// extension's background SW startup (it then never creates the offscreen doc, so
// analysis never runs). Use after `node engine/build.mjs stage`.
async function cmdRestart(args) {
  const lock = await readLock();
  if (lock && lock.pid) {
    try {
      if (process.platform === "win32") execSync(`taskkill /F /T /PID ${lock.pid}`, { stdio: "ignore" });
      else { try { process.kill(-lock.pid, "SIGKILL"); } catch { process.kill(lock.pid, "SIGKILL"); } }
      log(`killed Chrome pid ${lock.pid}`);
    } catch (e) { log(`kill pid ${lock.pid}: ${e.message || e} (may already be gone)`); }
  }
  await clearLock();
  await sleep(1800);   // let the process group exit + release the profile's file locks
  // IndexedDB ONLY — the learned-state reset the user asked for. The new process
  // handles the engine cache; cookies/login live in other dirs and survive.
  try { await fsp.rm(path.join(PROFILE_DIR, "Default/IndexedDB"), { recursive: true, force: true }); log("cleared Default/IndexedDB"); }
  catch (e) { /* absent dir is fine */ }
  await cmdStart(args);
}

async function cmdStart(args) {
  const existing = await readLock();
  if (existing) {
    const ver = await pingPort(existing.port);
    if (ver) {
      log(`already running on port ${existing.port} (pid ${existing.pid}) — ${ver.Browser}`);
      return;
    }
    log("stale lock file, clearing");
    await clearLock();
  }

  const port = Number(args[0] || process.env.HARNESS_PORT || DEFAULT_PORT);
  log(`launching Chrome on port ${port} …`);

  // Detached process group so the launching shell exiting doesn't
  // take Chrome with it. stdio:'ignore' severs pipes; unref() means
  // node can exit without waiting on Chrome.
  const chromePath = await puppeteer.executablePath();
  const chromeArgs = [
    `--disable-extensions-except=${EXT_DIR}`,
    `--load-extension=${EXT_DIR}`,
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${PROFILE_DIR}`,
    "--no-first-run",
    "--no-default-browser-check",
  ];
  const chromeProc = spawn(chromePath, chromeArgs, {
    detached: true,
    stdio: "ignore",
    windowsHide: false,
  });
  chromeProc.unref();
  if (!chromeProc.pid) { log("failed to spawn Chrome"); process.exit(1); }

  // Wait for the debug port to be reachable.
  let ready = false;
  for (let i = 0; i < 100; i++) {
    await sleep(200);
    if (await pingPort(port)) { ready = true; break; }
  }
  if (!ready) {
    log(`Chrome launched (pid ${chromeProc.pid}) but port ${port} isn't responding`);
    process.exit(1);
  }

  // Deterministic extension ID derived from the unpacked path — same
  // algorithm Chrome uses, so we know up-front which chrome-extension://
  // ID belongs to OUR extension. Avoids the "first chrome-extension
  // target wins" bug where Chrome's own welcome / built-in extension
  // targets could be picked instead.
  const extId = computeExtensionId(EXT_DIR);

  await writeLock({ port, pid: chromeProc.pid, extId, startedAt: Date.now() });
  log(`started. chrome pid=${chromeProc.pid} port=${port} extId=${extId || "(unknown)"}`);
}

async function cmdPage(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: page <js-expression>");
  const body = /(^|\s)return\s|;|\{/.test(expr) ? expr : "return (" + expr + ");";
  await withBrowser(async (browser) => {
    const page = await getActivePage(browser);
    const out = await page.evaluate(new Function("return (async () => { " + body + " })()"));
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdPopup(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: popup <js-expression>");
  const body = /(^|\s)return\s|;|\{/.test(expr) ? expr : "return (" + expr + ");";
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    const out = await popup.evaluate(new Function("return (async () => { " + body + " })()"));
    log(JSON.stringify(out, null, 2));
  });
}

// Fire the embedded PoC sandbox the way a USER does: a real (trusted) click on
// the "Run PoC" button INSIDE poc-sandbox.html. A trusted click is the user
// activation window.open needs — a JS dispatchEvent would be untrusted and the
// PoC's window.open would be popup-blocked, so this can't be done via `popup`.
// After the click, polls the finding card's verdict (REAL EXPLOIT / NOT REPRO).
async function cmdPocRun(args) {
  const waitMs = parseInt(args[0] || "12000", 10);
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    let frame = null;
    for (let i = 0; i < 40 && !frame; i++) {
      frame = popup.frames().find((f) => f.url().indexOf("poc-sandbox.html") >= 0) || null;
      if (!frame) await sleep(150);
    }
    if (!frame) { log("no poc-sandbox frame — click 'Load PoC' in the popup first"); return; }
    await frame.waitForFunction(() => { const b = document.getElementById("run"); return b && !b.disabled; }, { timeout: 8000 }).catch(() => {});
    const pocLen = await popup.evaluate(() => { const c = document.querySelector("pre.poc-js code"); return c ? c.textContent.length : 0; });
    // The popup tab must be FOREGROUND for window.open to count as an active-tab
    // user gesture — a real toolbar popup always is; a puppeteer-opened popup tab
    // sits in the background, so window.open would be popup-blocked (returns null,
    // the PoC's own alert fires and a modal wedges the renderer). bringToFront
    // mirrors the real interaction; it is NOT a dialog workaround.
    await popup.bringToFront();
    await sleep(500);   // let the foreground/activation state settle so the click counts as an active-tab gesture (else window.open is popup-blocked on a fresh restart)
    await frame.click("#run");   // TRUSTED, FOREGROUND gesture → window.open allowed
    log("clicked Run PoC (trusted gesture); PoC length=" + pocLen);
    // Poll the popup card's verdict for up to waitMs.
    const deadline = Date.now() + waitMs;
    let verdict = "(no verdict yet)";
    while (Date.now() < deadline) {
      verdict = await popup.evaluate(() => {
        const c = document.querySelector("#security-findings .card");
        if (!c) return "(no card)";
        const hit = c.querySelector(".probe-hit-head");
        if (hit) return hit.innerText;
        const miss = c.querySelector(".probe-miss-head");
        if (miss) return miss.innerText;
        return (c.querySelector(".poc-status") || {}).innerText || "(staged)";
      });
      if (/REAL EXPLOIT|NOT REPRODUCED/.test(verdict)) break;
      await sleep(500);
    }
    log("verdict: " + verdict);
  });
}

// Navigate a real tab to a URL so the extension analyzes a live page
// end-to-end (content.js captures scripts+HTML → background → worker).
// Reuses the active non-extension tab, else opens one.
async function cmdGoto(args) {
  const url = args.join(" ").trim();
  if (!url) throw new Error("usage: goto <url>");
  await withBrowser(async (browser) => {
    let page;
    try { page = await getActivePage(browser); }
    catch (e) { page = await browser.newPage(); }
    await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
    await page.bringToFront();
    log("navigated to " + page.url());
  });
}

// Diagnostics: list CDP targets (service worker, offscreen doc, pages) so
// a wedged/slow analysis is visible — and optionally tail the service
// worker + offscreen console for a few seconds. Read-only observation of
// what's running; never queries/mutates extension state.
async function cmdDiag(args) {
  const secs = Number(args[0] || 0);
  const reload = args.includes("reload");
  await withBrowser(async (browser) => {
    const targets = browser.targets();
    log(JSON.stringify(targets.map((t) => ({ type: t.type(), url: t.url().slice(0, 90) })), null, 2));
    if (secs <= 0) return;
    const logs = [];
    const attach = (label, emitter) => emitter.on("console", (m) => logs.push(label + ": " + m.text()));
    const attachTarget = async (t) => {
      try {
        if (t.type() === "service_worker") { const w = await t.worker(); if (w) attach("SW", w); }
        else if (t.type() === "other" && t.url().startsWith("chrome-extension://") && t.url().endsWith(".js")) {
          // The analysis Web Worker (ast-thread.js) is a dedicated-worker target —
          // reach its console via t.worker(), NOT t.page(). importScripts/analysis
          // errors surface HERE, which t.page() silently missed before.
          const w = await t.worker().catch(() => null);
          if (w) attach("worker:" + t.url().split("/").pop(), w);
        }
        else if ((t.type() === "page" || t.type() === "background_page" || t.type() === "other")
                 && t.url().startsWith("chrome-extension://")) {   // extension context only — skip analyzed page noise
          const pg = await t.page().catch(() => null);
          if (pg) { attach(t.type(), pg); for (const w of pg.workers()) attach("webworker", w); pg.on("workercreated", (w) => attach("webworker", w)); }
        }
      } catch (e) {}
    };
    for (const t of targets) await attachTarget(t);
    browser.on("targetcreated", attachTarget);   // catch the MV3 SW + offscreen worker as they wake
    if (reload) { try { const pg = await getActivePage(browser); await pg.reload({ waitUntil: "domcontentloaded", timeout: 30000 }); } catch (e) { log("reload err: " + e.message); } }
    await sleep(secs * 1000);
    log("--- console (" + secs + "s, " + logs.length + " lines) ---");
    log(logs.slice(-80).join("\n") || "(no console output captured)");
  });
}

// ─── dispatch ──────────────────────────────────────────────────────────────

// Evaluate an expression in the extension service worker (background.js)
// context — for DIAGNOSING a stalled pipeline (script buffers, tab meta,
// last analysis error). The SW is MV3 and sleeps; opening the popup first
// wakes it (the popup messages the SW on render). Verification of RESULTS
// still goes through the popup UI; this is for debugging plumbing only.
async function cmdSweval(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: sweval <js-expression>");
  const body = /(^|\s)return\s|;|\{/.test(expr) ? expr : "return (" + expr + ");";
  await withBrowser(async (browser) => {
    // Wake the SW: touch the popup (its render pings the SW).
    try { await getPopupPage(browser); } catch (_) {}
    const lock = await readLock();
    const extId = lock?.extId;
    let target = null;
    for (let i = 0; i < 40 && !target; i++) {
      target = browser.targets().find(t => t.type() === "service_worker" &&
        (!extId || t.url().includes(extId)));
      if (!target) await sleep(150);
    }
    if (!target) { log("(no service_worker target — SW not running)"); return; }
    const worker = await target.worker();
    if (!worker) { log("(service_worker target has no worker handle)"); return; }
    const out = await worker.evaluate(new Function("return (async () => { " + body + " })()"));
    log(JSON.stringify(out, null, 2));
  });
}

// Capture the active page's RAW server-rendered HTML (a fresh same-origin
// fetch, so react-partial elements + their embedded <script type=json> data
// islands are present — post-JS the DOM has them replaced by rendered React)
// and write it as an engine test fixture (`globalThis.__pageUrl=…;
// globalThis.__pageHtml=…;`). Used to refresh _realph.js with a COMPLETE page
// (the old fixture was a partial capture missing the global-nav-bar partial +
// breadcrumb crumbs the render-gated learning needs).
async function cmdCapture(args) {
  const out = args.join(" ").trim();
  if (!out) throw new Error("usage: capture <output-file.js>");
  await withBrowser(async (browser) => {
    const page = await getActivePage(browser);
    const cap = await page.evaluate(async () => {
      const r = await fetch(location.href, { credentials: "include", redirect: "follow" });
      return { url: location.href.split("#")[0], status: r.status, html: await r.text() };
    });
    const js = "globalThis.__pageUrl=" + JSON.stringify(cap.url) + ";globalThis.__pageHtml=" + JSON.stringify(cap.html) + ";\n";
    const abs = path.isAbsolute(out) ? out : path.join(ROOT, out);
    fs.writeFileSync(abs, js, "utf8");
    log(`captured ${cap.html.length} chars (HTTP ${cap.status}) from ${cap.url} -> ${abs}`);
  });
}

// Dump the exact combined bundle the deep grind booted (and crashed on) from
// the offscreen doc's feDeepDB, so the wasm OOB can be reproduced natively in
// _wdeep.cjs with a real stack instead of guessed-at. Writes <out> (the code)
// and <out>.pre.js (the page HTML) — drop-in replacements for _curbundle_all.js
// / _realph.js so the live, crashing chunk set is what runs natively.
async function cmdDumpBundle(args) {
  const out = (args.join(" ").trim()) || "engine/qjs/_livebundle.js";
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const extId = lock?.extId;
    const url = `chrome-extension://${extId}/ast-worker.html`;
    let pg = null;
    for (let i = 0; i < 40 && !pg; i++) {
      const t = browser.targets().find(t => t.url().startsWith(url));
      if (t) pg = await t.page().catch(() => null);
      if (!pg) await sleep(150);
    }
    if (!pg) { log("(no offscreen target — extension running?)"); return; }
    const rec = await pg.evaluate(() => new Promise((res) => {
      let r;
      try { r = indexedDB.open("feDeepDB"); } catch (e) { return res({ err: "open throw " + e.message }); }
      r.onerror = () => res({ err: "open error" });
      r.onsuccess = () => {
        const db = r.result;
        if (!db.objectStoreNames.contains("code")) return res({ err: "no code store; have=" + [...db.objectStoreNames].join(",") });
        const g = db.transaction("code", "readonly").objectStore("code").getAll();
        g.onerror = () => res({ err: "getAll error" });
        g.onsuccess = () => {
          const recs = g.result || [];
          if (!recs.length) return res({ err: "no code records (grind didn't persist / cleared)" });
          const c = recs[recs.length - 1];
          res({ key: c.key, sourceUrl: c.sourceUrl || "", len: (c.code || "").length, code: c.code || "", pageHtml: c.pageHtml || "", scriptUrls: c.scriptUrls || null });
        };
      };
    }));
    if (rec.err) { log("feDeepDB: " + rec.err); return; }
    const abs = path.isAbsolute(out) ? out : path.join(ROOT, out);
    fs.writeFileSync(abs, rec.code, "utf8");
    if (rec.pageHtml) fs.writeFileSync(abs.replace(/\.js$/, ".pre.js"), "globalThis.__pageUrl=" + JSON.stringify(rec.sourceUrl.split("#")[0]) + ";globalThis.__pageHtml=" + JSON.stringify(rec.pageHtml) + ";\n", "utf8");
    log(`dumped ${rec.len} bytes (sourceUrl=${rec.sourceUrl}) -> ${abs}` + (rec.pageHtml ? " + .pre.js" : ""));
  });
}

// Dump the LIVE in-flight shallow-review script buffer (_scriptBuffers) from
// the offscreen brain for the active page tab — captures the *pre-analysis*
// combined JS that the worker is about to chew on, BEFORE a heavy bundle
// review starts (or while it's running — the brain side stays responsive
// even when the worker is wedged on a synchronous wasm schedule). Writes
// <out> (the code, in execution order: order-sorted, concatenated with
// "\n;\n" separators — same as _analyzeCombinedScriptsInner) and a sibling
// <out>.scripts.json describing the per-script urls/offsets/lengths so a
// native profiler can attribute time back to a specific source script. Used
// to obtain a representative real-site fixture without depending on
// feDeepDB (which is only populated by the cross-session deep grind, not
// the shallow review where the wall-time regression manifests).
async function cmdDumpScripts(args) {
  // Optional `find:<substr>` selects the buffer whose COMBINED code contains
  // <substr> (e.g. find:index-docs), regardless of which tab is active — the
  // freezing chunk is often in a background tab's buffer, not the foreground
  // page, so active-page matching grabs the wrong one.
  let find = null;
  const rest = args.filter((a) => { const m = /^find:(.*)$/.exec(a); if (m) { find = m[1]; return false; } return true; });
  const out = (rest.join(" ").trim()) || "engine/qjs/_curbundle.js";
  await withBrowser(async (browser) => {
    const page = await getActivePage(browser).catch(() => null);
    const pageUrl = page ? await page.evaluate(() => location.href).catch(() => "") : "";
    const lock = await readLock();
    const extId = lock?.extId;
    const ourl = `chrome-extension://${extId}/ast-worker.html`;
    let pg = null;
    for (let i = 0; i < 40 && !pg; i++) {
      const t = browser.targets().find(t => t.url().startsWith(ourl));
      if (t) pg = await t.page().catch(() => null);
      if (!pg) await sleep(150);
    }
    if (!pg) { log("(no offscreen target — extension running?)"); return; }
    const rec = await pg.evaluate((wantUrl, find) => {
      if (typeof _scriptBuffers === "undefined") return { err: "_scriptBuffers not visible from offscreen scope" };
      let best = null;
      const haveTabs = [];
      for (const [tid, b] of _scriptBuffers.entries()) {
        if (!b || !b.scripts || b.scripts.length === 0) continue;
        haveTabs.push((b.pageUrl || "") + "(" + b.scripts.length + " scripts)");
        if (find) {
          // Select the buffer that actually CONTAINS the wanted code (by url or
          // source text) — the freezing chunk's tab, not the active one.
          if (b.scripts.some((s) => (s.url && s.url.indexOf(find) >= 0) || (s.code && s.code.indexOf(find) >= 0))) { best = { tid, b }; break; }
          continue;
        }
        if (wantUrl && b.pageUrl && b.pageUrl.split("#")[0] === wantUrl.split("#")[0]) { best = { tid, b }; break; }
        if (!best) best = { tid, b };
      }
      if (!best) return { err: "no live buffer" + (find ? " containing '" + find + "'" : " (any tab); pageUrl=" + wantUrl) + "; buffers=[" + haveTabs.join(" | ") + "]" };
      const scripts = best.b.scripts.slice().sort((a, b) => (a.order == null ? 1e9 : a.order) - (b.order == null ? 1e9 : b.order));
      let combined = "";
      const map = [];
      for (const s of scripts) {
        map.push({ url: s.url || "(inline)", offset: combined.length, length: s.code.length });
        combined += s.code + "\n;\n";
      }
      // pageHtml is on the per-tab state (set by CONTENT_HTML), not the script buffer.
      // The worker needs it to bootstrap Lexbor with the real SSR DOM so the
      // bundle's render-gated paths run.
      let html = null;
      try { html = (typeof getTab === "function") ? (getTab(best.tid)._pageHtml || null) : null; } catch (_) {}
      return { tid: best.tid, pageUrl: best.b.pageUrl || wantUrl || "", combined, map, html };
    }, pageUrl, find);
    if (rec.err) { log("dumpscripts: " + rec.err); return; }
    const abs = path.isAbsolute(out) ? out : path.join(ROOT, out);
    fs.writeFileSync(abs, rec.combined, "utf8");
    fs.writeFileSync(abs + ".scripts.json", JSON.stringify({ pageUrl: rec.pageUrl, scripts: rec.map }, null, 2), "utf8");
    if (rec.html) fs.writeFileSync(abs.replace(/\.js$/, ".pre.js"), "globalThis.__pageUrl=" + JSON.stringify((rec.pageUrl || "").split("#")[0]) + ";globalThis.__pageHtml=" + JSON.stringify(rec.html) + ";\n", "utf8");
    log(`dumped ${rec.combined.length} bytes (${rec.map.length} scripts, pageUrl=${rec.pageUrl}, tab=${rec.tid}) -> ${abs}`);
  });
}

// Evaluate an expression in the OFFSCREEN document (the learning brain lives
// there now, not the SW) — for DIAGNOSING a stalled learning pipeline (script
// buffers, swFetch results, analysis/resolver errors). Same role cmdSweval had
// for the old SW brain; results verification still goes through the popup UI.
async function cmdOffscreen(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: offscreen <js-expression>");
  const body = /(^|\s)return\s|;|\{/.test(expr) ? expr : "return (" + expr + ");";
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const extId = lock?.extId;
    const url = `chrome-extension://${extId}/ast-worker.html`;
    let pg = null;
    for (let i = 0; i < 40 && !pg; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(url));
      if (t) pg = await t.page().catch(() => null);
      if (!pg) await sleep(150);
    }
    if (!pg) { log("(no offscreen target — extension running?)"); return; }
    const out = await pg.evaluate(new Function("return (async () => { " + body + " })()"));
    log(JSON.stringify(out, null, 2));
  });
}

// Eval inside the analysis Web Worker (ast-thread.js) — a dedicated-worker CDP
// target reached via t.worker(), NOT t.page(). The place to inspect a stuck grind
// (self._poolLiveness is in the offscreen; the worker holds the live combined
// source + driven set + the engine instance). If the worker is blocked in a sync
// C call with no JSPI yield, this eval will time out — itself a useful signal
// (sync-block vs resumed-but-non-terminating).
async function cmdWorker(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: worker <js-expression>");
  const body = /(^|\s)return\s|;|\{/.test(expr) ? expr : "return (" + expr + ");";
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const extId = lock?.extId;
    const ourl = `chrome-extension://${extId}/ast-worker.html`;
    // The analysis worker is a DEDICATED worker of the offscreen PAGE, so it is
    // reached via offscreenPage.workers(), not browser.targets() (a dedicated
    // worker isn't a top-level target). There may be several (the pool) — pick
    // one running ast-thread.js.
    let w = null, diag = "";
    for (let i = 0; i < 40 && !w; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(ourl));
      const pg = t ? await t.page().catch(() => null) : null;
      if (pg) {
        const workers = pg.workers();
        diag = "offscreen workers=[" + workers.map((x) => x.url().split("/").pop()).join(",") + "]";
        w = workers.find((x) => x.url().indexOf("ast-thread.js") >= 0) || null;
      }
      if (!w) await sleep(150);
    }
    if (!w) { log("(no analysis worker; " + (diag || "no offscreen page") + ")"); return; }
    const out = await w.evaluate(new Function("return (async () => { " + body + " })()"));
    log(JSON.stringify(out, null, 2));
  });
}

// Resolve an engine-reported combined-bundle location (file:line[:col]) back to
// a readable source snippet. The forced-exec engine line-numbers stack frames
// (@DSTART, @WHY spin loopFile/loopLine) against the COMBINED bundle (slices
// joined with per-slice start-line offsets), but scriptCache holds each chunk as
// one minified line and combined line numbers shift between dumps — so a raw
// `devsite...:4944:158` is otherwise unreadable. This reads the SAME
// feDeepDB.code store the grind booted from and prints the line (windowed around
// the column) so a fixpoint/keying bug can be read in the real source. The
// missing piece that made the Google spin fix blind.
async function cmdSrcLoc(args) {
  const spec = (args[0] || "").trim();          // file:line[:col]
  const ctxCols = parseInt(args[1] || "200", 10); // chars of context each side of col
  // Non-greedy filename: a greedy `.*` ate the line number (file:4944:158 →
  // line=158), so anchor the trailing :line[:col] and take the SHORTEST file.
  const m = spec.match(/^(.*?):(\d+)(?::(\d+))?$/);
  if (!m) throw new Error("usage: srcloc <file:line[:col]> [ctxCols]");
  const wantFile = m[1], wantLine = parseInt(m[2], 10), wantCol = m[3] ? parseInt(m[3], 10) : 0;
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const ourl = `chrome-extension://${lock?.extId}/ast-worker.html`;
    let pg = null;
    for (let i = 0; i < 40 && !pg; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(ourl));
      if (t) pg = await t.page().catch(() => null);
      if (!pg) await sleep(150);
    }
    if (!pg) { log("(no offscreen)"); return; }
    const rec = await pg.evaluate(async (wantFileArg) => {
      let r; try { r = indexedDB.open("feDeepDB"); } catch (e) { return { err: "open throw " + e.message }; }
      const db = await new Promise((ok) => { r.onsuccess = () => ok(r.result); r.onerror = () => ok(null); });
      if (!db) return { err: "no feDeepDB" };
      if (!db.objectStoreNames.contains("code")) return { err: "no code store" };
      const g = db.transaction("code", "readonly").objectStore("code").getAll();
      const recs = await new Promise((ok) => { g.onsuccess = () => ok(g.result || []); g.onerror = () => ok([]); });
      if (!recs.length) return { err: "code store empty" };
      // Prefer the record whose scriptOffsets reference the wanted chunk; else largest.
      let best = null;
      for (const rc of recs) {
        const offs = rc.scriptOffsets || [];
        const hasChunk = offs.some((o) => (o.url || "").indexOf(wantFileArg) >= 0);
        const sz = (rc.code || "").length;
        const score = (hasChunk ? 1e12 : 0) + sz;
        if (!best || score > best.score) best = { score, rc };
      }
      const rc = best.rc;
      return { key: rc.key || "", code: rc.code || "", scriptOffsets: rc.scriptOffsets || null };
    }, wantFile);
    if (rec.err) { log("srcloc: " + rec.err); return; }
    const lines = rec.code.split("\n");
    if (wantLine < 1 || wantLine > lines.length) {
      log(`srcloc: line ${wantLine} out of range (combined has ${lines.length} lines)`);
      return;
    }
    const line = lines[wantLine - 1];
    // Which chunk owns this combined line (scriptOffsets[].lineStart is 1-based
    // combined space). Pick the chunk with the GREATEST lineStart <= wantLine —
    // NOT array order (scriptOffsets isn't guaranteed sorted; a from-end break
    // mis-attributed devsite@3247 as a googleapis chunk@84).
    let owner = null;
    const offs = rec.scriptOffsets || [];
    for (let i = 0; i < offs.length; i++) {
      const ls = offs[i].lineStart | 0;
      if (ls <= wantLine && (!owner || ls > (owner.lineStart | 0))) owner = offs[i];
    }
    log(`srcloc ${spec}  (combined ${lines.length} lines, key=${rec.key})`);
    log(`chunk: ${owner ? (owner.url || "?") + " @lineStart " + owner.lineStart : "(no scriptOffsets)"}`);
    log(`lineLen=${line.length} col=${wantCol}`);
    if (wantCol > 0 && line.length > ctxCols * 2) {
      const a = Math.max(0, wantCol - ctxCols), b = Math.min(line.length, wantCol + ctxCols);
      log(`…[${a}..${b}]…`);
      log(line.slice(a, b));
      log(" ".repeat(Math.min(ctxCols, wantCol - a)) + "^ col " + wantCol);
    } else {
      log(line.slice(0, Math.min(line.length, ctxCols * 4)));
    }
  });
}

// Run a gate FIXTURE through the REAL Chrome worker (live JSPI + safeFetch +
// Chrome crypto — the actual learning system), the Chrome-only replacement for
// the banned `node qjs_wasm.js <fixture>` flow. A fixture is a synthetic INPUT
// to the real target, not an unrelated target: same wasm, same host, just a
// fixed code string instead of a live page's scripts. Fast (seconds), real
// host, no false confidence. `--deep` exercises the deep-grind orphan drive
// (where opaque loops spin); a non-converging fixture returns the spin
// telemetry (loopFile:line) instead of hanging the harness.
//   gate engine/qjs/_xss.js [--deep] [--timeout=30000]
async function cmdGate(args) {
  const flags = args.filter((a) => a.startsWith("--"));
  const files = args.filter((a) => !a.startsWith("--"));
  if (!files.length) throw new Error("usage: gate <fixture.js> [--deep] [--timeout=ms]");
  const deep = flags.includes("--deep");
  const tmf = flags.find((f) => f.startsWith("--timeout="));
  const timeoutMs = tmf ? parseInt(tmf.slice(10), 10) : 30000;
  const abs = path.isAbsolute(files[0]) ? files[0] : path.join(ROOT, files[0]);
  if (!fs.existsSync(abs)) throw new Error("no such fixture: " + abs);
  const code = fs.readFileSync(abs, "utf8");
  const name = path.basename(abs);
  // --html=<file>: pass pre-existing SSR HTML so the engine parses it into the
  // Lexbor DOM FIRST (then runs the fixture code). Models the real-site case
  // where a custom element (<include-fragment src>) is already in the SSR HTML
  // and customElements.define() upgrades the EXISTING element — distinct from
  // a JS-createElement'd one.
  const hf = flags.find((f) => f.startsWith("--html="));
  let pageHtml = null;
  if (hf) {
    const hp = hf.slice(7);
    const habs = path.isAbsolute(hp) ? hp : path.join(ROOT, hp);
    if (!fs.existsSync(habs)) throw new Error("no such html file: " + habs);
    pageHtml = fs.readFileSync(habs, "utf8");
  }
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const ourl = `chrome-extension://${lock?.extId}/ast-worker.html`;
    let w = null, diag = "";
    for (let i = 0; i < 60 && !w; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(ourl));
      const pg = t ? await t.page().catch(() => null) : null;
      if (pg) {
        const workers = pg.workers();
        diag = "workers=[" + workers.map((x) => x.url().split("/").pop()).join(",") + "]";
        w = workers.find((x) => x.url().indexOf("ast-thread.js") >= 0) || null;
      }
      if (!w) await sleep(150);
    }
    if (!w) { log("(no analysis worker; " + (diag || "no offscreen") + ") — run `harness restart` first"); return; }
    // The worker's astDispatch resolves AST_GATE with the compact summary.
    const res = await w.evaluate(async (code, name, deep, timeoutMs, pageHtml) => {
      return await new Promise((resolve) => {
        // astDispatch is the in-worker entry; AST_GATE runs forcedAnalyze and
        // resolves via the message `done` callback shape the dispatcher uses.
        const msg = { type: "AST_GATE", code, name, deep, timeoutMs, pageHtml, id: "gate-" + name };
        // self.astDispatch posts back through the same channel; but inside the
        // worker we call the handler directly and await its done().
        let settled = false;
        const done = (payload) => { if (!settled) { settled = true; resolve(payload); } };
        // The dispatcher reads `done` from the message-port; emulate by calling
        // the global handler with an injected done.
        if (typeof self.__gateRun === "function") { self.__gateRun(msg, done); }
        else { resolve({ success: false, error: "__gateRun not exposed in worker" }); }
      });
    }, code, name, deep, timeoutMs, pageHtml).catch((e) => ({ success: false, error: String(e && e.message || e) }));
    log(JSON.stringify(res, null, 2));
  });
}

const CMDS = { start: cmdStart, restart: cmdRestart, page: cmdPage, popup: cmdPopup, pocrun: cmdPocRun, goto: cmdGoto, diag: cmdDiag, sweval: cmdSweval, offscreen: cmdOffscreen, worker: cmdWorker, capture: cmdCapture, dumpbundle: cmdDumpBundle, dumpscripts: cmdDumpScripts, srcloc: cmdSrcLoc, gate: cmdGate };

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  const fn = cmd && CMDS[cmd];
  if (!fn) {
    err("usage: node testing/harness.js <start | page | popup> [args…]");
    process.exit(2);
  }
  try { await fn(rest); }
  catch (e) { err(e.stack || e.message || e); process.exit(1); }
}

main();
