// Live harness for driving Chrome + the extension interactively.
//
// Not a test suite. This is a single tool you can use to review and
// control the extension's behavior on real sites, inspect raw page
// scripts, compare them to the extension's AST findings, and drive the
// popup as a user would.
//
// Workflow:
//   node testing/harness.js start                     # launch persistent Chrome (background)
//   node testing/harness.js goto https://reddit.com
//   node testing/harness.js scripts                   # list scripts the SW has analysed
//   node testing/harness.js script <url|index>        # dump raw JS for a script
//   node testing/harness.js ast <url|index>           # AST findings for a script + surrounding JS
//   node testing/harness.js findings                  # security findings list
//   node testing/harness.js finding <index>           # finding + ±20 lines of raw JS
//   node testing/harness.js logs                      # captured requests (all tabs)
//   node testing/harness.js log <reqId>               # one request: headers, body, response
//   node testing/harness.js keys                      # api-key registry
//   node testing/harness.js services                  # service groupings
//   node testing/harness.js popup                     # open popup page
//   node testing/harness.js popup.select <reqId>      # replay that request in popup
//   node testing/harness.js popup.form                # dump current form state
//   node testing/harness.js popup.fill <name> <value> # type into field (custom value)
//   node testing/harness.js popup.send                # click Send
//   node testing/harness.js popup.response            # dump response render + raw body
//   node testing/harness.js sw <js-expression>        # run JS inside the SW context
//   node testing/harness.js page <js-expression>      # run JS inside the current page
//   node testing/harness.js stop                      # shut the browser down
//
// The persistent browser keeps running between commands so you can
// browse manually in the same window while issuing commands here.

"use strict";

const path = require("path");
const fs = require("fs");
const fsp = fs.promises;
const net = require("net");
const http = require("http");
const { spawn } = require("child_process");
const puppeteer = require("puppeteer");

const ROOT = path.resolve(__dirname, "..");
const EXT_DIR = path.join(ROOT, "extension");
const PROFILE_DIR = path.join(__dirname, "profile");
const DUMP_DIR = path.join(__dirname, "harness-dumps");
const LOCK_FILE = path.join(__dirname, "harness.lock");
const DEFAULT_PORT = 9337;

function log(...args) { console.log(...args); }
function err(...args) { console.error(...args); }
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
function short(s, n) { s = String(s == null ? "" : s); return s.length > n ? s.slice(0, n) + "…" : s; }
function labelScript(url) {
  if (!url) return "(inline)";
  if (url.startsWith("data:")) {
    // Data URL inline scripts — too long to print. Keep just scheme+size.
    return `(data-url inline, ${url.length} chars)`;
  }
  return url;
}

// Minimal arg parser: splits args into positionals + {--key: value, --flag: true}.
// Supports --k=v, --k v, and bare --flag.
function parseArgs(rest) {
  const positional = [];
  const flags = {};
  for (let i = 0; i < rest.length; i++) {
    const a = rest[i];
    if (a.startsWith("--")) {
      const eq = a.indexOf("=");
      if (eq >= 0) { flags[a.slice(2, eq)] = a.slice(eq + 1); }
      else {
        const nx = rest[i + 1];
        if (nx !== undefined && !nx.startsWith("--")) { flags[a.slice(2)] = nx; i++; }
        else { flags[a.slice(2)] = true; }
      }
    } else positional.push(a);
  }
  return { positional, flags };
}

// ─── port helpers ──────────────────────────────────────────────────────────

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
  try {
    const data = JSON.parse(await fsp.readFile(LOCK_FILE, "utf8"));
    return data;
  } catch { return null; }
}

async function writeLock(data) {
  await fsp.writeFile(LOCK_FILE, JSON.stringify(data, null, 2), "utf8");
}

async function clearLock() {
  try { await fsp.unlink(LOCK_FILE); } catch {}
}

