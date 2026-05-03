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
const https = require("https");
const crypto = require("crypto");
const zlib = require("zlib");
const { spawn } = require("child_process");
const puppeteer = require("puppeteer");

const ROOT = path.resolve(__dirname, "..");
const EXT_DIR = path.join(ROOT, "extension");
const PROFILE_DIR = path.join(__dirname, "profile");
const DUMP_DIR = path.join(__dirname, "harness-dumps");
const SNAP_DIR = path.join(__dirname, "finding-snapshots");
const LOCK_FILE = path.join(__dirname, "harness.lock");
const DEFAULT_PORT = 9337;

// Stable finding identifier so the same vulnerability can be tracked
// across reloads and classifier changes. Hash includes the source URL,
// location, and classifier output so two findings for different patterns
// at the same line can't collide. Short hex prefix is enough to be
// unique across a session and human-typeable.
function computeFindingId(f) {
  const line = f.location?.line ?? f.loc?.start?.line ?? "?";
  const col = f.location?.column ?? f.loc?.start?.column ?? "?";
  const kind = f.category === "sink"
    ? `${f.type || "?"}:${f.sink || "?"}`
    : `${f.type || "pattern"}`;
  const key = `${f.sourceUrl || "(inline)"}|L${line}:C${col}|${f.category}|${kind}`;
  return crypto.createHash("sha1").update(key).digest("hex").slice(0, 10);
}

// Fetch a script URL via Node (no browser). Handles Content-Encoding
// (brotli / gzip / deflate) — default https.get gives us the COMPRESSED
// bytes, so Figma's `.br` bundles come back as gibberish unless we
// decompress. Returns the decoded text on success; returns null on HTTP
// error so callers can fall back to page-context fetch.
function fetchDecoded(url, timeoutMs = 15000) {
  return new Promise((resolve, reject) => {
    const client = url.startsWith("https:") ? https : http;
    const req = client.get(url, {
      headers: {
        // Some CDNs insist on an Accept-Encoding to send raw — we want
        // them to compress so our decompressor path is exercised, but
        // also accept identity for servers that ignore the header.
        "Accept-Encoding": "br, gzip, deflate, identity",
        "User-Agent": "api-security-researcher-harness/1.0",
      },
    }, (res) => {
      if (res.statusCode && res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        res.resume();
        const next = new URL(res.headers.location, url).href;
        fetchDecoded(next, timeoutMs).then(resolve, reject);
        return;
      }
      if (!res.statusCode || res.statusCode >= 400) {
        res.resume();
        reject(new Error(`HTTP ${res.statusCode} on ${url}`));
        return;
      }
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => {
        const buf = Buffer.concat(chunks);
        const enc = (res.headers["content-encoding"] || "").toLowerCase();
        try {
          let out;
          if (enc === "br") out = zlib.brotliDecompressSync(buf);
          else if (enc === "gzip") out = zlib.gunzipSync(buf);
          else if (enc === "deflate") out = zlib.inflateSync(buf);
          else out = buf;
          resolve(out.toString("utf8"));
        } catch (e) { reject(e); }
      });
    });
    req.on("error", reject);
    req.setTimeout(timeoutMs, () => { req.destroy(new Error("timeout")); });
  });
}

function log(...args) { console.log(...args); }
function err(...args) { console.error(...args); }
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
function short(s, n) { s = String(s == null ? "" : s); return s.length > n ? s.slice(0, n) + "…" : s; }
function _joinUrl(origin, path) {
  const o = origin == null ? "" : String(origin);
  const p = path == null ? "" : String(path);
  if (!o) return p;
  if (!p) return o;
  // Ensure exactly one slash between origin and path.
  const oTrim = o.endsWith("/") ? o.slice(0, -1) : o;
  const pLead = p.startsWith("/") ? p : "/" + p;
  return oTrim + pLead;
}
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

  // Spawn Chrome as a fully detached process so the parent shell
  // exiting (or this node process completing) doesn't take Chrome
  // with it. An earlier `puppeteer.launch` + `await keepAlive`
  // version held the browser alive by keeping node running — but on
  // Windows that ties Chrome to the launching shell, and every
  // caller had to either leave the shell open or use a workaround
  // like `&; disown` that doesn't actually survive shell exit.
  // detached:true creates a new process group; unref() means node
  // won't wait for Chrome before exiting; stdio:'ignore' severs the
  // pipes so Chrome isn't blocked on closed descriptors.
  const chromePath = puppeteer.executablePath();
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
  if (!chromeProc.pid) {
    log("failed to spawn Chrome");
    process.exit(1);
  }

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

  // Capture the extension ID while the SW target is fresh so every
  // subsequent command can wake the SW even after `reload` invalidates
  // the target list. Briefly connect via puppeteer to enumerate.
  let extId = null;
  try {
    const probeBrowser = await puppeteer.connect({
      browserURL: `http://127.0.0.1:${port}`,
      defaultViewport: null,
      targetFilter: (t) => t.type() !== "browser",
    });
    for (let i = 0; i < 50; i++) {
      const t = probeBrowser.targets().find(t => t.url().startsWith("chrome-extension://"));
      if (t) { try { extId = new URL(t.url()).hostname; break; } catch {} }
      await sleep(200);
    }
    probeBrowser.disconnect();
  } catch (e) {
    log("extension id probe failed:", e.message);
  }

  await writeLock({ port, pid: chromeProc.pid, extId, startedAt: Date.now() });
  log(`started. chrome pid=${chromeProc.pid} port=${port}. Detached — survives this shell. Stop with: node testing/harness.js stop`);
}

async function cmdStop() {
  const lock = await readLock();
  if (!lock) { log("not running"); return; }
  // Ask Chrome to close gracefully via CDP first, then fall back to
  // killing the pid stored in the lock. The pid IS Chrome's (detached
  // child from cmdStart) — not a node launcher — so `process.kill`
  // reliably terminates the browser.
  try {
    const browser = await connect(lock.port);
    await browser.close();
  } catch (e) {
    log("browser already gone:", e.message);
  }
  if (lock.pid) {
    try { process.kill(lock.pid); } catch {}
  }
  await clearLock();
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
            timings: r._analysisTimings || tab._lastAstTimings || null,
          });
        }
      }
      return rows;
    `);
    if (!out.length) { log("no scripts analysed yet"); return; }
    log(`${out.length} scripts analysed:`);
    out.forEach((r, i) => {
      if (filter && !r.url.includes(filter)) return;
      const timings = r.timings
        ? `  [analyze=${r.timings.analyzeMs}ms chars=${r.timings.codeChars}]`
        : "";
      log(`${String(i).padStart(3)}  tab=${r.tabId} sink=${r.secSinks} danger=${r.dangerous} fetch=${r.fetchSites} fieldMaps=${r.protoFieldMaps} enums=${r.protoEnums} ${r.sourceMap ? "[srcmap]" : ""}  ${labelScript(r.url)}${timings}`);
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
    // Get the script source. Try page-context fetch first (uses page
    // credentials, hits the server fresh), fall back to direct HTTP+decode
    // when cross-origin blocks the page fetch — important when the
    // reviewer wants to read a JS file from a site they're not currently
    // on (e.g., reading sstatic.net while parked on codeberg).
    const page = await getActivePage(browser);
    let src = null;
    if (page) {
      src = await page.evaluate(async (u) => {
        try {
          const resp = await fetch(u, { credentials: "include" });
          return await resp.text();
        } catch (e) { return "__FETCH_ERR__: " + e.message; }
      }, url);
    }
    if (!src || src.startsWith("__FETCH_ERR__:")) {
      try { src = await fetchDecoded(url); }
      catch (e) { err(`fetch failed (page + direct): ${e.message}`); return; }
    }
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
      // Show all of them with dedup by message prefix. An analyzer run on
      // github emits ~60-80 errors from a handful of root causes; truncating
      // to 5 hides the gaps the reviewer most needs to fix. Dedup after
      // truncating the message so variations in trailing call/column info
      // don't drown out the underlying identifier name.
      const _seen = new Set();
      for (const e of result.resolverErrors) {
        const line = `    ${e.context}: ${short(e.message, 160)}`;
        if (_seen.has(line)) continue;
        _seen.add(line);
        log(line);
      }
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
    let lastCache = 0;
    let stableTicks = 0;
    while (Date.now() < deadline) {
      await sleep(3000);
      let cacheSize = 0;
      try {
        cacheSize = await evalSW(browser, "return globalStore.scriptCache.size;");
      } catch (_) { continue; }
      if (cacheSize > 0) {
        if (cacheSize === lastCache) { stableTicks++; if (stableTicks >= 2) break; }
        else stableTicks = 0;
        lastCache = cacheSize;
      }
    }
    // Pull results from globalStore.scriptCache — survives SW restarts
    // and is the source of truth even when tab._astResults is empty
    // (which happens when the analysis succeeded but produced no
    // findings, or when the early-return branches ran).
    const scripts = await evalSW(browser, `
      const rows = [];
      for (const [key, cached] of globalStore.scriptCache.entries()) {
        const r = cached.result || {};
        rows.push({
          tabUrl: cached.tabUrl || "(unknown)",
          secSinks: (r.securitySinks || []).length,
          dangerous: (r.dangerousPatterns || []).length,
          fetchSites: (r.fetchCallSites || []).length,
          gaps: (r.resolverErrors || []).length,
        });
      }
      return rows;
    `);
    if (!scripts.length) { log("no AST results after reload"); return; }
    log(`${scripts.length} bundle(s) analysed:`);
    for (const r of scripts) log(`  sink=${r.secSinks} danger=${r.dangerous} fetch=${r.fetchSites} gaps=${r.gaps}  ${labelScript(r.tabUrl)}`);
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
      const id = computeFindingId(f);
      log(`${id}  #${String(i).padStart(3)} [${f.severity || "?"}] ${f.category}/${kind} L${line}  ${short(desc, 140)}`);
      log(`           ${labelScript(f.sourceUrl)}`);
    });
  });
}

async function cmdFinding(args) {
  const { positional, flags } = parseArgs(args);
  const arg = positional[0];
  const dimsOnly = flags["dims-only"] === true;
  if (!arg) throw new Error("usage: finding <id|index> [--dims-only]");
  await withBrowser(async (browser) => {
    const rows = await evalSW(browser, `
      const out = [];
      for (const [sourceUrl, v] of globalStore.securityFindings) {
        for (const s of (v.securitySinks || [])) out.push({sourceUrl, category: "sink", ...s});
        for (const s of (v.dangerousPatterns || [])) out.push({sourceUrl, category: "dangerous", ...s});
      }
      return out;
    `);
    if (!rows || rows.length === 0) { err("no findings"); return; }
    // Accept either a numeric index (legacy, unstable across reloads) or
    // the stable ID shown by `findings`. Prefer ID when ambiguous.
    let f = null;
    const asIdx = Number(arg);
    if (!Number.isNaN(asIdx) && asIdx >= 0 && asIdx < rows.length && arg.length < 6) {
      f = rows[asIdx];
    } else {
      f = rows.find(r => computeFindingId(r) === arg) || null;
    }
    if (!f) { err("no such finding (id or index not found)"); return; }
    const line = f.location?.line ?? f.loc?.start?.line ?? null;
    const col = f.location?.column ?? f.loc?.start?.column ?? null;

    // Fetch raw source once — we reuse it for the minified snippet, the
    // sourcemap lookup (taintPath annotation), and the enclosing-function
    // print. Prefer Node fetch (handles brotli); fall back to page fetch
    // for credentialed scripts.
    let src = null;
    if (f.sourceUrl && !f.sourceUrl.startsWith("data:") && line) {
      try { src = await fetchDecoded(f.sourceUrl); }
      catch (e) {
        const pages = await browser.pages();
        let fetchFrom = null;
        try {
          const scriptOrigin = new URL(f.sourceUrl).origin;
          fetchFrom = pages.find(pp => { try { return new URL(pp.url()).origin === scriptOrigin; } catch { return false; } });
        } catch {}
        if (!fetchFrom) fetchFrom = await getActivePage(browser);
        if (fetchFrom) {
          const pageSrc = await fetchFrom.evaluate(async (u) => {
            try { const r = await fetch(u, { credentials: "include" }); return await r.text(); } catch (e2) { return "__ERR__" + e2.message; }
          }, f.sourceUrl);
          src = pageSrc.startsWith("__ERR__") ? null : pageSrc;
        }
      }
    } else if (f.sourceUrl?.startsWith("data:") && line) {
      try {
        const u = new URL(f.sourceUrl);
        src = decodeURIComponent(u.pathname.split(",").slice(1).join(","));
      } catch {}
    }

    // Resolve the sourcemap once (may be null for unmapped bundles or
    // inline scripts). Used for finding-location and each taintPath hop.
    let smap = null;
    if (src && f.sourceUrl && !f.sourceUrl.startsWith("data:")) {
      try { smap = await resolveSourceMap(f.sourceUrl, src); } catch {}
    }
    const resolveOrig = (lineN, colN) => {
      if (!smap || !smap.consumer || lineN == null || colN == null) return null;
      try {
        const o = smap.consumer.originalPositionFor({ line: lineN, column: colN });
        if (o && o.source) return o;
      } catch {}
      return null;
    };

    log(`findingId: ${computeFindingId(f)}`);
    const kind = f.category === "sink"
      ? `${f.type || "?"} → ${f.sink || "?"}`
      : `${f.type || "pattern"}`;
    log(`[${f.severity || "?"}] ${f.category} / ${kind}`);
    log(`  source: ${labelScript(f.sourceUrl)}`);
    const origFinding = resolveOrig(line, col);
    if (origFinding) {
      log(`  location: minified L${line ?? "?"}:C${col ?? "?"}  →  ${origFinding.source} L${origFinding.line}:C${origFinding.column}${origFinding.name ? "  (symbol: " + origFinding.name + ")" : ""}`);
    } else {
      log(`  location: L${line ?? "?"} col ${col ?? "?"}`);
    }
    if (f.category === "sink") {
      log(`  sourceType: ${f.sourceType || "?"}  source: ${short(f.source || "-", 200)}`);
      if (f.receiverType) log(`  receiverType: ${f.receiverType}  (sink object's tracked type — e.g. Element ⇒ real DOM sink, FormData/URL ⇒ not a DOM sink)`);
      log(`  sanitized: ${!!f.sanitized}`);
    } else if (f.sourceType || f.source) {
      // Dangerous-pattern findings (prototype-pollution, postmessage-*,
      // regex-dynamic) now carry sourceType/source too, unlocking probe
      // strategy selection. Show them so the reviewer sees where the
      // taint came from without digging into taintPath[0].
      log(`  sourceType: ${f.sourceType || "?"}  source: ${short(f.source || "-", 200)}`);
    }
    if (Array.isArray(f.sinkDims)) {
      log(`  sinkDims: {${f.sinkDims.join(",") || "none"}}  (attacker-controlled dims surviving to the sink — {none} should have been suppressed by the all-dims-false gate)`);
    }
    // Latest exploit-probe verdict for this finding, if any. Closes the
    // loop between static analysis (finding as emitted) and dynamic
    // verification (the three-state probe outcome) so the reviewer
    // sees the current status without re-running the probe just to
    // check. Session TTL is 10 min in _probeSessions — older probes
    // drop off and the line disappears.
    const _fId = computeFindingId(f);
    const probe = await evalSW(browser, `
      let latest = null;
      for (const [, s] of _probeSessions) {
        if (s.findingId !== arg) continue;
        if (!latest || s.createdAt > latest.createdAt) latest = s;
      }
      if (!latest) return null;
      const firedKeys = Object.keys(latest.executed || {}).filter(k => (latest.executed || {})[k]);
      const activeExec = firedKeys.some(k => k !== "dom");
      const parsedOnly = firedKeys.length && !activeExec;
      let verdict;
      if (activeExec) verdict = "REAL EXPLOIT";
      else if (parsedOnly) verdict = "HTML PARSING CONFIRMED";
      else if ((latest.hits || []).length) verdict = "TAINT REACH ONLY";
      else verdict = "NOT REPRODUCED";
      return {
        verdict,
        marker: latest.marker,
        status: latest.status,
        strategy: latest.strategy,
        hitCount: (latest.hits || []).length,
        firedKeys,
        when: latest.finishedAt || latest.createdAt,
      };
    `, _fId);
    if (probe) {
      const whenStr = new Date(probe.when).toISOString().slice(0, 19).replace("T", " ");
      log(`  probe: ${probe.verdict}  (session ${probe.marker}, strategy=${probe.strategy}, hits=${probe.hitCount}${probe.firedKeys.length ? ", fired=" + probe.firedKeys.join("+") : ""}, ${whenStr})`);
    }
    if (f.description) log(`  description: ${f.description}`);
    if (f.codeContext) {
      log(`  codeContext:`);
      const ctx = String(f.codeContext);
      for (const l of ctx.split("\n").slice(0, 10)) log(`    ${l}`);
    }

    // Taint path, now with each hop source-map-resolved when possible.
    // Reviewers read top-to-bottom: source → … → sink. Original file/line
    // lets them jump directly to the real TS/JS instead of squinting at
    // minified columns.
    //
    // Per-hop code snippet: cut a ~140-char window around each hop's
    // (line, column) from the minified source so a reviewer can see
    // the surrounding syntax without having to run `finding.map` on
    // every hop. Transformations (method-call / concat / new-url-*)
    // and function boundaries (param-caller / call-arg / function-
    // return) are where taint changes shape — having the code inline
    // is what lets the agent judge "is dim-origin upgrading here
    // actually reachable?" without 20 extra tool calls. Uses the same
    // `src` already fetched for the top-level codeContext.
    if (Array.isArray(f.taintPath) && f.taintPath.length) {
      let lineStarts = null;
      if (src) {
        lineStarts = [0];
        for (let i = 0; i < src.length; i++) if (src.charCodeAt(i) === 10) lineStarts.push(i + 1);
      }
      const snippetAt = (line, column) => {
        if (!src || !lineStarts || line == null || column == null) return null;
        const lineIdx = line - 1;
        if (lineIdx < 0 || lineIdx >= lineStarts.length) return null;
        const absOffset = lineStarts[lineIdx] + column;
        if (absOffset < 0 || absOffset >= src.length) return null;
        const start = Math.max(0, absOffset - 70);
        const end = Math.min(src.length, absOffset + 70);
        let snippet = src.slice(start, end).replace(/\s+/g, " ");
        if (start > 0) snippet = "…" + snippet;
        if (end < src.length) snippet = snippet + "…";
        return snippet;
      };
      // --dims-only condenses a 100+ hop chain to the hops that actually
      // change what the attacker controls: the source, every hop with a
      // `dimsBefore` (dim transition), and the last hop (the taint transfer
      // to the sink). On a 110-hop react-router finding this drops the
      // listing from ~110 lines to ~5 — enough for the reviewer to judge
      // reachability without scrolling through binding/reassign noise.
      let hopsToShow;
      if (dimsOnly) {
        const last = f.taintPath.length - 1;
        const kept = new Set([0, last]);
        f.taintPath.forEach((h, i) => {
          if (h && h.dimsBefore) kept.add(i);
        });
        hopsToShow = Array.from(kept).sort((a, b) => a - b).map(i => ({ i, h: f.taintPath[i] }));
        log(`  taintPath (${f.taintPath.length} hops, --dims-only → ${hopsToShow.length}):`);
      } else {
        hopsToShow = f.taintPath.map((h, i) => ({ i, h }));
        log(`  taintPath (${f.taintPath.length} hops):`);
      }
      hopsToShow.forEach(({ i, h }) => {
        const atMin = h.at ? `min L${h.at.line}:C${h.at.column}` : "?";
        const o = h.at ? resolveOrig(h.at.line, h.at.column) : null;
        const atOrig = o ? `  →  ${o.source}:L${o.line}:C${o.column}${o.name ? " (" + o.name + ")" : ""}` : "";
        // Dim transition annotation — reviewer needs to see WHERE origin
        // becomes attacker-controlled. If this hop changed dims, show the
        // before→after mask alongside the hop.
        let dimAnn = "";
        if (h.dims) {
          const setKeys = Object.keys(h.dims).filter(k => h.dims[k]);
          const d = setKeys.length ? setKeys.join(",") : "none";
          if (h.dimsBefore) {
            const bef = Object.keys(h.dimsBefore).filter(k => h.dimsBefore[k]).join(",") || "none";
            dimAnn = `  dims: {${bef}} → {${d}}`;
          } else {
            dimAnn = `  dims={${d}}`;
          }
        }
        log(`    ${String(i).padStart(2)}. [${h.kind || "?"}]  ${h.desc || ""}  @${atMin}${atOrig}${dimAnn}`);
        // Prefer the snippet baked into the finding by the AST analyzer
        // (it has the whole bundle and writes one snippet per hop at
        // emit time). Fall back to cutting from the locally-fetched
        // source for findings produced before the snippet-on-hop change
        // landed, or when the bundle fetch failed.
        const snip = (h.code && String(h.code)) || (h.at ? snippetAt(h.at.line, h.at.column) : null);
        if (snip) log(`        ${snip}`);
      });
    }
    if (f.sanitizerReport && Array.isArray(f.sanitizerReport.candidates) && f.sanitizerReport.candidates.length) {
      const sr = f.sanitizerReport;
      log(`  sanitizerReport: ${sr.decision}  (${sr.candidates.length} sanitizer-shaped calls in scope)`);
      sr.candidates.forEach((c, i) => {
        const at = c.loc ? `L${c.loc.line}:C${c.loc.column}` : "?";
        const verdict = c.matched ? (c.onPath ? "matched, on-path" : "matched, branch-only") : "rejected";
        log(`    ${String(i).padStart(2)}. ${c.label}  @${at}  [${verdict}]`);
        log(`        ${c.matchReason}`);
      });
    }
    // Preconditions: guards the AST detected between source and sink.
    // Each entry pins an attacker-visible field to a literal value.
    if (Array.isArray(f.preconditions) && f.preconditions.length) {
      log(`  preconditions (${f.preconditions.length}): sink reachable only when`);
      f.preconditions.forEach((p, i) => {
        const pathLabel = Array.isArray(p.path) && p.path.length ? p.path.map(JSON.stringify).join(".") : "<root>";
        log(`    ${String(i).padStart(2)}. ${pathLabel} ${p.op} ${JSON.stringify(p.value)}`);
      });
    }

    // Enclosing function from ORIGINAL source (via sourcemap). Gives the
    // reviewer the readable, un-minified code around the sink — which is
    // almost always what they want to judge the taint chain. Gated to
    // <120 lines so we don't dump giant classes; anything bigger, fall
    // back to finding.func.
    let enclosingPrinted = false;
    if (src && line != null && origFinding) {
      try {
        const content = smap && smap.consumer ? smap.consumer.sourceContentFor(origFinding.source, true) : null;
        if (content) {
          const ast = parseForAnalysis(content);
          const fn = findEnclosingFunction(ast, origFinding.line, origFinding.column || 0);
          if (fn && fn.loc) {
            const spanLines = fn.loc.end.line - fn.loc.start.line + 1;
            if (spanLines <= 120) {
              const lines = content.split("\n");
              const label = fn.type + (fn.id && fn.id.name ? " " + fn.id.name : "");
              log(`\n  enclosing ${label}  (${origFinding.source}  L${fn.loc.start.line}..L${fn.loc.end.line}):`);
              for (let i = fn.loc.start.line - 1; i < fn.loc.end.line && i < lines.length; i++) {
                const marker = (i + 1 === origFinding.line) ? ">>" : "  ";
                log(`  ${marker} ${String(i + 1).padStart(5)}  ${lines[i] ?? ""}`);
              }
              enclosingPrinted = true;
            } else {
              log(`\n  enclosing ${fn.type} spans ${spanLines} lines — too large to inline; run: finding.func ${computeFindingId(f)}`);
            }
          }
        }
      } catch (e) { /* parse errors are fine — fall through to raw */ }
    }
    try { if (smap && smap.consumer && smap.consumer.destroy) smap.consumer.destroy(); } catch {}

    // Raw (minified) snippet. Still print it so the reviewer can see
    // the exact byte sequence the analyzer processed — useful when the
    // source-mapped view looks odd (rare bundler rewriting).
    if (src && line != null) {
      const lines = src.split("\n");
      const targetLine = lines[line - 1] || "";
      if (targetLine.length > 500 && col != null) {
        const radius = 200;
        const cStart = Math.max(0, col - radius);
        const cEnd = Math.min(targetLine.length, col + radius);
        const snippet = targetLine.slice(cStart, cEnd);
        const relCol = col - cStart;
        log(`\n  raw JS (minified) L${line} col ${col}  [±${radius} chars]`);
        log(`     ${snippet}`);
        log(`     ${" ".repeat(relCol)}^`);
      } else if (!enclosingPrinted) {
        const start = Math.max(0, line - 11);
        const end = Math.min(lines.length, line + 10);
        log(`\n  raw JS L${start + 1}..L${end}:`);
        for (let i = start; i < end; i++) {
          const marker = (i + 1 === line) ? ">>" : "  ";
          log(`  ${marker} ${String(i + 1).padStart(5)}  ${lines[i].slice(0, 240)}`);
        }
      }
    } else if (f.sourceUrl && !f.sourceUrl.startsWith("data:") && line) {
      err(`  (couldn't fetch source for ${f.sourceUrl})`);
    }
  });
}

