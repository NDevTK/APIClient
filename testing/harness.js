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

// The honest "is a finding MISSING or still being LOOKED FOR" verdict — the
// signal a network-vs-AST diff MUST gate on. Polls the worker's _learningState
// until it reaches "complete" (every orphan driven → absence is a real gap) or
// "stalled" (priority frontier failed to advance → a SCHEDULING defect, not a
// coverage gap), or times out still "analyzing" (slow, not done — absence is
// NOT a gap yet). Without this, calling something a gap mid-analysis is
// unfalsifiable. `--wait` polls up to --timeout for a terminal verdict.
async function cmdLearnState(args) {
  const flags = args.filter((a) => a.startsWith("--"));
  const wait = flags.includes("--wait");
  const tmf = flags.find((f) => f.startsWith("--timeout="));
  const timeoutMs = tmf ? parseInt(tmf.slice(10), 10) : 120000;
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const ourl = `chrome-extension://${lock?.extId}/ast-worker.html`;
    let w = null;
    for (let i = 0; i < 60 && !w; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(ourl));
      const pg = t ? await t.page().catch(() => null) : null;
      if (pg) w = pg.workers().find((x) => x.url().indexOf("ast-thread.js") >= 0) || null;
      if (!w) await sleep(150);
    }
    if (!w) { log("(no analysis worker — run `harness restart` first)"); return; }
    const deadline = Date.now() + timeoutMs;
    let last = null;
    do {
      last = await w.evaluate(() => (typeof self._learningState === "function" ? self._learningState() : { state: "no-verdict-fn" }))
        .catch((e) => ({ state: "eval-error", error: String(e && e.message || e) }));
      if (!wait || last.state === "complete" || last.state === "stalled" || last.state === "no-verdict-fn") break;
      await sleep(2000);
    } while (Date.now() < deadline);
    // Reaching the deadline while still "analyzing" is itself the answer: not done.
    log(JSON.stringify(last, null, 2));
  });
}