// Each command connects afresh, runs, disconnects without closing the
// browser. The start command holds the browser alive.
async function connect(port) {
  const ver = await pingPort(port);
  if (!ver) throw new Error(`no browser on port ${port} — run: node testing/harness.js start`);
  return puppeteer.connect({
    browserURL: `http://127.0.0.1:${port}`,
    defaultViewport: null,
    // Default filter excludes extension pages and offscreen workers, so
    // chrome-extension://<id>/popup.html gets dropped. Override to see
    // everything — we rely on popup + SW targets explicitly.
    targetFilter: (target) => target.type() !== "browser",
    // Default 30s protocolTimeout is too short for large-site AST runs:
    // concatenating + parsing Figma's ~1 MB vendor bundle through Babel
    // in the offscreen worker often exceeds that and the whole command
    // fails with a generic Protocol error. 300s gives real headroom.
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

async function getSWWorker(browser) {
  // MV3 SWs go dormant and chrome.runtime.reload() briefly removes ALL
  // extension targets. The extension ID is persisted in the lock file,
  // so we can reliably wake the SW by navigating to any extension page.
  const lock = await readLock();
  let tries = 0;
  while (tries++ < 5) {
    const deadline = Date.now() + 3000;
    while (Date.now() < deadline) {
      const t = browser.targets().find(t => t.type() === "service_worker" && t.url().startsWith("chrome-extension://"));
      if (t) {
        try { const w = await t.worker(); if (w) return w; } catch {}
      }
      await sleep(200);
    }
    // Wake by opening a page under the extension origin.
    let extId = lock?.extId;
    if (!extId) {
      const extTarget = browser.targets().find(t => t.url().startsWith("chrome-extension://"));
      if (extTarget) { try { extId = new URL(extTarget.url()).hostname; } catch {} }
    }
    if (!extId) { await sleep(500); continue; }
    const waker = await browser.newPage();
    try {
      await waker.goto(`chrome-extension://${extId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 5000 });
      await sleep(600);
    } catch {}
    try { await waker.close(); } catch {}
  }
  throw new Error("SW target not wakable");
}

async function evalSW(browser, expr, arg) {
  const w = await getSWWorker(browser);
  if (arg === undefined) {
    return w.evaluate(new Function("return (async () => { " + expr + " })()"));
  }
  // Thread a single argument through so commands can pass bulky payloads
  // (e.g. whole script bodies to re-analyse) instead of string-interpolating.
  return w.evaluate(new Function("arg", "return (async () => { " + expr + " })()"), arg);
}

async function getExtId(browser) {
  const lock = await readLock();
  if (lock?.extId) return lock.extId;
  const t = browser.targets().find(t => t.url().startsWith("chrome-extension://"));
  if (!t) throw new Error("SW target not found");
  return new URL(t.url()).hostname;
}

async function getActivePage(browser) {
  const pages = await browser.pages();
  // Prefer the most recently focused non-extension tab.
  const nonExt = pages.filter(p => !p.url().startsWith("chrome-extension://") && !p.url().startsWith("devtools://"));
  if (nonExt.length === 0) return pages[0];
  // No reliable "focused" signal via CDP — take the last one, which puppeteer tends to update to most recent.
  return nonExt[nonExt.length - 1];
}

async function getPopupPage(browser) {
  const extId = await getExtId(browser);
  // puppeteer.connect only registers targets it witnesses opening, so
  // pages created before connect (including our popup) don't show up in
  // `browser.pages()`. Discover targets via raw CDP, then attach to the
  // popup target and return a CDP-session-backed adapter that mimics the
  // subset of `Page` the harness uses: evaluate, url, close.
  const pagesKnown = await browser.pages();
  const hit = pagesKnown.find(p => p.url().startsWith(`chrome-extension://${extId}/popup.html`));
  if (hit) return hit;

  const cdp = await browser.target().createCDPSession();
  let targetInfo;
  try {
    const { targetInfos } = await cdp.send("Target.getTargets");
    targetInfo = targetInfos.find(t => t.type === "page" && (t.url || "").startsWith(`chrome-extension://${extId}/popup.html`));
  } catch { /* fall through */ }
  if (!targetInfo) { try { await cdp.detach(); } catch {} return null; }

  // Attach flatly so we can talk directly.
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId: targetInfo.targetId, flatten: true });
  const connection = cdp.connection();

  // Minimal Page-like adapter — the harness only uses `evaluate(fn, ...args)`,
  // `url()`, and `close()`.
  return {
    _popupAdapter: true,
    _sessionId: sessionId,
    _targetId: targetInfo.targetId,
    url() { return targetInfo.url; },
    async evaluate(fn, ...args) {
      const expression = `((${fn.toString()}))(${args.map(a => JSON.stringify(a)).join(", ")})`;
      const res = await connection.send("Runtime.evaluate", {
        expression,
        awaitPromise: true,
        returnByValue: true,
        allowUnsafeEvalBlockedByCSP: true,
      }, sessionId);
      if (res.exceptionDetails) {
        const msg = res.exceptionDetails.exception?.description || res.exceptionDetails.text || "eval error";
        throw new Error(msg);
      }
      return res.result?.value;
    },
    async close() {
      try { await connection.send("Target.closeTarget", { targetId: targetInfo.targetId }); } catch {}
    },
  };
}

// ─── commands ──────────────────────────────────────────────────────────────

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

  const launched = await puppeteer.launch({
    headless: false,
    userDataDir: PROFILE_DIR,
    defaultViewport: null,
    args: [
      `--disable-extensions-except=${EXT_DIR}`,
      `--load-extension=${EXT_DIR}`,
      `--remote-debugging-port=${port}`,
      "--no-first-run",
      "--no-default-browser-check",
    ],
    handleSIGINT: false,
    handleSIGTERM: false,
    handleSIGHUP: false,
  });

  // Wait for the debug port to be reachable.
  for (let i = 0; i < 50; i++) {
    await sleep(200);
    const v = await pingPort(port);
    if (v) break;
  }

  // Capture the extension ID now while the SW target is fresh — stored
  // in the lock so every subsequent command can wake the SW even after
  // `reload` invalidates the target list.
  let extId = null;
  for (let i = 0; i < 50; i++) {
    const t = launched.targets().find(t => t.url().startsWith("chrome-extension://"));
    if (t) { try { extId = new URL(t.url()).hostname; break; } catch {} }
    await sleep(200);
  }

  await writeLock({ port, pid: process.pid, extId, startedAt: Date.now() });
  log(`started. pid=${process.pid} port=${port}. Leave this process running; use another shell or a second invocation of \`node testing/harness.js <cmd>\`.`);

  // Keep the browser (and this process) alive until the user issues `stop`
  // (or Ctrl+C).  puppeteer.launch's wrapper otherwise exits when the
  // script finishes — we explicitly keep a reference.
  const keepAlive = new Promise(() => {});
  process.on("SIGINT", async () => { await clearLock(); try { await launched.close(); } catch {} process.exit(0); });
  await keepAlive;
}

async function cmdStop() {
  const lock = await readLock();
  if (!lock) { log("not running"); return; }
  // Ask the browser to close via CDP — puppeteer connect then close works.
  try {
    const browser = await connect(lock.port);
    await browser.close();
  } catch (e) {
    log("browser already gone:", e.message);
  }
  await clearLock();
  // The launcher process stays alive (its `keepAlive` never resolves). Kill it.
  try { process.kill(lock.pid); } catch {}
  log("stopped");
}

async function cmdStatus() {
  const lock = await readLock();
  if (!lock) { log("not running"); return; }
  const ver = await pingPort(lock.port);
  if (!ver) { log(`lock exists but port ${lock.port} not responding`); return; }
  log(`running pid=${lock.pid} port=${lock.port}`);
  log(`  browser: ${ver.Browser}`);
  log(`  webSocketDebuggerUrl: ${ver.webSocketDebuggerUrl}`);
  await withBrowser(async (browser) => {
    const pages = await browser.pages();
    log(`  pages: ${pages.length}`);
    for (const p of pages.slice(0, 20)) log(`    - ${p.url()}`);
  });
}

async function cmdGoto(args) {
  const url = args[0];
  if (!url) throw new Error("usage: goto <url>");
  await withBrowser(async (browser) => {
    const pages = await browser.pages();
    // If no real content page exists, open one.
    let page = pages.find(p => !p.url().startsWith("chrome-extension://") && !p.url().startsWith("devtools://"));
    if (!page) page = await browser.newPage();
    try { await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 }); }
    catch (e) { log("navigation error (continuing):", e.message); }
    log(`→ ${page.url()}  (title: ${await page.title().catch(() => "?")})`);
  });
}

async function cmdTabs() {
  await withBrowser(async (browser) => {
    const pages = await browser.pages();
    for (let i = 0; i < pages.length; i++) {
      const p = pages[i];
      log(`${i}  ${p.url()}`);
    }
  });
}

async function cmdScripts(args) {
  const filter = args[0] || null;
  await withBrowser(async (browser) => {
    // Read AST analysis results. Each tab has `_astResults` with
    // { sourceUrl, protoEnums, protoFieldMaps, fetchCallSites, securitySinks, dangerousPatterns, sourceMapUrl }.
    const out = await evalSW(browser, `
      const rows = [];
      for (const [tabId, tab] of state.tabs.entries()) {
        for (const r of (tab._astResults || [])) {
          rows.push({
            tabId,
            url: r.sourceUrl || "(inline)",
            protoEnums: (r.protoEnums || []).length,
            protoFieldMaps: (r.protoFieldMaps || []).length,
            fetchSites: (r.fetchCallSites || []).length,
            secSinks: (r.securitySinks || []).length,
            dangerous: (r.dangerousPatterns || []).length,
            sourceMap: r.sourceMapUrl || null,
          });
        }
      }
      return rows;
    `);
    if (!out.length) { log("no scripts analysed yet"); return; }
    log(`${out.length} scripts analysed:`);
    out.forEach((r, i) => {
      if (filter && !r.url.includes(filter)) return;
      log(`${String(i).padStart(3)}  tab=${r.tabId} sink=${r.secSinks} danger=${r.dangerous} fetch=${r.fetchSites} fieldMaps=${r.protoFieldMaps} enums=${r.protoEnums} ${r.sourceMap ? "[srcmap]" : ""}  ${labelScript(r.url)}`);
    });
  });
}

async function resolveScriptUrl(browser, indexOrUrl) {
  if (/^https?:\/\//.test(indexOrUrl) || indexOrUrl === "(inline)") return indexOrUrl;
  const i = Number(indexOrUrl);
  if (Number.isNaN(i)) return indexOrUrl;
  const urls = await evalSW(browser, `
    const rows = [];
    for (const [tabId, tab] of state.tabs.entries()) {
      for (const r of (tab._astResults || [])) rows.push(r.sourceUrl || "(inline)");
    }
    return rows;
  `);
  return urls[i];
}

async function cmdScript(args) {
  const target = args[0];
  if (!target) throw new Error("usage: script <index|url>");
  await withBrowser(async (browser) => {
    const url = await resolveScriptUrl(browser, target);
    if (!url) { err("no such script"); return; }
    // Get the script source. The SW may still have it in `_scriptBuffers`,
    // but that clears on analysis completion — fetch from the page instead.
    const page = await getActivePage(browser);
    const src = await page.evaluate(async (u) => {
      try {
        const resp = await fetch(u, { credentials: "include" });
        return await resp.text();
      } catch (e) { return "__FETCH_ERR__: " + e.message; }
    }, url);
    if (src.startsWith("__FETCH_ERR__:")) { err(src); return; }
    await fsp.mkdir(DUMP_DIR, { recursive: true });
    const file = path.join(DUMP_DIR, (url.replace(/[^a-zA-Z0-9._-]/g, "_")).slice(-160) + ".js");
    await fsp.writeFile(file, src, "utf8");
    log(`source written: ${file}  (${src.length} chars)`);
    log("first 40 lines:");
    const lines = src.split("\n");
    for (let i = 0; i < Math.min(40, lines.length); i++) log(`${String(i + 1).padStart(5)}  ${lines[i].slice(0, 160)}`);
  });
}

async function cmdAst(rest) {
  const { positional, flags } = parseArgs(rest);
  const target = positional[0];
  if (!target) throw new Error("usage: ast <index|url> [--full]");
  const full = flags.full === true;
  await withBrowser(async (browser) => {
    const url = await resolveScriptUrl(browser, target);
    const result = await evalSW(browser, `
      for (const [tabId, tab] of state.tabs.entries()) {
        for (const r of (tab._astResults || [])) {
          if ((r.sourceUrl || "(inline)") === ${JSON.stringify(url)} || r.sourceUrl === ${JSON.stringify(url)}) {
            return r;
          }
        }
      }
      return null;
    `);
    if (!result) { err("no AST analysis for " + url); return; }
    log(`AST for ${labelScript(url)}`);

    log(`\n  proto enums: ${result.protoEnums.length}`);
    const enumLimit = full ? result.protoEnums.length : 10;
    for (const e of result.protoEnums.slice(0, enumLimit)) {
      const values = e.values || [];
      const shown = full ? values : values.slice(0, 8);
      log(`    ${e.name || "(unnamed)"}: ${shown.map(v => v.name + "=" + v.value).join(", ")}${!full && values.length > 8 ? `, … (${values.length - 8} more)` : ""}`);
    }

    log(`\n  proto field maps: ${result.protoFieldMaps.length}`);
    const mapLimit = full ? result.protoFieldMaps.length : 10;
    for (const m of result.protoFieldMaps.slice(0, mapLimit)) {
      const fields = m.fields || {};
      const keys = Object.keys(fields);
      const shown = full ? keys : keys.slice(0, 12);
      log(`    ${m.messageName || "(unnamed)"} (${keys.length} fields):`);
      for (const k of shown) {
        const v = fields[k];
        log(`      ${k} = ${v.number || "?"}  type=${v.type || "?"}${v.label ? " " + v.label : ""}`);
      }
      if (!full && keys.length > 12) log(`      … (${keys.length - 12} more)`);
    }

    log(`\n  fetch call sites: ${result.fetchCallSites.length}`);
    const fetchLimit = full ? result.fetchCallSites.length : 10;
    for (const f of result.fetchCallSites.slice(0, fetchLimit)) {
      log(`    [${f.callKind}] ${f.method || "?"} ${short(f.url || "<dynamic>", 100)}  L${f.loc?.start?.line || "?"}`);
      if (full) {
        if (f.headers) log(`      headers: ${JSON.stringify(f.headers).slice(0, 200)}`);
        if (f.body) log(`      body: ${short(JSON.stringify(f.body), 200)}`);
        if (f.queryParams) log(`      query: ${JSON.stringify(f.queryParams).slice(0, 200)}`);
      }
    }

    log(`\n  security sinks: ${result.securitySinks.length}`);
    for (const s of result.securitySinks) {
      const line = s.location?.line ?? s.loc?.start?.line ?? "?";
      log(`    [${s.severity}] ${s.type || "?"}→${s.sink || "?"}  L${line}  sourceType=${s.sourceType} source=${short(s.source || "-", 80)}`);
    }

    log(`\n  dangerous patterns: ${result.dangerousPatterns.length}`);
    for (const s of result.dangerousPatterns) {
      const line = s.location?.line ?? s.loc?.start?.line ?? "?";
      log(`    [${s.severity}] ${s.type || "?"}  L${line} — ${short(s.description || "", 120)}`);
    }

    if (result.sourceMapUrl) log(`\n  sourceMapUrl: ${result.sourceMapUrl}`);
    if (full && result.resolverErrors && result.resolverErrors.length) {
      log(`\n  resolverErrors: ${result.resolverErrors.length}`);
      for (const e of result.resolverErrors.slice(0, 5)) log(`    ${e.context}: ${short(e.message, 120)}`);
    }
  });
}

async function cmdAstRerun(args) {
  // Clear AST state in the SW (findings + cache + per-tab results) then
  // force a page reload so content.js re-intercepts scripts and the
  // normal capture pipeline runs with the CURRENT AST code. The buffered
  // scripts are drained after each analysis, so "rerun in place" isn't
  // possible without a reload — this models the real reload loop a
  // developer would trigger, minus the full Chrome restart.
  await withBrowser(async (browser) => {
    await evalSW(browser, `
      for (const [tid, tab] of state.tabs.entries()) tab._astResults = [];
      globalStore.securityFindings.clear();
      if (globalStore.scriptCache?.clear) globalStore.scriptCache.clear();
      return true;
    `);
    const page = await getActivePage(browser);
    const url = page.url();
    log(`reloading ${url} to re-trigger AST capture pipeline…`);
    try { await page.reload({ waitUntil: "domcontentloaded", timeout: 60000 }); }
    catch (e) { log("reload warning: " + e.message); }

    // Wait for scripts to populate the buffer, then fire-and-forget
    // _analyzeCombinedScripts. For large sites (Figma / Google / Discord
    // bundle ~1 MB through offscreen Babel), the analysis can take >60s
    // — well past puppeteer's per-evaluate protocol timeout. Don't await
    // the result; poll for AST rows to appear instead.
    await sleep(5000);
    try {
      await evalSW(browser, `
        for (const [tid, buf] of _scriptBuffers.entries()) {
          if (buf.timer) { clearTimeout(buf.timer); buf.timer = null; }
          if ((buf.scripts || []).length > 0) {
            // Don't await — just kick off. Analysis completion will
            // surface in tab._astResults when it's done.
            _analyzeCombinedScripts(tid);
          }
        }
        return true;
      `);
    } catch (e) { log("kick-off warning: " + (e.message || e)); }

    log("analysis kicked off; polling up to 5 min for results…");
    const deadline = Date.now() + 300000;
    let lastAst = 0;
    let stableTicks = 0;
    while (Date.now() < deadline) {
      await sleep(3000);
      let astCount = 0;
      try {
        astCount = await evalSW(browser, `
          let n = 0;
          for (const tab of state.tabs.values()) n += (tab._astResults || []).length;
          return n;
        `);
      } catch (_) { continue; }
      if (astCount > 0) {
        if (astCount === lastAst) { stableTicks++; if (stableTicks >= 2) break; }
        else stableTicks = 0;
        lastAst = astCount;
      }
    }
    const scripts = await evalSW(browser, `
      const rows = [];
      for (const [tid, tab] of state.tabs.entries()) {
        for (const r of (tab._astResults || [])) {
          rows.push({
            tabId: tid,
            url: r.sourceUrl || "(inline)",
            secSinks: (r.securitySinks || []).length,
            dangerous: (r.dangerousPatterns || []).length,
            fetchSites: (r.fetchCallSites || []).length,
          });
        }
      }
      return rows;
    `);
    if (!scripts.length) { log("no AST results after reload"); return; }
    log(`${scripts.length} scripts analysed:`);
    for (const r of scripts) log(`  sink=${r.secSinks} danger=${r.dangerous} fetch=${r.fetchSites}  ${labelScript(r.url)}`);
  });
}

async function cmdAstClear() {
  await withBrowser(async (browser) => {
    const res = await evalSW(browser, `
      for (const [tid, tab] of state.tabs.entries()) tab._astResults = [];
      globalStore.securityFindings.clear();
      if (globalStore.scriptCache?.clear) globalStore.scriptCache.clear();
      return { ok: true };
    `);
    log(JSON.stringify(res));
  });
}

async function cmdFindings(args) {
  const filterKind = args[0] || null;
  await withBrowser(async (browser) => {
    const rows = await evalSW(browser, `
      const out = [];
      for (const [sourceUrl, v] of globalStore.securityFindings) {
        for (const s of (v.securitySinks || [])) out.push({sourceUrl, category: "sink", ...s});
        for (const s of (v.dangerousPatterns || [])) out.push({sourceUrl, category: "dangerous", ...s});
      }
      return out;
    `);
    if (!rows.length) { log("no security findings"); return; }
    rows.forEach((f, i) => {
      const kind = f.category === "sink"
        ? `${f.type || "?"}:${f.sink || "?"}`
        : `${f.type || "pattern"}`;
      if (filterKind && !kind.toLowerCase().includes(filterKind.toLowerCase())) return;
      const line = f.location?.line ?? f.loc?.start?.line ?? "?";
      const desc = f.description || (f.category === "sink" ? `${f.sourceType}→${f.sink}` : "");
      log(`${String(i).padStart(3)} [${f.severity || "?"}] ${f.category}/${kind} L${line}  ${short(desc, 140)}`);
      log(`      ${labelScript(f.sourceUrl)}`);
    });
  });
}

async function cmdFinding(args) {
  const idx = Number(args[0]);
  if (Number.isNaN(idx)) throw new Error("usage: finding <index>");
  await withBrowser(async (browser) => {
    const f = await evalSW(browser, `
      const out = [];
      for (const [sourceUrl, v] of globalStore.securityFindings) {
        for (const s of (v.securitySinks || [])) out.push({sourceUrl, category: "sink", ...s});
        for (const s of (v.dangerousPatterns || [])) out.push({sourceUrl, category: "dangerous", ...s});
      }
      return out[${idx}] || null;
    `);
    if (!f) { err("no such finding"); return; }
    const line = f.location?.line ?? f.loc?.start?.line ?? null;
    const col = f.location?.column ?? f.loc?.start?.column ?? null;
    const kind = f.category === "sink"
      ? `${f.type || "?"} → ${f.sink || "?"}`
      : `${f.type || "pattern"}`;
    log(`[${f.severity || "?"}] ${f.category} / ${kind}`);
    log(`  source: ${labelScript(f.sourceUrl)}`);
    log(`  location: L${line ?? "?"} col ${col ?? "?"}`);
    if (f.category === "sink") {
      log(`  sourceType: ${f.sourceType || "?"}  source: ${short(f.source || "-", 200)}`);
      log(`  sanitized: ${!!f.sanitized}`);
    }
    if (f.description) log(`  description: ${f.description}`);
    if (f.codeContext) {
      log(`  codeContext:`);
      const ctx = String(f.codeContext);
      for (const l of ctx.split("\n").slice(0, 10)) log(`    ${l}`);
    }

    // Fetch the raw script. Try the page matching the script's origin
    // first so CORS cookies/headers are native; fall back to Node fetch.
    const pages = await browser.pages();
    let fetchFrom = null;
    try {
      const scriptOrigin = new URL(f.sourceUrl).origin;
      fetchFrom = pages.find(p => { try { return new URL(p.url()).origin === scriptOrigin; } catch { return false; } });
    } catch {}
    if (!fetchFrom) fetchFrom = await getActivePage(browser);
    if (f.sourceUrl && !f.sourceUrl.startsWith("data:") && line) {
      let src = await fetchFrom.evaluate(async (u) => {
        try { const r = await fetch(u, { credentials: "include" }); return await r.text(); } catch (e) { return "__ERR__" + e.message; }
      }, f.sourceUrl);
      // Node fallback (no credentials needed for public script URLs).
      if (src.startsWith("__ERR__")) {
        try {
          const resp = await new Promise((resolve, reject) => {
            const req = require("https").get(f.sourceUrl, res => {
              let data = []; res.on("data", c => data.push(c)); res.on("end", () => resolve(Buffer.concat(data).toString("utf8")));
            });
            req.on("error", reject); req.setTimeout(10000, () => req.destroy(new Error("timeout")));
          });
          src = resp;
        } catch (e) { src = "__ERR__" + e.message; }
      }
      if (!src.startsWith("__ERR__")) {
        const lines = src.split("\n");
        const targetLine = lines[line - 1] || "";
        // Minified bundles have one giant line. A line number alone is
        // useless — slice around the column so we actually see the
        // finding's region.
        if (targetLine.length > 500 && col != null) {
          const radius = 200;
          const cStart = Math.max(0, col - radius);
          const cEnd = Math.min(targetLine.length, col + radius);
          const snippet = targetLine.slice(cStart, cEnd);
          const relCol = col - cStart;
          log(`\n  raw JS L${line} col ${col}  [±${radius} chars]`);
          log(`     ${snippet}`);
          log(`     ${" ".repeat(relCol)}^`);
        } else {
          const start = Math.max(0, line - 11);
          const end = Math.min(lines.length, line + 10);
          log(`\n  raw JS L${start + 1}..L${end}:`);
          for (let i = start; i < end; i++) {
            const marker = (i + 1 === line) ? ">>" : "  ";
            log(`  ${marker} ${String(i + 1).padStart(5)}  ${lines[i].slice(0, 240)}`);
          }
        }
      } else {
        err(`  (couldn't fetch source: ${src})`);
      }
    } else if (f.sourceUrl?.startsWith("data:") && line) {
      // Inline data-URL script — decode and slice around the column.
      try {
        const u = new URL(f.sourceUrl);
        const body = decodeURIComponent(u.pathname.split(",").slice(1).join(","));
        const lines = body.split("\n");
        const targetLine = lines[line - 1] || "";
        if (targetLine.length > 500 && col != null) {
          const radius = 200;
          const cStart = Math.max(0, col - radius);
          const cEnd = Math.min(targetLine.length, col + radius);
          const snippet = targetLine.slice(cStart, cEnd);
          const relCol = col - cStart;
          log(`\n  inline L${line} col ${col}  [±${radius} chars]`);
          log(`     ${snippet}`);
          log(`     ${" ".repeat(relCol)}^`);
        } else {
          const start = Math.max(0, line - 11);
          const end = Math.min(lines.length, line + 10);
          log(`\n  inline L${start + 1}..L${end}:`);
          for (let i = start; i < end; i++) {
            const marker = (i + 1 === line) ? ">>" : "  ";
            log(`  ${marker} ${String(i + 1).padStart(5)}  ${lines[i].slice(0, 240)}`);
          }
        }
      } catch (e) { err("  (couldn't decode inline source: " + e.message + ")"); }
    }
  });
}

async function cmdLogs(rest) {
  const { positional, flags } = parseArgs(rest);
  // Filters: substring match on entire row, plus targeted fields.
  const sub = positional[0] || null;
  const svc = flags.service || null;
  const host = flags.host || null;
  const ct = flags.ct || null;
  const method = flags.method ? String(flags.method).toUpperCase() : null;
  const status = flags.status ? String(flags.status) : null;
  const onlyBodies = flags["with-body"] === true;
  const limit = flags.limit ? Number(flags.limit) : 0;
  await withBrowser(async (browser) => {
    const rows = await evalSW(browser, `
      const out = [];
      for (const [tabId, tab] of state.tabs.entries()) {
        for (const r of (tab.requestLog || [])) {
          out.push({
            id: r.id, tabId, url: r.url, method: r.method,
            status: r.status, service: r.service, methodId: r.methodId,
            ct: r.contentType || "",
            bodyLen: r.rawBodyB64 ? r.rawBodyB64.length : 0,
            respLen: r.responseBody ? r.responseBody.length : 0,
            timestamp: r.timestamp,
          });
        }
      }
      return out;
    `);
    if (!rows.length) { log("no captured requests yet"); return; }
    let shown = 0;
    rows.sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
    for (const r of rows) {
      if (sub && !JSON.stringify(r).toLowerCase().includes(sub.toLowerCase())) continue;
      if (svc && !(r.service || "").includes(svc)) continue;
      if (host && !(r.url || "").includes(host)) continue;
      if (ct && !(r.ct || "").toLowerCase().includes(ct.toLowerCase())) continue;
      if (method && r.method !== method) continue;
      if (status && String(r.status) !== status) continue;
      if (onlyBodies && r.bodyLen === 0) continue;
      if (limit && shown >= limit) break;
      shown++;
      log(`${r.id}  [${r.status}] ${r.method.padEnd(6)} ${short(r.url, 100)}`);
      log(`      svc=${r.service} method=${r.methodId || "-"}  ct=${r.ct}  body=${r.bodyLen}B  resp=${r.respLen}B`);
    }
    if (shown === 0) log(`(no match among ${rows.length} captured requests)`);
  });
}

async function cmdLog(rest) {
  const { positional, flags } = parseArgs(rest);
  const id = positional[0];
  if (!id) throw new Error("usage: log <reqId> [--decoded] [--save <dir>] [--full]");
  const wantDecoded = flags.decoded === true || flags.decoded === "true";
  const saveDir = typeof flags.save === "string" ? flags.save : (flags.save === true ? DUMP_DIR : null);
  const full = flags.full === true;
  await withBrowser(async (browser) => {
    const r = await evalSW(browser, `
      for (const [tabId, tab] of state.tabs.entries()) {
        for (const rec of (tab.requestLog || [])) {
          if (String(rec.id) === ${JSON.stringify(id)}) return { tabId, ...rec };
        }
      }
      return null;
    `);
    if (!r) { err("no such request"); return; }
    log(`id=${r.id} tab=${r.tabId}`);
    log(`  ${r.method} ${r.url}`);
    log(`  status=${r.status}  service=${r.service}  methodId=${r.methodId || "-"}`);
    log(`  request headers:`);
    for (const [k, v] of Object.entries(r.requestHeaders || {})) log(`    ${k}: ${short(v, 200)}`);
    if (r.rawBodyB64) {
      const body = Buffer.from(r.rawBodyB64, "base64").toString("utf8");
      log(`  request body (${body.length} chars):`);
      log(full || body.length <= 2000 ? body : body.slice(0, 2000) + "\n  … (truncated — use --full)");
    }
    log(`  response headers:`);
    for (const [k, v] of Object.entries(r.responseHeaders || {})) log(`    ${k}: ${short(v, 200)}`);
    if (r.responseBody) {
      const body = r.responseBase64 ? Buffer.from(r.responseBody, "base64").toString("utf8") : r.responseBody;
      log(`  response body (${body.length} chars):`);
      log(full || body.length <= 2000 ? body : body.slice(0, 2000) + "\n  … (truncated — use --full)");
    }

    if (wantDecoded) {
      const decoded = await evalSW(browser, decodedReqResp(id));
      log("\n─── decoded request ────────────────────────────────");
      log(JSON.stringify(decoded.request, null, 2));
      log("\n─── decoded response ───────────────────────────────");
      log(JSON.stringify(decoded.response, null, 2));
    }

    if (saveDir) {
      const dir = path.resolve(saveDir, id);
      await fsp.mkdir(dir, { recursive: true });
      await fsp.writeFile(path.join(dir, "meta.json"), JSON.stringify({
        id: r.id, tabId: r.tabId, url: r.url, method: r.method,
        status: r.status, service: r.service, methodId: r.methodId, contentType: r.contentType,
        mimeType: r.mimeType, frameId: r.frameId, timestamp: r.timestamp,
      }, null, 2), "utf8");
      await fsp.writeFile(path.join(dir, "request.headers.json"), JSON.stringify(r.requestHeaders || {}, null, 2), "utf8");
      await fsp.writeFile(path.join(dir, "response.headers.json"), JSON.stringify(r.responseHeaders || {}, null, 2), "utf8");
      if (r.rawBodyB64) await fsp.writeFile(path.join(dir, "request.body.bin"), Buffer.from(r.rawBodyB64, "base64"));
      if (r.responseBody) {
        const buf = r.responseBase64 ? Buffer.from(r.responseBody, "base64") : Buffer.from(r.responseBody, "utf8");
        await fsp.writeFile(path.join(dir, "response.body.bin"), buf);
      }
      if (wantDecoded) {
        const decoded = await evalSW(browser, decodedReqResp(id));
        await fsp.writeFile(path.join(dir, "decoded.json"), JSON.stringify(decoded, null, 2), "utf8");
      }
      log(`\nsaved to ${dir}`);
    }
  });
}

// Build the SW-eval expression that decodes a captured request + response
// using the protocol parsers already in the SW's global scope (imported by
// background.js). Covers batchexecute, JSPB f.req, protobuf, gRPC-Web,
// GraphQL, async-chunked, SSE, NDJSON, multipart, and plain JSON.
function decodedReqResp(id) {
  return `
    function toBytes(b64) { return base64ToUint8(b64); }
    function utf8(b64) { try { return new TextDecoder().decode(toBytes(b64)); } catch { return null; } }

    function decodeBody(text, bytes, ct, url, isResponse) {
      ct = (ct || "").toLowerCase();
      const out = { kind: "unknown", summary: null, tree: null, raw: null };
      if (!bytes) return out;
      if (text == null && ct.indexOf("protobuf") < 0 && ct.indexOf("grpc") < 0) {
        try { text = new TextDecoder().decode(bytes); } catch {}
      }

      // batchexecute request: form-urlencoded f.req
      if (!isResponse && text && url && url.indexOf("batchexecute") >= 0 && typeof parseBatchExecuteRequest === "function") {
        try {
          const calls = parseBatchExecuteRequest(text);
          if (calls && calls.length) {
            out.kind = "batchexecute-req";
            out.tree = calls.map(c => ({ rpcId: c.rpcId, data: c.data }));
            return out;
          }
        } catch (e) { out.raw = "batchexecute parse error: " + e.message; }
      }
      // batchexecute response
      if (isResponse && text && typeof parseBatchExecuteResponse === "function") {
        try {
          const resp = parseBatchExecuteResponse(text);
          if (resp && resp.calls && resp.calls.length) {
            out.kind = "batchexecute-resp";
            out.tree = resp.calls;
            return out;
          }
        } catch (e) {}
      }
      // async chunked response
      if (isResponse && text && typeof parseAsyncChunkedResponse === "function") {
        try {
          const chunks = parseAsyncChunkedResponse(text);
          if (chunks && chunks.length) { out.kind = "async-chunked"; out.tree = chunks; return out; }
        } catch {}
      }
      // gRPC-Web frames
      if (ct.indexOf("grpc-web") >= 0 && typeof parseGrpcWebFrames === "function") {
        try {
          const frames = parseGrpcWebFrames(bytes);
          if (frames) {
            out.kind = "grpc-web";
            out.tree = frames.frames.map(f => ({
              type: f.type,
              bytes: f.data ? f.data.length : 0,
              tree: f.type === "data" && f.data ? pbDecodeTree(f.data, 8) : null,
              trailerText: f.type === "trailers" ? f.data : null,
            }));
            return out;
          }
        } catch (e) { out.raw = "grpc-web parse: " + e.message; }
      }
      // pure protobuf
      if (ct.indexOf("protobuf") >= 0 || ct.indexOf("x-protobuffer") >= 0) {
        try {
          const tree = pbDecodeTree(bytes, 8);
          out.kind = "protobuf";
          out.tree = tree;
          return out;
        } catch (e) { out.raw = "protobuf parse: " + e.message; }
      }
      // SSE / NDJSON
      if (isResponse && typeof parseSSE === "function" && text && (ct.indexOf("event-stream") >= 0)) {
        try { const evs = parseSSE(text); if (evs && evs.length) { out.kind = "sse"; out.tree = evs; return out; } } catch {}
      }
      if (isResponse && typeof parseNDJSON === "function" && text && (ct.indexOf("ndjson") >= 0 || ct.indexOf("x-ndjson") >= 0)) {
        try { const recs = parseNDJSON(text); if (recs) { out.kind = "ndjson"; out.tree = recs; return out; } } catch {}
      }
      // multipart
      if (text && ct.startsWith("multipart/") && typeof parseMultipartBatch === "function") {
        try {
          const mp = isResponse ? parseMultipartBatch(text, ct) : (typeof parseMultipartBatchRequest === "function" ? parseMultipartBatchRequest(text, ct) : null);
          if (mp) { out.kind = "multipart"; out.tree = mp; return out; }
        } catch {}
      }
      // GraphQL
      if (text && ct.indexOf("json") >= 0) {
        if (!isResponse && typeof parseGraphQLRequest === "function") {
          try { const g = parseGraphQLRequest(text); if (g) { out.kind = "graphql-req"; out.tree = g; return out; } } catch {}
        }
        if (isResponse && typeof parseGraphQLResponse === "function") {
          try { const g = parseGraphQLResponse(text); if (g) { out.kind = "graphql-resp"; out.tree = g; return out; } } catch {}
        }
        // plain JSON
        try { out.kind = "json"; out.tree = JSON.parse(text); return out; } catch {}
      }
      // form-urlencoded
      if (text && ct.indexOf("x-www-form-urlencoded") >= 0) {
        const kv = {};
        for (const [k, v] of new URLSearchParams(text)) kv[k] = v;
        // Try f.req JSPB
        if (kv["f.req"]) {
          try { kv["__f.req__decoded"] = JSON.parse(kv["f.req"]); } catch {}
        }
        out.kind = "form-urlencoded";
        out.tree = kv;
        return out;
      }
      out.kind = "text";
      out.raw = text || ("<" + bytes.length + " binary bytes>");
      return out;
    }

    let target = null;
    for (const [tabId, tab] of state.tabs.entries()) {
      for (const rec of (tab.requestLog || [])) {
        if (String(rec.id) === ${JSON.stringify(id)}) { target = rec; break; }
      }
      if (target) break;
    }
    if (!target) return { error: "not found" };

    const reqBytes = target.rawBodyB64 ? toBytes(target.rawBodyB64) : null;
    const reqText = target.rawBodyB64 ? utf8(target.rawBodyB64) : null;
    const respBytes = target.responseBody
      ? (target.responseBase64 ? toBytes(target.responseBody) : new TextEncoder().encode(target.responseBody))
      : null;
    const respText = target.responseBody
      ? (target.responseBase64 ? utf8(target.responseBody) : target.responseBody)
      : null;
    const reqCt = target.contentType || "";
    const respCt = (target.responseHeaders && (target.responseHeaders["content-type"] || target.responseHeaders["Content-Type"])) || target.mimeType || "";
    return {
      request: decodeBody(reqText, reqBytes, reqCt, target.url, false),
      response: decodeBody(respText, respBytes, respCt, target.url, true),
    };
  `;
}

async function cmdKeys() {
  await withBrowser(async (browser) => {
    const rows = await evalSW(browser, `
      const out = [];
      for (const [key, data] of globalStore.apiKeys) {
        out.push({
          key,
          name: data.name || null,
          source: data.source || null,
          hosts: [...(data.hosts instanceof Set ? data.hosts : data.hosts || [])],
          services: [...(data.services instanceof Set ? data.services : data.services || [])],
        });
      }
      return out;
    `);
    if (!rows.length) { log("no keys learned yet"); return; }
    for (const k of rows) {
      log(`${short(k.key, 40)}  name=${k.name || "-"}  source=${k.source || "-"}`);
      log(`  hosts: ${k.hosts.join(", ") || "-"}`);
      log(`  services: ${k.services.slice(0, 4).join(", ") || "-"}`);
    }
  });
}

async function cmdServices(args) {
  const filter = args[0] || null;
  await withBrowser(async (browser) => {
    const rows = await evalSW(browser, `
      const out = [];
      for (const [svc, e] of globalStore.discoveryDocs) {
        if (e.status !== "found" || !e.doc) continue;
        const methods = [];
        for (const bObj of Object.values(e.doc.resources || {})) {
          for (const m of Object.values(bObj.methods || {})) methods.push(m);
        }
        out.push({ svc, isVirtual: !!e.isVirtual, title: e.doc.title, methodCount: methods.length, sampleMethods: methods.slice(0, 3).map(m => m.id) });
      }
      return out;
    `);
    for (const r of rows) {
      if (filter && !r.svc.includes(filter)) continue;
      log(`${r.svc}   methods=${r.methodCount}${r.isVirtual ? " (virtual)" : ""}`);
      for (const m of r.sampleMethods) log(`    ${m}`);
    }
  });
}

async function cmdService(rest) {
  const { positional, flags } = parseArgs(rest);
  const name = positional[0];
  if (!name) throw new Error("usage: service <name> [--methods] [--schemas]");
  const full = flags.full === true;
  await withBrowser(async (browser) => {
    const data = await evalSW(browser, `
      const e = globalStore.discoveryDocs.get(${JSON.stringify(name)});
      if (!e || !e.doc) return null;
      const doc = e.doc;
      const methods = [];
      for (const [bName, bObj] of Object.entries(doc.resources || {})) {
        for (const [mName, m] of Object.entries(bObj.methods || {})) {
          const paramList = [];
          for (const [pName, pDef] of Object.entries(m.parameters || {})) {
            paramList.push({
              name: pDef.name || pName,
              type: pDef.type || "?",
              location: pDef.location || "?",
              required: !!pDef.required,
              enum: pDef.enum || null,
              _astValidValues: pDef._astValidValues || null,
              _detectedEnum: !!pDef._detectedEnum,
              _defaultValue: pDef._defaultValue ?? null,
              _requiredConfidence: pDef._requiredConfidence ?? null,
              _range: pDef._range || null,
            });
          }
          methods.push({
            id: m.id,
            bucket: bName,
            name: mName,
            path: m.path,
            origin: m.origin,
            httpMethod: m.httpMethod,
            description: m.description || null,
            contentTypes: m.contentTypes || [],
            requestRef: m.request?.$ref || null,
            responseRef: m.response?.$ref || null,
            scopes: m.scopes || [],
            params: paramList,
            requestCount: m._stats?.requestCount || 0,
          });
        }
      }
      const schemas = {};
      for (const [schName, sch] of Object.entries(doc.schemas || {})) {
        const props = Object.keys(sch.properties || {});
        schemas[schName] = {
          type: sch.type,
          propertyCount: props.length,
          // Don't let the property's "name" field (an alias when set via
          // RENAME_FIELD) shadow the actual wire key. Surface wireKey so
          // readers see the canonical identifier and alias when present.
          properties: props.slice(0, 30).map(p => {
            const def = sch.properties[p] || {};
            return {
              wireKey: p,
              alias: def.customName && def.name ? def.name : null,
              type: def.type || def.$ref || null,
              number: def.number ?? null,
              label: def.label || null,
            };
          }),
        };
      }
      const keys = [];
      for (const [k, data] of globalStore.apiKeys) {
        if (data.services?.has(${JSON.stringify(name)})) keys.push({ key: k, source: data.source, name: data.name });
      }
      return {
        svc: ${JSON.stringify(name)},
        title: doc.title,
        isVirtual: !!e.isVirtual,
        method: e.method,
        url: e.url,
        rootUrl: doc.rootUrl,
        methods,
        schemas,
        keys,
      };
    `);
    if (!data) { err("no such service"); return; }
    log(`${data.svc}${data.isVirtual ? "  (virtual)" : ""}`);
    log(`  title: ${data.title || "-"}`);
    log(`  root: ${data.rootUrl || "-"}`);
    log(`  discovered via: ${data.method || "-"}${data.url ? " " + short(data.url, 80) : ""}`);
    log(`  api keys known for this service: ${data.keys.length}`);
    for (const k of data.keys) log(`    ${short(k.key, 40)} (source=${k.source || "-"})`);

    log(`\n  methods (${data.methods.length}):`);
    for (const m of data.methods) {
      log(`    ${m.httpMethod} ${m.origin || ""}/${m.path || ""}  [${m.bucket}/${m.name}]`);
      log(`      id=${m.id}  reqCount=${m.requestCount}  req=${m.requestRef || "-"}  resp=${m.responseRef || "-"}  ct=${(m.contentTypes||[]).join(",") || "-"}`);
      if (m.description) log(`      description: ${short(m.description, 160)}`);
      if (m.scopes?.length) log(`      scopes: ${m.scopes.join(", ")}`);
      for (const p of m.params) {
        const badges = [];
        if (p.required) badges.push("required");
        if (p._detectedEnum) badges.push("enum-detected");
        if (p._defaultValue != null) badges.push(`default=${p._defaultValue}`);
        if (p._requiredConfidence != null) badges.push(`seen ${Math.round(p._requiredConfidence * 100)}%`);
        if (p._range) badges.push(`range[${p._range.min}..${p._range.max}]`);
        if (Array.isArray(p._astValidValues) && p._astValidValues.length) badges.push(`ast=[${p._astValidValues.slice(0, 4).map(v => JSON.stringify(v)).join(",")}${p._astValidValues.length > 4 ? ",…" : ""}]`);
        if (Array.isArray(p.enum) && p.enum.length) badges.push(`enum=[${p.enum.slice(0, 4).join(",")}${p.enum.length > 4 ? ",…" : ""}]`);
        log(`      · ${p.name}: ${p.type} @${p.location}${badges.length ? "  [" + badges.join(", ") + "]" : ""}`);
      }
    }

    if (full) {
      log(`\n  schemas (${Object.keys(data.schemas).length}):`);
      for (const [sn, s] of Object.entries(data.schemas)) {
        log(`    ${sn} (${s.propertyCount} props):`);
        for (const p of s.properties) {
          const aliasTag = p.alias ? `  (alias: "${p.alias}")` : "";
          log(`      · ${p.wireKey}: ${p.type || "?"}${p.number != null ? " #" + p.number : ""}${p.label ? " " + p.label : ""}${aliasTag}`);
        }
      }
    } else if (Object.keys(data.schemas).length) {
      log(`\n  schemas: ${Object.keys(data.schemas).length} (use --full for details)`);
    }
  });
}

async function cmdPopup() {
  await withBrowser(async (browser) => {
    let popup = await getPopupPage(browser);
    if (!popup) {
      const extId = await getExtId(browser);
      popup = await browser.newPage();
      await popup.goto(`chrome-extension://${extId}/popup.html`, { waitUntil: "domcontentloaded" });
      await sleep(1200);
      // Switch to "all tabs" so cross-tab replay works.
      await popup.evaluate(async () => {
        const sel = document.getElementById("log-tab-filter");
        if (sel) { sel.value = "all"; sel.dispatchEvent(new Event("change", { bubbles: true })); }
      });
      await sleep(600);
    }
    log(`popup open: ${popup.url()}`);
  });
}

async function cmdPopupSelect(args) {
  const reqId = args[0];
  if (!reqId) throw new Error("usage: popup.select <reqId>");
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open — run `popup` first"); return; }

    // Discover the origin tab+frame for this request via the SW so the
    // popup can replay it from the right context. The popup's own
    // `allTabsData` / `tabData` are let-scoped (not on window), so
    // querying the popup for them always comes back undefined; the SW
    // holds the authoritative capture.
    const ctx = await evalSW(browser, `
      for (const [tid, tab] of state.tabs.entries()) {
        for (const r of (tab.requestLog || [])) {
          if (String(r.id) === ${JSON.stringify(reqId)}) {
            return { tabId: tid, frameId: r.frameId ?? 0, found: true };
          }
        }
      }
      return { found: false };
    `);
    if (!ctx.found) { err("request not found in any captured tab"); return; }

    // Switch filter=all and refresh so the popup's internal allTabsData
    // populates for cross-tab replay. Dispatch a real change event so the
    // popup's own handler runs (loadRequestLog → GET_ALL_LOGS).
    await popup.evaluate(async () => {
      const sel = document.getElementById("log-tab-filter");
      if (sel && sel.value !== "all") {
        sel.value = "all";
        sel.dispatchEvent(new Event("change", { bubbles: true }));
      }
      // Allow the change handler to finish.
      await new Promise(r => setTimeout(r, 800));
    });

    const result = await popup.evaluate(async (id, sourceTabId) => {
      if (typeof window.replayRequest !== "function") return { ok: false, detail: "replayRequest not on window" };
      try { await window.replayRequest(id, sourceTabId); } catch (e) { return { ok: false, detail: "threw " + e.message }; }
      for (let i = 0; i < 60; i++) {
        await new Promise(r => setTimeout(r, 100));
        const f = document.getElementById("send-form-fields");
        const raw = document.getElementById("send-raw-body");
        if ((f && f.childElementCount > 0 && !f.innerHTML.includes("Loading schema")) || (raw && raw.value?.length > 0)) break;
      }
      return {
        ok: true,
        sourceTabId,
        epSelected: document.getElementById("send-ep-select")?.value || null,
        epCount: document.querySelectorAll("#send-ep-select option").length,
      };
    }, reqId, ctx.tabId);
    log(JSON.stringify({ ...result, resolvedFrom: ctx }, null, 2));
  });
}