// Read all current findings, flattened, so snapshot + diff see the same
// shape as `findings` / `finding`. Keyed by stable ID so fix verification
// can line up "before" and "after" even when arrays reorder.
async function collectFindings(browser) {
  const rows = await evalSW(browser, `
    const out = [];
    for (const [sourceUrl, v] of globalStore.securityFindings) {
      for (const s of (v.securitySinks || [])) out.push({sourceUrl, category: "sink", ...s});
      for (const s of (v.dangerousPatterns || [])) out.push({sourceUrl, category: "dangerous", ...s});
    }
    return out;
  `);
  const byId = {};
  for (const f of rows) {
    const id = computeFindingId(f);
    const line = f.location?.line ?? f.loc?.start?.line ?? null;
    const col = f.location?.column ?? f.loc?.start?.column ?? null;
    // Keep the record small but keep enough to audit a fix later:
    // - kind identity (type/sink/sourceType)
    // - severity + sanitized (these are the main things a classifier
    //   change flips)
    // - codeContext (AST's surrounding source snippet — stable enough
    //   to diff even if column drifts)
    byId[id] = {
      id,
      sourceUrl: f.sourceUrl || null,
      category: f.category,
      type: f.type || null,
      sink: f.sink || null,
      sourceType: f.sourceType || null,
      severity: f.severity || null,
      sanitized: !!f.sanitized,
      line, col,
      description: f.description || null,
      source: f.source || null,
      codeContext: f.codeContext ? String(f.codeContext).slice(0, 400) : null,
      taintPath: Array.isArray(f.taintPath) ? f.taintPath : null,
      sanitizerReport: f.sanitizerReport && Array.isArray(f.sanitizerReport.candidates) ? f.sanitizerReport : null,
      preconditions: Array.isArray(f.preconditions) ? f.preconditions : null,
    };
  }
  return byId;
}