// TRIAGE — distinguishes the failure modes `learnstate` CANNOT: a non-terminating
// HANG vs a RECYCLE (uncatchable stack-overflow re-spawn) vs a SPIN (sync loop) vs
// a LARGE TASK (progressing, fix = prioritisation not a cap) vs DONE. learnstate
// shows only {state,driven} and HIDES that e.g. 100+ catchable stack overflows
// fired-and-recovered (measured on sentry_inline: complete/1302 yet stack_overflow
// _recursion×N). This samples _learningState()+_whyRecords TWICE (--gap ms apart)
// so the driven/overflow DELTA classifies the mode; a worker eval that TIMES OUT is
// itself the HANG signal (worker sync-blocked in a C call, no JSPI yield).
//   usage: triage [--gap=4000]
async function cmdTriage(args) {
  const gapf = args.find((a) => a.startsWith("--gap="));
  const gapMs = gapf ? parseInt(gapf.slice(6), 10) : 4000;
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const ourl = `chrome-extension://${lock?.extId}/ast-worker.html`;
    let w = null;
    for (let i = 0; i < 60 && !w; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(ourl));
      const pg = t ? await t.page().catch(() => null) : null;
      if (pg) w = pg.workers().find((x) => x.url().indexOf("ast-thread.js") >= 0) || null;
      if (!w) await sleep(150);
    }
    if (!w) { log("(no analysis worker — run `harness restart` first)"); return; }
    const probe = async () => {
      const t0 = Date.now();
      try {
        const r = await w.evaluate(() => {
          const st = (typeof self._learningState === "function") ? self._learningState() : { state: "no-fn" };
          const recs = self._whyRecords || [];
          const by = {}; for (const x of recs) { const p = (x && x.phase) || "?"; by[p] = (by[p] || 0) + 1; }
          // Surface the THROW err strings + recycle reasons — the distinguishing
          // root signal for a wasm_trap recycle loop that the bare phase COUNT
          // hides: "Cannot enlarge memory"/"Out of memory"=scale (boot bundle too
          // big), "out of bounds"/"unreachable"=a specific orphan bug, "Aborted()"
          // =an assert. Without this a large-site wedge shows 59× deep_callmain_throw
          // with no CAUSE, so it can't be root-caused (CLAUDE.md: READ throw.err).
          const throws = recs.filter((x) => x && x.phase === "deep_callmain_throw").slice(-6).map((x) => ({ err: x.err, step: x.step, culprit: x.culprit, loc: x.culpritLoc }));
          const recycles = {}; for (const x of recs) { if (x && x.phase === "deep_recycle") { const rr = x.reason || "?"; recycles[rr] = (recycles[rr] || 0) + 1; } }
          return { st, by, nWhy: recs.length, throws, recycles };
        });
        return { ms: Date.now() - t0, ...r };
      } catch (e) {
        // A hung worker (sync-blocked in a C recursion/loop with no JSPI yield)
        // cannot service the eval — the timeout IS the hang signal.
        return { ms: Date.now() - t0, evalError: String(e && e.message || e) };
      }
    };
    const sig = (by) => ({
      overflow: ((by || {}).stack_overflow_recursion || 0) + ((by || {}).stack_overflow_nonrecursive || 0),
      spin: (by || {}).spin_nonterminating || 0,
      caught: (by || {}).caught_throw || 0,
    });
    const a = await probe();
    if (a.evalError) { log(JSON.stringify({ verdict: "HANG — worker sync-blocked (eval timed out: " + a.evalError + ")", note: "a C-level recursion/loop with no JSPI yield; srcloc the last @DSTART / @WHY spin" }, null, 2)); return; }
    await sleep(gapMs);
    const b = await probe();
    if (b.evalError) { log(JSON.stringify({ verdict: "HANG — worker became sync-blocked between samples", firstSample: a.st }, null, 2)); return; }
    const dn = (x) => (x && typeof x.st.driven === "number") ? x.st.driven : null;
    const dDriven = (dn(a) !== null && dn(b) !== null) ? dn(b) - dn(a) : null;
    const sa = sig(a.by), sb = sig(b.by);
    const dOverflow = sb.overflow - sa.overflow, dWhy = b.nWhy - a.nWhy;
    let verdict;
    if (b.st.state === "complete" || b.st.state === "stalled") {
      verdict = (b.st.state === "complete" ? "DONE (terminated)" : "STALLED (worker self-reported stalled)") +
        (sb.overflow ? ` — RECOVERED from ${sb.overflow} catchable stack-overflow(s); the gitlab pathology is the UNCATCHABLE variant of these` : "");
    } else if (dDriven !== null && dDriven > 0) {
      verdict = `LARGE TASK — driven +${dDriven} in ${gapMs}ms (progressing). Fix is PRIORITISATION (run productive paths first), NEVER a cap.`;
    } else if (dOverflow > 0) {
      verdict = `RECYCLE — driven flat (+${dDriven}) while stack-overflows climb (+${dOverflow}). The uncatchable-overflow re-spawn; root-cause the trap (reserve throw headroom).`;
    } else if (sb.spin > 0 && dDriven !== null && dDriven <= 0) {
      verdict = `SPIN — driven flat, ${sb.spin} spin_nonterminating @WHY. A non-terminating sync loop the fixpoint missed; srcloc the loop.`;
    } else if (dWhy > 0 && (dDriven === null || dDriven <= 0)) {
      verdict = `BUSY-NO-DRIVE — emitting @WHY (+${dWhy}) but driven flat; inspect whyByPhase for the active phase.`;
    } else {
      verdict = `IDLE/STUCK — driven flat (+${dDriven}), no overflow/spin/emit delta. Either finished-but-not-flagged or a silent stall.`;
    }
    log(JSON.stringify({
      verdict, state: b.st.state, driven: dn(b), dDriven, rem: b.st.rem, total: b.st.total,
      gapMs, signals: sb, deltas: { dOverflow, dWhy }, whyByPhase: b.by,
      recycles: b.recycles, throws: b.throws,
    }, null, 2));
  });
}