async function cmdPopupDebug() {
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    const d = await popup.evaluate(() => {
      const all = window.allTabsData || {};
      return {
        filter: window.logFilter,
        currentTabId: window.currentTabId,
        allTabsKeys: Object.keys(all),
        allTabsCounts: Object.fromEntries(Object.entries(all).map(([k, v]) => [k, v?.requestLog?.length || 0])),
        tabDataLogCount: window.tabData?.requestLog?.length || 0,
        tabDataDiscovery: Object.keys(window.tabData?.discoveryDocs || {}).length,
      };
    });
    log(JSON.stringify(d, null, 2));
  });
}

async function cmdPopupForm() {
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open — run `popup` first"); return; }
    const state = await popup.evaluate(async () => {
      const panels = [
        ["form", "send-form-fields"], ["raw", "send-raw-body"],
        ["graphql", "send-graphql-fields"], ["multipart", "send-multipart-fields"],
        ["msgconsole", "send-ws-console"],
      ];
      let mode = null;
      for (const [m, id] of panels) {
        const el = document.getElementById(id);
        if (el && !el.classList.contains("hidden") && el.style.display !== "none") { mode = m; break; }
      }
      const fields = [];
      function walk(wrappers, depth) {
        for (const w of wrappers) {
          const input = w.querySelector(":scope > .form-input");
          let val = null;
          if (input) val = input.type === "checkbox" ? input.checked : input.value;
          fields.push({
            depth,
            name: w.dataset.name,
            type: w.dataset.type,
            number: w.dataset.number || null,
            location: w.dataset.location || null,
            value: val === null ? null : String(val),
            hasAstChips: !!w.querySelector(":scope > .field-ast-values"),
            badges: Array.from(w.querySelectorAll(":scope > .form-field-label .field-stat, :scope > .form-field-label .field-required, :scope > .form-field-label .field-repeated")).map(b => b.textContent.trim()),
          });
          const kids = w.querySelectorAll(":scope > .form-message-group > .form-message-children > .form-field, :scope > .form-repeated-list.form-repeated-message-list > .form-repeated-item > .form-message-children > .form-field");
          if (kids.length) walk(kids, depth + 1);
        }
      }
      walk(document.querySelectorAll("#send-form-fields > .form-section > .form-field"), 0);
      const ep = document.getElementById("send-ep-select");
      const sel = ep?.options?.[ep.selectedIndex] || null;
      return {
        bodyMode: mode,
        endpointSelected: sel ? { text: sel.textContent, svc: sel.dataset.svc, discoveryId: sel.dataset.discoveryId } : null,
        endpointCount: ep ? Array.from(ep.options).filter(o => o.value).length : 0,
        url: document.getElementById("send-url")?.value || document.getElementById("send-url")?.textContent || null,
        fieldCount: fields.length,
        fields,
        rawBody: {
          visible: document.getElementById("send-raw-body") && !document.getElementById("send-raw-body").classList.contains("hidden"),
          len: (document.getElementById("send-raw-body")?.value || "").length,
          sample: (document.getElementById("send-raw-body")?.value || "").slice(0, 200),
        },
      };
    });
    log(`bodyMode: ${state.bodyMode}`);
    log(`endpoint: ${state.endpointSelected?.text || "(none)"}  [${state.endpointCount} options]`);
    log(`URL: ${state.url || "(n/a)"}`);
    log(`fields: ${state.fieldCount}`);
    for (const f of state.fields) {
      const pad = "  ".repeat(f.depth);
      const badges = f.badges.length ? ` [${f.badges.join(", ")}]` : "";
      const ast = f.hasAstChips ? " 🔎ast" : "";
      log(`${pad}- ${f.name}: ${f.type}${f.number ? " #" + f.number : ""}${f.location ? " @" + f.location : ""} = ${short(f.value ?? "(empty)", 80)}${badges}${ast}`);
    }
    if (state.rawBody?.visible) log(`raw body textarea visible (${state.rawBody.len} chars): ${short(state.rawBody.sample, 140)}`);
  });
}