// Recover a sourcemap for a minified script: read the `//# sourceMappingURL`
// trailer, resolve it (absolute / relative / data: URL), decode, and hand
// back a `SourceMapConsumer`. Returns null when the script has no map or
// the map can't be fetched/parsed — caller falls back to minified view.
async function resolveSourceMap(scriptUrl, scriptText) {
  const m = scriptText.match(/\/\/[#@]\s*sourceMappingURL\s*=\s*([^\s'"]+)/);
  if (!m) return null;
  const ref = m[1].trim();
  let rawJson = null;
  try {
    if (ref.startsWith("data:")) {
      const comma = ref.indexOf(",");
      const meta = ref.slice(5, comma); // after "data:"
      const payload = ref.slice(comma + 1);
      if (meta.includes("base64")) rawJson = Buffer.from(payload, "base64").toString("utf8");
      else rawJson = decodeURIComponent(payload);
    } else {
      const absUrl = new URL(ref, scriptUrl).href;
      rawJson = await fetchDecoded(absUrl);
    }
  } catch (e) {
    err(`  (sourcemap fetch failed: ${e.message})`);
    return null;
  }
  try {
    const { SourceMapConsumer } = require("source-map");
    const parsed = JSON.parse(rawJson);
    return { consumer: new SourceMapConsumer(parsed), raw: parsed };
  } catch (e) {
    err(`  (sourcemap parse failed: ${e.message})`);
    return null;
  }
}

async function cmdFindingMap(args) {
  const arg = args[0];
  if (!arg) throw new Error("usage: finding.map <id|index>");
  await withBrowser(async (browser) => {
    const byId = await collectFindings(browser);
    const rows = Object.values(byId);
    let f = null;
    const asIdx = Number(arg);
    if (!Number.isNaN(asIdx) && asIdx >= 0 && asIdx < rows.length && arg.length < 6) f = rows[asIdx];
    else f = byId[arg] || null;
    if (!f) { err("no such finding"); return; }
    const line = f.line, col = f.col;
    if (!f.sourceUrl || f.sourceUrl.startsWith("data:") || line == null) {
      err("  (finding has no external script / no line info — can't source-map)");
      return;
    }
    log(`findingId: ${f.id}  ${labelScript(f.sourceUrl)}  min L${line}:C${col ?? "?"}`);
    let src;
    try { src = await fetchDecoded(f.sourceUrl); }
    catch (e) { err(`  (script fetch failed: ${e.message})`); return; }
    const resolved = await resolveSourceMap(f.sourceUrl, src);
    if (!resolved) { log("  (no sourcemap for this script)"); return; }
    const { consumer } = resolved;
    // Babel stores `loc.start.column` 0-based; source-map expects the same.
    const orig = consumer.originalPositionFor({ line, column: col ?? 0 });
    if (!orig || !orig.source) {
      log("  (mapping returned no original position — column may fall in an unmapped region)");
      try { consumer.destroy && consumer.destroy(); } catch {}
      return;
    }
    log(`\n  original: ${orig.source}  L${orig.line}:C${orig.column}${orig.name ? `  (symbol: ${orig.name})` : ""}`);
    // Print ±15 lines of the original source around the mapped line if
    // the map embedded `sourcesContent`. Most modern bundlers do.
    let content = null;
    try { content = consumer.sourceContentFor(orig.source, true); } catch {}
    if (!content) {
      log("  (sourcesContent not embedded — can't show surrounding source)");
    } else {
      const lines = content.split("\n");
      const start = Math.max(0, orig.line - 16);
      const end = Math.min(lines.length, orig.line + 15);
      log(`\n  original source L${start + 1}..L${end}:`);
      for (let i = start; i < end; i++) {
        const marker = (i + 1 === orig.line) ? ">>" : "  ";
        log(`  ${marker} ${String(i + 1).padStart(5)}  ${lines[i].slice(0, 240)}`);
      }
    }
    try { consumer.destroy && consumer.destroy(); } catch {}
  });
}

// Parse `code` with Babel's parser accepting TS/JSX/class fields. Error
// recovery is on because CDN bundles sometimes contain stage-N syntax
// that the current parser build doesn't fully understand — we still want
// a mostly-complete AST to walk.
function parseForAnalysis(code) {
  const parser = require("@babel/parser");
  return parser.parse(code, {
    sourceType: "unambiguous",
    allowReturnOutsideFunction: true,
    allowAwaitOutsideFunction: true,
    allowImportExportEverywhere: true,
    errorRecovery: true,
    plugins: ["typescript", "jsx", "decorators-legacy", "classProperties", "classPrivateProperties", "classPrivateMethods", "topLevelAwait"],
  });
}

// Collect ALL function-like ancestors whose source span contains (line, col).
// `line` 1-based, `col` 0-based. Returned in order from outermost to
// innermost so callers can pick a nesting depth (0 = innermost, 1 = next
// enclosing, …). Useful when the innermost function fails to reproduce a
// finding and we need to widen the scope until it does.
function findEnclosingFunctions(ast, line, col) {
  const traverse = require("@babel/traverse").default || require("@babel/traverse");
  const FUNC_TYPES = new Set([
    "FunctionDeclaration", "FunctionExpression", "ArrowFunctionExpression",
    "ObjectMethod", "ClassMethod", "ClassPrivateMethod",
  ]);
  const hits = [];
  const inside = (loc) => {
    if (!loc) return false;
    const s = loc.start, e = loc.end;
    if (line < s.line || line > e.line) return false;
    if (line === s.line && col < s.column) return false;
    if (line === e.line && col > e.column) return false;
    return true;
  };
  traverse(ast, {
    enter(p) {
      if (!FUNC_TYPES.has(p.node.type)) return;
      if (!inside(p.node.loc)) { p.skip(); return; }
      hits.push(p.node);
    },
  });
  // Traverse enters parents before children → already outermost-first.
  return hits;
}
function findEnclosingFunction(ast, line, col) {
  const all = findEnclosingFunctions(ast, line, col);
  return all.length ? all[all.length - 1] : null;
}

async function cmdFindingFunc(args) {
  const arg = args[0];
  if (!arg) throw new Error("usage: finding.func <id|index>");
  await withBrowser(async (browser) => {
    const byId = await collectFindings(browser);
    const rows = Object.values(byId);
    let f = null;
    const asIdx = Number(arg);
    if (!Number.isNaN(asIdx) && asIdx >= 0 && asIdx < rows.length && arg.length < 6) f = rows[asIdx];
    else f = byId[arg] || null;
    if (!f) { err("no such finding"); return; }
    if (!f.sourceUrl || f.sourceUrl.startsWith("data:") || f.line == null) { err("  (no external script with line info)"); return; }
    let src;
    try { src = await fetchDecoded(f.sourceUrl); }
    catch (e) { err(`  (script fetch failed: ${e.message})`); return; }

    // Prefer original source via sourcemap — minified bundles are one
    // giant line, which makes "enclosing function" about as useful as the
    // whole file. If the map embeds sourcesContent, walk *that*.
    let analysisSrc = src;
    let analysisLine = f.line;
    let analysisCol = f.col ?? 0;
    let originalFile = f.sourceUrl;
    const resolved = await resolveSourceMap(f.sourceUrl, src);
    if (resolved) {
      const orig = resolved.consumer.originalPositionFor({ line: f.line, column: f.col ?? 0 });
      let content = null;
      try { content = orig && orig.source ? resolved.consumer.sourceContentFor(orig.source, true) : null; } catch {}
      if (orig && orig.source && content) {
        analysisSrc = content;
        analysisLine = orig.line;
        analysisCol = orig.column;
        originalFile = orig.source;
      }
      try { resolved.consumer.destroy && resolved.consumer.destroy(); } catch {}
    }

    log(`findingId: ${f.id}  ${originalFile}  L${analysisLine}:C${analysisCol}`);
    let ast;
    try { ast = parseForAnalysis(analysisSrc); }
    catch (e) { err(`  (parse failed: ${e.message})`); return; }
    const fn = findEnclosingFunction(ast, analysisLine, analysisCol);
    if (!fn) { log("  (no enclosing function — point is at top-level)"); return; }
    const { start, end } = fn.loc;
    const idTxt = fn.type === "FunctionDeclaration" && fn.id ? ` ${fn.id.name}` : "";
    log(`\n  enclosing ${fn.type}${idTxt}  L${start.line}:C${start.column}..L${end.line}:C${end.column}`);
    const lines = analysisSrc.split("\n");
    const first = start.line, last = end.line;
    const limit = 120;
    const spanEnd = Math.min(last, first + limit - 1);
    for (let i = first - 1; i < spanEnd; i++) {
      const n = i + 1;
      const marker = (n === analysisLine) ? ">>" : "  ";
      log(`  ${marker} ${String(n).padStart(5)}  ${lines[i] ?? ""}`);
    }
    if (spanEnd < last) log(`  … (${last - spanEnd} more lines)`);
  });
}

// Run the real extension analyzer on a supplied code blob (no tab, no
// globalStore write — we route through the offscreen AST worker). The
// point is to rerun a classifier over isolated code so a FP can be
// reproduced without fetching the whole bundle each time.
async function runAstAnalyzer(browser, code, sourceUrl) {
  const res = await evalSW(browser,
    `return await sendToOffscreen({ type: "AST_ANALYZE", code: arg.code, sourceUrl: arg.sourceUrl });`,
    { code, sourceUrl: sourceUrl || "probe://inline" });
  if (!res) throw new Error("no response from offscreen");
  if (!res.success) throw new Error(`offscreen AST_ANALYZE failed: ${res.error || "(no error)"}`);
  return res.result;
}

function printAstProbeResult(r) {
  const sinks = r.securitySinks || [];
  const dang = r.dangerousPatterns || [];
  const resErrs = r.resolverErrors || [];
  log(`  sinks: ${sinks.length}  dangerousPatterns: ${dang.length}  fetchSites: ${(r.fetchCallSites || []).length}  resolverErrors: ${resErrs.length}`);
  if (r.perf) {
    const p = r.perf;
    const ph = p.phaseMs || {};
    const mb = (p.sizeBytes / 1048576).toFixed(2);
    const sec = (p.totalMs / 1000).toFixed(2);
    const mbps = (p.sizeBytes / 1048576 / (p.totalMs / 1000));
    log(`  perf: ${sec}s for ${mb}MB (${mbps.toFixed(2)}MB/s)  phases: parse=${ph.parse?.toFixed(0)}ms prePass=${ph.prePass?.toFixed(0)}ms mainPass=${ph.mainPass?.toFixed(0)}ms structuralExport=${ph.structuralExport?.toFixed(0)}ms`);
    if (p.counters) {
      log(`  counters: interProc=${p.counters.interProcTraces} resolvedUrls=${p.counters.resolvedUrls} globals=${p.counters.globalAssignments} winAliases=${p.counters.windowAliases} protoMethods=${p.counters.protoMethods}/${p.counters.protoMethodsNoField}-unmatched`);
    }
  }
  for (const f of (r.fetchCallSites || [])) {
    const line = f.location?.line ?? "?";
    const col = f.location?.column ?? "?";
    log(`    fetch ${f.method || "GET"} ${f.url}  L${line}:C${col}`);
  }
  // Dedup identical gap messages for display — the analyzer emits
  // one per fetch call site, but on a bundle with many fetches
  // resolving through the same underlying unresolvable leaf we'd print
  // the same line 30+ times. Each unique gap (message) is shown once
  // with its hit count so reviewers see density without the noise.
  const gapCounts = new Map();
  for (const e of resErrs) {
    const msg = e.message || "";
    gapCounts.set(msg, (gapCounts.get(msg) || 0) + 1);
  }
  for (const [msg, count] of gapCounts) {
    const suffix = count > 1 ? ` (×${count})` : "";
    log(`    [resolver-gap] ${short(msg, 180)}${suffix}`);
  }
  for (const s of sinks) {
    const line = s.location?.line ?? "?";
    const col = s.location?.column ?? "?";
    log(`    [${s.severity}] sink/${s.type}:${s.sink}  L${line}:C${col}  sourceType=${s.sourceType}  sanitized=${!!s.sanitized}`);
    if (s.description) log(`        ${short(s.description, 160)}`);
    if (s.codeContext) log(`        ${short(String(s.codeContext).replace(/\s+/g, " "), 160)}`);
  }
  for (const d of dang) {
    const line = d.location?.line ?? "?";
    const col = d.location?.column ?? "?";
    log(`    [${d.severity}] danger/${d.type}  L${line}:C${col}`);
    if (d.description) log(`        ${short(d.description, 160)}`);
    if (d.codeContext) log(`        ${short(String(d.codeContext).replace(/\s+/g, " "), 160)}`);
  }
}

async function cmdAstProbe(rest) {
  const { positional, flags } = parseArgs(rest);
  let code = null;
  let sourceUrl = flags.source || null;
  if (flags.inline) code = String(flags.inline);
  else if (positional[0]) {
    code = await fsp.readFile(positional[0], "utf8");
    sourceUrl = sourceUrl || `probe://${path.basename(positional[0])}`;
  }
  if (!code) throw new Error("usage: ast.probe <file> | --inline <snippet>");
  await withBrowser(async (browser) => {
    const r = await runAstAnalyzer(browser, code, sourceUrl);
    log(`probe: ${sourceUrl || "(inline)"}  (${code.length} chars)`);
    printAstProbeResult(r);
  });
}

// Isolate a single finding's enclosing function (source-map resolved when
// available), wrap it minimally, and rerun the analyzer. If the same
// classifier fires on the isolated function body, the decision is local
// to that function's structure — a pure classifier change will flip it.
// If it doesn't fire, the signal depends on surrounding scope (imports,
// globals, inter-procedural tracing) and the classifier change needs a
// wider reproducer.
async function cmdAstProbeFinding(rest) {
  const { positional, flags } = parseArgs(rest);
  const arg = positional[0];
  if (!arg) throw new Error("usage: ast.probe.finding <id|index> [--raw]");
  // --raw probes the ORIGINAL minified bundle (matches what the real
  // analyzer saw). Default is source-mapped TS/original, which is easier
  // to read but has different identifier names and shape — a useful
  // comparison point but not the same input as the live classifier.
  const useRaw = !!flags.raw;
  await withBrowser(async (browser) => {
    const byId = await collectFindings(browser);
    const rows = Object.values(byId);
    let f = null;
    const asIdx = Number(arg);
    if (!Number.isNaN(asIdx) && asIdx >= 0 && asIdx < rows.length && arg.length < 6) f = rows[asIdx];
    else f = byId[arg] || null;
    if (!f) { err("no such finding"); return; }
    if (!f.sourceUrl || f.sourceUrl.startsWith("data:") || f.line == null) { err("  (no external script / line info)"); return; }
    let src;
    try { src = await fetchDecoded(f.sourceUrl); }
    catch (e) { err(`  (script fetch failed: ${e.message})`); return; }
    let analysisSrc = src, analysisLine = f.line, analysisCol = f.col ?? 0, originalFile = f.sourceUrl;
    if (!useRaw) {
      const resolved = await resolveSourceMap(f.sourceUrl, src);
      if (resolved) {
        const orig = resolved.consumer.originalPositionFor({ line: f.line, column: f.col ?? 0 });
        let content = null;
        try { content = orig && orig.source ? resolved.consumer.sourceContentFor(orig.source, true) : null; } catch {}
        if (orig && orig.source && content) {
          analysisSrc = content; analysisLine = orig.line; analysisCol = orig.column; originalFile = orig.source;
        }
        try { resolved.consumer.destroy && resolved.consumer.destroy(); } catch {}
      }
    }
    let ast;
    try { ast = parseForAnalysis(analysisSrc); }
    catch (e) { err(`  (parse failed: ${e.message})`); return; }
    const ancestors = findEnclosingFunctions(ast, analysisLine, analysisCol);
    if (!ancestors.length) { err("  (no enclosing function — can't isolate)"); return; }
    // depth = 0 → innermost; depth = N → Nth outer. --depth=all picks outermost.
    const depthSpec = flags.depth;
    let depthIdx = 0;
    if (depthSpec === "all" || depthSpec === "outer") depthIdx = ancestors.length - 1;
    else if (depthSpec != null) {
      const n = Number(depthSpec);
      depthIdx = Math.min(ancestors.length - 1, Math.max(0, Number.isNaN(n) ? 0 : n));
    }
    const fn = ancestors[ancestors.length - 1 - depthIdx];
    log(`  enclosing chain (${ancestors.length}): ` + ancestors.map(a => `${a.type}@L${a.loc.start.line}:C${a.loc.start.column}`).reverse().join(" ← "));
    log(`  probing depth=${depthIdx} (${fn.type} L${fn.loc.start.line}:C${fn.loc.start.column})`);
    const lines = analysisSrc.split("\n");
    // Slice by (line,col) bounds. Babel loc is line=1-based, col=0-based.
    const segStart = fn.loc.start, segEnd = fn.loc.end;
    const sliced = lines.slice(segStart.line - 1, segEnd.line);
    if (sliced.length === 0) { err("  (empty slice)"); return; }
    sliced[0] = sliced[0].slice(segStart.column);
    sliced[sliced.length - 1] = sliced[sliced.length - 1].slice(0, segEnd.line === segStart.line ? segEnd.column - segStart.column : segEnd.column);
    let body = sliced.join("\n");
    // Arrow/function expressions aren't valid at top level on their own.
    // Wrap so the analyzer sees it as an expression statement.
    if (fn.type !== "FunctionDeclaration") body = `(${body});`;
    log(`findingId: ${f.id}  ${originalFile}  ${fn.type}  L${segStart.line}:C${segStart.column}..L${segEnd.line}:C${segEnd.column}`);
    log(`  isolated body size: ${body.length} chars`);
    const r = await runAstAnalyzer(browser, body, `probe://finding-${f.id}`);
    printAstProbeResult(r);
    // Tell the user whether the same-type classifier fired on the isolated slice.
    const kind = f.category === "sink" ? `${f.type}:${f.sink}` : f.type;
    const same = (r.securitySinks || []).some(s => `${s.type}:${s.sink}` === kind) ||
                 (r.dangerousPatterns || []).some(d => d.type === f.type);
    log(`\n  → same-kind classifier ${same ? "FIRED" : "did NOT fire"} on isolated function body`);
    log(`     ${same ? "  (verdict: decision is local to this function — fix here)" : "  (verdict: signal needs surrounding scope — trace inter-procedural context)"}`);
  });
}

// Find the Path wrapping a specific raw node — Babel's traverse API gives
// us paths, but `findEnclosingFunctions` returns raw nodes (so the
// enclosing chain can be computed without re-traversing). To use scope
// and referencePaths we need the Path, so re-traverse and match on node
// identity.
function findPathForNode(ast, target) {
  const traverse = require("@babel/traverse").default || require("@babel/traverse");
  let out = null;
  traverse(ast, {
    enter(p) { if (p.node === target) { out = p; p.stop(); } },
  });
  return out;
}

// Resolve a function node to the binding under which it is callable —
// FunctionDeclaration by its own name, or a VariableDeclarator/
// AssignmentExpression where the function is the initializer. Returns
// null when the function has no externally-callable binding (anonymous
// callback, inline IIFE, object-method — those need different handling).
function resolveFunctionCallBinding(fnPath) {
  const t = require("@babel/types");
  if (fnPath.isFunctionDeclaration() && fnPath.node.id) {
    const name = fnPath.node.id.name;
    const scope = fnPath.scope.parent || fnPath.scope;
    return { kind: "named-function", name, binding: scope.getBinding(name) };
  }
  const parent = fnPath.parentPath;
  if (parent && parent.isVariableDeclarator() && parent.node.id && t.isIdentifier(parent.node.id)) {
    const name = parent.node.id.name;
    return { kind: "var-bound", name, binding: parent.scope.getBinding(name) };
  }
  if (parent && parent.isAssignmentExpression() && parent.node.right === fnPath.node) {
    const left = parent.node.left;
    if (t.isIdentifier(left)) {
      return { kind: "assigned", name: left.name, binding: parent.scope.getBinding(left.name) };
    }
    if (t.isMemberExpression(left) && !left.computed && t.isIdentifier(left.property)) {
      return { kind: "member-assigned", name: left.property.name, binding: null };
    }
  }
  if (parent && parent.isObjectProperty() && parent.node.value === fnPath.node) {
    const k = parent.node.key;
    if (t.isIdentifier(k)) return { kind: "obj-property", name: k.name, binding: null };
    if (t.isStringLiteral(k)) return { kind: "obj-property", name: k.value, binding: null };
  }
  if (fnPath.isObjectMethod() || fnPath.isClassMethod()) {
    const k = fnPath.node.key;
    const t2 = require("@babel/types");
    if (t2.isIdentifier(k)) return { kind: "method", name: k.name, binding: null };
    if (t2.isStringLiteral(k)) return { kind: "method", name: k.value, binding: null };
  }
  return null;
}

// Walk every reference of a binding and keep only the ones used as call
// targets (fn(...) or fn?.(...)). Each entry carries the full argument
// array so the caller can inspect the value flowing into the parameter
// we care about.
function findCallSitesFromBinding(binding) {
  const sites = [];
  if (!binding || !binding.referencePaths) return sites;
  for (const ref of binding.referencePaths) {
    const parent = ref.parentPath;
    if (!parent) continue;
    if ((parent.isCallExpression() || parent.isOptionalCallExpression()) && parent.node.callee === ref.node) {
      sites.push(parent);
    }
  }
  return sites;
}

// Structural fallback for methods: `obj.foo(...)` where we only know the
// method name. This is imprecise — we can't tell which object without
// more analysis — but it's the best we can do for ObjectMethod /
// ClassMethod / MemberExpression-assigned handlers, and often narrows
// the audit target enough to be useful.
function findCallSitesByMethodName(ast, methodName) {
  const traverse = require("@babel/traverse").default || require("@babel/traverse");
  const t = require("@babel/types");
  const sites = [];
  traverse(ast, {
    enter(p) {
      if (!(p.isCallExpression() || p.isOptionalCallExpression())) return;
      const c = p.node.callee;
      if ((t.isMemberExpression(c) || t.isOptionalMemberExpression(c)) && !c.computed &&
          t.isIdentifier(c.property, { name: methodName })) {
        sites.push(p);
      }
    },
  });
  return sites;
}

// Slice raw source by Babel loc without re-walking the AST. Babel's loc
// uses 1-based lines + 0-based columns. Single-line slices are common
// on minified bundles, so we optimise for that path.
function sliceByLoc(src, loc, maxLen) {
  if (!loc) return "";
  const lines = src.split("\n");
  const startLine = loc.start.line, endLine = loc.end.line;
  if (startLine === endLine) {
    const line = lines[startLine - 1] || "";
    const slice = line.slice(loc.start.column, loc.end.column);
    return maxLen ? slice.slice(0, maxLen) : slice;
  }
  let out = (lines[startLine - 1] || "").slice(loc.start.column);
  for (let i = startLine; i < endLine - 1 && i < lines.length; i++) out += "\n" + lines[i];
  if (endLine - 1 < lines.length) out += "\n" + (lines[endLine - 1] || "").slice(0, loc.end.column);
  return maxLen ? out.slice(0, maxLen) : out;
}

async function cmdFindingCallers(rest) {
  const { positional, flags } = parseArgs(rest);
  const arg = positional[0];
  if (!arg) throw new Error("usage: finding.callers <id|index> [--arg N] [--depth K] [--max-len N]");
  const focusArg = flags.arg != null ? Number(flags.arg) : null;
  const depthSpec = flags.depth != null ? Number(flags.depth) : null;
  const maxLen = flags["max-len"] != null ? Number(flags["max-len"]) : 200;
  await withBrowser(async (browser) => {
    const byId = await collectFindings(browser);
    const rows = Object.values(byId);
    let f = null;
    const asIdx = Number(arg);
    if (!Number.isNaN(asIdx) && asIdx >= 0 && asIdx < rows.length && arg.length < 6) f = rows[asIdx];
    else f = byId[arg] || null;
    if (!f) { err("no such finding"); return; }
    if (!f.sourceUrl || f.sourceUrl.startsWith("data:") || f.line == null) {
      err("  (no external script / line info — caller discovery needs scope on raw bundle)");
      return;
    }

    // Always operate on the raw bundle the analyzer actually saw —
    // source-mapped lookup is for human readability, but scope +
    // reference paths have to match what the classifier traversed.
    let src;
    try { src = await fetchDecoded(f.sourceUrl); }
    catch (e) { err(`  (script fetch failed: ${e.message})`); return; }
    let ast;
    try { ast = parseForAnalysis(src); }
    catch (e) { err(`  (parse failed: ${e.message})`); return; }

    // Surface the inter-procedural hops from the taint path. Each
    // param-caller / param-iife hop pinpoints a function boundary the
    // classifier crossed — those are the ones worth auditing.
    const paramHops = Array.isArray(f.taintPath)
      ? f.taintPath.filter(h => h.kind === "param-caller" || h.kind === "param-iife")
      : [];
    log(`findingId: ${f.id}  ${labelScript(f.sourceUrl)}`);
    if (paramHops.length) {
      log(`  taint path crossed ${paramHops.length} function boundaries (param hops):`);
      paramHops.forEach((h, i) => {
        const at = h.at ? `L${h.at.line}:C${h.at.column}` : "?";
        log(`    ${i}. [${h.kind}]  ${h.desc}  @${at}`);
      });
    } else {
      log(`  (no param-caller hops in taint path — finding is local to one function)`);
    }

    // Enclosing functions give us a callable-function set to probe. We
    // walk outer → inner so the user sees outermost contextual scope
    // first (where inter-procedural tracing usually starts).
    const ancestors = findEnclosingFunctions(ast, f.line, f.col || 0);
    if (!ancestors.length) { err("  (no enclosing function — point is top-level)"); return; }

    let depths;
    if (depthSpec != null) depths = [Math.min(ancestors.length - 1, Math.max(0, depthSpec))];
    else depths = ancestors.map((_, i) => ancestors.length - 1 - i); // outer to inner

    let anyShown = false;
    for (const d of depths) {
      const fnNode = ancestors[ancestors.length - 1 - d];
      const fnPath = findPathForNode(ast, fnNode);
      if (!fnPath) continue;
      const resolved = resolveFunctionCallBinding(fnPath);
      const header = `\n  depth ${d}  ${fnNode.type}${fnNode.id ? " " + fnNode.id.name : ""}  L${fnNode.loc.start.line}:C${fnNode.loc.start.column}..L${fnNode.loc.end.line}:C${fnNode.loc.end.column}`;
      if (!resolved) {
        log(`${header}  (no externally-resolvable binding — inline callback / IIFE)`);
        continue;
      }
      let sites;
      let method;
      if (resolved.binding) {
        sites = findCallSitesFromBinding(resolved.binding);
        method = `binding "${resolved.name}" (${resolved.kind}) → ${resolved.binding.referencePaths.length} refs`;
      } else if (resolved.kind === "method" || resolved.kind === "obj-property" || resolved.kind === "member-assigned") {
        sites = findCallSitesByMethodName(ast, resolved.name);
        method = `structural search .${resolved.name}(…) — includes false positives from other objects`;
      } else {
        sites = [];
        method = `unresolved (${resolved.kind})`;
      }
      log(`${header}`);
      log(`    name:    ${resolved.name}  (${resolved.kind})`);
      log(`    method:  ${method}`);
      log(`    callers: ${sites.length}`);
      if (!sites.length) continue;
      anyShown = true;
      const shownMax = 25;
      for (let ci = 0; ci < Math.min(sites.length, shownMax); ci++) {
        const callPath = sites[ci];
        const callNode = callPath.node;
        const callLoc = callNode.loc;
        log(`      @ L${callLoc.start.line}:C${callLoc.start.column}`);
        const args = callNode.arguments || [];
        for (let ai = 0; ai < args.length; ai++) {
          if (focusArg != null && ai !== focusArg) continue;
          const argNode = args[ai];
          const snippet = argNode && argNode.loc ? sliceByLoc(src, argNode.loc, maxLen) : "?";
          log(`        arg ${ai}: ${snippet}`);
        }
      }
      if (sites.length > shownMax) log(`      … and ${sites.length - shownMax} more (re-run with --depth ${d} to widen)`);
    }
    if (!anyShown) log(`\n  (no resolvable callers across ancestors — function is only called anonymously)`);
  });
}

// Classify a finding's taint source into a probe strategy the
// extension knows how to run. Same mapping the SW's EXPLOIT_PROBE
// handler uses (no strategy = unsupported source type).
function classifyProbeStrategy(finding) {
  const src = finding && finding.source ? String(finding.source) : "";
  if (!src) return null;

  // Strategy picked primarily from source, but for `location` sources
  // the actual attack vector depends on which URL dimension the code
  // extracts. Inspect the taint path for a discriminating hop:
  //   .hash → hash strategy
  //   .search / .searchParams / .get(paramName) → search
  //   .pathname → pathname
  // This is a FACT about which path the code takes, not a guess.
  const tp = Array.isArray(finding && finding.taintPath) ? finding.taintPath : [];
  let pathHasHash = false, pathHasSearch = false, pathHasPathname = false;
  let getParamArg = null;
  for (const h of tp) {
    if (!h) continue;
    const d = String(h.desc || "");
    if (h.kind === "member") {
      if (d === ".hash") pathHasHash = true;
      else if (d === ".search" || d === ".searchParams") pathHasSearch = true;
      else if (d === ".pathname") pathHasPathname = true;
    }
    // URLSearchParams / URL .get("name") — the "name" argument is the
    // exact query-string key the handler reads. Captured as hop.arg.
    if (h.kind === "method-call" && (h.method === "get" || /\.get\("/.test(d)) && typeof h.arg === "string") {
      getParamArg = h.arg;
      pathHasSearch = true;
    }
  }

  if (src === "location.hash" || /location\.hash/i.test(src)) return { kind: "hash", sourceName: src };
  if (src === "location.search" || /location\.search/i.test(src)) return { kind: "search", sourceName: src };
  if (src === "location.pathname") return { kind: "pathname", sourceName: src };
  if (src === "event.data" || /postMessage|event\.data/i.test(src)) return { kind: "postmessage", sourceName: src };

  // `location` / `location.href` — pick strategy from taint path dims.
  if (src === "location.href" || /location\.href/i.test(src) || src === "location") {
    if (pathHasSearch) return { kind: "search", sourceName: src || "location", paramName: getParamArg };
    if (pathHasHash) return { kind: "hash", sourceName: src || "location" };
    if (pathHasPathname) return { kind: "pathname", sourceName: src || "location" };
    // No discriminating hop — default to hash (most common DOM-XSS vector).
    return { kind: "hash", sourceName: src || "location" };
  }
  return null;
}

// finding.view — open the source viewer for a specific finding, wait for
// the tree-shaken focused view to render, and report its size. Lets the
// agent reviewer verify the focused slice captures the relevant code
// without manually opening the viewer tab. Prints:
//   • total beautified lines in the original bundle
//   • focused-view line count (after tree-shake)
//   • reduction ratio (how aggressive the tree-shake was)
//   • the function ranges in the focused view
//   • a preview of the focused code (first N lines)
async function cmdFindingView(args) {
  const arg = args[0];
  if (!arg) throw new Error("usage: finding.view <id|index> [--preview N]");
  const rest = args.slice(1);
  let previewN = 40;
  for (let i = 0; i < rest.length; i++) {
    if (rest[i] === "--preview" && i + 1 < rest.length) previewN = Number(rest[i + 1]) || previewN;
  }
  await withBrowser(async (browser) => {
    const byId = await collectFindings(browser);
    const rows = Object.values(byId);
    let f = null;
    const asIdx = Number(arg);
    if (!Number.isNaN(asIdx) && asIdx >= 0 && asIdx < rows.length && arg.length < 6) f = rows[asIdx];
    else f = byId[arg] || null;
    if (!f) { err("no such finding"); return; }
    if (!f.sourceUrl || f.sourceUrl.startsWith("data:") || f.line == null) {
      err("  (finding has no external script / line info — viewer needs both)");
      return;
    }
    // Finding's sourceUrl may be a source-map-resolved path (e.g.
    // "node_modules/@github/remote-input-element/dist/index.js") that
    // isn't HTTP-fetchable. The viewer's GET_SCRIPT_SOURCE resolves
    // such paths by scanning buffered bundle sourcemaps for a match —
    // use that lookup here so the URL we open works the same way as
    // a popup-click would.
    let viewerSourceUrl = f.sourceUrl;
    if (!/^https?:\/\//i.test(f.sourceUrl)) {
      const mapped = await evalSW(browser, `
        for (const [tid, tab] of state.tabs.entries()) {
          const merged = (typeof mergedSecurityFindings === "function") ? mergedSecurityFindings(tab) : [];
          for (const m of merged) {
            if (m.sourceUrl && /^https?:\\/\\//i.test(m.sourceUrl)) {
              const sinks = m.securitySinks || [];
              const pats = m.dangerousPatterns || [];
              const all = [...sinks, ...pats];
              for (const s of all) {
                if (s.location && s.location.line === arg.line && s.location.column === arg.col && s.type === arg.type && s.sink === arg.sink) {
                  return m.sourceUrl;
                }
              }
            }
          }
        }
        return null;
      `, { line: f.line, col: f.col, type: f.type, sink: f.sink });
      if (!mapped) {
        err(`  (finding's sourceUrl "${f.sourceUrl}" is a source-mapped path and no sibling HTTP-hosted finding has the same sink at L${f.line}:C${f.col})`);
        return;
      }
      viewerSourceUrl = mapped;
    }
    const extId = await getExtId(browser);
    const tabId = await evalSW(browser, `
      for (const [tid, tab] of state.tabs.entries()) {
        const merged = (typeof mergedSecurityFindings === "function") ? mergedSecurityFindings(tab) : [];
        if (merged.some(e => e.sourceUrl === arg)) return tid;
      }
      return 0;
    `, viewerSourceUrl);
    const colPart = f.col != null ? `&col=${f.col}` : "";
    const url = `chrome-extension://${extId}/viewer.html?sourceUrl=${encodeURIComponent(viewerSourceUrl)}&line=${f.line}${colPart}&tabId=${tabId || 0}`;
    const page = await browser.newPage();
    try {
      // viewer opens its own resources async (fetch bundle, parse, beautify,
      // render) — domcontentloaded fires almost immediately; timeout here
      // only protects against the `goto` call itself hanging. Bump to 30 s
      // for slow bundles; the deadline loop below enforces the real
      // render-completion deadline.
      await page.goto(url, { waitUntil: "domcontentloaded", timeout: 30000 });
      // Wait until viewer either renders a <code> with text or shows an
      // error message. 20s covers the worst case of re-fetching a big
      // bundle + beautify + focus-range compute.
      const deadline = Date.now() + 20000;
      let state = null;
      while (Date.now() < deadline) {
        state = await page.evaluate(() => {
          const code = document.getElementById("code-output");
          const text = code ? code.textContent : "";
          return {
            textLen: text.length,
            hasFocus: !!document.getElementById("btn-focus") &&
                      document.getElementById("btn-focus").textContent.includes("Focus"),
            text: text.length > 0 ? text : null,
            statusText: (document.getElementById("status") || {}).textContent || "",
          };
        });
        if (state.textLen > 0) break;
        await sleep(300);
      }
      if (!state || state.textLen === 0) { err("  (viewer did not render within 20 s)"); return; }
      const rendered = state.text || "";
      const renderedLines = rendered.split("\n").length;
      // Button label convention in viewer.js: "Focus" while FOCUSED
      // (clicking un-focuses to Full), "Full" while full (clicking
      // focuses). Toggle it once to get the full-code line count.
      const initialBtnText = await page.evaluate(() => {
        const btn = document.getElementById("btn-focus");
        return btn ? btn.textContent.trim() : null;
      });
      if (initialBtnText === "Focus") {
        // Currently in focused mode — click to see full.
        await page.evaluate(() => document.getElementById("btn-focus").click());
      }
      await sleep(600);
      const full = await page.evaluate(() => {
        const code = document.getElementById("code-output");
        return code ? code.textContent : "";
      });
      const fullLines = full.split("\n").length;
      // Restore focused mode so the preview below matches what the
      // reviewer would see after clicking through from the popup.
      const afterToggleText = await page.evaluate(() => {
        const btn = document.getElementById("btn-focus");
        return btn ? btn.textContent.trim() : null;
      });
      if (afterToggleText === "Full") {
        await page.evaluate(() => document.getElementById("btn-focus").click());
        await sleep(400);
      }
      log(`findingId: ${f.id}  ${f.sourceUrl}  L${f.line}:C${f.col}`);
      log(`  viewer URL: ${url}`);
      log(`  full beautified code: ${fullLines.toLocaleString()} lines`);
      log(`  focused code:         ${renderedLines.toLocaleString()} lines` +
          (fullLines > 0 ? `  (${(100 * renderedLines / fullLines).toFixed(1)}% — ${fullLines - renderedLines} lines hidden)` : ""));
      // Summarise the range structure — how many contiguous blocks the
      // focus carved out. Each "lines hidden" separator marks a gap
      // between ranges.
      const gapCount = (rendered.match(/\u00b7\u00b7\u00b7 \d+ lines hidden \u00b7\u00b7\u00b7/g) || []).length;
      log(`  focus ranges:         ${gapCount + 1} contiguous blocks (${gapCount} gap separators)`);
      if (state.statusText) log(`  status bar: ${state.statusText}`);
      if (previewN > 0) {
        log(`\n  focused preview (first ${previewN} lines):`);
        rendered.split("\n").slice(0, previewN).forEach((l, i) => {
          log(`  ${String(i + 1).padStart(4)}  ${l.length > 200 ? l.slice(0, 200) + "…" : l}`);
        });
      }
    } finally {
      await page.close().catch(() => {});
    }
  });
}

async function cmdFindingExploit(rest) {
  const { positional, flags } = parseArgs(rest);
  const arg = positional[0];
  if (!arg) throw new Error("usage: finding.exploit <id|index> [--page-url <url>] [--wait <ms>] [--param <name>]");
  const pageUrlOverride = flags["page-url"] || null;
  const waitMs = flags.wait != null ? Number(flags.wait) : 5000;
  const paramOverride = flags.param || null;
  await withBrowser(async (browser) => {
    const byId = await collectFindings(browser);
    const rows = Object.values(byId);
    let f = null;
    const asIdx = Number(arg);
    if (!Number.isNaN(asIdx) && asIdx >= 0 && asIdx < rows.length && arg.length < 6) f = rows[asIdx];
    else f = byId[arg] || null;
    if (!f) { err("no such finding"); return; }

    const strategy = classifyProbeStrategy(f);
    if (!strategy) {
      err(`  (no probe strategy yet for source "${f.source || f.sourceType}" — supported: location.hash/search/href/pathname + event.data postmessage)`);
      return;
    }

    // Harness doesn't run the probe itself — orchestration lives in
    // the extension so the exact same code runs when the user clicks
    // the popup "Verify" button. Harness is a thin dispatcher:
    // classify, send, print.
    //
    // pageUrl resolution:
    //   1. --page-url explicit override from reviewer
    //   2. the finding's own recorded pageUrl (the page where the
    //      extension observed this finding at analysis time)
    //   3. if neither — refuse; we never guess which tab to probe
    let pageUrl = pageUrlOverride;
    if (!pageUrl) {
      // Pull the finding's observed pageUrl from the SW's store.
      pageUrl = await evalSW(browser, `
        const e = globalStore.securityFindings.get(arg.sourceUrl);
        return e && e.pageUrl ? e.pageUrl : null;
      `, { sourceUrl: f.sourceUrl });
    }
    if (!pageUrl) {
      err(`  (no pageUrl — finding's sourceUrl has no recorded observation page. Use --page-url <https://…> after reading the code.)`);
      return;
    }

    log(`findingId: ${f.id}`);
    log(`  strategy:  ${strategy.kind}  (source: ${strategy.sourceName})`);
    log(`  pageUrl:   ${pageUrl}`);

    // For search strategy, derive the exact param name the AST
    // observed the page reading (e.g. `.get("q")` → "q"). We DON'T
    // guess from a common-names list — the finding's own taint path
    // already knows which key the sink traced back to. Reviewer can
    // override with --param when they've audited the code themselves.
    let paramName = paramOverride;
    if (strategy.kind === "search" && !paramName) {
      if (Array.isArray(f.taintPath)) {
        for (const h of f.taintPath) {
          if (h.kind === "method-call" && typeof h.arg === "string" && h.arg.length > 0) { paramName = h.arg; break; }
        }
      }
      if (!paramName) {
        err(`  (can't derive paramName for search strategy from finding's taint path —`);
        err(`   the AST didn't observe a .get("…") / .has("…") call on the tainted URLSearchParams.`);
        err(`   Re-run with --param <name> after reading the code at the finding's location.)`);
        return;
      }
      log(`  paramName: ${JSON.stringify(paramName)}  (from taint path's method-call hop)`);
    } else if (paramName) {
      log(`  paramName: ${JSON.stringify(paramName)}  (reviewer-supplied --param)`);
    }

    // For postmessage strategy, derive the field path the handler
    // reads from message.data. Walk the taint path from the source
    // outward, collecting [member] hop names (other hop kinds are
    // intermediaries we ignore for shape). The first ".data" hop
    // after an event.data source is the implicit MessageEvent.data
    // access — skip it, since our payload IS what event.data becomes.
    // This is the AST's direct observation of which fields the sink
    // reads, NOT a guess about common shapes.
    let fieldPath = null;
    // decoders: the taint path may traverse string-decode calls between
    // the source and the sink (JSON.parse, unescape, decodeURIComponent,
    // atob, …). The probe must apply the matching ENCODER to its payload
    // in the opposite order so the handler's decoder chain yields the
    // shaped structure back. Recorded in source-to-sink order; probe
    // applies in sink-to-source order (i.e. reverse) when wrapping.
    const decoders = [];
    if (strategy.kind === "postmessage") {
      fieldPath = [];
      if (Array.isArray(f.taintPath)) {
        let sawSource = false;
        for (const h of f.taintPath) {
          if (h.kind === "source") { sawSource = true; continue; }
          if (!sawSource) continue;
          if (h.kind === "call-arg") {
            const d = String(h.desc || "");
            if (/\.parse\(\)/.test(d)) { decoders.push("json"); continue; }
            if (/^unescape\(\)/.test(d)) { decoders.push("escape"); continue; }
            if (/^decodeURIComponent\(\)/.test(d)) { decoders.push("uri-component"); continue; }
            if (/^decodeURI\(\)/.test(d)) { decoders.push("uri"); continue; }
            if (/^atob\(\)/.test(d)) { decoders.push("base64"); continue; }
          }
          if (h.kind !== "member") continue;
          const name = String(h.desc || "").replace(/^\./, "");
          if (!name || name === "?") continue;
          // Drop the implicit `.data` hop on the MessageEvent —
          // postMessage delivers data AS event.data, so our payload
          // is event.data by construction; don't wrap it in another
          // {data:…} layer.
          if (fieldPath.length === 0 && name === "data" && /event\.data/.test(f.source || "")) continue;
          fieldPath.push(name);
        }
      }
      const decodersLabel = decoders.length ? `, decoders: ${decoders.join("→")}` : "";
      if (fieldPath.length) {
        log(`  fieldPath: ${fieldPath.map(JSON.stringify).join(" → ")}  (from taint path's member hops${decodersLabel})`);
      } else {
        log(`  fieldPath: (empty — handler reads event.data directly as a string${decodersLabel})`);
      }
    }

    // Drive the same start/poll path the popup uses — same orchestration
    // code runs regardless of caller, so a bug in harness-surface is
    // also a bug in the popup button and vice versa.
    const start = await evalSW(browser, `
      return startExploitProbe({
        strategy: arg.strategy,
        pageUrl: arg.pageUrl,
        sourceUrl: arg.sourceUrl,
        waitMs: arg.waitMs,
        findingId: arg.findingId,
        paramName: arg.paramName,
        fieldPath: arg.fieldPath,
        sinkType: arg.sinkType,
        sinkName: arg.sinkName,
        decoders: arg.decoders,
        preconditions: arg.preconditions,
      });
    `, { strategy: strategy.kind, pageUrl, sourceUrl: f.sourceUrl, waitMs, findingId: f.id, paramName, fieldPath, sinkType: f.type || null, sinkName: f.sink || null, decoders, preconditions: Array.isArray(f.preconditions) ? f.preconditions : null });
    if (!start || !start.marker) { err(`  (start failed: ${JSON.stringify(start)})`); return; }
    log(`  sessionId: ${start.marker}  (running…)`);

    // Poll status until done. Observation window is waitMs + overhead
    // (postmessage strategy needs ~4s extra for attacker→target hop).
    const maxWait = waitMs + (strategy.kind === "postmessage" ? 9000 : 3000);
    const pollInterval = 500;
    const deadline = Date.now() + maxWait;
    let status;
    while (Date.now() < deadline) {
      status = await evalSW(browser, `
        const s = _probeSessions.get(arg.m);
        return s ? { status: s.status, hits: s.hits.slice(), executed: s.executed || null, error: s.error, strategy: s.strategy, pageUrl: s.pageUrl, marker: s.marker } : null;
      `, { m: start.marker });
      if (status && (status.status === "done" || status.status === "error")) break;
      await sleep(pollInterval);
    }
    if (!status) { err(`  (session vanished before poll completed)`); return; }
    if (status.error) { err(`  probe error: ${status.error}`); return; }

    const hits = Array.isArray(status.hits) ? status.hits : [];
    const executed = status.executed || null;

    if (!hits.length && !executed) {
      log(`\n  RESULT: no sink observed and no payload executed (marker "${start.marker}")`);
      log(`  VERDICT: NOT REPRODUCED — may require user interaction, specific DOM state, or a handler shape the payload didn't match.`);
      return;
    }

    if (hits.length) {
      log(`\n  TAINT REACH (${hits.length} sink call with attacker marker):`);
      hits.forEach((h, i) => {
        log(`    ${i}. [${h.sink}]  url=${short(h.url || "?", 100)}`);
        log(`       value: ${short(h.value || "", 300)}`);
      });
    } else {
      log(`\n  TAINT REACH: none recorded by intercept.js wrappers.`);
    }

    if (executed) {
      const vectorDescriptions = {
        html: "img onerror fired (inline handler ran)",
        svg: "svg onload fired (inline handler ran)",
        js: "eval/Function ran the JS payload",
        href: "javascript: URL navigation ran the payload",
        dom: "DOM parsed the <div> (HTML parsing confirmed; CSP-tolerant signal)",
      };
      const firedKeys = Object.keys(executed).filter(k => executed[k]);
      const activeExec = firedKeys.some(k => k !== "dom"); // inline handler ran
      const parsedOnly = firedKeys.length && !activeExec;  // dom only
      log(`\n  EXECUTION CONFIRMED — target interpreted the payload:`);
      for (const v of firedKeys) {
        log(`    ${v}: ${new Date(executed[v]).toISOString()}  — ${vectorDescriptions[v] || "unknown vector"}`);
      }
      if (activeExec) {
        log(`\n  VERDICT: REAL EXPLOIT — attacker input was parsed AND an inline handler executed.`);
      } else if (parsedOnly) {
        log(`\n  VERDICT: HTML PARSING CONFIRMED — attacker input was parsed as HTML (the <div> landed in the DOM).`);
        log(`  The target's CSP appears to block inline event handlers, so the onerror/onload vectors did not fire.`);
        log(`  A CSP-bypass payload (MathML / iframe srcdoc / DOM clobbering) might still achieve script execution on this target.`);
      }
    } else if (hits.length) {
      log(`\n  EXECUTION: none — the payload reached a sink but was not parsed as HTML / evaluated as JS.`);
      log(`  VERDICT: TAINT REACH ONLY — likely the sink received the marker as a plain string (e.g. innerText, or a sanitizer stripped active content before innerHTML).`);
    }
  });
}

async function cmdAuditSchema(rest) {
  const { positional, flags } = parseArgs(rest);
  const svcName = positional[0];
  const minObs = flags["min-obs"] != null ? Number(flags["min-obs"]) : 1;
  const showWorst = flags["show-worst"] != null ? Number(flags["show-worst"]) : 5;
  if (!svcName) throw new Error("usage: audit.schema <service> [--min-obs N] [--show-worst N]");
  await withBrowser(async (browser) => {
    const data = await evalSW(browser, `
      const e = globalStore.discoveryDocs.get(${JSON.stringify(svcName)});
      if (!e || !e.doc) return { error: "service not found" };
      const doc = e.doc;
      const methods = [];
      for (const [bName, bObj] of Object.entries(doc.resources || {})) {
        for (const [mName, m] of Object.entries(bObj.methods || {})) {
          methods.push({
            id: m.id,
            bucketName: bName + "/" + mName,
            httpMethod: m.httpMethod,
            origin: m.origin,
            path: m.path,
            requestCount: m._stats?.requestCount || 0,
            paramCount: Object.keys(m.parameters || {}).length,
            requestRef: m.request?.$ref || null,
            responseRef: m.response?.$ref || null,
            parameters: m.parameters || {},
            bodyFieldStatKeys: Object.keys(m._stats?.bodyFields || {}),
          });
        }
      }
      return { schemas: doc.schemas, methods };
    `);
    if (data.error) { err(data.error); return; }

    // Per-method readiness rollup. Leaves are counted across params + body.
    const rows = [];
    for (const m of data.methods) {
      if (m.requestCount < minObs) continue;
      const paramProv = Object.create(null);
      let paramLeaves = 0;
      let paramRequiredGaps = 0;
      for (const [pName, pDef] of Object.entries(m.parameters)) {
        paramLeaves++;
        const prov = pDef._exampleValueSource || "none";
        paramProv[prov] = (paramProv[prov] || 0) + 1;
        if (pDef.required && !pDef._exampleValueSource) paramRequiredGaps++;
      }
      const bodySum = m.requestRef
        ? _summariseProvenance(data.schemas[m.requestRef], { schemas: data.schemas })
        : { byProvenance: {}, leaves: [], gaps: [] };
      const combined = Object.assign(Object.create(null), paramProv);
      for (const [k, v] of Object.entries(bodySum.byProvenance)) combined[k] = (combined[k] || 0) + v;
      const totalLeaves = paramLeaves + bodySum.leaves.length;
      const realLeaves = (combined["observed-default"] || 0) + (combined["observed-top"] || 0) +
                        (combined["ast-constraint"] || 0) + (combined["enum"] || 0);
      const synthLeaves = (combined["format-synth"] || 0) + (combined["range-min"] || 0);
      const typeDefLeaves = combined["type-default"] || 0;
      const unsourcedLeaves = combined["none"] || 0;
      rows.push({
        id: m.id,
        httpMethod: m.httpMethod,
        requestCount: m.requestCount,
        totalLeaves,
        paramLeaves,
        bodyLeaves: bodySum.leaves.length,
        realLeaves,
        synthLeaves,
        typeDefLeaves,
        unsourcedLeaves,
        requiredGaps: paramRequiredGaps + bodySum.gaps.length,
        bodyFieldStatKeys: m.bodyFieldStatKeys.length,
        realPct: totalLeaves ? realLeaves / totalLeaves : 0,
        combined,
      });
    }
    if (!rows.length) { log(`no methods in ${svcName} with requestCount >= ${minObs}`); return; }

    // Service-wide aggregate
    const totalLeaves = rows.reduce((s, r) => s + r.totalLeaves, 0);
    const realLeaves = rows.reduce((s, r) => s + r.realLeaves, 0);
    const synthLeaves = rows.reduce((s, r) => s + r.synthLeaves, 0);
    const typeDefLeaves = rows.reduce((s, r) => s + r.typeDefLeaves, 0);
    const unsourcedLeaves = rows.reduce((s, r) => s + r.unsourcedLeaves, 0);
    const requiredGaps = rows.reduce((s, r) => s + r.requiredGaps, 0);

    log(`audit.schema: ${svcName}  ${rows.length}/${data.methods.length} methods meet min-obs=${minObs}`);
    log(`\n  service-wide readiness:`);
    log(`    total leaves: ${totalLeaves}`);
    log(`    real observations: ${realLeaves}  (${totalLeaves ? Math.round(realLeaves/totalLeaves*100) : 0}%)`);
    log(`    synthesized:        ${synthLeaves}  (${totalLeaves ? Math.round(synthLeaves/totalLeaves*100) : 0}%)`);
    log(`    type-default:       ${typeDefLeaves}  (${totalLeaves ? Math.round(typeDefLeaves/totalLeaves*100) : 0}%)`);
    log(`    unsourced:          ${unsourcedLeaves}  (${totalLeaves ? Math.round(unsourcedLeaves/totalLeaves*100) : 0}%)`);
    log(`    required gaps:      ${requiredGaps}  (required fields with NO example → verify will fail auth/validation)`);

    // Per-method summary, sorted by readiness ASC so worst methods float up
    rows.sort((a, b) => a.realPct - b.realPct);
    log(`\n  per-method readiness (sorted worst-first):`);
    log(`    ${"method".padEnd(60)} ${"HTTP".padEnd(5)} ${"rc".padStart(5)} ${"leaves".padStart(7)} ${"real%".padStart(6)}  ${"gaps".padStart(5)}  ${"body".padStart(5)}`);
    for (const r of rows.slice(0, 40)) {
      const pct = Math.round(r.realPct * 100);
      log(`    ${short(r.id, 60).padEnd(60)} ${(r.httpMethod||"?").padEnd(5)} ${String(r.requestCount).padStart(5)} ${String(r.totalLeaves).padStart(7)} ${String(pct + "%").padStart(6)}  ${String(r.requiredGaps).padStart(5)}  ${String(r.bodyLeaves).padStart(5)}`);
    }
    if (rows.length > 40) log(`    … +${rows.length - 40} more`);

    // Drill into worst N — show top-gap methods with their provenance histogram
    log(`\n  worst ${Math.min(showWorst, rows.length)} methods (most gaps or lowest readiness):`);
    for (const r of rows.slice(0, showWorst)) {
      log(`\n    ${r.id}  (rc=${r.requestCount}, ${r.totalLeaves} leaves, ${Math.round(r.realPct*100)}% real)`);
      for (const [src, n] of Object.entries(r.combined).sort((a, b) => b[1] - a[1])) {
        log(`      ${String(n).padStart(4)}  ${src}`);
      }
      if (r.requiredGaps) log(`      ${r.requiredGaps} required fields with no example`);
      if (r.bodyFieldStatKeys && r.bodyLeaves > 0 && r.bodyFieldStatKeys === 0) {
        log(`      (body schema present but m._stats.bodyFields is empty — method captured before JSON decode, or body never observed)`);
      }
    }
  });
}

async function cmdAuditSinks(rest) {
  const { positional, flags } = parseArgs(rest);
  const substr = positional[0] || null;
  const showPerKind = flags.show != null ? Number(flags.show) : 4;
  const missedOnly = flags["missed-only"] === true;
  const contextChars = flags.context != null ? Number(flags.context) : 140;
  await withBrowser(async (browser) => {
    // Pull every (scriptUrl, findings-by-line) map so we can cross-check
    // in a single SW roundtrip. Line-grained index is enough for MVP;
    // column-grained would help distinguish two sinks on the same
    // minified line but is easy to add later.
    const scriptIndex = await evalSW(browser, `
      const out = [];
      // Collect per-tab resolver errors first, keyed by the script URL
      // they point at. A fetch( grep match that lines up with a
      // resolverError is a KNOWN gap (identifier didn't trace) — not a
      // missed classifier. Distinguishing these two saves the reviewer
      // from opening 50 fetch( sites that are all resolver gaps.
      const resolverErrorsByUrlLine = {};
      // Resolver errors live on the cached combined-analysis result in
      // globalStore.scriptCache — each entry has the analysis +
      // scriptOffsets[] mapping combined-bundle line to per-script URL.
      // Remap combined-bundle (line,col) to per-script (line,col) so the
      // gap key matches securityFindings' per-script coordinates.
      for (const [, cached] of globalStore.scriptCache.entries()) {
        const analysis = cached.result;
        const offsets = cached.scriptOffsets || [];
        if (!analysis || !offsets.length) continue;
        for (const e of (analysis.resolverErrors || [])) {
          const loc = (e.message || "").match(/@L(\\d+):C(\\d+)/);
          if (!loc) continue;
          const combinedLine = Number(loc[1]);
          // Walk offsets[] (sorted by lineStart) to find the script
          // whose range spans combinedLine. Last script runs to EOF.
          let found = null;
          for (let oi = 0; oi < offsets.length; oi++) {
            const cur = offsets[oi].lineStart;
            const nxt = oi + 1 < offsets.length ? offsets[oi + 1].lineStart : Number.MAX_SAFE_INTEGER;
            if (combinedLine >= cur && combinedLine < nxt) { found = offsets[oi]; break; }
          }
          if (!found) continue;
          const perScriptLine = combinedLine - found.lineStart + 1;
          const key = found.url + "|" + perScriptLine;
          (resolverErrorsByUrlLine[key] = resolverErrorsByUrlLine[key] || []).push(e.message);
        }
      }
      const out2 = [];
      for (const [url, v] of globalStore.securityFindings) {
        const byLine = {};
        const record = (s, category) => {
          const line = s.location?.line;
          if (line == null) return;
          const tag = category === "sink" ? (s.type + ":" + s.sink) : s.type;
          (byLine[line] = byLine[line] || []).push(tag);
        };
        for (const s of (v.securitySinks || [])) record(s, "sink");
        for (const s of (v.dangerousPatterns || [])) record(s, "dangerous");
        out2.push({
          url, byLine,
          findingCount: (v.securitySinks||[]).length + (v.dangerousPatterns||[]).length,
        });
      }
      return { scripts: out2, resolverErrorsByUrlLine };
    `);
    const { scripts, resolverErrorsByUrlLine } = scriptIndex;
    const filtered = substr ? scripts.filter(s => s.url.toLowerCase().includes(substr.toLowerCase())) : scripts;
    if (!filtered.length) { log("no AST-analyzed scripts match"); return; }

    const aggregate = Object.create(null);
    for (const p of SINK_GREP_PATTERNS) aggregate[p.name] = { grep: 0, covered: 0, resolverGap: 0, missed: 0, samples: [] };
    let scriptsScanned = 0, scriptsSkipped = 0;

    for (const script of filtered) {
      if (script.url.startsWith("data:")) { scriptsSkipped++; continue; }
      let src;
      try { src = await fetchDecoded(script.url); }
      catch (e) { scriptsSkipped++; continue; }
      scriptsScanned++;
      const lineStarts = _buildLineStarts(src);
      for (const pat of SINK_GREP_PATTERNS) {
        pat.re.lastIndex = 0;
        let match;
        while ((match = pat.re.exec(src)) !== null) {
          // Pre-filter known-safe patterns per sink type. For HTML-writing
          // sinks, empty-string RHS (`el.innerHTML = ""`) is a DOM reset,
          // not content injection — the AST correctly ignores it, and
          // surfacing it here makes reviewers sift through inert noise
          // on first pass. The grep regex captures `.innerHTML =` plus
          // one non-`=` char; that char is the first RHS char (`"`, `'`,
          // or `\``). If the next char is the matching closing quote,
          // the RHS is an empty-string literal — not an XSS vector.
          if (pat.name === "innerHTML=" || pat.name === "outerHTML=") {
            const lastCh = src[match.index + match[0].length - 1];
            if (lastCh === '"' || lastCh === "'" || lastCh === "`") {
              const nextCh = src[match.index + match[0].length];
              if (nextCh === lastCh) continue;
            }
          }
          // `new Function(code)` with a string literal as the LAST argument
          // is a compile-time constant — the AST's _processSecurityNewSink
          // correctly ignores it since the literal can't carry attacker
          // content. Most real-world occurrences are the `new Function("return this")()`
          // globalThis polyfill. Skip to avoid polluting the unexplained count.
          if (pat.name === "new Function(") {
            let p = match.index + match[0].length;
            // Walk arguments shallowly: find the last top-level string literal
            // by scanning until the matching `)` at paren-depth 0.
            let depth = 1;
            let lastLiteralEnd = -1;
            let lastLiteralStart = -1;
            let strCh = null;
            while (p < src.length && depth > 0) {
              const c = src[p];
              if (strCh) {
                if (c === "\\") { p += 2; continue; }
                if (c === strCh) {
                  lastLiteralEnd = p;
                  strCh = null;
                }
              } else if (c === '"' || c === "'" || c === "`") {
                strCh = c;
                lastLiteralStart = p;
              } else if (c === "(") depth++;
              else if (c === ")") depth--;
              p++;
            }
            // Last arg is the function body; if we saw a string literal
            // immediately before the closing `)` (only whitespace between),
            // it's a literal-code Function. Skip.
            if (lastLiteralEnd > 0 && depth === 0) {
              let q = lastLiteralEnd + 1;
              while (q < p && /\s/.test(src[q])) q++;
              if (q === p - 1) continue; // literal was the last arg
            }
          }
          aggregate[pat.name].grep++;
          const pos = _offsetToLineCol(lineStarts, match.index);
          const findingsHere = script.byLine[pos.line] || [];
          const covered = findingsHere.some(tag => pat.astTags.includes(tag));
          const resolverGapsHere = resolverErrorsByUrlLine[script.url + "|" + pos.line] || [];
          if (covered) {
            aggregate[pat.name].covered++;
          } else if (NETWORK_EGRESS_GREP_NAMES.has(pat.name) && resolverGapsHere.length) {
            // Network-egress grep (fetch/sendBeacon/xhr.open/importScripts) +
            // resolverError on same line = known gap, not a missed
            // classifier. Count separately so the reviewer sees how many
            // "unexplained" matches are actually resolver work waiting
            // to be done vs patterns the classifier never saw.
            aggregate[pat.name].resolverGap++;
          } else {
            aggregate[pat.name].missed++;
            if (aggregate[pat.name].samples.length < showPerKind) {
              const half = Math.floor(contextChars / 2);
              const start = Math.max(0, match.index - half);
              const end = Math.min(src.length, match.index + half);
              const snippet = src.slice(start, end).replace(/\s+/g, " ");
              aggregate[pat.name].samples.push({
                url: script.url,
                line: pos.line,
                column: pos.column,
                snippet,
                findingsOnLine: findingsHere,
                resolverGaps: resolverGapsHere.slice(0, 3),
              });
            }
          }
        }
      }
    }

    log(`audit.sinks: scanned ${scriptsScanned} AST-analyzed scripts (${scriptsSkipped} skipped — data-url or fetch-failed)${substr ? "  [filter: " + substr + "]" : ""}`);

    // Categorize resolver gaps by root-cause shape so a reviewer sees
    // WHICH kinds of static analyses are missing, not just the raw count.
    // Each gap's error message contains either a descriptor like
    // "this.collectorUrl@L…", "e@L…", "new URL(…, …)@L…", etc. Group by
    // the structural prefix. Categories intentionally overlap with the
    // CLAUDE.md resolver targets (this-field chains, DOM reads, HOF dead
    // ends) so progress on a specific category is directly measurable.
    const gapCategories = {
      "this.X (class field / getter)": 0,
      "DOM attribute read (getAttribute/dataset/.action/.href)": 0,
      "parameter dead-end (identifier)": 0,
      "new URL(…) with unresolved input": 0,
      "template literal (unresolved interpolation)": 0,
      "OptionalMemberExpression / Conditional / ObjectExpression": 0,
      "other": 0,
    };
    for (const key of Object.keys(resolverErrorsByUrlLine)) {
      for (const msg of resolverErrorsByUrlLine[key]) {
        const desc = (msg || "").replace(/^fetch URL resolver gap: /, "").replace(/@L.*$/, "");
        if (/^this\./.test(desc)) gapCategories["this.X (class field / getter)"]++;
        else if (/getAttribute\(\)|dataset\.|\.action\b|\.href\b|currentTarget/.test(desc)) {
          gapCategories["DOM attribute read (getAttribute/dataset/.action/.href)"]++;
        }
        else if (/^new URL/.test(desc)) gapCategories["new URL(…) with unresolved input"]++;
        else if (/^<template>/.test(desc)) gapCategories["template literal (unresolved interpolation)"]++;
        else if (/^(OptionalMemberExpression|ConditionalExpression|ObjectExpression|UnaryExpression|<Optional)/.test(desc)) {
          gapCategories["OptionalMemberExpression / Conditional / ObjectExpression"]++;
        }
        else if (/^[a-zA-Z_][a-zA-Z_0-9]*$/.test(desc)) gapCategories["parameter dead-end (identifier)"]++;
        else gapCategories["other"]++;
      }
    }
    const totalGaps = Object.values(gapCategories).reduce((a, b) => a + b, 0);
    if (totalGaps > 0) {
      log(`\n  resolver-gap root-cause breakdown (total=${totalGaps}):`);
      for (const [cat, n] of Object.entries(gapCategories)) {
        if (n === 0) continue;
        log(`    ${String(n).padStart(5)}  ${cat}`);
      }
    }

    log(`\n  per-pattern counts (each grep-match either matches an AST finding on the same line, or is unexplained — unexplained hits are candidates to read):`);
    const keys = Object.keys(aggregate).sort((a, b) => (aggregate[b].grep) - (aggregate[a].grep));
    for (const k of keys) {
      const a = aggregate[k];
      if (a.grep === 0) continue;
      const gapCol = a.resolverGap > 0 ? `  resolver-gap=${String(a.resolverGap).padStart(3)}` : "";
      log(`    ${k.padEnd(25)} grep=${String(a.grep).padStart(5)}  ast-flagged=${String(a.covered).padStart(5)}  unexplained=${String(a.missed).padStart(5)}${gapCol}`);
    }

    log(`\n  unexplained grep-matches to read (up to ${showPerKind} per pattern). Each is EITHER a missed FP — go read the surrounding source to decide.`);
    log(`  Use: finding <id> for an AST finding on the same line, or script <url> for the raw JS around a specific location.`);
    for (const k of keys) {
      const a = aggregate[k];
      if (a.missed === 0) continue;
      if (missedOnly && a.samples.length === 0) continue;
      log(`\n  ${k}  (${a.missed} unexplained):`);
      for (const s of a.samples) {
        log(`    ${s.url}`);
        const annotations = [];
        if (s.findingsOnLine.length) annotations.push("other AST findings on this line: " + s.findingsOnLine.join(", "));
        if (s.resolverGaps && s.resolverGaps.length) annotations.push("resolver gap: " + s.resolverGaps[0].replace(/^fetch URL resolver gap: /, ""));
        log(`      L${s.line}:C${s.column}${annotations.length ? "   (" + annotations.join("; ") + ")" : ""}`);
        log(`      …${s.snippet}…`);
      }
    }
  });
}

// audit.gaps — for each unique resolver gap, fetch the per-script source
// and print the actual JS at the gap's root-cause line/column. Pillar 12:
// every harness command gives ground-truth, not summaries. Without this,
// closing a gap means writing a one-off node script every time — see
// `testing/dump-gaps.js` for the original ad-hoc form. Reuses
// scriptOffsets[] from globalStore.scriptCache to map combined-bundle
// (line,col) back to per-script (url,line,col).
async function cmdAuditGaps(rest) {
  const { positional, flags } = parseArgs(rest);
  const substr = positional[0] || null;
  const limit = flags.limit != null ? Number(flags.limit) : 30;
  const contextChars = flags.context != null ? Number(flags.context) : 250;
  await withBrowser(async (browser) => {
    const gapsRaw = await evalSW(browser, `
      const out = [];
      const seen = new Set();
      for (const [k, cached] of globalStore.scriptCache.entries()) {
        const a = cached.result;
        const offsets = cached.scriptOffsets || [];
        if (!a || !offsets.length) continue;
        for (const e of (a.resolverErrors || [])) {
          const m = (e.message || "").match(/@L(\\d+):C(\\d+)/);
          const m2 = (e.message || "").match(/root cause @L(\\d+):C(\\d+)/);
          if (!m) continue;
          const cl = Number(m[1]), cc = Number(m[2]);
          const rl = m2 ? Number(m2[1]) : cl;
          const rc = m2 ? Number(m2[2]) : cc;
          let found = null;
          for (let oi = 0; oi < offsets.length; oi++) {
            const cur = offsets[oi].lineStart;
            const nxt = oi + 1 < offsets.length ? offsets[oi + 1].lineStart : Number.MAX_SAFE_INTEGER;
            if (rl >= cur && rl < nxt) { found = { url: offsets[oi].url, lineStart: offsets[oi].lineStart }; break; }
          }
          if (!found) continue;
          const perLine = rl - found.lineStart + 1;
          const sigKey = found.url + "|" + perLine + "|" + rc;
          if (seen.has(sigKey)) continue;
          seen.add(sigKey);
          out.push({ msg: e.message, scriptUrl: found.url, perScriptLine: perLine, perScriptCol: rc });
        }
      }
      return out;
    `);
    const filtered = substr ? gapsRaw.filter(g => g.scriptUrl.toLowerCase().includes(substr.toLowerCase())) : gapsRaw;
    if (!filtered.length) { log(substr ? `no gaps match "${substr}"` : "no resolver gaps"); return; }
    log(`audit.gaps: ${filtered.length} unique gap${filtered.length === 1 ? "" : "s"}${substr ? ` matching "${substr}"` : ""} (showing ${Math.min(filtered.length, limit)}):`);
    const fetched = new Map();
    for (let i = 0; i < Math.min(filtered.length, limit); i++) {
      const g = filtered[i];
      let snippet;
      try {
        let src = fetched.get(g.scriptUrl);
        if (!src) { src = await fetchDecoded(g.scriptUrl); fetched.set(g.scriptUrl, src); }
        const lns = src.split("\n");
        const lnText = lns[g.perScriptLine - 1] || "";
        const start = Math.max(0, g.perScriptCol - 100);
        const end = Math.min(lnText.length, g.perScriptCol + contextChars);
        snippet = (start > 0 ? "..." : "") + lnText.slice(start, end) + (end < lnText.length ? "..." : "");
      } catch (e) { snippet = `<fetch error: ${e.message}>`; }
      log(`\n  ${g.msg}`);
      log(`    url:    ${g.scriptUrl}`);
      log(`    L${g.perScriptLine}:C${g.perScriptCol}`);
      log(`    src:    ${snippet}`);
    }
  });
}

// audit.perf — per-script analysis perf breakdown across captured scripts.
// Reads result.perf populated by analyzeJSBundle. Sorts worst-first by
// totalMs. Pillar 13: measure on real bundles, not toy fixtures.
async function cmdAuditPerf(rest) {
  const { positional, flags } = parseArgs(rest);
  const substr = positional[0] || null;
  const limit = flags.limit != null ? Number(flags.limit) : 30;
  const sortBy = flags.sort || "total";  // total | parse | mainPass | prePass | mb-per-s
  await withBrowser(async (browser) => {
    const rows = await evalSW(browser, `
      // A page is reviewed as the COMBINATION of all scripts it loads —
      // cross-script inter-procedural analysis only works when the
      // analyzer sees them together. So perf is per-combined-bundle,
      // and the cache key is the SHA256 of all script hashes joined.
      // audit.perf surfaces those combined-bundle costs plus any
      // per-script fallbacks (when the combined offscreen analysis
      // failed) from tab._astResults. Dedup by (sizeBytes, totalMs,
      // sourceUrl) tuple to collapse the same analysis appearing in
      // both stores.
      const out = [];
      const seenTuples = new Set();
      function pushUnique(url, perf, source) {
        if (!perf) return;
        const key = (perf.sizeBytes||0) + ":" + (perf.totalMs||0) + ":" + url;
        if (seenTuples.has(key)) return;
        seenTuples.add(key);
        out.push({ url, perf, source });
      }
      // Combined-script cache: one entry per multi-script bundle.
      // Skip entries without scriptOffsets — those are stale single-
      // script cache entries from a removed code path; combined-bundle
      // analyses always populate offsets.
      for (const [k, cached] of globalStore.scriptCache.entries()) {
        const a = cached.result;
        if (!a || !a.perf) continue;
        const offsets = cached.scriptOffsets || [];
        if (offsets.length === 0) continue;
        const url = (cached.tabUrl || offsets[0].url) +
          " (combined " + offsets.length + " script" + (offsets.length === 1 ? "" : "s") + ")";
        pushUnique(url, a.perf, "combined");
      }
      // Per-tab _astResults: individual analyses (fallback when combined fails)
      for (const [tid, tab] of state.tabs.entries()) {
        const ar = tab._astResults || [];
        for (const a of ar) {
          if (!a || !a.perf) continue;
          pushUnique(a.sourceUrl || "(tab " + tid + " inline)", a.perf, "individual-fallback");
        }
      }
      return out;
    `);
    const filtered = substr ? rows.filter(r => r.url.toLowerCase().includes(substr.toLowerCase())) : rows;
    if (!filtered.length) { log(substr ? `no scripts match "${substr}"` : "no scripts with perf data"); return; }
    const sortKey = (r) => {
      const p = r.perf;
      if (sortBy === "parse") return -(p.phaseMs?.parse || 0);
      if (sortBy === "mainPass") return -(p.phaseMs?.mainPass || 0);
      if (sortBy === "prePass") return -(p.phaseMs?.prePass || 0);
      if (sortBy === "mb-per-s") return (p.sizeBytes / 1048576) / (p.totalMs / 1000);
      return -(p.totalMs || 0);
    };
    filtered.sort((a, b) => sortKey(a) - sortKey(b));
    log(`audit.perf: ${filtered.length} script${filtered.length === 1 ? "" : "s"}${substr ? ` matching "${substr}"` : ""} (showing ${Math.min(filtered.length, limit)}, sorted by ${sortBy}):`);
    let totalBytes = 0, totalMs = 0;
    for (const r of filtered) { totalBytes += r.perf.sizeBytes || 0; totalMs += r.perf.totalMs || 0; }
    const totalMb = totalBytes / 1048576;
    const totalSec = totalMs / 1000;
    log(`  aggregate: ${totalSec.toFixed(2)}s for ${totalMb.toFixed(2)}MB (${totalSec > 0 ? (totalMb / totalSec).toFixed(2) : "∞"}MB/s)`);
    for (let i = 0; i < Math.min(filtered.length, limit); i++) {
      const r = filtered[i];
      const p = r.perf;
      const ph = p.phaseMs || {};
      const mb = (p.sizeBytes / 1048576).toFixed(2);
      const sec = (p.totalMs / 1000).toFixed(2);
      const mbps = (p.sizeBytes / 1048576) / (p.totalMs / 1000);
      log(`\n  ${r.url}`);
      log(`    ${sec}s for ${mb}MB (${mbps.toFixed(2)}MB/s)  parse=${ph.parse?.toFixed(0)}ms prePass=${ph.prePass?.toFixed(0)}ms mainPass=${ph.mainPass?.toFixed(0)}ms structuralExport=${ph.structuralExport?.toFixed(0)}ms`);
      const f = p.findings || {};
      log(`    findings: fetchSites=${f.fetchSites} sinks=${f.sinks} dangerous=${f.dangerous} gaps=${f.gaps} enums=${f.enums} fieldMaps=${f.fieldMaps}`);
      const c = p.counters || {};
      log(`    counters: interProc=${c.interProcTraces} resolvedUrls=${c.resolvedUrls} globals=${c.globalAssignments} winAliases=${c.windowAliases}`);
    }
  });
}

// audit.profile — capture a real V8 CPU profile of the offscreen
// document during one analysis run. Uses CDP Profiler.start/stop so
// the output is a standard Chromium .cpuprofile file loadable in
// Chrome DevTools (Performance tab → "Load profile…"). Per-line and
// per-function attribution comes from V8's sampling profiler — that's
// the right tool for hot-path discovery, not custom in-bundle stats.
//
// Flow: pick the active tab's buffered scripts → start Profiler →
// kick the combined-bundle analysis → wait for cache to populate →
// stop Profiler → write profile to testing/finding-snapshots/<name>.cpuprofile.
async function cmdAuditProfile(rest) {
  const { positional, flags } = parseArgs(rest);
  const name = positional[0] || ("profile-" + Date.now());
  if (!/^[A-Za-z0-9_.-]+$/.test(name)) throw new Error("profile name must be [A-Za-z0-9_.-]+");
  // Reviewer-facing knobs: V8 sampling interval (microseconds), wait
  // deadline (ms), poll interval (ms), top-N leaves to print. Defaults
  // chosen so the command is useful out of the box but every tunable
  // is overridable via flag.
  const samplingUs = Number(flags.interval) || 100;          // V8 sampling interval, µs
  const deadlineMs = Number(flags.timeout) || 600000;        // overall wait budget
  const pollMs = Number(flags.poll) || 2000;                 // SW state poll interval
  const topN = Number(flags.top) || 20;                      // leaves to print
  const outPath = path.resolve(SNAP_DIR, name + ".cpuprofile");
  await fsp.mkdir(SNAP_DIR, { recursive: true });
  await withBrowser(async (browser) => {
    // Offscreen documents show up as type "page" in CDP /json/list but
    // puppeteer's browser.targets() filters them out. Pull the raw list
    // via CDP HTTP endpoint, find the ast-worker.html target, then ask
    // puppeteer to attach to its targetId via _targetManager.
    const lock = await readLock();
    const port = lock?.port || DEFAULT_PORT;
    const res = await fetch(`http://127.0.0.1:${port}/json/list`);
    const cdpTargets = await res.json();
    const offscreenInfo = cdpTargets.find(t => t.url && t.url.endsWith("/ast-worker.html"));
    if (!offscreenInfo) throw new Error("offscreen ast-worker.html target not found — run an analysis first to spawn it");

    // Puppeteer doesn't enumerate offscreen documents in browser.targets().
    // Connect raw CDP via WebSocket to the offscreen page's webSocketDebuggerUrl
    // (returned by /json/list) and drive Profiler directly.
    const WebSocket = require("ws");
    const wsUrl = offscreenInfo.webSocketDebuggerUrl;
    if (!wsUrl) throw new Error("offscreen target has no webSocketDebuggerUrl");
    const ws = new WebSocket(wsUrl, { perMessageDeflate: false });
    await new Promise((res, rej) => {
      ws.once("open", res);
      ws.once("error", rej);
    });
    let cdpId = 0;
    const pending = new Map();
    ws.on("message", (data) => {
      const msg = JSON.parse(String(data));
      if (msg.id != null && pending.has(msg.id)) {
        const { resolve, reject } = pending.get(msg.id);
        pending.delete(msg.id);
        if (msg.error) reject(new Error(msg.error.message || JSON.stringify(msg.error)));
        else resolve(msg.result);
      }
    });
    function cdpSend(method, params) {
      const id = ++cdpId;
      ws.send(JSON.stringify({ id, method, params: params || {} }));
      return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
    }
    let started = false;
    try {
      await cdpSend("Profiler.enable");
      await cdpSend("Profiler.setSamplingInterval", { interval: samplingUs });
      await cdpSend("Profiler.start");
      started = true;

      // Trigger the actual analysis — kick combined analysis on the active tab's buffered scripts
      const triggered = await evalSW(browser, `
        const tabId = state.tabs.keys().next().value;
        if (!tabId) return { ok: false, error: 'no tab' };
        const buf = _scriptBuffers.get(tabId);
        if (!buf || !buf.scripts || !buf.scripts.length) return { ok: false, error: 'no buffered scripts for tab ' + tabId };
        const totalChars = buf.scripts.reduce((s, x) => s + x.code.length, 0);
        const t0 = Date.now();
        // Fire-and-forget so the SW eval doesn't block the puppeteer protocol
        _analyzeCombinedScripts(tabId).then(() => { globalThis._profDone = { ok: true, elapsed: Date.now() - t0 }; })
          .catch(e => { globalThis._profDone = { error: e.message, elapsed: Date.now() - t0 }; });
        return { ok: true, tabId, scripts: buf.scripts.length, totalChars };
      `);
      if (!triggered.ok) throw new Error("could not trigger analysis: " + triggered.error);
      log(`profiling ${triggered.scripts} scripts (${triggered.totalChars} chars) for tab ${triggered.tabId}…`);

      // Poll for completion (cache populated, OR _profDone set with error)
      const deadline = Date.now() + deadlineMs;
      while (Date.now() < deadline) {
        await sleep(pollMs);
        const check = await evalSW(browser, `
          return JSON.stringify({
            done: !!globalThis._profDone,
            cacheSize: globalStore.scriptCache.size,
            result: globalThis._profDone || null
          });
        `);
        const parsed = JSON.parse(check);
        if (parsed.done) {
          log(`analysis returned: ${JSON.stringify(parsed.result)}`);
          break;
        }
      }

      const profileResult = await cdpSend("Profiler.stop");
      started = false;
      const profile = profileResult.profile;
      await fsp.writeFile(outPath, JSON.stringify(profile));
      log(`wrote CPU profile to ${outPath}`);
      log(`load in Chrome DevTools: chrome://inspect/#service-workers → Performance → Load profile`);

      // Quick top-functions summary so the reviewer sees hot leaves without
      // having to open DevTools. CDP profile.nodes is a tree; .hitCount is
      // the sample count attributed directly to that frame (self time, in
      // sampling-interval units). Sort by hitCount desc, take the top.
      const nodes = profile.nodes || [];
      const totalSamples = nodes.reduce((s, n) => s + (n.hitCount || 0), 0) || 1;
      const intervalMs = samplingUs / 1000;
      const top = nodes
        .filter(n => (n.hitCount || 0) > 0)
        .sort((a, b) => (b.hitCount || 0) - (a.hitCount || 0))
        .slice(0, topN);
      log(`\ntop self-time leaves (${totalSamples} samples ≈ ${(totalSamples * intervalMs).toFixed(0)}ms total):`);
      for (const n of top) {
        const fn = n.callFrame || {};
        const pct = ((n.hitCount / totalSamples) * 100).toFixed(1);
        const ms = (n.hitCount * intervalMs).toFixed(0);
        const fname = fn.functionName || "(anonymous)";
        const url = fn.url ? fn.url.split("/").pop() : "?";
        const loc = fn.lineNumber != null ? ":" + (fn.lineNumber + 1) + ":" + (fn.columnNumber + 1) : "";
        log(`  ${pct.padStart(5)}%  ${ms.padStart(6)}ms  ${fname}  ${url}${loc}`);
      }
    } finally {
      try { if (started) await cdpSend("Profiler.stop"); } catch {}
      try { ws.close(); } catch {}
    }
  });
}

// audit.profile.summary — parse a saved .cpuprofile and surface the top
// hot frames grouped by ast.js function. Pillar 12: reviewer-facing
// tooling that converts a raw cpuprofile into actionable per-function
// inclusive/self time without needing Chrome DevTools.
async function cmdAuditProfileSummary(rest) {
  const { positional, flags } = parseArgs(rest);
  const fileArg = positional[0];
  if (!fileArg) throw new Error("usage: audit.profile.summary <name|path> [--top N] [--filter ast|all]");
  const topN = Number(flags.top) || 20;
  const filter = flags.filter || "ast";  // "ast" = ast.js only; "all" = all frames
  const profilePath = fileArg.endsWith(".cpuprofile") ? path.resolve(fileArg) :
    path.resolve(SNAP_DIR, fileArg + ".cpuprofile");
  const profile = JSON.parse(await fsp.readFile(profilePath, "utf8"));
  const nodes = profile.nodes || [];
  const totalSamples = nodes.reduce((s, n) => s + (n.hitCount || 0), 0) || 1;
  const idToNode = new Map();
  for (const n of nodes) idToNode.set(n.id, n);
  function inclusive(id, seen) {
    if (seen.has(id)) return 0;
    seen.add(id);
    const n = idToNode.get(id);
    if (!n) return 0;
    let h = n.hitCount || 0;
    if (n.children) for (const c of n.children) h += inclusive(c, seen);
    return h;
  }
  // Group by function-name + line so multiple call-stack instances of the
  // same function aggregate. Inclusive sample count is summed across
  // all instances — each instance is a unique stack position, but they
  // all execute the same function code, so the cumulative time is the
  // function's total cost.
  const byName = new Map();
  for (const n of nodes) {
    if (!n.callFrame) continue;
    const url = n.callFrame.url || "?";
    if (filter === "ast" && !url.endsWith("ast.js")) continue;
    const key = (n.callFrame.functionName || "(anon)") + ":L" + ((n.callFrame.lineNumber || 0) + 1);
    const incl = inclusive(n.id, new Set());
    const cur = byName.get(key) || { name: key, self: 0, incl: 0, count: 0, urlBase: url.split("/").pop() };
    cur.self += n.hitCount || 0;
    cur.incl += incl;
    cur.count++;
    byName.set(key, cur);
  }
  const sorted = [...byName.values()].sort((a, b) => b.incl - a.incl).slice(0, topN);
  log(`audit.profile.summary: ${profilePath}`);
  log(`  ${totalSamples} samples · filter=${filter} · top ${topN}`);
  log(`  incl%   self%   stacks  function (file:line)`);
  for (const e of sorted) {
    const incl = (e.incl / totalSamples * 100).toFixed(1).padStart(5);
    const self = (e.self / totalSamples * 100).toFixed(1).padStart(5);
    log(`  ${incl}%  ${self}%  ${String(e.count).padStart(6)}  ${e.name} (${e.urlBase})`);
  }
}

async function cmdFindingSnapshot(args) {
  const name = args[0];
  if (!name) throw new Error("usage: finding.snapshot <name>");
  if (!/^[A-Za-z0-9_.-]+$/.test(name)) throw new Error("snapshot name must be [A-Za-z0-9_.-]+");
  await fsp.mkdir(SNAP_DIR, { recursive: true });
  await withBrowser(async (browser) => {
    const byId = await collectFindings(browser);
    const payload = {
      name,
      createdAt: new Date().toISOString(),
      count: Object.keys(byId).length,
      findings: byId,
    };
    const out = path.join(SNAP_DIR, `${name}.json`);
    await fsp.writeFile(out, JSON.stringify(payload, null, 2), "utf8");
    log(`wrote ${payload.count} findings → ${out}`);
  });
}

// Diff current findings against a named snapshot. Groups by stable ID so
// "added" means genuinely new (not just a reordered array index), and
// "removed" means the specific (sourceUrl,line,col,kind) tuple stopped
// firing. Severity / sanitized / description flips for same ID surface
// under "changed" — that's the signal a classifier refinement landed.
async function cmdFindingDiff(args) {
  const name = args[0];
  if (!name) throw new Error("usage: finding.diff <name>");
  const file = path.join(SNAP_DIR, `${name}.json`);
  let prior;
  try { prior = JSON.parse(await fsp.readFile(file, "utf8")); }
  catch (e) { throw new Error(`can't read snapshot ${file}: ${e.message}`); }
  await withBrowser(async (browser) => {
    const current = await collectFindings(browser);
    const priorIds = new Set(Object.keys(prior.findings || {}));
    const currIds = new Set(Object.keys(current));
    const added = [...currIds].filter(id => !priorIds.has(id)).sort();
    const removed = [...priorIds].filter(id => !currIds.has(id)).sort();
    const shared = [...currIds].filter(id => priorIds.has(id));
    const changed = [];
    for (const id of shared) {
      const a = prior.findings[id], b = current[id];
      const diffs = [];
      for (const k of ["severity", "sanitized", "type", "sink", "sourceType", "description", "line", "col"]) {
        if (a[k] !== b[k]) diffs.push(`${k}: ${JSON.stringify(a[k])} → ${JSON.stringify(b[k])}`);
      }
      // Taint-path delta: compare hop kinds as a sequence. A classifier
      // change that keeps the same verdict but takes a different route is
      // still a behavior change worth flagging — e.g. fewer hops usually
      // means the tracer bailed out early on some branch.
      const aHops = (a.taintPath || []).map(h => h.kind).join("→") || "(none)";
      const bHops = (b.taintPath || []).map(h => h.kind).join("→") || "(none)";
      if (aHops !== bHops) diffs.push(`taintPath: ${aHops} → ${bHops}`);
      if (diffs.length) changed.push({ id, b, diffs });
    }
    log(`snapshot: ${name}  (${prior.count} findings @ ${prior.createdAt})`);
    log(`current:  ${Object.keys(current).length} findings @ ${new Date().toISOString()}`);
    log(`+${added.length} added  -${removed.length} removed  ~${changed.length} changed  =${shared.length - changed.length} unchanged`);
    if (added.length) {
      log(`\nADDED (new findings — classifier got broader, or new code loaded):`);
      for (const id of added) {
        const f = current[id];
        log(`  ${id}  [${f.severity}] ${f.category}/${f.type || "?"}${f.sink ? ":" + f.sink : ""}  L${f.line ?? "?"}`);
        log(`           ${labelScript(f.sourceUrl)}`);
        if (f.description) log(`           ${short(f.description, 140)}`);
      }
    }
    if (removed.length) {
      log(`\nREMOVED (no longer firing — classifier got narrower, or script gone):`);
      for (const id of removed) {
        const f = prior.findings[id];
        log(`  ${id}  [${f.severity}] ${f.category}/${f.type || "?"}${f.sink ? ":" + f.sink : ""}  L${f.line ?? "?"}`);
        log(`           ${labelScript(f.sourceUrl)}`);
        if (f.description) log(`           ${short(f.description, 140)}`);
      }
    }
    if (changed.length) {
      log(`\nCHANGED (same location, classifier output differs):`);
      for (const c of changed) {
        log(`  ${c.id}  [${c.b.severity}] ${c.b.category}/${c.b.type || "?"}${c.b.sink ? ":" + c.b.sink : ""}  L${c.b.line ?? "?"}`);
        log(`           ${labelScript(c.b.sourceUrl)}`);
        for (const d of c.diffs) log(`           ${d}`);
      }
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
  // Filter by the extension's content-based asset classification (api /
  // asset / empty / unknown). Used to audit the classifier's decisions —
  // `logs --kind asset` lists every request the extension treated as a
  // static resource so the reviewer can spot false-positives (e.g. a
  // JSON API incorrectly bucketed because of a binary-looking magic
  // byte). `logs --boring` narrows further to boring-asset requests
  // (asset + GET + no query + no body + no auth) — the tier that gets
  // zero schema synthesis.
  const kind = flags.kind || null;
  const boringOnly = flags.boring === true;
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
            assetKind: r._assetKind || null,
            assetLabel: r._assetLabel || null,
            boring: !!r._boring,
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
      if (kind && r.assetKind !== kind) continue;
      if (boringOnly && !r.boring) continue;
      if (limit && shown >= limit) break;
      shown++;
      // Classification badge: "asset:image/png" means body magic bytes
      // identified it as a PNG; "api" means structured; "boring" is
      // only set when additionally GET + no query + no body + no auth.
      const classTag = r.assetKind
        ? (r.boring ? "boring " : "") + r.assetKind + (r.assetLabel ? ":" + r.assetLabel : "")
        : "?";
      log(`${r.id}  [${r.status}] ${r.method.padEnd(6)} ${short(r.url, 100)}`);
      log(`      svc=${r.service} method=${r.methodId || "-"}  ct=${r.ct}  body=${r.bodyLen}B  resp=${r.respLen}B  class=${classTag}`);
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
        // Tag each method with its bucket name (learned / probed /
        // static / ...) so the reviewer can see when the SAME method
        // id exists in multiple buckets. Without the bucket prefix,
        // duplicate-looking rows like "svc_shreddit_events" are
        // actually distinct records from different discovery paths.
        const methods = [];
        for (const [bName, bObj] of Object.entries(e.doc.resources || {})) {
          for (const [mName, m] of Object.entries(bObj.methods || {})) {
            methods.push({ ...m, _bucket: bName, _localName: mName });
          }
        }
        // Backfill grouping for entries created before .grouping was
        // stored. The classifier is deterministic — running it on the
        // stored method URL reproduces exactly the rule that fired at
        // creation time. We mark the result as backfilled so readers
        // know firstUrl is a SAMPLE URL, not the original first.
        // Joins origin+path tolerantly — method.path may or may not
        // have a leading slash; naive concat produces invalid URLs.
        let grouping = e.grouping || null;
        if (!grouping && methods.length && methods[0].origin) {
          try {
            let origin = String(methods[0].origin);
            while (origin.endsWith("/")) origin = origin.slice(0, -1);
            const pathPart = String(methods[0].path || "/");
            const sampleUrl = new URL(origin + (pathPart.startsWith("/") ? "" : "/") + pathPart);
            const c = classifyInterface(sampleUrl);
            grouping = { rule: c.rule, matched: c.matched, firstUrl: sampleUrl.href, backfilled: true };
          } catch (_) {}
        }
        out.push({
          svc, isVirtual: !!e.isVirtual, title: e.doc.title,
          methodCount: methods.length,
          sampleMethods: methods.slice(0, 3).map(m => (m._bucket || "?") + "/" + (m._localName || m.id)),
          grouping,
        });
      }
      return out;
    `);
    for (const r of rows) {
      if (filter && !r.svc.includes(filter)) continue;
      // Each service row shows the grouping rule so a reviewer can
      // see at a glance why requests ended up in this bucket —
      // `path-keyword: api` vs `grpc-over-http` vs `hostname-fallback`
      // are very different confidence levels.
      const ruleTag = r.grouping
        ? ` [${r.grouping.rule}${r.grouping.matched ? ": " + r.grouping.matched : ""}${r.grouping.backfilled ? " (backfill)" : ""}]`
        : "";
      log(`${r.svc}   methods=${r.methodCount}${r.isVirtual ? " (virtual)" : ""}${ruleTag}`);
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
            _responseKind: m._responseKind || null,    // "asset" when magic bytes confirmed static
            _responseLabel: m._responseLabel || null,  // e.g. "image/png"
            _astInferred: !!m._astInferred,            // registered only from AST (no live traffic)
            _astCallSites: Array.isArray(m._astCallSites) ? m._astCallSites.slice() : [],
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
      // Backfill grouping for entries created before .grouping was
      // stored; classifier is deterministic so rerunning on a sample
      // method URL reproduces the original rule exactly. backfilled:true
      // flags that firstUrl is a sample, not the real first.
      let grouping = e.grouping || null;
      if (!grouping && methods.length && methods[0].origin) {
        try {
          let origin = String(methods[0].origin);
          while (origin.endsWith("/")) origin = origin.slice(0, -1);
          const pathPart = String(methods[0].path || "/");
          const sampleUrl = new URL(origin + (pathPart.startsWith("/") ? "" : "/") + pathPart);
          const c = classifyInterface(sampleUrl);
          grouping = { rule: c.rule, matched: c.matched, firstUrl: sampleUrl.href, backfilled: true };
        } catch (_) {}
      }
      // Walk every tab's requestLog and pull entries for THIS service.
      // Gives the reviewer a factual basis for judging grouping quality:
      // 50 GET requests with no body, all image/png, means hostname-
      // fallback is sweeping assets into one bucket. A handful of POST
      // requests with JSON bodies looks like a real API boundary.
      const urlDist = {};                    // url → count
      const assetBreakdown = { asset: 0, api: 0, empty: 0, unknown: 0 };
      const methodBreakdown = {};
      const ctBreakdown = {};
      let totalObserved = 0;
      for (const [, tab] of state.tabs) {
        for (const r of (tab.requestLog || [])) {
          if (r.service !== ${JSON.stringify(name)}) continue;
          totalObserved++;
          urlDist[r.url] = (urlDist[r.url] || 0) + 1;
          const kind = r._assetKind || "unknown";
          assetBreakdown[kind] = (assetBreakdown[kind] || 0) + 1;
          methodBreakdown[r.method] = (methodBreakdown[r.method] || 0) + 1;
          const ct = (r.contentType || "").split(";")[0].trim() || "(none)";
          ctBreakdown[ct] = (ctBreakdown[ct] || 0) + 1;
        }
      }
      // Top distinct URLs (sorted by frequency, then alphabetically) —
      // a scannable snapshot of what's actually in this bucket.
      const topUrls = Object.entries(urlDist)
        .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
        .slice(0, 12)
        .map(([url, count]) => ({ url, count }));
      return {
        svc: ${JSON.stringify(name)},
        title: doc.title,
        isVirtual: !!e.isVirtual,
        method: e.method,
        url: e.url,
        rootUrl: doc.rootUrl,
        grouping,
        methods,
        schemas,
        keys,
        quality: {
          totalObserved,
          distinctUrls: Object.keys(urlDist).length,
          assetBreakdown,
          methodBreakdown,
          ctBreakdown,
          topUrls,
        },
      };
    `);
    if (!data) { err("no such service"); return; }
    log(`${data.svc}${data.isVirtual ? "  (virtual)" : ""}`);
    log(`  title: ${data.title || "-"}`);
    log(`  root: ${data.rootUrl || "-"}`);
    log(`  discovered via: ${data.method || "-"}${data.url ? " " + short(data.url, 80) : ""}`);
    // Show the reviewer exactly which URL-structure rule produced
    // this service name, and the first URL that triggered it. Lets
    // them judge whether the grouping is over-broad (e.g. hostname-
    // fallback on a site with many distinct APIs) or correctly tight.
    if (data.grouping) {
      const tag = data.grouping.backfilled ? " (backfilled from sample URL — service was captured before grouping was tracked)" : "";
      log(`  grouping: rule="${data.grouping.rule}"  matched="${data.grouping.matched || ""}"${tag}`);
      if (data.grouping.firstUrl) log(`    ${data.grouping.backfilled ? "sample URL" : "first request"}: ${data.grouping.firstUrl}`);
    }
    log(`  api keys known for this service: ${data.keys.length}`);
    for (const k of data.keys) log(`    ${short(k.key, 40)} (source=${k.source || "-"})`);

    // Grouping-quality readout: raw counts of what actually landed in
    // this service bucket. Mixed asset/api splits or a hostname-
    // fallback grouping with many distinct URLs is a signal that the
    // grouping may be over-broad and worth splitting manually.
    const q = data.quality || {};
    if (q.totalObserved) {
      log(`\n  bucket quality  (${q.totalObserved} requests across ${q.distinctUrls} distinct URLs):`);
      const ab = q.assetBreakdown || {};
      const mb = q.methodBreakdown || {};
      const cb = q.ctBreakdown || {};
      const assetBits = Object.entries(ab).filter(([, v]) => v > 0).map(([k, v]) => `${k}=${v}`).join(", ");
      const methodBits = Object.entries(mb).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join(", ");
      log(`    classification: ${assetBits || "(none)"}`);
      log(`    methods:        ${methodBits || "(none)"}`);
      log(`    content-types:  ${Object.entries(cb).sort((a, b) => b[1] - a[1]).slice(0, 6).map(([k, v]) => `${k}=${v}`).join(", ")}`);
      if (q.topUrls && q.topUrls.length) {
        log(`    top URLs:`);
        for (const u of q.topUrls) log(`      ${String(u.count).padStart(4)}× ${short(u.url, 110)}`);
      }
      // Flag likely mis-groupings: hostname-fallback + lots of asset
      // traffic = the bucket is catching CDN static resources under
      // the same name as the real API. Surface it without scoring it.
      if (data.grouping && data.grouping.rule === "hostname-fallback" && ab.asset > 0 && ab.api > 0) {
        log(`    note: this hostname-fallback bucket mixes ${ab.asset} asset(s) with ${ab.api} API call(s) — consider whether some URLs should live in a tighter service.`);
      }
    } else {
      log(`\n  bucket quality: no captured requests in this session yet`);
    }

    log(`\n  methods (${data.methods.length}):`);
    for (const m of data.methods) {
      // Discovery-source tag. Reviewer's primary focus is AST-derived
      // methods; live-only is legit too but less interesting for code
      // review; asset is noise.
      //   [ast]       discovered from source code, not yet exercised
      //   [ast+live]  found by both AST and real traffic
      //   [live]      real traffic only (e.g. traffic from non-analysed code)
      //   [asset:X]   response magic-byte-confirmed static
      let kindTag = "";
      if (m._responseKind === "asset") {
        kindTag = `  [response=asset${m._responseLabel ? ":" + m._responseLabel : ""}]`;
      } else {
        const hasAst = !!m._astInferred;
        const hasLive = !!m.requestCount;
        if (hasAst && hasLive) kindTag = "  [ast+live]";
        else if (hasAst) kindTag = "  [ast]";
        else if (hasLive) kindTag = "  [live]";
      }
      log(`    ${m.httpMethod} ${m.origin || ""}/${m.path || ""}  [${m.bucket}/${m.name}]${kindTag}`);
      log(`      id=${m.id}  reqCount=${m.requestCount}  req=${m.requestRef || "-"}  resp=${m.responseRef || "-"}  ct=${(m.contentTypes||[]).join(",") || "-"}`);
      // AST call sites: where in the bundle each method was discovered.
      // Reviewer uses this to jump directly to the source code — the
      // method card's reason-for-existence, not just its shape.
      if (Array.isArray(m._astCallSites) && m._astCallSites.length) {
        for (const cs of m._astCallSites.slice(0, 3)) {
          const loc = cs.line != null ? `L${cs.line}:C${cs.column ?? "?"}` : "?";
          const enc = cs.enclosingFunction ? `  ${cs.enclosingFunction}()` : "";
          log(`      ast-site: ${cs.script || "?"}  @${loc}${enc}`);
        }
        if (m._astCallSites.length > 3) log(`      ast-site: … +${m._astCallSites.length - 3} more`);
      }
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

// Recursively materialize a schema's example values into a JSON-shaped
// object. `doc` is needed to resolve $refs; visited guards recursive
// types. Leaf fields fall back to pickExampleValue's type-default if no
// stored _exampleValue exists yet (e.g. the field was never observed).
function materializeSchemaExample(schema, doc, visited) {
  if (!schema) return null;
  if (schema.$ref) {
    if (visited.has(schema.$ref)) return null;
    visited.add(schema.$ref);
    const resolved = doc.schemas && doc.schemas[schema.$ref];
    const res = materializeSchemaExample(resolved, doc, visited);
    visited.delete(schema.$ref);
    return res;
  }
  if (schema.type === "array" || schema.label === "repeated") {
    if (schema.items && schema.items.$ref) {
      const inner = materializeSchemaExample(schema.items, doc, visited);
      return inner == null ? [] : [inner];
    }
    if (schema._exampleValue !== undefined) return [schema._exampleValue];
    return [];
  }
  if (schema.properties && Object.keys(schema.properties).length) {
    const out = {};
    for (const [k, def] of Object.entries(schema.properties)) {
      const val = materializeSchemaExample(def, doc, visited);
      // Omit null leaves (no signal, no observations) so the request
      // stays minimal. Caller can manually force-include when testing
      // required/optional boundaries.
      if (val !== null && val !== undefined) out[k] = val;
    }
    return out;
  }
  if (schema._exampleValue !== undefined) return schema._exampleValue;
  return null;
}

// Compare a server response against a learned response schema. Returns
// counts + lists of shape mismatches. Conservative — only notes:
//   - schema-declared fields missing in response
//   - response fields not in schema (potential drift)
//   - scalar type mismatches at declared fields
// Does NOT fail on missing fields (could be optional) or on drift alone;
// those are surfaced as informational diffs.
function diffResponseAgainstSchema(response, schema, doc, path, out, visited) {
  path = path || "";
  out = out || { missing: [], extra: [], typeMismatches: [] };
  visited = visited || new Set();
  if (!schema) return out;
  if (schema.$ref) {
    if (visited.has(schema.$ref)) return out;
    visited.add(schema.$ref);
    diffResponseAgainstSchema(response, doc.schemas[schema.$ref], doc, path, out, visited);
    visited.delete(schema.$ref);
    return out;
  }
  if (schema.type === "array") {
    if (!Array.isArray(response)) {
      if (response != null) out.typeMismatches.push({ path, expected: "array", got: typeof response });
      return out;
    }
    if (schema.items && response.length) {
      diffResponseAgainstSchema(response[0], schema.items, doc, path + "[0]", out, visited);
    }
    return out;
  }
  if (schema.properties) {
    if (response == null || typeof response !== "object" || Array.isArray(response)) {
      out.typeMismatches.push({ path, expected: "object", got: response === null ? "null" : Array.isArray(response) ? "array" : typeof response });
      return out;
    }
    for (const [k, def] of Object.entries(schema.properties)) {
      const sub = path ? path + "." + k : k;
      if (!(k in response)) { out.missing.push(sub); continue; }
      diffResponseAgainstSchema(response[k], def, doc, sub, out, visited);
    }
    // Drift: response keys not in schema
    for (const rk of Object.keys(response)) {
      if (!schema.properties[rk]) out.extra.push(path ? path + "." + rk : rk);
    }
    return out;
  }
  // Scalar leaf: compare declared type vs typeof
  const declared = schema.type;
  if (declared && response != null) {
    const got = typeof response;
    const numeric = /^(int|uint|sint|float|double|fixed|sfixed|number|integer)/.test(declared);
    const isBool = declared === "bool" || declared === "boolean";
    if (declared === "string" && got !== "string") out.typeMismatches.push({ path, expected: declared, got });
    else if (numeric && got !== "number") out.typeMismatches.push({ path, expected: declared, got });
    else if (isBool && got !== "boolean") out.typeMismatches.push({ path, expected: declared, got });
  }
  return out;
}

// Walk a schema and count example-value provenance per leaf. Also
// surfaces "gaps": required leaves that have NO example value (the
// verifier will send null/default for these — often the reason for
// 4xx/5xx responses during verify).
function _summariseProvenance(schema, doc, out, visited, path, depth) {
  out = out || {
    byProvenance: Object.create(null),
    leaves: [],
    gaps: [],
  };
  visited = visited || new Set();
  path = path || "";
  depth = depth || 0;
  if (!schema || depth > 32) return out;
  if (schema.$ref) {
    if (visited.has(schema.$ref)) return out;
    visited.add(schema.$ref);
    _summariseProvenance(doc.schemas && doc.schemas[schema.$ref], doc, out, visited, path, depth + 1);
    visited.delete(schema.$ref);
    return out;
  }
  if (schema.type === "array" && schema.items) {
    _summariseProvenance(schema.items, doc, out, visited, path + "[]", depth + 1);
    return out;
  }
  if (schema.properties && Object.keys(schema.properties).length) {
    for (const [k, def] of Object.entries(schema.properties)) {
      _summariseProvenance(def, doc, out, visited, path ? path + "." + k : k, depth + 1);
    }
    return out;
  }
  // Leaf
  const prov = schema._exampleValueSource || "none";
  out.byProvenance[prov] = (out.byProvenance[prov] || 0) + 1;
  out.leaves.push({
    path: path || "(root)",
    type: schema.type || schema.$ref || "?",
    value: schema._exampleValue,
    source: prov,
    confidence: schema._exampleConfidence || null,
  });
  if (schema.required && !schema._exampleValueSource) {
    out.gaps.push({ path: path || "(root)", type: schema.type || "?" });
  }
  return out;
}

// Regex patterns for sinks we expect the AST to catch. Cross-checking
// grep-found match positions against AST-found finding positions is the
// single best way to surface BOTH parser-miss (AST didn't reach the
// code) AND classifier-miss (AST reached it but didn't flag). Every new
// classifier should have a corresponding entry here so audits stay
// honest as coverage grows.
//
// Patterns intentionally permissive — we WANT false positives at this
// layer so they show up as "unexplained" and the reviewer can decide
// whether each was a real miss, a legitimate suppression, or a pattern
// the grep shouldn't have matched.
const SINK_GREP_PATTERNS = [
  { name: "innerHTML=",         re: /\.innerHTML\s*=\s*[^=]/g,      astTags: ["xss:innerHTML"] },
  { name: "outerHTML=",         re: /\.outerHTML\s*=\s*[^=]/g,      astTags: ["xss:outerHTML"] },
  { name: "insertAdjacentHTML", re: /\.insertAdjacentHTML\s*\(/g,   astTags: ["xss:insertAdjacentHTML", "xss:.insertAdjacentHTML"] },
  { name: "document.write",     re: /\bdocument\.write(?:ln)?\s*\(/g, astTags: ["xss:document.write", "xss:document.writeln"] },
  { name: "parseHTMLUnsafe",    re: /\bparseHTMLUnsafe\s*\(/g,      astTags: ["xss:parseHTMLUnsafe"] },
  { name: "setHTMLUnsafe",      re: /\.setHTMLUnsafe\s*\(/g,        astTags: ["xss:setHTMLUnsafe"] },
  { name: "eval(",              re: /(?<![.\w])eval\s*\(/g,         astTags: ["eval:eval"] },
  { name: "new Function(",      re: /\bnew\s+Function\s*\(/g,       astTags: ["eval:new Function"] },
  { name: "setTimeout-string",  re: /\bsetTimeout\s*\(\s*['"`]/g,   astTags: ["eval:setTimeout"] },
  { name: "setInterval-string", re: /\bsetInterval\s*\(\s*['"`]/g,  astTags: ["eval:setInterval"] },
  { name: "location.href=",     re: /\blocation\.href\s*=\s*[^=]/g, astTags: ["redirect:href"] },
  { name: ".href= (element)",   re: /(?<!location)\.href\s*=\s*[^=]/g, astTags: ["redirect:href"] },
  { name: "location.assign(",   re: /\blocation\.assign\s*\(/g,     astTags: ["redirect:location.assign"] },
  { name: "location.replace(",  re: /\blocation\.replace\s*\(/g,    astTags: ["redirect:location.replace"] },
  { name: "postMessage(",       re: /\.postMessage\s*\(/g,          astTags: ["postmessage-wildcard-target", "postmessage-dynamic-target"] },
  { name: "fetch(",             re: /(?<![.\w])fetch\s*\(/g,        astTags: ["request-forgery:fetch"] },
  { name: "sendBeacon(",        re: /\.sendBeacon\s*\(/g,           astTags: ["request-forgery:navigator.sendBeacon"] },
  { name: "xhr.open(",          re: /\.open\s*\(\s*['"`]/g,         astTags: ["request-forgery:XMLHttpRequest.open"] },
  { name: "importScripts(",     re: /(?<![.\w])importScripts\s*\(/g, astTags: ["eval:importScripts"] },
];

// Grep patterns whose first arg is a URL — a resolverError on the same
// line means the URL identifier didn't trace, not that the classifier
// missed the sink. audit.sinks counts these as resolver-gap rather than
// unexplained so the reviewer can triage resolver work separately.
const NETWORK_EGRESS_GREP_NAMES = new Set(["fetch(", "sendBeacon(", "xhr.open(", "importScripts("]);

// Precompute newline offsets once per source so each match's line/col
// lookup is O(log n). Caller owns the `starts` array and threads it
// through — no global cache (strings aren't valid WeakMap keys).
function _buildLineStarts(src) {
  const starts = [0];
  for (let i = 0; i < src.length; i++) if (src.charCodeAt(i) === 10) starts.push(i + 1);
  return starts;
}
function _offsetToLineCol(starts, offset) {
  let lo = 0, hi = starts.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >>> 1;
    if (starts[mid] <= offset) lo = mid; else hi = mid - 1;
  }
  return { line: lo + 1, column: offset - starts[lo] };
}

// Normalise origin + path into a real URL. origin may or may not end in
// a slash; path may or may not start with one — callers mixed the
// conventions early on, so the builder has to be lenient.
function _joinOriginPath(origin, path) {
  const o = String(origin || "").replace(/\/+$/, "");
  const p = String(path || "/");
  return o + (p.startsWith("/") ? "" : "/") + p;
}

async function cmdSchemaReview(rest) {
  const { positional } = parseArgs(rest);
  const svcName = positional[0];
  const methodIdArg = positional[1];
  if (!svcName || !methodIdArg) throw new Error("usage: schema.review <service> <methodId|methodName>");
  await withBrowser(async (browser) => {
    const plan = await evalSW(browser, `
      const e = globalStore.discoveryDocs.get(${JSON.stringify(svcName)});
      if (!e || !e.doc) return { error: "service not found" };
      const doc = e.doc;
      let match = null, bucket = null;
      for (const [bName, bObj] of Object.entries(doc.resources || {})) {
        for (const [mName, m] of Object.entries(bObj.methods || {})) {
          if (m.id === ${JSON.stringify(methodIdArg)} || mName === ${JSON.stringify(methodIdArg)}) {
            match = m; bucket = bName; break;
          }
        }
        if (match) break;
      }
      if (!match) return { error: "method not found" };
      return {
        bucket,
        method: {
          id: match.id, httpMethod: match.httpMethod, origin: match.origin, path: match.path,
          description: match.description || null, contentTypes: match.contentTypes || [],
          requestRef: match.request?.$ref || null, responseRef: match.response?.$ref || null,
          parameters: match.parameters || {},
        },
        schemas: doc.schemas,
        stats: match._stats,
      };
    `);
    if (plan.error) { err(plan.error); return; }
    const m = plan.method;
    log(`schema.review: ${m.httpMethod} ${m.id}`);
    log(`  endpoint: ${_joinOriginPath(m.origin, m.path)}`);
    if (m.description) log(`  description: ${short(m.description, 180)}`);
    log(`  request schema:  ${m.requestRef || "(none)"}`);
    log(`  response schema: ${m.responseRef || "(none)"}`);

    // Params provenance summary
    const paramSummary = Object.create(null);
    const paramLeaves = [];
    const paramGaps = [];
    for (const [pName, pDef] of Object.entries(m.parameters)) {
      const prov = pDef._exampleValueSource || "none";
      paramSummary[prov] = (paramSummary[prov] || 0) + 1;
      paramLeaves.push({ name: pName, loc: pDef.location, type: pDef.type, value: pDef._exampleValue, source: prov, confidence: pDef._exampleConfidence });
      if (pDef.required && !pDef._exampleValueSource) paramGaps.push({ name: pName, type: pDef.type });
    }

    // Body provenance summary
    const bodySum = m.requestRef
      ? _summariseProvenance(plan.schemas[m.requestRef], { schemas: plan.schemas })
      : { byProvenance: {}, leaves: [], gaps: [] };

    // Combine provenance counts for a one-line readiness snapshot
    const combined = Object.create(null);
    for (const [k, v] of Object.entries(paramSummary)) combined[k] = (combined[k] || 0) + v;
    for (const [k, v] of Object.entries(bodySum.byProvenance)) combined[k] = (combined[k] || 0) + v;
    const total = Object.values(combined).reduce((a, b) => a + b, 0);
    const real = (combined["observed-default"] || 0) + (combined["observed-top"] || 0) + (combined["ast-constraint"] || 0) + (combined["enum"] || 0);
    const synth = (combined["format-synth"] || 0) + (combined["range-min"] || 0);
    const typeDef = combined["type-default"] || 0;
    const none = combined["none"] || 0;
    log(`\n  readiness: ${total} leaves total — ${real} real observations, ${synth} synthesized, ${typeDef} type-default, ${none} unsourced`);
    log(`  by provenance:`);
    for (const [k, v] of Object.entries(combined).sort((a, b) => b[1] - a[1])) log(`    ${v.toString().padStart(4)}  ${k}`);

    // Parameters tree
    if (paramLeaves.length) {
      log(`\n  parameters (${paramLeaves.length}):`);
      for (const p of paramLeaves) {
        const conf = p.confidence != null ? `  (dom ${Math.round(p.confidence * 100)}%)` : "";
        log(`    · ${p.name} @${p.loc} (${p.type})  = ${JSON.stringify(p.value)}  [${p.source}]${conf}`);
      }
    }

    // Body tree (schema leaves with example values + provenance)
    if (bodySum.leaves.length) {
      log(`\n  body leaves (${bodySum.leaves.length}):`);
      const shown = bodySum.leaves.slice(0, 120);
      for (const leaf of shown) {
        const conf = leaf.confidence != null ? `  (dom ${Math.round(leaf.confidence * 100)}%)` : "";
        log(`    · ${leaf.path} (${leaf.type})  = ${JSON.stringify(leaf.value)}  [${leaf.source}]${conf}`);
      }
      if (bodySum.leaves.length > 120) log(`    … +${bodySum.leaves.length - 120} more`);
    }

    const totalGaps = paramGaps.length + bodySum.gaps.length;
    if (totalGaps) {
      log(`\n  GAPS (required fields with no example — verify will fall back to empty/null):`);
      for (const g of paramGaps) log(`    · param ${g.name} (${g.type})`);
      for (const g of bodySum.gaps) log(`    · body  ${g.path} (${g.type})`);
    }

    // Preview the ready-to-send request (same logic as schema.verify)
    try {
      const url = new URL(_joinOriginPath(m.origin, m.path));
      for (const [pName, pDef] of Object.entries(m.parameters)) {
        if (pDef.location !== "query" || pDef._exampleValue == null) continue;
        url.searchParams.set(pName, String(pDef._exampleValue));
      }
      log(`\n  PREVIEW:`);
      log(`    ${m.httpMethod} ${url.href}`);
      if (m.requestRef) {
        const materialized = materializeSchemaExample(plan.schemas[m.requestRef], { schemas: plan.schemas }, new Set([m.requestRef]));
        log(`    body: ${short(JSON.stringify(materialized), 600)}`);
      }
      log(`\n  To send this for real and diff the response, run:`);
      log(`    node testing/harness.js schema.verify ${JSON.stringify(svcName)} ${m.id.split(".").pop()}`);
    } catch (e) { err(`  preview error: ${e.message}`); }
  });
}

async function cmdSchemaVerify(rest) {
  const { positional, flags } = parseArgs(rest);
  const svcName = positional[0];
  const methodIdArg = positional[1];
  if (!svcName || !methodIdArg) throw new Error("usage: schema.verify <service> <methodId|methodName>");
  await withBrowser(async (browser) => {
    const plan = await evalSW(browser, `
      const e = globalStore.discoveryDocs.get(${JSON.stringify(svcName)});
      if (!e || !e.doc) return { error: "service not found" };
      const doc = e.doc;
      let match = null;
      for (const [bName, bObj] of Object.entries(doc.resources || {})) {
        for (const [mName, m] of Object.entries(bObj.methods || {})) {
          if (m.id === ${JSON.stringify(methodIdArg)} || mName === ${JSON.stringify(methodIdArg)}) { match = m; break; }
        }
        if (match) break;
      }
      if (!match) return { error: "method not found" };
      return {
        method: {
          httpMethod: match.httpMethod,
          origin: match.origin,
          path: match.path,
          contentTypes: match.contentTypes || [],
          parameters: match.parameters,
          requestRef: match.request?.$ref || null,
          responseRef: match.response?.$ref || null,
          id: match.id,
        },
        schemas: doc.schemas,
        stats: match._stats,
      };
    `);
    if (plan.error) { err(plan.error); return; }
    const m = plan.method;
    const contentType = (m.contentTypes || [])[0] || "application/json";
    const isGet = m.httpMethod === "GET" || m.httpMethod === "HEAD";
    const supportsVerify = isGet || /json/.test(contentType);
    if (!supportsVerify) {
      err(`  (schema.verify MVP only handles GET and JSON POST — this method uses ${contentType})`);
      return;
    }

    // Substitute path parameters into the URL template BEFORE constructing
    // the URL. m.path is stored as `posts/{path_param1}` after the path-
    // param detection block templates it; without substitution the URL
    // parser keeps `{path_param1}` literal (URL-encoded as %7B%7D), which
    // sends a request to a path that doesn't exist on the server.
    let pathStr = m.path || "";
    const pathParamReport = [];
    for (const [pName, pDef] of Object.entries(m.parameters || {})) {
      if (pDef.location !== "path") continue;
      const v = pDef._exampleValue;
      const src = pDef._exampleValueSource || "none";
      if (v !== undefined && v !== null && v !== "") {
        const tpl = "{" + pName + "}";
        if (pathStr.includes(tpl)) {
          pathStr = pathStr.split(tpl).join(encodeURIComponent(String(v)));
          pathParamReport.push({ name: pName, value: v, source: src });
        }
      }
    }
    // Build URL + query params from parameter example values.
    let url;
    try { url = new URL(_joinOriginPath(m.origin, pathStr)); }
    catch (e) { err(`  invalid origin+path: ${m.origin} + ${pathStr} (${e.message})`); return; }
    const paramReport = [];
    for (const [pName, pDef] of Object.entries(m.parameters || {})) {
      if (pDef.location !== "query") continue;
      const v = pDef._exampleValue;
      const src = pDef._exampleValueSource || "none";
      if (v !== undefined && v !== null) {
        url.searchParams.set(pName, String(v));
        paramReport.push({ name: pName, value: v, source: src });
      }
    }

    // Build body if JSON POST
    let body = null;
    if (!isGet && m.requestRef) {
      const rootSchema = plan.schemas[m.requestRef];
      const materialized = materializeSchemaExample(rootSchema, { schemas: plan.schemas }, new Set([m.requestRef]));
      if (materialized) body = JSON.stringify(materialized);
    }

    log(`verify: ${m.httpMethod} ${url.href}`);
    log(`  method: ${m.id}`);
    log(`  contentType (request): ${contentType}`);
    log(`  responseRef: ${m.responseRef || "-"}`);
    if (paramReport.length) {
      log(`  query params from exampleValues:`);
      for (const p of paramReport) log(`    ${p.name} = ${JSON.stringify(p.value)}  [${p.source}]`);
    }
    if (body) log(`  body: ${short(body, 400)}`);

    // Send via the live page's fetch so cookies/auth are native. The
    // target page must be open; if not, navigate first.
    const page = await getActivePage(browser);
    if (!page) { err("  (no active page — navigate to the service's origin first)"); return; }
    const wantOrigin = new URL(url.href).origin;
    const pageOrigin = new URL(page.url()).origin;
    if (pageOrigin !== wantOrigin) {
      log(`  navigating: ${pageOrigin} → ${wantOrigin}`);
      await page.goto(wantOrigin, { waitUntil: "domcontentloaded", timeout: 15000 }).catch(e => err(`  (nav warn: ${e.message})`));
    }

    const fetchInit = {
      method: m.httpMethod,
      headers: !isGet ? { "Content-Type": contentType } : {},
      credentials: "include",
    };
    if (body) fetchInit.body = body;
    // Fetch the full body first, parse against it, and only truncate
    // for DISPLAY. Truncating pre-parse cuts mid-object on responses
    // larger than the display window and drops the schema diff — which
    // is the whole point of schema.verify — so JSON parsing must see
    // the complete bytes the server actually sent.
    const result = await page.evaluate(async (u, init) => {
      try {
        const r = await fetch(u, init);
        const txt = await r.text();
        let parsed = null, parseError = null;
        try { parsed = JSON.parse(txt); } catch (e) { parseError = e.message; }
        return {
          status: r.status, ok: r.ok, length: txt.length,
          text: txt.slice(0, 4000),
          parsed, parseError,
        };
      } catch (e) { return { error: e.message }; }
    }, url.href, fetchInit);

    if (result.error) { err(`  fetch error: ${result.error}`); return; }
    log(`\n  RESPONSE: HTTP ${result.status}  ${result.length}B${result.length > 4000 ? " (display truncated to 4000B — schema parse saw full body)" : ""}`);

    const respJson = result.parsed;
    if (respJson === null || respJson === undefined) {
      log(`  (response is not JSON${result.parseError ? ": " + result.parseError : ""})`);
      log(`  body: ${short(result.text, 400)}`);
      return;
    }
    log(`  parsed: ${typeof respJson === "object" ? (Array.isArray(respJson) ? `array[${respJson.length}]` : `object with ${Object.keys(respJson).length} keys`) : typeof respJson}`);
    if (!m.responseRef) { log(`  (no learned response schema to diff against)`); log(`  sample: ${short(JSON.stringify(respJson), 400)}`); return; }
    const rootResp = plan.schemas[m.responseRef];
    if (!rootResp) { err(`  (response schema ${m.responseRef} not found)`); return; }
    const diff = diffResponseAgainstSchema(respJson, rootResp, { schemas: plan.schemas });
    log(`\n  SCHEMA DIFF against ${m.responseRef}:`);
    log(`    missing from response: ${diff.missing.length}${diff.missing.length ? "  (" + diff.missing.slice(0, 10).join(", ") + (diff.missing.length > 10 ? ", …" : "") + ")" : ""}`);
    log(`    drift (new fields in response): ${diff.extra.length}${diff.extra.length ? "  (" + diff.extra.slice(0, 10).join(", ") + (diff.extra.length > 10 ? ", …" : "") + ")" : ""}`);
    log(`    type mismatches: ${diff.typeMismatches.length}`);
    for (const tm of diff.typeMismatches.slice(0, 10)) log(`      ${tm.path}: expected ${tm.expected}, got ${tm.got}`);
    const verdict = result.ok && diff.missing.length === 0 && diff.typeMismatches.length === 0
      ? (diff.extra.length === 0 ? "exact-match" : "schema is stale (new fields in response)")
      : result.ok
        ? "partial-match"
        : "error-response (check auth/CSRF, or this method genuinely requires a real session)";
    log(`  verdict: ${verdict}`);
  });
}

// Format a values-frequency map as a compact top-N list with counts.
// Long values are truncated so the output stays within terminal width.
function _fmtTopValues(valuesMap, n, strLen) {
  if (!valuesMap || typeof valuesMap !== "object") return "(none)";
  const entries = Object.entries(valuesMap).sort((a, b) => b[1] - a[1]);
  if (entries.length === 0) return "(none)";
  const take = entries.slice(0, n || 5);
  return take.map(([v, c]) => `${JSON.stringify(v).slice(0, strLen || 60)}×${c}`).join(", ")
    + (entries.length > (n || 5) ? `  (+${entries.length - (n || 5)} more)` : "");
}

// Non-zero format hints as "uri:12, email:3" — silent when all are zero.
function _fmtFormatHints(hints) {
  if (!hints || typeof hints !== "object") return null;
  const nonZero = Object.entries(hints).filter(([, v]) => v > 0);
  if (!nonZero.length) return null;
  return nonZero.map(([k, v]) => `${k}:${v}`).join(", ");
}

async function cmdMethod(rest) {
  const { positional, flags } = parseArgs(rest);
  const svcName = positional[0];
  const methodIdArg = positional[1];
  if (!svcName || !methodIdArg) throw new Error("usage: method <service> <methodId|methodName>");
  const topN = flags.top != null ? Number(flags.top) : 5;
  await withBrowser(async (browser) => {
    const data = await evalSW(browser, `
      const e = globalStore.discoveryDocs.get(${JSON.stringify(svcName)});
      if (!e || !e.doc) return null;
      const doc = e.doc;
      let match = null;
      for (const [bName, bObj] of Object.entries(doc.resources || {})) {
        for (const [mName, m] of Object.entries(bObj.methods || {})) {
          if (m.id === ${JSON.stringify(methodIdArg)} || mName === ${JSON.stringify(methodIdArg)}) {
            match = { bucket: bName, name: mName, method: m };
            break;
          }
        }
        if (match) break;
      }
      if (!match) return null;
      const m = match.method;
      const stats = m._stats || { requestCount: 0, params: {}, bodyFields: {} };

      // Flatten the request body schema via $ref + children recursion —
      // callers want a flat map of dot-path → field def so we can join
      // with stats.bodyFields lookups.
      function flattenSchema(ref, visited) {
        const out = [];
        if (!ref || visited.has(ref)) return out;
        visited.add(ref);
        const sch = doc.schemas?.[ref];
        if (!sch) return out;
        function walk(parentPath, props) {
          if (!props) return;
          for (const [k, def] of Object.entries(props)) {
            const dotPath = parentPath ? parentPath + "." + k : k;
            out.push({
              dotPath,
              wireKey: k,
              alias: def.customName && def.name ? def.name : null,
              type: def.type || def.$ref || "?",
              number: def.number ?? null,
              label: def.label || null,
              enum: def.enum || null,
              description: def.description || null,
              _detectedEnum: !!def._detectedEnum,
              _defaultValue: def._defaultValue ?? null,
              _defaultConfidence: def._defaultConfidence ?? null,
              _requiredConfidence: def._requiredConfidence ?? null,
              _range: def._range || null,
              _astValidValues: def._astValidValues || null,
              _exampleValue: def._exampleValue === undefined ? null : def._exampleValue,
              _exampleValueSource: def._exampleValueSource || null,
              _exampleConfidence: def._exampleConfidence ?? null,
              format: def.format || null,
            });
            if (def.$ref) {
              const sub = flattenSchema(def.$ref, visited);
              for (const s of sub) out.push({ ...s, dotPath: dotPath + "." + s.dotPath });
            } else if (def.type === "array" && def.items && def.items.$ref) {
              const sub = flattenSchema(def.items.$ref, visited);
              for (const s of sub) out.push({ ...s, dotPath: dotPath + "[]." + s.dotPath });
            } else if (def.type === "array" && def.items && def.items.properties) {
              walk(dotPath + "[]", def.items.properties);
            } else if (def.properties) {
              walk(dotPath, def.properties);
            }
          }
        }
        walk("", sch.properties || {});
        return out;
      }
      const bodyFields = m.request?.$ref ? flattenSchema(m.request.$ref, new Set()) : [];

      const paramStats = stats.params || {};
      const bodyFieldStats = stats.bodyFields || {};
      // Aggregate stats keys matching a schema dotPath that contains [].
      // Stats keys like "info[0].source" merge into the schema path
      // "info[].source"; without this, every nested array-item field
      // shows as having no observed stats and gets surfaced in the
      // orphan list redundantly. Escape a regex special-char list
      // dynamically to avoid breaking the surrounding template literal
      // (which would clash with the literal "\${" sequence in a regex).
      function _aggregateStatsForPath(dotPath) {
        if (dotPath.indexOf("[]") < 0) return null;
        const SENTINEL = " ARR ";
        const REGEX_SPECIAL = ".*+?^\${}()|[]\\\\".split("");
        function _escRe(ch) { return REGEX_SPECIAL.indexOf(ch) >= 0 ? "\\\\" + ch : ch; }
        const escaped = dotPath
          .split("[]").join(SENTINEL)
          .split("").map(_escRe).join("")
          .split(SENTINEL).join("\\\\[\\\\d+\\\\]");
        const re = new RegExp("^" + escaped + "$");
        let merged = null;
        for (const k of Object.keys(bodyFieldStats)) {
          if (!re.test(k)) continue;
          const fs = bodyFieldStats[k];
          if (!fs) continue;
          if (!merged) merged = { observedCount: 0, values: {}, formatHints: {}, numericRange: null };
          merged.observedCount += fs.observedCount || 0;
          if (fs.values) for (const v in fs.values) merged.values[v] = (merged.values[v] || 0) + fs.values[v];
          if (fs.formatHints) for (const h in fs.formatHints) merged.formatHints[h] = (merged.formatHints[h] || 0) + fs.formatHints[h];
          if (fs.numericRange) {
            if (!merged.numericRange) merged.numericRange = { min: fs.numericRange.min, max: fs.numericRange.max };
            else { merged.numericRange.min = Math.min(merged.numericRange.min, fs.numericRange.min);
                   merged.numericRange.max = Math.max(merged.numericRange.max, fs.numericRange.max); }
          }
        }
        return merged;
      }
      const out = {
        svc: ${JSON.stringify(svcName)},
        bucket: match.bucket,
        name: match.name,
        method: {
          id: m.id,
          httpMethod: m.httpMethod,
          origin: m.origin,
          path: m.path,
          description: m.description || null,
          requestRef: m.request?.$ref || null,
          responseRef: m.response?.$ref || null,
          contentTypes: m.contentTypes || [],
        },
        requestCount: stats.requestCount || 0,
        parameters: Object.entries(m.parameters || {}).map(([pName, pDef]) => ({
          name: pDef.name || pName,
          type: pDef.type || "?",
          location: pDef.location || "?",
          required: !!pDef.required,
          enum: pDef.enum || null,
          format: pDef.format || null,
          _detectedEnum: !!pDef._detectedEnum,
          _defaultValue: pDef._defaultValue ?? null,
          _defaultConfidence: pDef._defaultConfidence ?? null,
          _requiredConfidence: pDef._requiredConfidence ?? null,
          _range: pDef._range || null,
          _astValidValues: pDef._astValidValues || null,
          _exampleValue: pDef._exampleValue === undefined ? null : pDef._exampleValue,
          _exampleValueSource: pDef._exampleValueSource || null,
          _exampleConfidence: pDef._exampleConfidence ?? null,
          stats: paramStats[pName] || null,
        })),
        bodyFields: bodyFields.map(bf => ({
          ...bf,
          stats: bodyFieldStats[bf.dotPath] ||
            (bf.dotPath.indexOf("[]") >= 0 ? _aggregateStatsForPath(bf.dotPath) : null),
        })),
        orphanBodyFields: Object.keys(bodyFieldStats).filter(k => !bodyFields.some(bf => bf.dotPath === k)).map(k => ({
          dotPath: k,
          stats: bodyFieldStats[k],
        })),
        correlations: stats.correlations || [],
      };
      return out;
    `);
    if (!data) { err("no such service or method"); return; }
    log(`${data.svc}  ${data.method.httpMethod} ${_joinUrl(data.method.origin, data.method.path)}`);
    log(`  id: ${data.method.id}  bucket/name: ${data.bucket}/${data.name}`);
    log(`  requestCount: ${data.requestCount}`);
    if (data.method.description) log(`  description: ${short(data.method.description, 200)}`);
    log(`  req=${data.method.requestRef || "-"}  resp=${data.method.responseRef || "-"}  ct=${(data.method.contentTypes||[]).join(",") || "-"}`);

    log(`\n  parameters (${data.parameters.length}):`);
    for (const p of data.parameters) {
      const head = [p.name + ":", p.type, "@" + p.location];
      if (p.required) head.push("[required]");
      if (p.format) head.push("[fmt:" + p.format + "]");
      log(`    · ${head.join(" ")}`);
      if (p.stats) {
        log(`        observed: ${p.stats.observedCount}/${data.requestCount}` +
            (p._requiredConfidence != null ? ` (${Math.round(p._requiredConfidence * 100)}%)` : ""));
        log(`        topValues: ${_fmtTopValues(p.stats.values, topN)}`);
        const hints = _fmtFormatHints(p.stats.formatHints);
        if (hints) log(`        formatHints: ${hints}`);
        if (p.stats.numericRange) log(`        numericRange: [${p.stats.numericRange.min} .. ${p.stats.numericRange.max}]`);
      } else {
        log(`        (no observed stats — never captured in traffic)`);
      }
      if (p._defaultValue != null) {
        // _defaultConfidence = dominant-count / observedCount (how dominant
        // the value is among present observations)
        // _requiredConfidence = observedCount / requestCount (how often the
        // parameter is present at all)
        // Both matter: a "default" at 100% dominance but 20% presence is
        // still meaningful — when the caller DOES pass this param, they
        // pass the same value — but it's also an OPTIONAL param.
        const dc = p._defaultConfidence != null ? Math.round(p._defaultConfidence * 100) + "%" : "?";
        const rc = p._requiredConfidence != null ? Math.round(p._requiredConfidence * 100) + "%" : "?";
        log(`        picked default: ${JSON.stringify(p._defaultValue)}  (dominance ${dc} of observations, present in ${rc} of requests)`);
      }
      if (p._exampleValueSource) {
        const suf = p._exampleConfidence != null ? `  (dominance ${Math.round(p._exampleConfidence * 100)}%)` : "";
        log(`        exampleValue: ${JSON.stringify(p._exampleValue)}  [source: ${p._exampleValueSource}]${suf}`);
      }
      if (Array.isArray(p._astValidValues) && p._astValidValues.length) {
        log(`        ast values: ${p._astValidValues.slice(0, 8).map(v => JSON.stringify(v)).join(", ")}${p._astValidValues.length > 8 ? "  (+" + (p._astValidValues.length - 8) + " more)" : ""}`);
      }
      if (Array.isArray(p.enum) && p.enum.length) {
        log(`        enum: ${p.enum.slice(0, 8).map(v => JSON.stringify(v)).join(", ")}${p.enum.length > 8 ? "  (+" + (p.enum.length - 8) + " more)" : ""}`);
      }
    }

    log(`\n  body fields (${data.bodyFields.length} declared in schema):`);
    for (const bf of data.bodyFields.slice(0, 100)) {
      const head = [bf.dotPath + ":", bf.type];
      if (bf.number != null) head.push("#" + bf.number);
      if (bf.label) head.push(bf.label);
      if (bf.alias) head.push("(alias: " + JSON.stringify(bf.alias) + ")");
      log(`    · ${head.join(" ")}`);
      if (bf.stats) {
        log(`        observed: ${bf.stats.observedCount}/${data.requestCount}`);
        log(`        topValues: ${_fmtTopValues(bf.stats.values, topN)}`);
        const hints = _fmtFormatHints(bf.stats.formatHints);
        if (hints) log(`        formatHints: ${hints}`);
        if (bf.stats.numericRange) log(`        numericRange: [${bf.stats.numericRange.min} .. ${bf.stats.numericRange.max}]`);
      } else {
        log(`        (no observed stats for this body field — schema present but traffic uncovered it)`);
      }
      if (bf._defaultValue != null) log(`        schema default: ${JSON.stringify(bf._defaultValue)}`);
      if (bf._exampleValueSource) {
        const suf = bf._exampleConfidence != null ? `  (dominance ${Math.round(bf._exampleConfidence * 100)}%)` : "";
        log(`        exampleValue: ${JSON.stringify(bf._exampleValue)}  [source: ${bf._exampleValueSource}]${suf}`);
      }
      if (Array.isArray(bf._astValidValues) && bf._astValidValues.length) {
        log(`        ast values: ${bf._astValidValues.slice(0, 8).map(v => JSON.stringify(v)).join(", ")}`);
      }
      if (Array.isArray(bf.enum) && bf.enum.length) {
        log(`        enum: ${bf.enum.slice(0, 8).map(v => JSON.stringify(v)).join(", ")}`);
      }
    }
    if (data.bodyFields.length > 100) log(`    … (+${data.bodyFields.length - 100} more body fields)`);

    if (data.orphanBodyFields.length) {
      log(`\n  orphan body-field stats (${data.orphanBodyFields.length}) — observed in traffic but NOT attached to the declared schema (Phase 5c gap):`);
      for (const of_ of data.orphanBodyFields.slice(0, 30)) {
        log(`    · ${of_.dotPath}`);
        log(`        observed: ${of_.stats.observedCount}  topValues: ${_fmtTopValues(of_.stats.values, topN)}`);
      }
      if (data.orphanBodyFields.length > 30) log(`    … (+${data.orphanBodyFields.length - 30} more orphan stats)`);
    }

    if (data.correlations.length) {
      log(`\n  correlations (${data.correlations.length}):`);
      for (const c of data.correlations.slice(0, 10)) {
        log(`    ${c.paramA} ↔ ${c.paramB}  (confidence ${Math.round(c.confidence * 100)}%)`);
      }
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
  // Auto-wrap a bare expression as `return <expr>` so `page "location.href"`
  // works like a REPL. Explicit statements (containing `return`, `;`, or
  // block braces at top level) are passed through as-is for multi-step
  // scripts like `var a = 1; return a + 2`.
  const body = /(^|\s)return\s|;|\{/.test(expr) ? expr : "return (" + expr + ");";
  await withBrowser(async (browser) => {
    const page = await getActivePage(browser);
    const out = await page.evaluate(new Function("return (async () => { " + body + " })()"));
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

  findings [substring]         security findings (sinks + dangerous patterns) — prefixed with stable ID
  finding <id|index> [--dims-only]
                               one finding + column-aware raw JS snippet (Brotli-aware Node fetch).
                               --dims-only condenses a long taint path (100+ hops) to just the
                               source, dim-transitions, and sink — for fast reachability review.
  finding.snapshot <name>      save current findings keyed by stable ID → testing/finding-snapshots/<name>.json
  finding.diff <name>          compare current findings to snapshot; shows added / removed / severity-flipped
  finding.map <id|index>       resolve minified line/col through //# sourceMappingURL; print original source
  finding.func <id|index>      same + Babel-parse the resolved source, print the full enclosing function
  finding.callers <id|index> [--arg N] [--depth K] [--max-len N]
                               list call sites of each enclosing function + their arguments; highlights
                               which function boundaries the taint path crossed (param-caller hops)
  finding.exploit <id|index> [--page-url <url>] [--wait <ms>]
                               install a DOM/fetch/XHR/eval probe, navigate with a unique marker in
                               the hash/query, and report whether the marker actually reached a sink.
                               Proves exploitability end-to-end for location.hash/search/href sources.
  finding.view <id|index> [--preview N]
                               open the source viewer for this finding, let it tree-shake to the
                               functions the taint path crosses, and report focused-vs-full line
                               counts + a short preview. Verifies the viewer's per-finding focus
                               actually collapses a multi-MB bundle to the relevant slice.
  ast.probe <file>|--inline <code> [--source <url>]
                               run the extension's real AST analyzer on an arbitrary code blob
  ast.probe.finding <id|index> [--raw] [--depth <N|outer>]
                               isolate enclosing function, rerun analyzer, report whether the
                               same-kind classifier still fires. --raw probes the minified
                               bundle (matches live analyzer input); default probes source-mapped
                               original (readable but different identifier names). --depth 0 =
                               innermost (default), N = Nth outer function, outer = outermost.

  logs [filter] [--service=X] [--host=X] [--ct=X] [--method=X] [--status=N] [--with-body] [--limit=N]
                               [--kind api|asset|empty] [--boring]
                               --kind filters by the content-based asset classification (magic
                               bytes from body); --boring narrows to the static-resource tier
                               (asset + GET + no query + no body + no auth) \u2014 audit view for
                               whether the classifier got it right.
  log <reqId> [--decoded] [--save <dir>] [--full]
                               one request. --decoded runs the extension's parsers
                               (batchexecute/JSPB/protobuf/gRPC-Web/GraphQL/SSE/NDJSON/multipart).
                               --save writes meta+headers+raw bytes under <dir>/<reqId>/.

  keys                         API key registry
  services [substring]         service list (name, method count, samples)
  service <name> [--full]      drill-down: methods, params with AST badges, schemas, keys
  method <service> <id|name> [--top N]
                               per-field stats (params + body tree): observedCount, top values,
                               formatHints, numericRange, AST values, picked default + provenance.
                               Surfaces orphan body-field stats that Phase 5c will attach.
  schema.verify <service> <id|name>
                               build a request from the learned exampleValues and send it through
                               the active page (credentialed). Diffs the response against the learned
                               response schema: missing fields / drift / type mismatches. MVP covers
                               GET + JSON POST.
  schema.review <service> <id|name>
                               scannable readiness view: schema tree with each leaf's example value
                               and provenance tag, gap list (required fields with no observation),
                               provenance counts, and a PREVIEW of the ready-to-send request.

  audit.sinks [substr] [--show N] [--missed-only] [--context N]
                               regex-grep every AST-analyzed script for known sink patterns
                               (innerHTML=, eval(, location.href=, etc.) and list grep-matches
                               that have no AST finding on the same line — each one is a site
                               to read and decide whether the AST correctly ignored it or missed
                               something.
  audit.schema <service> [--min-obs N] [--show-worst N]
                               service-wide schema readiness: per-method % fields with real
                               observations, required-field gaps, provenance histogram,
                               worst-methods drill-down.
  audit.gaps [substr] [--limit N] [--context N]
                               for each unique resolver gap, fetch the per-script source
                               and print the actual JS at the gap's root-cause line/col.
                               Maps combined-bundle (line,col) back to per-script (url,line,col)
                               via the cached scriptOffsets[]. substr filters by script URL.
  audit.perf [substr] [--limit N] [--sort total|parse|prePass|mainPass|mb-per-s]
                               per-script analysis perf breakdown: totalMs, per-phase ms (parse/
                               prePass/mainPass/structuralExport), MB/s throughput, finding
                               counts, and inter-procedural counters. Sorted worst-first by
                               totalMs by default. Pillar 13: surface where time is spent on
                               real bundles so regressions are visible per-phase, not just in
                               aggregate.
  audit.profile [name] [--interval µs] [--timeout ms] [--poll ms] [--top N]
                               attach Chromium's V8 CPU profiler to the offscreen ast-worker
                               document, kick the combined-bundle analysis on the active tab's
                               buffered scripts, and write a standard .cpuprofile file to
                               testing/finding-snapshots/<name>.cpuprofile (loadable in Chrome
                               DevTools Performance tab). Also prints the top-N self-time leaves
                               inline so the reviewer sees the hotspot without opening DevTools.
                               Use this for line/function-level perf attribution on real bundles
                               — sampling profiler is the right tool, not custom in-bundle stats.

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
  "finding.snapshot": cmdFindingSnapshot, "finding.diff": cmdFindingDiff,
  "finding.map": cmdFindingMap, "finding.func": cmdFindingFunc,
  "finding.callers": cmdFindingCallers,
  "finding.exploit": cmdFindingExploit,
  "finding.view": cmdFindingView,
  "ast.probe": cmdAstProbe, "ast.probe.finding": cmdAstProbeFinding,
  logs: cmdLogs, log: cmdLog,
  keys: cmdKeys, services: cmdServices, service: cmdService,
  method: cmdMethod, "schema.verify": cmdSchemaVerify, "schema.review": cmdSchemaReview,
  "audit.sinks": cmdAuditSinks, "audit.schema": cmdAuditSchema, "audit.gaps": cmdAuditGaps,
  "audit.perf": cmdAuditPerf, "audit.profile": cmdAuditProfile, "audit.profile.summary": cmdAuditProfileSummary,
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