// Multi-tab concurrent-grind measurement (continuous-session scheduler test).
// Opens each url in its OWN real tab, spaced by --stagger ms, then polls the
// offscreen brain's learned endpoints grouped by host every --tick ms for
// --window ms. The decisive test for the concurrent-grind change: with the
// serial gate, a tab opened behind another's grind STARVES (stuck at ~1
// endpoint); with the relevance-bounded concurrent cap, every tab's endpoint
// count should GROW over the window. Per-host grouping shows which tab each
// endpoint belongs to (host substring of the tab url) vs cross-tab "shared".
//   usage: multitab <url1> <url2> [...] [--stagger=15000] [--tick=8000] [--window=180000]
async function cmdMultiTab(args) {
  const urls = args.filter((a) => !a.startsWith("--"));
  if (urls.length < 2) throw new Error("usage: multitab <url1> <url2> [...] [--stagger=ms] [--tick=ms] [--window=ms]");
  const num = (p, d) => { const f = args.find((a) => a.startsWith(p)); return f ? parseInt(f.slice(p.length), 10) : d; };
  const stagger = num("--stagger=", 15000);
  const tick = num("--tick=", 8000);
  const windowMs = num("--window=", 180000);
  // Label each url by its registrable-ish host token (e.g. github.com → "github",
  // learn.microsoft.com → "microsoft") so the byHost report is readable.
  const labelOf = (h) => {
    const parts = String(h || "").split(".").filter(Boolean);
    if (parts.length >= 2) return parts[parts.length - 2];
    return h || "?";
  };
  const tabLabels = urls.map((u) => { try { return labelOf(new URL(u).host); } catch { return "?"; } });
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const ourl = `chrome-extension://${lock?.extId}/ast-worker.html`;
    const getOffscreen = async () => {
      for (let i = 0; i < 40; i++) {
        const t = browser.targets().find((t) => t.url().startsWith(ourl));
        const pg = t ? await t.page().catch(() => null) : null;
        if (pg) return pg;
        await sleep(150);
      }
      return null;
    };
    // Open each url in its own tab, staggered.
    for (let i = 0; i < urls.length; i++) {
      const page = await browser.newPage();
      try { await page.goto(urls[i], { waitUntil: "domcontentloaded", timeout: 60000 }); }
      catch (e) { log(`tab${i} goto err: ${e.message}`); }
      await page.bringToFront();
      log(`opened tab${i} [${tabLabels[i]}] ${urls[i]}`);
      if (i < urls.length - 1) await sleep(stagger);
    }
    // Poll endpoints-by-host through the offscreen brain.
    const t0 = Date.now();
    let n = 0;
    while (Date.now() - t0 < windowMs) {
      const pg = await getOffscreen();
      if (!pg) { log("(no offscreen target)"); break; }
      const snap = await pg.evaluate((tabHosts) => {
        const gs = (typeof globalStore !== "undefined") ? globalStore : null;
        if (!gs || !gs.endpoints) return { total: 0, byHost: {} };
        const byHost = {};
        for (const [, v] of gs.endpoints) {
          let host = v && v.host;
          if (!host && v && v.url) { try { host = new URL(v.url, "https://x/").host; } catch (e) {} }
          const parts = String(host || "").split(".").filter(Boolean);
          const lab = parts.length >= 2 ? parts[parts.length - 2] : (host || "?");
          // Bucket into one of the tab labels, else "other".
          const bucket = tabHosts.indexOf(lab) >= 0 ? lab : "other";
          byHost[bucket] = (byHost[bucket] || 0) + 1;
        }
        return { total: gs.endpoints.size, byHost };
      }, tabLabels).catch((e) => ({ total: -1, byHost: { error: String(e && e.message || e) } }));
      log(`t${++n} +${Math.round((Date.now() - t0) / 1000)}s ${JSON.stringify(snap)}`);
      await sleep(tick);
    }
  });
}