async function cmdPopupFill(args) {
  const name = args[0];
  const value = args.slice(1).join(" ");
  if (!name) throw new Error("usage: popup.fill <fieldName> <value>");
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    const out = await popup.evaluate((fieldName, val) => {
      // Prefer the GraphQL variables tree (the contextual editor for
      // GraphQL requests) when both it and the body form have a field
      // with the same name — otherwise a user filling `token` would
      // silently update the hidden body-form copy and the GQL send would
      // still use the captured value.
      const inGqlTree = Array.from(document.querySelectorAll(`.gql-variables-tree .form-field[data-name="${fieldName}"]`));
      const inMultipart = Array.from(document.querySelectorAll(`#send-multipart-fields .form-field[data-name="${fieldName}"]`));
      const inFormBody = Array.from(document.querySelectorAll(`#send-form-fields .form-field[data-name="${fieldName}"]`));
      const wrappers = inGqlTree.length ? inGqlTree : (inMultipart.length ? inMultipart : inFormBody);
      if (!wrappers.length) return { ok: false, detail: "no field named " + fieldName };
      const w = wrappers[0];
      const scope = inGqlTree.length ? "gql" : (inMultipart.length ? "multipart" : "form");
      const input = w.querySelector(":scope > .form-input");
      if (!input) return { ok: false, detail: "field has no direct input (might be nested message)" };
      if (input.type === "checkbox") input.checked = (val === "true" || val === "1");
      else input.value = val;
      input.dispatchEvent(new Event("input", { bubbles: true }));
      input.dispatchEvent(new Event("change", { bubbles: true }));
      return { ok: true, scope, newValue: input.type === "checkbox" ? input.checked : input.value };
    }, name, value);
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdPopupSend() {
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    const out = await popup.evaluate(async () => {
      const btn = document.getElementById("btn-send");
      if (!btn) return { ok: false, detail: "no #btn-send" };
      btn.click();
      for (let i = 0; i < 200; i++) {
        await new Promise(r => setTimeout(r, 100));
        if (!btn.disabled) break;
      }
      return { ok: true };
    });
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdPopupRename(args) {
  const name = args[0];
  const newName = args.slice(1).join(" ");
  if (!name || !newName) throw new Error("usage: popup.rename <fieldName> <newName>");
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    const result = await popup.evaluate(async (n, v) => {
      const origPrompt = window.prompt;
      window.prompt = () => v;
      try {
        // Prefer a field inside the GraphQL variables tree when there is
        // one — that's the contextual GUI we want to exercise. Fall back
        // to any renameable wrapper elsewhere in the popup.
        const preferred = Array.from(document.querySelectorAll(`.gql-variables-tree .form-field[data-name="${n}"]`));
        const fallback = Array.from(document.querySelectorAll(`#send-form-fields .form-field[data-name="${n}"]`));
        const wrappers = preferred.length ? preferred : fallback;
        if (!wrappers.length) return { ok: false, detail: "no field named " + n };
        const btn = wrappers[0].querySelector(".btn-rename");
        if (!btn) return { ok: false, detail: "field has no rename button (not a learned/probed field)" };
        const origLabel = btn.previousElementSibling?.textContent?.trim() || null;
        const searchRoot = preferred.length ? ".gql-variables-tree" : "#send-form-fields";
        btn.click();
        for (let i = 0; i < 40; i++) {
          await new Promise(r => setTimeout(r, 100));
          const labels = Array.from(document.querySelectorAll(`${searchRoot} .field-name`)).map(e => e.textContent.trim());
          if (labels.includes(v)) return { ok: true, oldName: origLabel, newName: v, scope: searchRoot };
        }
        return { ok: false, detail: "rename did not appear in " + searchRoot + " within 4s" };
      } finally { window.prompt = origPrompt; }
    }, name, newName);
    log(JSON.stringify(result, null, 2));
  });
}

async function cmdPopupKey(args) {
  const choice = args[0];
  if (!choice) throw new Error("usage: popup.key <auto|<index>|custom=<value>|disable>");
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    const out = await popup.evaluate(async (c) => {
      const radios = Array.from(document.querySelectorAll("#send-key-options input[type='radio']"));
      if (!radios.length) return { ok: false, detail: "no key selector (no keys known for this service)" };
      const findRadio = (val) => radios.find(r => r.value === val);
      if (c === "auto") {
        const r = findRadio("auto"); if (!r) return { ok: false, detail: "no 'auto' option" };
        r.checked = true; r.dispatchEvent(new Event("change", { bubbles: true }));
        return { ok: true, chose: "auto" };
      }
      if (c === "disable") {
        const r = findRadio("disabled"); if (!r) return { ok: false, detail: "no 'disabled' option" };
        r.checked = true; r.dispatchEvent(new Event("change", { bubbles: true }));
        return { ok: true, chose: "disabled" };
      }
      if (c.startsWith("custom=")) {
        const custom = c.slice("custom=".length);
        const r = findRadio("custom"); if (!r) return { ok: false, detail: "no 'custom' option" };
        r.checked = true; r.dispatchEvent(new Event("change", { bubbles: true }));
        const input = document.getElementById("send-key-custom");
        if (input) { input.value = custom; input.dispatchEvent(new Event("input", { bubbles: true })); }
        return { ok: true, chose: "custom", value: custom };
      }
      const idx = Number(c);
      if (!Number.isNaN(idx)) {
        const r = findRadio(`key-${idx}`); if (!r) return { ok: false, detail: `no key-${idx} option` };
        r.checked = true; r.dispatchEvent(new Event("change", { bubbles: true }));
        return { ok: true, chose: `key-${idx}`, valueTruncated: r.closest(".key-option")?.querySelector(".key-value-truncated")?.textContent };
      }
      return { ok: false, detail: "unrecognised choice" };
    }, choice);
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdPopupGql(args) {
  const action = args[0] || "state";
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    if (action === "state") {
      const st = await popup.evaluate(() => {
        function walkTree(container, depth) {
          const out = [];
          for (const w of container.querySelectorAll(":scope > .form-field")) {
            const input = w.querySelector(":scope > .form-input");
            let val = null;
            if (input) val = input.type === "checkbox" ? input.checked : input.value;
            const childContainer = w.querySelector(":scope > .form-message-group > .form-message-children");
            const children = childContainer && depth < 4 ? walkTree(childContainer, depth + 1) : null;
            out.push({
              name: w.dataset.name,
              type: w.dataset.type,
              label: w.dataset.label || null,
              value: val === null ? null : String(val).slice(0, 200),
              displayName: w.querySelector(":scope > .form-field-label .field-name")?.textContent?.trim(),
              hasRename: !!w.querySelector(":scope > .form-field-label .btn-rename"),
              aliasActive: w.querySelector(":scope > .form-field-label .field-name")?.textContent?.trim() !== w.dataset.name,
              children,
            });
          }
          return out;
        }
        const panels = Array.from(document.querySelectorAll("#gql-op-container .gql-op-panel"));
        const active = document.querySelector("#gql-op-container .gql-op-panel.active") || panels[0];
        return {
          opCount: panels.length,
          activeIdx: panels.indexOf(active),
          ops: panels.map(p => {
            const treeContainer = p.querySelector(".gql-variables-tree");
            return {
              idx: Number(p.dataset.gqlIdx),
              operationName: p.querySelector(".gql-opname")?.value || null,
              operation: p.querySelector(".gql-operation")?.value || null,
              queryLen: (p.querySelector(".gql-query")?.value || "").length,
              querySample: (p.querySelector(".gql-query")?.value || "").slice(0, 120),
              variablesRaw: (p.querySelector(".gql-variables")?.value || ""),
              variablesTree: treeContainer ? walkTree(treeContainer, 0) : null,
              variablesTreeFieldCount: treeContainer ? treeContainer.querySelectorAll(".form-field").length : 0,
              extensions: (p.querySelector(".gql-extensions")?.value || ""),
            };
          }),
        };
      });
      log(JSON.stringify(st, null, 2));
      return;
    }
    if (action === "set") {
      const idx = Number(args[1] || 0);
      const field = args[2];
      const value = args.slice(3).join(" ");
      if (!field) throw new Error("usage: popup.gql set <opIdx> <query|variables|operationName|operation|extensions> <value>");
      const out = await popup.evaluate((i, f, v) => {
        const panel = document.querySelector(`#gql-op-container .gql-op-panel[data-gql-idx="${i}"]`);
        if (!panel) return { ok: false, detail: "no panel at idx " + i };
        const sel = { query: ".gql-query", variables: ".gql-variables", operationName: ".gql-opname", operation: ".gql-operation", extensions: ".gql-extensions" }[f];
        if (!sel) return { ok: false, detail: "unknown field " + f };
        const el = panel.querySelector(sel);
        if (!el) return { ok: false, detail: "field element missing" };
        el.value = v;
        el.dispatchEvent(new Event("input", { bubbles: true }));
        el.dispatchEvent(new Event("change", { bubbles: true }));
        return { ok: true, newValue: el.value };
      }, idx, field, value);
      log(JSON.stringify(out, null, 2));
      return;
    }
    throw new Error("usage: popup.gql [state|set <opIdx> <field> <value>]");
  });
}