// Network ↔ QuickJS diff (review methodology (b), CLAUDE.md): every LIVE
// fetch/XHR the page actually made (captured in the brain's per-tab requestLog)
// that AST forced-execution did NOT learn is a host-model or forced-exec gap.
// Reports gapCount + the unlearned live requests so a coverage hole is concrete
// and quantified, per-vendor, instead of eyeballed. Read-only over the brain's
// own state — does NOT mutate, does NOT craft state messages.
//   usage: netdiff [--all]   (--all = include cross-origin 3rd-party; default first-party-ish)
// CAVEAT: a request absent from the learned set is only a definitive gap once
// learnstate is "complete" (rem==0). Mid-analysis it may still be learned — the
// command prints the current learnstate alongside so the reader judges.
async function cmdNetDiff(args) {
  const all = args.includes("--all");
  const showAssets = args.includes("--assets");   // include static-asset GETs in the gap list (default: filter them)
  const unused = args.includes("--unused");        // LEARNED-NOT-LIVE: the unused API surface forced exec found (the VALUE), not gaps
  await withBrowser(async (browser) => {
    const lock = await readLock();
    const ourl = `chrome-extension://${lock?.extId}/ast-worker.html`;
    let pg = null;
    for (let i = 0; i < 40 && !pg; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(ourl));
      if (t) pg = await t.page().catch(() => null);
      if (!pg) await sleep(150);
    }
    if (!pg) { log("(no offscreen target — run `harness restart` first)"); return; }
    const out = await pg.evaluate(({ all, showAssets, unused }) => {
      // Normalize a URL to host+path with volatile id/number segments collapsed,
      // so /repo/pull/123 and /repo/pull/456 are one endpoint shape.
      const norm = (u, base) => {
        try { const x = new URL(u, base || "https://x/");
          return x.host + x.pathname.replace(/\/[0-9a-f]{16,}/g, "/:id").replace(/\/\d+/g, "/:n"); }
        catch (e) { return String(u).slice(0, 80); }
      };
      // A live GET to a STATIC ASSET is not an API endpoint the analyzer missed —
      // it's correctly not learned. The analyzer ALREADY classifies each captured
      // request (magic-byte + mime → _assetKind/_boring, CLAUDE.md #9), so trust
      // that first; the captured content-type is often "" for cross-origin/opaque
      // asset responses (the real type lives in mimeType). Fall back to mime/
      // content-type for entries that predate the classification. text/html is
      // NOT an asset — an include-fragment / SSR partial returns text/html and IS
      // a real endpoint. Unknown → keep (conservative; never hide a real gap).
      const isAsset = (r) => {
        if (r._assetKind === "asset") return true;
        const ct = String(r.contentType || r.mimeType || "").toLowerCase().split(";")[0].trim();
        if (!ct) return false;
        return /^(image|font|video|audio|model)\//.test(ct)
          || ct === "text/css"
          || /^(application|text)\/(javascript|ecmascript)$/.test(ct)
          || ct === "application/wasm"
          || /^application\/(x-font|font-|vnd\.ms-fontobject)/.test(ct);
      };
      const learned = new Set(), learnedMap = new Map();
      if (typeof globalStore !== "undefined") {
        for (const e of globalStore.endpoints.values()) {
          const k = (e.method || "GET") + " " + norm((e.host ? "https://" + e.host : "") + (e.path || e.url || ""));
          learned.add(k); if (!learnedMap.has(k)) learnedMap.set(k, e);
        }
      }
      // An endpoint with opaque path params is learned as a TEMPLATE
      // (`/{userLocale}/content-nav/X.json`) — the hole is a structural opaque
      // param (CLAUDE.md structural learning) and the endpoint IS learned. The
      // live request carries the concrete segment (`/en-us/...`), so exact-key
      // membership misses it and the oracle would FALSELY flag a learned
      // endpoint as a coverage gap (or as unused when it did fire). Match
      // concrete live URLs against learned templates: each {hole} (literal, or
      // %7B..%7D after norm()'s URL re-encode) is one path-segment wildcard.
      const tmplRe = (k) => {
        if (k.indexOf("{") < 0 && k.indexOf("%7B") < 0 && k.indexOf("%7b") < 0) return null;
        const src = "^" + k
          .replace(/%7B[^%/]*%7D/gi, "\x00")
          .replace(/\{[^}/]*\}/g, "\x00")
          .replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
          .replace(/\x00/g, "[^/]+") + "$";
        try { return new RegExp(src); } catch (e) { return null; }
      };
      const learnedTemplates = [];
      for (const k of learned) { const re = tmplRe(k); if (re) learnedTemplates.push(re); }
      const matchedByTemplate = (liveKey) => learnedTemplates.some((re) => re.test(liveKey));
      const seen = new Set(), gaps = [], byTab = []; let assetFiltered = 0;
      if (typeof state !== "undefined") {
        for (const [tid, t] of state.tabs) {
          if (!t.requestLog || !t.requestLog.length) continue;
          const pageUrl = (state.tabMeta && state.tabMeta.get && state.tabMeta.get(tid) && state.tabMeta.get(tid).url) || "";
          let tabGaps = 0;
          for (const r of t.requestLog) {
            if (!r.url || !/^https?:/.test(r.url)) continue;
            const k = (r.method || "GET") + " " + norm(r.url);
            if (seen.has(k)) continue; seen.add(k);
            if (learned.has(k) || matchedByTemplate(k)) continue;
            if (!showAssets && isAsset(r)) { assetFiltered++; continue; }
            // first-party-ish filter unless --all: same registrable-ish host as the page
            // The bundle call-site that fired this live request (intercept.js
            // captured new Error().stack, wrapper frames stripped). A gap's
            // FIRST frame IS the function forced exec failed to drive — turns
            // "missed URL X" into "missed the path at <fn> that fires X", the
            // actionable signal for closing the coverage gap (the oracle role).
            const site = (r.callStack || "").split("\n").map((s) => s.trim()).filter(Boolean)[0] || "";
            gaps.push({ k, status: r.status, svc: r.service || "", site }); tabGaps++;
          }
          byTab.push({ tab: pageUrl.slice(0, 50), gaps: tabGaps });
        }
      }
      // Reached-but-opaque host edges: a fetch/XHR forced exec DID reach but
      // whose URL/method didn't resolve to a concrete string (an opaque
      // component reached the sink). On a 0-endpoint page this is THE deciding
      // signal — "reached but went opaque" (a driving/resolver gap to CLOSE) vs
      // "never reached" (a coverage gap). From scriptCache[].result.resolverErrors.
      const _reachedSet = new Set();
      const _moduleLinkSet = new Set();   // ESM "could not load module '/x'" — a DISCOVERY/LINK gap, not endpoint opacity
      if (typeof globalStore !== "undefined" && globalStore.scriptCache) {
        // Use only the LATEST scriptCache entry per page. The analysis re-runs as
        // ESM modules arrive across rounds (each round is a new bundle-hash entry);
        // an early round's resolverErrors are STALE — they name modules not yet
        // fetched THEN. Reporting the union shows phantom link-failures the final
        // round already resolved (esm.sh: 7 "could not load module" surfaced while
        // its converged 18-script round had 0). Group by tabUrl, keep max-timestamp.
        const _latestPerTab = new Map();
        for (const sc of globalStore.scriptCache.values()) {
          const _tab = (sc && sc.tabUrl) || "";
          const _ts = (sc && sc.timestamp) || 0;
          const _prev = _latestPerTab.get(_tab);
          if (!_prev || _ts >= (_prev.timestamp || 0)) _latestPerTab.set(_tab, sc);
        }
        for (const sc of _latestPerTab.values()) {
          const re = sc && sc.result && sc.result.resolverErrors;
          if (Array.isArray(re)) for (const r of re) {
            const msg = String((r && r.message) || JSON.stringify(r));
            if (msg.indexOf("could not load module") >= 0) {
              // A failed ESM import link, NOT a host edge that went opaque. Separating
              // it keeps reachedButOpaque a clean driving-gap signal and surfaces the
              // distinct "module didn't link" gap (e.g. esm.sh transitive deps).
              const mm = msg.match(/could not load module filename '([^']+)'/);
              _moduleLinkSet.add(mm ? mm[1] : msg.slice(0, 120));
            } else {
              _reachedSet.add(
                (r && r.loc ? "@" + (r.loc.file ? r.loc.file + ":" : "") + r.loc.line + ":" + r.loc.column + " " : "") +
                msg.slice(0, 200));
            }
          }
        }
      }
      const _reached = Array.from(_reachedSet);
      const _moduleLink = Array.from(_moduleLinkSet);
      if (unused) {
        // LEARNED-NOT-LIVE = the unused API surface forced exec found (THE VALUE,
        // the inverse of gaps): AST-learned endpoints the page never fired, with
        // their example values. source !== ast_analysis means it was learned FROM
        // live traffic (so it DID fire) — exclude those.
        // A learned TEMPLATE whose hole matched a concrete live URL DID fire —
        // exclude it from "unused" too (same template-match as the gap side).
        const liveMatchesTemplate = (k) => { const re = tmplRe(k); if (!re) return false; for (const lk of seen) if (re.test(lk)) return true; return false; };
        // globalStore.endpoints carries only method+host+path; the per-param
        // KEYS+VALUES — including BODY keys (the {email,password} login shape,
        // THE MOAT here) — live in scriptCache[].result.fetchCallSites[].params
        // ({name, location:"path"|"query"|"body", validValues}). Without this
        // cross-ref the unused list shows bare URLs and the diagnostic hides the
        // very thing it measures (forced exec's recovered body shape). Build a
        // method+pathname → merged-params map (numeric segments collapsed to
        // match the endpoint key's norm).
        const _paramKey = (method, url) => {
          let path = String(url || "");
          try { path = new URL(url, "https://x/").pathname; }
          catch (e) { const qi = path.indexOf("?"); if (qi >= 0) path = path.slice(0, qi); }
          path = path.replace(/\/[0-9a-f]{16,}/g, "/:id").replace(/\/\d+/g, "/:n");
          return (method || "GET") + " " + path;
        };
        const _fcsParams = new Map();   // paramKey -> Map(name -> {loc, vals:Set})
        if (typeof globalStore !== "undefined" && globalStore.scriptCache) {
          for (const sc of globalStore.scriptCache.values()) {
            const fcs = sc && sc.result && sc.result.fetchCallSites;
            if (!Array.isArray(fcs)) continue;
            for (const cs of fcs) {
              if (!cs || !Array.isArray(cs.params) || !cs.params.length) continue;
              const pk = _paramKey(cs.method, cs.url);
              let pm = _fcsParams.get(pk); if (!pm) { pm = new Map(); _fcsParams.set(pk, pm); }
              for (const p of cs.params) {
                if (!p || !p.name) continue;
                let pe = pm.get(p.name); if (!pe) { pe = { loc: p.location || "query", vals: new Set() }; pm.set(p.name, pe); }
                if (Array.isArray(p.validValues)) for (const v of p.validValues) if (v != null && v !== "") pe.vals.add(v);
              }
            }
          }
        }
        const _fmtParams = (e) => {
          const pm = _fcsParams.get(_paramKey(e.method, e.path || e.url || ""));
          if (!pm || !pm.size) return "";
          // Body keys first (the moat), then path, then query.
          const ord = { body: 0, path: 1, query: 2 };
          const parts = Array.from(pm.entries())
            .sort((a, b) => (ord[a[1].loc] ?? 3) - (ord[b[1].loc] ?? 3))
            .slice(0, 8)
            .map(([name, info]) => String(name).slice(0, 40) + "@" + info.loc + (info.vals.size
              ? "=" + Array.from(info.vals).slice(0, 3).map((x) => String(x).slice(0, 24)).join("|")
              : "={opaque}"));
          return "  [" + parts.join(", ") + "]";
        };
        const unusedList = [];
        for (const [k, e] of learnedMap) {
          if (seen.has(k) || liveMatchesTemplate(k)) continue;
          if (e.source && e.source !== "ast_analysis") continue;
          unusedList.push(k + _fmtParams(e));
        }
        return { mode: "unused = learned-but-not-live (the unused API surface forced exec found — THE VALUE)",
                 learnedCount: learnedMap.size, liveDistinct: seen.size, unusedCount: unusedList.length,
                 reachedButOpaque: _reached.length, reachedSamples: _reached.slice(0, 10),
                 moduleLinkFailures: _moduleLink.length, moduleLinkSamples: _moduleLink.slice(0, 10),
                 unused: unusedList.slice(0, 100) };
      }
      return { learnedCount: learned.size, liveDistinct: seen.size, gapCount: gaps.length,
               reachedButOpaque: _reached.length, reachedSamples: _reached.slice(0, 10),
               moduleLinkFailures: _moduleLink.length, moduleLinkSamples: _moduleLink.slice(0, 10),
               assetFiltered: assetFiltered + (showAssets ? " (shown; --assets)" : " (static-asset GETs excluded; --assets to show)"),
               gaps: gaps.slice(0, 60).map((g) => g.k + (g.status ? " [" + g.status + "]" : "") + (g.site ? "  ← " + g.site : "  ← (no stack)")) };
    }, { all, showAssets, unused }).catch((e) => ({ error: String(e && e.message || e) }));
    log(JSON.stringify(out, null, 2));
    log("NOTE: a gap is definitive only when learnstate is `complete` (rem==0); mid-analysis, re-check after.");
  });
}


const CMDS = { start: cmdStart, restart: cmdRestart, page: cmdPage, popup: cmdPopup, pocrun: cmdPocRun, goto: cmdGoto, diag: cmdDiag, sweval: cmdSweval, offscreen: cmdOffscreen, worker: cmdWorker, capture: cmdCapture, dumpbundle: cmdDumpBundle, dumpscripts: cmdDumpScripts, srcloc: cmdSrcLoc, learnstate: cmdLearnState, triage: cmdTriage, multitab: cmdMultiTab, netdiff: cmdNetDiff };

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