async function cmdPopupMp(args) {
  const action = args[0] || "state";
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    if (action === "state") {
      const st = await popup.evaluate(() => {
        const parts = Array.from(document.querySelectorAll("#send-multipart-fields .mp-part"));
        return {
          partCount: parts.length,
          parts: parts.map((p, idx) => ({
            idx,
            contentType: p.querySelector(".mp-ct")?.value || p.querySelector("[data-mp-ct]")?.textContent || null,
            contentId: p.querySelector(".mp-cid")?.value || null,
            bodyLen: (p.querySelector(".mp-body")?.value || p.querySelector("textarea")?.value || "").length,
            bodySample: (p.querySelector(".mp-body")?.value || p.querySelector("textarea")?.value || "").slice(0, 120),
          })),
        };
      });
      log(JSON.stringify(st, null, 2));
      return;
    }
    if (action === "set") {
      const idx = Number(args[1] || 0);
      const field = args[2];
      const value = args.slice(3).join(" ");
      if (!field) throw new Error("usage: popup.mp set <partIdx> <ct|body|contentId> <value>");
      const out = await popup.evaluate((i, f, v) => {
        const parts = Array.from(document.querySelectorAll("#send-multipart-fields .mp-part"));
        const p = parts[i];
        if (!p) return { ok: false, detail: "no part " + i };
        const sel = { ct: ".mp-ct", body: ".mp-body, textarea", contentId: ".mp-cid" }[f];
        if (!sel) return { ok: false, detail: "unknown field " + f };
        const el = p.querySelector(sel);
        if (!el) return { ok: false, detail: "element missing" };
        el.value = v;
        el.dispatchEvent(new Event("input", { bubbles: true }));
        el.dispatchEvent(new Event("change", { bubbles: true }));
        return { ok: true, newValue: el.value };
      }, idx, field, value);
      log(JSON.stringify(out, null, 2));
      return;
    }
    throw new Error("usage: popup.mp [state|set <partIdx> <field> <value>]");
  });
}

async function cmdPopupResponse(rest) {
  const { flags } = parseArgs(rest || []);
  const full = flags.full === true;
  await withBrowser(async (browser) => {
    const popup = await getPopupPage(browser);
    if (!popup) { err("popup not open"); return; }
    const out = await popup.evaluate(async (doFull) => {
      const container = document.getElementById("send-response");
      if (!container || container.style.display === "none") return { displayed: false };
      const status = document.getElementById("send-response-status")?.textContent?.trim() || "";
      const headersTable = document.getElementById("send-response-headers");
      const headers = headersTable ? Array.from(headersTable.querySelectorAll("tr")).slice(0, 40).map(r => {
        const c = r.querySelectorAll("td");
        return c.length >= 2 ? { name: c[0].textContent.trim(), value: c[1].textContent.trim() } : null;
      }).filter(Boolean) : [];
      const body = document.getElementById("send-response-body");

      // Walk the rendered pb-tree / JSON-tree so callers can see the
      // actual field names and values the popup decoded, not just a
      // classifier bucket. Each .pb-field in the renderer carries data-*
      // attributes and labels we can scrape.
      function walkTree(root, depth) {
        if (!root || depth > 6) return null;
        const nodes = [];
        const fields = root.querySelectorAll(":scope > .pb-field, :scope > .pb-container > .pb-field, :scope > .pb-children > .pb-field");
        for (const f of fields) {
          const name = f.querySelector(":scope > .pb-field-header .pb-field-name")?.textContent?.trim()
            || f.querySelector(":scope > .pb-field-header .field-name")?.textContent?.trim()
            || f.dataset.fieldName || null;
          const valueEl = f.querySelector(":scope > .pb-field-value, :scope > .pb-field-body, :scope > .pb-scalar");
          const value = valueEl ? valueEl.textContent.trim().slice(0, 200) : null;
          const children = walkTree(f, depth + 1);
          nodes.push({ name, value, children: children && children.length ? children : null });
        }
        return nodes;
      }

      const classes = [];
      if (body?.querySelector(".pb-tree, .pb-container")) classes.push("pb-tree");
      if (body?.querySelector("pre.resp-body, pre.resp-body-scroll-sm")) classes.push("pre-text-blob");
      if (body?.querySelector("#btn-download-response")) classes.push("binary-download");
      const rawFromState = (window.lastSendResult?.body?.raw) || null;
      const preText = body?.querySelector("pre.resp-body, pre.resp-body-scroll-sm")?.textContent || null;

      let tree = null;
      const pbRoot = body?.querySelector(".pb-tree, .pb-container");
      if (pbRoot) {
        tree = walkTree(pbRoot, 0);
      }

      return {
        displayed: true,
        status,
        headers,
        bodyHtmlLen: body?.innerHTML?.length || 0,
        bodyTextSample: (body?.textContent || "").trim().slice(0, doFull ? 5000 : 400),
        renderClasses: classes,
        preText: preText ? preText.slice(0, doFull ? 20000 : 400) : null,
        tree,
        rawResponseSample: rawFromState ? String(rawFromState).slice(0, doFull ? 20000 : 400) : null,
      };
    }, full);
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdPage(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: page <js-expression>");
  await withBrowser(async (browser) => {
    const page = await getActivePage(browser);
    const out = await page.evaluate(new Function("return (async () => { " + expr + " })()"));
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdSw(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: sw <js-expression>");
  await withBrowser(async (browser) => {
    const out = await evalSW(browser, expr);
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdReload() {
  // chrome.runtime.reload() triggers a full extension reload (SW + all
  // pages). Tabs keep their URLs; content scripts get re-injected on the
  // next navigation (or we can refresh tabs manually). This lets code
  // changes take effect without restarting the whole Chrome session.
  await withBrowser(async (browser) => {
    const oldExtId = await getExtId(browser).catch(() => null);
    try {
      await evalSW(browser, "chrome.runtime.reload(); return true;");
    } catch (_) { /* expected — SW terminates before returning */ }
    // Poll for a fresh SW target — wake via popup.html if dormant.
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline) {
      await sleep(300);
      try { await getSWWorker(browser); log("extension reloaded"); return; } catch {}
    }
    log(`reload fired — couldn't confirm a live SW within 10s (extId=${oldExtId})`);
  });
}

async function cmdHelp() {
  log(`
harness commands
  start [port]                 launch Chrome; keep running
  stop                         close Chrome
  status                       lock state + pages

  reload                       chrome.runtime.reload() — picks up extension code edits
  goto <url>                   navigate active tab
  tabs                         list tabs
  page <expr>                  eval JS in active page, return result

  scripts [substring]          AST-analysed scripts
  script <index|url>           dump raw JS of a script (to testing/harness-dumps/)
  ast <index|url> [--full]     AST findings (enums/fieldMaps/fetch/sinks). --full = verbose
  ast.rerun                    clear AST state + reload current page so the real
                               capture pipeline runs with the current AST code
  ast.clear                    drop all AST results + security findings + script cache

  findings [substring]         security findings (sinks + dangerous patterns)
  finding <index>              one finding + column-aware raw JS snippet

  logs [filter] [--service=X] [--host=X] [--ct=X] [--method=X] [--status=N] [--with-body] [--limit=N]
  log <reqId> [--decoded] [--save <dir>] [--full]
                               one request. --decoded runs the extension's parsers
                               (batchexecute/JSPB/protobuf/gRPC-Web/GraphQL/SSE/NDJSON/multipart).
                               --save writes meta+headers+raw bytes under <dir>/<reqId>/.

  keys                         API key registry
  services [substring]         service list (name, method count, samples)
  service <name> [--full]      drill-down: methods, params with AST badges, schemas, keys

  popup                        open popup page, switch filter=all
  popup.select <reqId>         replay(reqId, tabId) — refreshes allTabsData first
  popup.form                   dump form state (fields, types, values, badges, AST chips)
  popup.fill <name> <value>    type a value into a field
  popup.send                   click Send
  popup.response [--full]      rendered response: status, headers, decoded tree nodes
  popup.rename <field> <name>  click rename button, persist new name via RENAME_FIELD
  popup.key <auto|<idx>|custom=<value>|disable>
                               pick an API key from the selector
  popup.gql state|set <opIdx> <query|variables|operationName|operation|extensions> <v>
  popup.mp state|set <partIdx> <ct|body|contentId> <v>

  sw <expr>                    eval JS in the extension service worker
`);
}

// ─── dispatch ──────────────────────────────────────────────────────────────

const CMDS = {
  start: cmdStart, stop: cmdStop, status: cmdStatus, reload: cmdReload,
  goto: cmdGoto, tabs: cmdTabs, page: cmdPage, sw: cmdSw,
  scripts: cmdScripts, script: cmdScript, ast: cmdAst,
  "ast.rerun": cmdAstRerun, "ast.clear": cmdAstClear,
  findings: cmdFindings, finding: cmdFinding,
  logs: cmdLogs, log: cmdLog,
  keys: cmdKeys, services: cmdServices, service: cmdService,
  popup: cmdPopup,
  "popup.select": cmdPopupSelect,
  "popup.form": cmdPopupForm,
  "popup.fill": cmdPopupFill,
  "popup.send": cmdPopupSend,
  "popup.response": cmdPopupResponse,
  "popup.rename": cmdPopupRename,
  "popup.key": cmdPopupKey,
  "popup.gql": cmdPopupGql,
  "popup.mp": cmdPopupMp,
  "popup.debug": cmdPopupDebug,
  help: cmdHelp, "--help": cmdHelp, "-h": cmdHelp,
};

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  if (!cmd) return cmdHelp();
  const fn = CMDS[cmd];
  if (!fn) { err("unknown command: " + cmd); await cmdHelp(); process.exit(2); }
  try { await fn(rest); }
  catch (e) { err(e.stack || e.message || e); process.exit(1); }
}

main();
