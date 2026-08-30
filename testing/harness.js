// Live harness for driving Chrome + the extension interactively.
//
// Three commands. Real interactive UI testing means driving the
// popup as a user would and observing the page DOM; nothing
// shortcuts into the service worker. Any SW behaviour the harness
// needs to verify gets driven via the popup's own chrome.runtime
// .sendMessage surface (or via popup clicks that trigger it).
//
// THIS LIST WAS THREE COMMANDS LONG WHILE `CMDS` HELD TWENTY-ONE, and CLAUDE.md is right that a stale
// description reads as authoritative and sends the next reader to build what is already there. The full set is
// the `CMDS` table at the bottom of this file, which is the only place that cannot go stale; named here are the
// ones a reader reaches for. `restart` in particular was undocumented here and is the one CLAUDE.md tells you
// to use — `start` reuses a stale wasm and a poisoned IndexedDB.
//
//   start [port]              launch Chrome with the extension loaded;
//                             detached so it survives this shell.
//   restart [port]            the same, after clearing IndexedDB and the code cache first. USE THIS ONE:
//                             `start` keeps whatever a previous run left, and a stale wasm plus a poisoned
//                             IDB is a measurement of a program no revision contains.
//   restart-keep [port]       restart WITHOUT clearing storage — for the cross-session resume question,
//                             where the residue left by the previous session IS the subject.
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
/* THE PROFILE AND THE LOCK ARE ONE PAIR PER BROWSER, AND THIS CHECKOUT HOLDS SEVERAL AGENTS AT ONCE. Both
   were fixed paths, so a second agent running `restart` killed the first agent's Chrome by the pid in the
   shared lock and then wiped the IndexedDB out from under it — a measurement destroyed by a process that was
   never asked about it, and silently, because the survivor's next command simply reconnects to a browser that
   is not the one it started. Naming the pair through the environment makes a private browser one variable
   away; unset, every path is exactly what it was. The debug port must be distinct too (it is already an
   argument to `start`), and the extension ID is derived from EXT_DIR, so two browsers on one checkout still
   agree on which chrome-extension:// origin is ours. */
const PROFILE_DIR = process.env.HARNESS_PROFILE
  ? path.resolve(process.env.HARNESS_PROFILE) : path.join(__dirname, "profile");
const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const DEFAULT_PORT = Number(process.env.HARNESS_PORT || 9337);
/* WHERE A WORLD-READABLE BROWSER LIVES when the resolved one sits under a home directory nobody else can
   traverse. One constant so the guard below and the message it prints cannot disagree about the path. */
const SHARED_CHROME = process.env.HARNESS_SHARED_CHROME || "/opt/chrome-for-testing/chrome";

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
  /* A REAL ACTION POPUP IS NOT A TAB, AND THIS ONE IS — WHICH IS THE WHOLE ANSWER TO "WHICH PAGE AM I
     LOOKING AT". popup.js asks `chrome.tabs.query({active:true, currentWindow:true})` at DOMContentLoaded.
     From a toolbar popup that answers the WEB PAGE under it, because the popup owns no tab. From the popup
     opened as a tab here it answers THE POPUP'S OWN TAB — and chrome.webNavigation reports no frame for a
     chrome-extension:// page, so every popup measurement this harness has ever taken was about a tab holding
     no web document. That is not a small skew: it produced a whole reported product defect ("the crash notice
     renders for no page, ever") whose real subject was this function. MEASURED, load 3.68: driving the popup
     unchanged gave documentId null and analysisRun null; setting currentTabId to the page's tab and calling
     loadState() on the SAME build gave the real documentId and analysisRun "complete".
     So the arrangement is made faithful rather than described: activate the web tab, then RELOAD the popup so
     its DOMContentLoaded query runs while that tab is the active one (a background reload does not steal the
     activation back). Then ASSERT the popup captured it — a harness that silently drives the wrong tab
     teaches the next reader that the tab was never load-bearing, and this one already taught that once. */
  const webPage = (await browser.pages()).find(p => /^https?:/i.test(p.url()));
  if (!webPage) return popup;   // no web page open: the popup IS over no web document, and that is the truth
  // Checked before it is fixed, so repeated `popup` commands neither reload the view out from under a
  // measurement nor cost a second init pass.
  const capturedTab = () => popup.evaluate(async () => {
    const [t] = await chrome.tabs.query({ active: true, currentWindow: true });
    return { captured: currentTabId, active: t ? { id: t.id, url: t.url } : null };
  });
  let seen = await capturedTab();
  if (!seen.active || seen.captured !== seen.active.id || !/^https?:/i.test(seen.active.url || "")) {
    await webPage.bringToFront();
    await popup.reload({ waitUntil: "domcontentloaded", timeout: 10000 });
    await sleep(800);
    seen = await capturedTab();
  }
  if (!seen.active || seen.captured !== seen.active.id || !/^https?:/i.test(seen.active.url || "")) {
    throw new Error("harness could not seat the popup over the web page the way a toolbar popup sits over it: "
      + "popup captured tabId=" + seen.captured + ", active tab is " + JSON.stringify(seen.active)
      + " — every per-document panel would be measured against the wrong tab, so refusing to drive it");
  }
  return popup;
}

// ─── commands ──────────────────────────────────────────────────────────────

// restart [port] — kill the harness Chrome and relaunch CLEAN. A running
// analysis worker re-imports `lib/qjs/qjs_worker.js` (the SINGLE_FILE wasm) and
// Chrome caches it in worker memory; chrome.runtime.reload() + bin/Clear (worker
// respawn) do NOT bust that in-memory cache, so a freshly-BUILT engine isn't
// picked up live (verified: the grind stuck at the SAME emission count pre/post
// reload = stale wasm). A new Chrome PROCESS has no in-memory cache, BUT V8's
// ON-DISK code cache (Default/Code Cache) does NOT reliably invalidate on a
// rebuild: it caches the compiled bytecode of importScripts'd classic scripts —
// INCLUDING qjs_wasm.cow.gen.js, whose body is one giant `atob("<base64 wasm>")`
// string literal. A stale cache entry replays the OLD base64 → the OLD wasm runs
// even though the on-disk blob is fresh (MEASURED: disk blob had the v2 assert
// string `propTrk`, yet the running engine still emitted the pre-v2 `@addr`
// message — a silently-stale measurement, the exact false-confidence CLAUDE.md
// bans). So WIPE Default/Code Cache on every restart — it is a pure V8
// compilation cache (Chrome regenerates it on next compile; no correctness/state
// in it). Also clears IndexedDB (learned-state reset). PRESERVES cookies / Login
// Data / the "Service Worker" dir (deleting the SW dir breaks the extension's
// background SW startup → no offscreen doc → analysis never runs) and the rest of
// the profile so authenticated sites stay logged in. Use after `node engine/build.mjs stage`.
async function cmdRestart(args, keepIdb) {
  const lock = await readLock();
  if (lock && lock.pid) {
    try {
      if (process.platform === "win32") execSync(`taskkill /F /T /PID ${lock.pid}`, { stdio: "ignore" });
      else { try { process.kill(-lock.pid, "SIGKILL"); } catch { process.kill(lock.pid, "SIGKILL"); } }
      log(`killed Chrome pid ${lock.pid}`);
    } catch (e) { log(`kill pid ${lock.pid}: ${e.message || e} (may already be gone)`); }
  }
  /* Reap LEAKED harness Chromes not in the lock: a test script killed mid-restart (TaskStop) leaves its
     Chrome behind, and they accumulate until the grind wedges (measured: ~10 instances -> sentry barely
     grinds). A launch whose port never answered leaves one too — cmdStart exits before it writes the lock,
     so the process it started becomes unkillable by every later restart, and the NEXT launch then fails on a
     profile another process still holds. That failure is silent in the way §Testing warns about: the row
     reports on a browser nobody started.
     THE REAP IS SCOPED BY THE PROFILE, WHICH IS THIS LANE'S NAME FOR ITS OWN BROWSER. It used to match
     `remote-debugging-port`, which is carried by EVERY harness Chrome on the box — and this checkout holds
     several agents at once, each with one, which is the exact defect the PROFILE_DIR/LOCK_FILE split was
     introduced to fix one screen up. A reap that kills a browser it did not start destroys somebody else's
     measurement and does it invisibly. `--user-data-dir=<PROFILE_DIR>` is on the command line of this lane's
     Chrome and of no other, so it is the one predicate that reaps exactly what this command owns. */
  const mine = `--user-data-dir=${PROFILE_DIR}`;
  if (process.platform === "win32") {
    try {
      const ps = "Get-CimInstance Win32_Process -Filter \"name='chrome.exe'\" | Where-Object { $_.CommandLine -match " +
                 JSON.stringify(mine.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).replace(/"/g, "'") +
                 " } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }";
      execSync(`powershell -NoProfile -EncodedCommand ${Buffer.from(ps, "utf16le").toString("base64")}`, { stdio: "ignore" });
    } catch (e) { /* best-effort reap */ }
  } else {
    try {
      const stray = execSync("ps -eo pid,args", { stdio: ["ignore", "pipe", "ignore"] }).toString()
        .split("\n").filter((l) => l.includes(mine))
        .map((l) => Number(l.trim().split(/\s+/)[0])).filter((p) => p && p !== process.pid);
      for (const p of stray) { try { process.kill(p, "SIGKILL"); } catch (e) { /* already gone */ } }
      if (stray.length) log(`reaped ${stray.length} stray process(es) still holding ${PROFILE_DIR}`);
    } catch (e) { /* no ps: best-effort reap */ }
  }
  await clearLock();
  await sleep(1800);   // let the process group exit + release the profile's file locks
  // IndexedDB (learned-state reset) + V8 Code Cache (so a rebuilt wasm blob is NOT
  // served from a stale compiled-bytecode entry — see the header comment). Both are
  // pure caches; cookies/login live in other dirs and survive.
  // `restart-keep` PRESERVES IndexedDB so cross-SESSION resume (the UNBOUNDED frontier surviving a browser
  // restart — a core design claim) is verifiable on the REAL thing: park recipes, restart-keep, re-visit,
  // confirm they rehydrate. Plain `restart` still wipes IDB for a clean-state test.
  if (keepIdb) { log("KEEPING Default/IndexedDB (cross-session resume test)"); }
  else {
    try { await fsp.rm(path.join(PROFILE_DIR, "Default/IndexedDB"), { recursive: true, force: true }); log("cleared Default/IndexedDB"); }
    catch (e) { /* absent dir is fine */ }
  }
  try { await fsp.rm(path.join(PROFILE_DIR, "Default/Code Cache"), { recursive: true, force: true }); log("cleared Default/Code Cache"); }
  catch (e) { /* absent dir is fine */ }
  await cmdStart(args);
}

/* WHO CHROME SHOULD RUN AS, answered once. Returns {uid,gid,name} to drop to, or null meaning "run as this
   process" — which is the normal answer for a developer, who is already unprivileged and whose sandbox works.
   It returns null for root ONLY when the operator has explicitly allowed an unsandboxed run; every other
   failure to arrange one THROWS, naming the single command that fixes it. A harness that quietly downgrades
   its own isolation teaches the next reader that the isolation was never load-bearing. */
function resolveUnprivileged(chromePath) {
  /* `chromePath` is reassigned below when a readable copy is used, so the caller must take the resolved one. */
  if (typeof process.getuid !== "function" || process.getuid() !== 0) return null;   // already unprivileged
  if (process.env.HARNESS_ALLOW_NO_SANDBOX === "1") {
    log("WARNING: HARNESS_ALLOW_NO_SANDBOX=1 — Chrome will run as root with --no-sandbox.");
    log("         The renderer will have Seccomp: 0 and init's user namespace, and this harness drives");
    log("         attacker-controlled documents. This is an explicit operator choice, not a default.");
    return null;
  }
  const name = process.env.HARNESS_USER || "chromerun";
  let uid, gid;
  try {
    uid = Number(execSync(`id -u ${name}`, { stdio: ["ignore", "pipe", "ignore"] }).toString().trim());
    gid = Number(execSync(`id -g ${name}`, { stdio: ["ignore", "pipe", "ignore"] }).toString().trim());
  } catch {
    throw new Error(
      `running as root, and the unprivileged account \`${name}\` does not exist. Chrome refuses to run as root\n` +
      `  with its sandbox on, and this harness will not turn the sandbox off for you — it drives the attacker-\n` +
      `  controlled documents the tool exists to analyse. Create the account once:\n` +
      `      useradd -m ${name}\n` +
      `  or set HARNESS_USER to an existing one, or HARNESS_ALLOW_NO_SANDBOX=1 to accept an unsandboxed run.`);
  }
  /* THE TARGET MUST BE ABLE TO EXECUTE THE BROWSER AND WRITE THE PROFILE, and both are checked HERE rather
     than discovered as a launch that dies with an empty log. puppeteer installs under the invoking user's
     home, which for root is mode 0700 — so the usual failure is a binary the dropped user cannot reach, and
     the message says which of the two it was. */
  const canExec = (p) => {
    try { execSync(`su -s /bin/sh ${name} -c 'test -x ${JSON.stringify(p)}'`, { stdio: "ignore" }); return true; }
    catch { return false; }
  };
  /* AND IT LOOKS FOR ONE BEFORE IT REFUSES. The first version of this guard only rejected, which is the right
     default and the wrong whole behaviour: it made every invocation carry HARNESS_CHROME by hand, and a check
     that must be worked around on every run is one somebody eventually works around permanently — with
     HARNESS_ALLOW_NO_SANDBOX, which is the outcome this guard exists to prevent. So the conventional readable
     location is TRIED, and using it is announced rather than silent, because a harness that quietly runs a
     different binary than the one puppeteer resolved is its own kind of lie. */
  if (!canExec(chromePath) && canExec(SHARED_CHROME)) {
    log(`${chromePath} is not reachable by ${name}; using the readable copy at ${SHARED_CHROME}`);
    chromePath = SHARED_CHROME;
  }
  if (!canExec(chromePath)) {
    throw new Error(
      `\`${name}\` cannot execute ${chromePath} — puppeteer installs under root's home, which is not\n` +
      `  traversable by anyone else, and no readable copy was found at ${SHARED_CHROME}. Make one:\n` +
      `      cp -r "$(dirname ${JSON.stringify(chromePath)})" /opt/chrome-for-testing && chmod -R a+rX /opt/chrome-for-testing\n` +
      `      HARNESS_CHROME=/opt/chrome-for-testing/$(basename ${JSON.stringify(chromePath)}) node testing/harness.js restart`);
  }
  try { fs.mkdirSync(PROFILE_DIR, { recursive: true }); execSync(`chown -R ${uid}:${gid} ${JSON.stringify(PROFILE_DIR)}`); }
  catch (e) { throw new Error(`could not hand ${PROFILE_DIR} to ${name}: ${e.message}`); }
  log(`dropping privileges to ${name} (uid ${uid}) so Chrome keeps its sandbox`);
  return { uid, gid, name, chromePath };
}

/* AND THE CLAIM IS CHECKED RATHER THAN ASSERTED. A flag that stops being passed, a kernel that forbids the
   namespace, a Chrome that degrades quietly — each leaves a harness that reports a sandboxed run and delivers
   an unsandboxed one, which is worse than never having claimed it. Linux states the answer per process:
   `Seccomp: 2` is filter mode, and a sandboxed renderer also carries `NoNewPrivs: 1`. */
/* THE PROCESSES BELOW ONE ROOT, because this checkout holds several agents at once and each of them has a
   Chrome. Walked from /proc's own parent links rather than from `ps --ppid`, which answers one level. */
function descendantsOf(rootPid) {
  const parent = new Map();
  let names = [];
  try { names = fs.readdirSync("/proc").filter((n) => /^\d+$/.test(n)); } catch { return null; }
  for (const n of names) {
    try {
      const st = fs.readFileSync(`/proc/${n}/stat`, "utf8");
      /* comm is parenthesised and may contain spaces, so the fields after it are read from the LAST ')'. */
      const after = st.slice(st.lastIndexOf(")") + 2).split(" ");
      parent.set(Number(n), Number(after[1]));
    } catch { /* the process exited between the readdir and the read */ }
  }
  /* The depth bound is a guard against a /proc read that raced into a cycle, not a limit on process depth:
     Chrome's browser → zygote → renderer is three, and no real chain here is anywhere near it. */
  const under = (pid) => {
    for (let p = pid, i = 0; p > 0 && i < 64; i++) {
      if (p === rootPid) return true;
      const q = parent.get(p);
      if (q === undefined || q === p) return false;
      p = q;
    }
    return false;
  };
  return new Set([...parent.keys()].filter(under));
}

/* THE ASSERTION IS ABOUT THE BROWSER THIS FUNCTION JUST LAUNCHED, WHICH IS WHY IT TAKES ITS PID. It scanned
   EVERY `--type=renderer` on the machine, and on a shared box that is every agent's Chrome: a launch that died
   before it forked a renderer at all still printed "sandbox verified: 6/6", because six of somebody else's
   were sandboxed. A security assertion passing on evidence from a process it did not start is the same defect
   as a gate run against a tree no revision contains — the number is real and it is about nothing. */
function assertRendererSandboxed(expectSandbox, rootPid) {
  let renderers = [];
  const ours = descendantsOf(rootPid);
  try {
    renderers = execSync("ps -eo pid,args", { stdio: ["ignore", "pipe", "ignore"] }).toString()
      .split("\n").filter((l) => l.includes("--type=renderer"))
      .map((l) => Number(l.trim().split(/\s+/)[0])).filter(Boolean)
      .filter((p) => !ours || ours.has(p));
  } catch { /* no ps: the check is unavailable, which is not the same as passing — say so below */ }
  if (!renderers.length) { log("sandbox check: no renderer process of THIS Chrome yet — nothing to verify against"); return; }
  const read = (pid, key) => {
    try {
      const m = fs.readFileSync(`/proc/${pid}/status`, "utf8").match(new RegExp("^" + key + ":\\s*(\\d+)", "m"));
      return m ? Number(m[1]) : null;
    } catch { return null; }
  };
  const sandboxed = renderers.filter((p) => read(p, "Seccomp") === 2 && read(p, "NoNewPrivs") === 1);
  if (expectSandbox && !sandboxed.length) {
    throw new Error(
      `the launch asked for a SANDBOXED Chrome and no renderer is sandboxed — checked ${renderers.length}\n` +
      `  renderer(s), none with Seccomp: 2 and NoNewPrivs: 1. Something downgraded the sandbox silently;\n` +
      `  do not treat this run's results as taken under one.`);
  }
  log(expectSandbox
    ? `sandbox verified: ${sandboxed.length}/${renderers.length} renderer(s) at Seccomp: 2, NoNewPrivs: 1`
    : `sandbox NOT in use (${sandboxed.length}/${renderers.length} renderer(s) sandboxed) — as explicitly allowed`);
}

/* THE CALLER AND THE CALLEE ARE TWO FILES AND NOTHING STATED THEIR PAIRING, so a browser launched here can be
   handed an engine that cannot answer a single one of the calls the extension makes. Measured, on this
   checkout, against five real sites: every run aborted at `qjs_init` with `native function \`qjs_init\` called
   with 8 args but expects 5`, because the artifact was built two ABI-widening commits earlier — the document's
   length and the inherited policy container's self-origin had each been added to the C entry and to the
   extension's own description, and the wasm beside them was from before either. Five sites, five crashes,
   one signature, and NOTHING in the output named a stale artifact: the message names an arity, and the reader
   is 58 commits away from the cause.
   THE OTHER DIRECTION IS WORSE AND HAS NO MESSAGE AT ALL. Emscripten's own guard is `args.length <= nargs` and
   its comment says why — "too few can be valid since the missing arguments will be zero filled" — so an
   artifact that expects MORE than the extension places runs happily with NULL for every operand the caller
   never learned about. A `qjs_init` whose inherited-policy pointers arrive as 0 builds a document with a policy
   container out of nothing and then reports endpoints and sinks: a plausible datum, which is the one failure
   this project has no way to see.
   SO THE PAIRING IS ASSERTED FROM BOTH SIDES BEFORE A BROWSER IS LAUNCHED, and it is asserted out of files that
   are ALWAYS present wherever this harness runs — all of them live under `extension/`, so a corpus lane
   (testing/corpus/run.sh copies harness.js + extension/ and nothing else) checks exactly the pair it froze.
   The engine SOURCE is deliberately not consulted: `engine/host/main.c` is not copied into a lane, and a check
   that can only be made in the checkout is a check the frozen-artifact runs would have to skip.
   WHAT THE EXTENSION PLACES IS ASKED, NOT SCRAPED, AND THAT CHANGED WHAT THIS CHECK IS. It used to read a
   hand-aligned `place: [...]` array out of renderer.html's source text by regex and add it up against a token
   vocabulary restated here — so the operand count reached this file through a SECOND reader of a hand-kept
   list, and the check compared one copy of a fact against another. The extension derives its placements from
   its own wire description now, so this runs that description (check.js + mojo.js + mojom.js, in an isolated
   context) and asks `abiPlacement` the same question the renderer asks it. The comparison left is the one
   between two GENERATIONS — source against artifact — which is the only one a rebuild can be the answer to.
   IT IS EQUALITY, NOT A BOUND, and a shape this cannot read is FATAL rather than skipped. A parser that
   silently matches nothing reports agreement it never established — the instrument reading zero, which is the
   defect this whole check exists to stop being invisible. */
function assertEngineAbiPairing() {
  const rendererPath = path.join(EXT_DIR, "renderer.html");
  const gluePath = path.join(EXT_DIR, "lib", "qjs", "qjs.mjs");
  let rendererSrc, glueSrc;
  try { rendererSrc = fs.readFileSync(rendererPath, "utf8"); }
  catch (e) { throw new Error(`cannot read ${rendererPath} — the renderer's ABI binding table is one half of the\n  engine pairing this refuses to launch without: ${e.message}`); }
  try { glueSrc = fs.readFileSync(gluePath, "utf8"); }
  catch (e) { throw new Error(`cannot read ${gluePath} — the built engine glue is the other half of the\n  pairing. Build it: node engine/build.mjs cow  (${e.message})`); }

  /* WHAT THE ARTIFACT ACCEPTS, in its own words. Emscripten writes one `createExportWrapper(name, export,
     nargs)` per exported entry and `nargs` is the wasm signature's parameter count, baked in at link time —
     so this is the built program describing itself, not a restatement of it. */
  const glue = new Map();
  for (const m of glueSrc.matchAll(/createExportWrapper\(\s*"(\w+)"\s*,[^,]+,\s*(\d+)\s*\)/g)) glue.set(m[1], Number(m[2]));
  if (!glue.size) throw new Error(`${gluePath} declares no createExportWrapper entries — this harness could not\n  read what the built engine accepts, and reporting that as agreement is the measurement no run could\n  contradict. The glue's shape changed; fix this parse rather than launching.`);

  /* WHAT THE EXTENSION PLACES, ASKED OF THE EXTENSION'S OWN DESCRIPTION RATHER THAN SCRAPED OUT OF IT. This
     used to REGEX a `place: [...]` array out of renderer.html's source text and add up a token vocabulary
     restated here — a second reader of a hand-aligned list, which is two ways for one fact to be wrong and one
     of them silent: a parse that matches nothing reports agreement it never established. The list is gone
     (renderer.html derives its placements) and so is the scrape. `mojom.js` is the description both ends load,
     it is a classic script over `self`, and running it in an isolated context is what `engine/route.mjs` does
     for the same reason — so the operand count comes from the same function the renderer itself places by.
     AN ISOLATED CONTEXT AND NOT THIS REALM: evaluating these files over the harness's own global would install
     `mojo`, `DCHECK` and `CHECK` into a driver whose other commands do not expect them. The load order is the
     frame's, because mojo.js asserts through check.js and mojom.js declares through mojo.js.
     IT IS STILL READ OUT OF FILES A LANE ALWAYS FREEZES — check.js, mojo.js and mojom.js all live under
     `extension/`, which run.sh copies whole — for the same reason the glue check does not consult
     `engine/host/main.c`: a check that can only be made in the checkout is one the frozen-artifact runs would
     have to skip. A file that will not load is FATAL, never skipped. */
  const vm = require("vm");
  const sandbox = { console };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  for (const f of ["check.js", "mojo.js", "mojom.js"]) {
    const at = path.join(EXT_DIR, f);
    try { vm.runInContext(fs.readFileSync(at, "utf8"), sandbox, { filename: at }); }
    catch (e) { throw new Error(`${at} would not load — it is one of the three files that ARE the extension's\n  wire description, and a harness that cannot read the description cannot state that the engine beside it\n  answers the same calls: ${e.message}`); }
  }
  const iface = sandbox.mojo.interfaceOf("content.mojom.Renderer");
  if (!iface || !iface.methods || !iface.methods.length)
    throw new Error(`${path.join(EXT_DIR, "mojom.js")} declares no \`content.mojom.Renderer\` methods — that\n  interface IS the engine ABI, so a description that parsed to nothing is a pairing nothing checked.`);
  /* ONE OPERAND COUNT PER METHOD, DERIVED. A byte parameter is a (pointer, length) PAIR and counts twice
     against the built engine's arity; everything else is one operand. `abiPlacement` crashes on a type or an
     ownership nobody has described rather than guessing a count — which is the whole reason this asks it
     instead of keeping its own table of what a placement is worth. */
  const operands = new Map();   // mojo method js-name -> C operand count
  for (const m of iface.methods) {
    let n = 0;
    for (const p of m.params) n += sandbox.mojo.abiPlacement(p).operands;
    operands.set(m.js, n);
  }

  /* THE ONE FACT THE WIRE CANNOT CARRY, AND THEREFORE THE ONE THING STILL READ OUT OF THE DOCUMENT: which
     `qjs_*` entry each method is. A mojom declares an interface, not a symbol in a wasm module, so this pairing
     cannot be made without renderer.html's binding table. It is found by the `fn:` key naming a `qjs_`-prefixed
     literal — never by the table's variable name — so a table that moves or is renamed is still read, and a
     table this cannot read at all is FATAL. */
  const tableSrc = rendererSrc.match(/var ABI = \{([\s\S]*?)\n\s*\};/);
  if (!tableSrc) throw new Error(`${rendererPath} holds no \`var ABI = {…}\` table — that table is what binds each\n  mojom method to its C entry, so a shape this cannot read is a pairing nothing checked.`);
  const entryOf = new Map();    // mojo method js-name -> qjs_* entry
  for (const m of tableSrc[1].replace(/\/\*[\s\S]*?\*\//g, "").matchAll(/(\w+)\s*:\s*\{\s*fn\s*:\s*"(qjs_\w+)"/g))
    entryOf.set(m[1], m[2]);
  if (!entryOf.size) throw new Error(`${rendererPath}'s ABI table parsed to no bindings — see above.`);

  const wire = [];
  for (const m of iface.methods)
    if (!entryOf.has(m.js)) wire.push(`  ${m.js}: the mojom declares it, ${path.basename(rendererPath)}'s ABI table binds it to no qjs_* entry`);
  for (const js of entryOf.keys())
    if (!operands.has(js)) wire.push(`  ${js}: the ABI table binds it, the mojom declares no such method`);
  if (wire.length) throw new Error(
    `the extension's ABI table and its own wire declaration describe different calls — REFUSING TO LAUNCH.\n` +
    `  Every renderer this browser started would abort at bind, before its first document, and the census\n` +
    `  would read those pages as ones that learned nothing.\n` +
    wire.join("\n") + "\n" +
    `  table: ${rendererPath}\n  wire:  ${path.join(EXT_DIR, "mojom.js")}\n` +
    `  This is a source-to-source skew and NO rebuild fixes it — the two lists are edited together.`);

  /* AND THE PAIRING A REBUILD DOES FIX. The mojom moves with the SOURCE and the glue is exactly as old as the
     last build, so this is where a stale artifact is named: emscripten's own wrapper asserts only on too MANY
     operands (too few are zero-filled silently, and a `qjs_init` whose inherited-policy pointers arrive as 0
     builds a document out of nothing and then reports endpoints and sinks), and its message names an arity
     rather than a parameter or a commit. */
  const skew = [];
  for (const [js, want] of operands) {
    const fn = entryOf.get(js);
    if (!glue.has(fn)) { skew.push(`  ${fn}: the extension calls it, the built engine does not export it`); continue; }
    if (glue.get(fn) !== want) skew.push(`  ${fn}: the extension places ${want} operand(s), the built engine accepts ${glue.get(fn)}`);
  }
  if (!skew.length) return;

  /* THE STAMP IS A HINT AND IS LABELLED AS ONE. `head` is what the SOURCE TREE was called when the artifact
     was built, and this tree is edited continuously by other lanes — testing/corpus/site.mjs says the same
     thing about the same field. It is printed because a wrong name still points at roughly when, and the
     rebuild command below is the answer regardless of what it says. */
  let stamp = "(no qjs.mjs.build.json beside the glue)";
  try {
    const b = JSON.parse(fs.readFileSync(path.join(EXT_DIR, "lib", "qjs", "qjs.mjs.build.json"), "utf8"));
    stamp = `built at ${b.at} from a tree then called ${b.head} (qjs ${b.qjsHead})`;
  } catch (e) { stamp = `(qjs.mjs.build.json unreadable: ${e.message})`; }
  throw new Error(
    `the built engine and the extension that calls it are from different generations — REFUSING TO LAUNCH.\n` +
    `  Every page this browser analysed would abort at the first skewed entry, or worse, run with the\n` +
    `  operands it never learned about zero-filled and report findings composed out of nulls.\n` +
    skew.join("\n") + "\n" +
    `  artifact: ${gluePath}\n            ${stamp}\n` +
    `  Rebuild it against this tree:  node engine/build.mjs cow\n` +
    `  (A frozen-artifact lane must freeze BOTH halves — copy extension/ whole, which run.sh does.)`);
}

async function cmdStart(args) {
  /* BEFORE ANYTHING ELSE, INCLUDING THE ALREADY-RUNNING SHORTCUT BELOW: a browser that is already up is
     running the same pair, so returning early would report a healthy launch for the same broken engine. */
  assertEngineAbiPairing();
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
  /* AND THE PORT MUST BE FREE, BECAUSE THE READINESS POLL BELOW CANNOT TELL WHOSE BROWSER ANSWERED. The lock
     above has already established that OUR browser is not running, so anything answering here belongs to
     another agent of this shared checkout — and the poll after the spawn would find it, report "started", and
     write our lock naming a port we do not own. Every command after that drives somebody else's Chrome: their
     extension, their profile, their tabs. Measured here — a launch whose own Chrome died on its first line
     reported success and then answered `diag` with another lane's extension id, and nothing in the output
     said so. Refuse instead, and name the fix, because a second agent's browser is a normal state of this box
     and not an error to work around silently. */
  /* WAITED OUT FIRST, because `restart` has just killed OUR browser and a killed Chrome does not release its
     listening socket on the same tick — a constant sleep before this point would be a guess, and this is the
     condition that sleep was guessing at. Only a port that is STILL answering after it belongs to somebody. */
  for (let i = 0; i < 20 && (await pingPort(port)); i++) await sleep(400);
  if (await pingPort(port)) {
    throw new Error(
      `port ${port} already has a browser on it, and it is not ours (no live lock at ${LOCK_FILE}).\n` +
      `  Another agent of this checkout owns it. Pick a free port and a private profile/lock:\n` +
      `      HARNESS_PROFILE=/tmp/<lane>/prof HARNESS_LOCK=/tmp/<lane>/harness.lock \\\n` +
      `      node testing/harness.js restart <free-port>`);
  }
  /* The container states its egress path in the environment; the harness does not invent one. */
  const proxy = process.env.HARNESS_NO_PROXY === "1" ? null
    : (process.env.HARNESS_PROXY || process.env.HTTPS_PROXY || process.env.https_proxy || null);
  log(`launching Chrome on port ${port} …`);
  if (proxy) log(`egress via ${proxy} (TLS capped at 1.2 for the interceptor — see the comment at the flag)`);


  // Detached process group so the launching shell exiting doesn't
  // take Chrome with it. stdio:'ignore' severs pipes; unref() means
  // node can exit without waiting on Chrome.
  let chromePath = process.env.HARNESS_CHROME || await puppeteer.executablePath();
  /* THE SANDBOX STAYS ON, AND ROOT IS THE THING THAT GETS FIXED — not the sandbox.
     This block used to add `--no-sandbox` whenever the process was root, with the argument that a container
     with no other user is where the harness runs unattended. That argument is wrong for THIS harness in a way
     it would be merely sloppy for another: the pages it drives are the attacker-controlled documents the whole
     tool exists to run, and the PoCs it fires are chosen for being executable. Turning off the one boundary
     between that content and the machine, to avoid making a user account, is the security-shaped version of
     the defensive fallback CLAUDE.md bans everywhere else.
     Chrome refuses to run as root with the sandbox on, so the answer is to not be root: `spawn` takes uid/gid,
     and an unprivileged account costs one `useradd`. Measured here, both instances live at once — the renderer
     of a dropped-privilege run has `Seccomp: 2` (filter), `NoNewPrivs: 1` and its own user namespace, while a
     `--no-sandbox` renderer has `Seccomp: 0`, `NoNewPrivs: 0` and init's namespace. That is the difference the
     flag was quietly making. */
  const dropTo = resolveUnprivileged(chromePath);
  if (dropTo && dropTo.chromePath) chromePath = dropTo.chromePath;
  const chromeArgs = [
    `--disable-extensions-except=${EXT_DIR}`,
    `--load-extension=${EXT_DIR}`,
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${PROFILE_DIR}`,
    "--no-first-run",
    "--no-default-browser-check",
    /* THE ESCAPE HATCH IS EXPLICIT AND LOUD, never inferred. An operator who genuinely cannot drop privileges
       says so by setting HARNESS_ALLOW_NO_SANDBOX=1, and every run then prints what was given up. What is gone
       is the SILENT version, where being root was itself taken as consent. */
    ...(dropTo ? [] : ["--no-sandbox"]),
    /* AND IT REFUSES TO RUN AT ALL WITH NO DISPLAY, which is the other environment fact and was the one that
       stopped this harness being runnable here. Chrome's own words, captured by running it by hand:
         ERROR:ui/ozone/platform/x11/ozone_platform_x11.cc:257] Missing X server or $DISPLAY
         ERROR:ui/aura/env.cc:246] The platform failed to initialize.  Exiting.
       so the debug port never opened and every command below it was unreachable. `--headless=new` is the one
       that matters rather than plain `--headless`: the OLD headless loaded no extensions, which for THIS
       harness is the whole subject. Verified by hand before landing — new headless serves /json/version and
       lists `chrome-extension://<our id>/background.js` and `ast-worker.html` as live targets.
       It is keyed on the DISPLAY being absent, not on a flag, for the same reason `--no-sandbox` is keyed on
       being root: a developer with a screen keeps their window, and nobody has to remember a switch. */
    ...(process.platform !== "darwin" && process.platform !== "win32" &&
        !process.env.DISPLAY && !process.env.WAYLAND_DISPLAY ? ["--headless=new"] : []),
    /* EGRESS THROUGH AN INTERCEPTING PROXY, and the one flag that makes it work, with the reason measured
       rather than guessed. In a sandboxed agent container all traffic goes through a local CONNECT proxy that
       terminates TLS. Chrome could not load ANY https origin through it — `ERR_CONNECTION_RESET` direct,
       `ERR_SSL_PROTOCOL_ERROR` through a relay — while `curl` through the same proxy, as the same user, got
       200. Narrowed by elimination, each step measured: not the sandbox (root + --no-sandbox reset
       identically), not the User-Agent (curl carrying Chrome's UA still got 200), not the CONNECT request
       (Chrome's exact CONNECT bytes replayed to the proxy answered `HTTP/1.1 200 Connection Established`),
       not QUIC. It is the ClientHello: Chrome 148 offers a post-quantum key share, which makes the hello
       span records that this interceptor drops. `--disable-features=PostQuantumKyber` no longer names it in
       148, so the lever that works is capping the version.
       WHAT THIS COSTS, precisely: the capped leg is Chrome to a LOOPBACK process that is already terminating
       TLS and reading everything in the clear. The leg that crosses the network is proxy-to-origin, which the
       proxy negotiates itself and this flag does not touch. So it is not a weakening of transport security to
       any real peer — but it IS a fidelity difference a page can observe, so it is applied ONLY when a proxy
       is configured, and never on a developer machine with direct egress.
       Set HARNESS_NO_PROXY=1 to opt out and see the failure for yourself. */
    /* Loopback keeps Chrome's DEFAULT bypass: the fixture server and the extension's own pages are local,
       and routing them through the interceptor would break the very documents under test. An explicit
       `<-loopback>` here would REMOVE that implicit rule, which is the opposite of what is wanted. */
    ...(proxy ? [`--proxy-server=${proxy}`, "--disable-quic", "--ssl-version-max=tls1.2"] : []),
  ];
  /* CHROME'S OWN EXPLANATION IS KEPT, because `stdio: "ignore"` threw it away and left this function able to
     say only "port isn't responding" — a report that names the symptom and discards the cause, which is the
     defect this project keeps finding in its own instruments. The two errors above were sitting in that
     discarded stream the whole time. Detached still works: the child gets an fd, not a pipe to this process,
     so nothing keeps node alive and nothing blocks when the buffer would have filled. */
  /* DOT-PREFIXED, because .gitignore's `testing/.*.out` rule exists for exactly this and its own comment says
     so — "a run's captured stdout". The first spelling of this line put the file at `testing/harness-chrome.log`,
     which is tracked ground: it showed up as untracked in every `git status` in a shared checkout that several
     agents read, which is noise they would each have had to dismiss. Reuse the rule rather than add a second
     one to keep in step with it. */
  const chromeLog = path.join(path.dirname(PROFILE_DIR), ".harness-chrome.out");
  let chromeFd = "ignore";
  try { chromeFd = fs.openSync(chromeLog, "w"); } catch { /* a read-only dir is not a reason to refuse to launch */ }
  const chromeProc = spawn(chromePath, chromeArgs, {
    detached: true,
    stdio: ["ignore", chromeFd, chromeFd],
    windowsHide: false,
    ...(dropTo ? { uid: dropTo.uid, gid: dropTo.gid } : {}),
  });
  chromeProc.unref();
  if (!chromeProc.pid) { log("failed to spawn Chrome"); process.exit(1); }

  /* WAIT FOR THE DEBUG PORT, AND THE VERDICT IS THE PROCESS — NOT A CLOCK. This waited 20 seconds and gave
     up, which is a wall-clock kill in §Testing's sense: a COLD profile (first launch of a lane, a fresh
     `--load-extension` to verify) legitimately takes longer than a warm one, and on a loaded box longer
     still, so the number decided the outcome. Measured: the first launch of a new profile exceeded it, and
     the port answered a few seconds after the harness had already declared failure. What the harness actually
     wants to know is whether Chrome is STILL COMING UP, and the process itself answers that — so the loop
     runs while the process is alive, and the elapsed cap below is only a BACKSTOP for a Chrome that is alive
     and will never listen. The two report through DIFFERENT messages so they can never collapse into one
     verdict, and the backstop's number is deliberately generous.
     AND A FAILED LAUNCH KILLS WHAT IT STARTED. Exiting here without doing so is what leaked the browser this
     command owns: the lock is not written, so no later `restart` can find that process, and the next launch
     fails on a profile it still holds — a cascade whose first symptom is a measurement of a browser nobody
     started. */
  let ready = false, exited = false;
  chromeProc.on("exit", () => { exited = true; });
  const BACKSTOP_MS = 180000;
  for (let t0 = Date.now(); Date.now() - t0 < BACKSTOP_MS; ) {
    await sleep(200);
    if (await pingPort(port)) { ready = true; break; }
    if (exited) break;
    /* The child is detached and unref'd, so node's own `exit` event is not the whole story once this process
       stops being its parent's waiter — signal 0 asks the kernel directly. */
    try { process.kill(chromeProc.pid, 0); } catch { exited = true; break; }
  }
  if (!ready) {
    if (exited) log(`Chrome (pid ${chromeProc.pid}) EXITED before opening port ${port}`);
    else {
      log(`Chrome (pid ${chromeProc.pid}) is ALIVE but never opened port ${port} within ${BACKSTOP_MS / 1000}s — killing it`);
      try { process.kill(-chromeProc.pid, "SIGKILL"); } catch { try { process.kill(chromeProc.pid, "SIGKILL"); } catch {} }
    }
    /* THE CAUSE, not just the symptom. Chrome states its own refusal and this is where a reader needs it. */
    try {
      const tail = fs.readFileSync(chromeLog, "utf8").split("\n").filter(Boolean).slice(-12);
      if (tail.length) { log(`Chrome said (${chromeLog}):`); for (const l of tail) log(`  ${l}`); }
      else log(`Chrome wrote nothing to ${chromeLog} — it died before it could explain, or never exec'd`);
    } catch { log(`could not read ${chromeLog}`); }
    process.exit(1);
  }

  // Deterministic extension ID derived from the unpacked path — same
  // algorithm Chrome uses, so we know up-front which chrome-extension://
  // ID belongs to OUR extension. Avoids the "first chrome-extension
  // target wins" bug where Chrome's own welcome / built-in extension
  // targets could be picked instead.
  const extId = computeExtensionId(EXT_DIR);

  /* Checked AFTER the port answers, because renderers do not exist until Chrome has something to render, and
     a check run before them would pass by finding nothing — which is the excluded-test shape. */
  assertRendererSandboxed(!!dropTo || (typeof process.getuid === "function" && process.getuid() !== 0),
                          chromeProc.pid);

  await writeLock({ port, pid: chromeProc.pid, extId, startedAt: Date.now() });
  log(`started. chrome pid=${chromeProc.pid} port=${port} extId=${extId || "(unknown)"}`);
}

async function cmdPage(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: page <js-expression>");
  // Wrap in `return (...)` UNLESS the expr STARTS with a statement keyword — so
  // expressions that merely CONTAIN `{`/`;`/`return` inside nested functions (an
  // IIFE, `.map(fn).join()`, `JSON.stringify([...].map(...))`) still return their
  // value. The old `contains {/;/return` test ran those with no return -> undefined.
  const body = /^\s*(return|var|let|const|if|for|while|function|throw|do|switch)\b/.test(expr) ? expr : "return (" + expr + ");";
  await withBrowser(async (browser) => {
    const page = await getActivePage(browser);
    const out = await page.evaluate(new Function("return (async () => { " + body + " })()"));
    log(JSON.stringify(out, null, 2));
  });
}

async function cmdPopup(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: popup <js-expression>");
  // Wrap in `return (...)` UNLESS the expr STARTS with a statement keyword — so
  // expressions that merely CONTAIN `{`/`;`/`return` inside nested functions (an
  // IIFE, `.map(fn).join()`, `JSON.stringify([...].map(...))`) still return their
  // value. The old `contains {/;/return` test ran those with no return -> undefined.
  const body = /^\s*(return|var|let|const|if|for|while|function|throw|do|switch)\b/.test(expr) ? expr : "return (" + expr + ");";
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
    // OFFENSIVE: a dead/404 fixture server must CRASH here, never silently load a Chrome error page that
    // then "analyses" as a passing run (the exact false-confidence that hid a down server for a whole
    // session). page.goto returns the main navigation Response; a null response (net error) or a non-OK
    // status is an unexpected state — fail hard so the broken server is seen, not papered over.
    const resp = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
    if (!resp) throw new Error("goto " + url + " returned NO response (server unreachable / net error) — fix the fixture server before testing");
    /* 304 IS A DELIVERED DOCUMENT, and `resp.ok()` says otherwise. Puppeteer's `ok()` is 200-299, so a
       conditional request the browser satisfied FROM CACHE — which is what a real site serves on the second
       visit within one profile — was refused here as "the target server is broken". That is the mirror of
       the failure this guard was written for: the guard exists so a Chrome error page is never analysed as a
       passing run, and it had started rejecting a page that loaded perfectly. The fixture era hid it, because
       a local fixture server sends no validators and never answers 304.
       The test is therefore "did a document arrive", not "is the status in the 2xx window". Redirects do not
       appear here at all — puppeteer follows them and reports the FINAL response — so the honest set is 2xx
       plus 304, and everything else still fails loud. */
    const okStatus = resp.ok() || resp.status() === 304;
    if (!okStatus) throw new Error("goto " + url + " -> HTTP " + resp.status() + " (not a delivered document) — the target/fixture server is broken; refusing to analyse an error page");
    await page.bringToFront();
    log("navigated to " + page.url() + " [" + resp.status() + "]");
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
          // A dedicated-worker target's console is reached via t.worker(), NOT t.page(), and t.page()
          // silently missed it. This branch names no particular worker and must not: it used to name
          // `ast-thread.js`, a file not on disk that ast-worker.html's `default-src 'none'` (no
          // worker-src) forbids that document from ever starting — so the comment described the one
          // target this branch can never see, on the command whose whole job is to list what is there.
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
    try { fs.writeFileSync(path.join(ROOT, "engine", ".work", "diag-full.log"), logs.join("\n")); log("(full " + logs.length + " lines -> engine/.work/diag-full.log)"); } catch (e) { log("diag-full write err: " + e.message); }
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
  // Wrap in `return (...)` UNLESS the expr STARTS with a statement keyword — so
  // expressions that merely CONTAIN `{`/`;`/`return` inside nested functions (an
  // IIFE, `.map(fn).join()`, `JSON.stringify([...].map(...))`) still return their
  // value. The old `contains {/;/return` test ran those with no return -> undefined.
  const body = /^\s*(return|var|let|const|if|for|while|function|throw|do|switch)\b/.test(expr) ? expr : "return (" + expr + ");";
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

/* `dumpscripts` DELETED with lib/analyze.js. It dumped `_scriptBuffers[].scripts` — the per-document
   list of script bodies the brain used to concatenate. The engine has sourced every script itself for a
   long time (Lexbor parses CONTENT_HTML; qjs_run_doc_scripts runs inline + external in document order),
   so the CONTENT_HTML handler sets that array to `[]` and nothing pushes to it: the command skipped
   every buffer as empty and answered "no live buffer" for every page there has ever been. A diagnostic
   that reads a collection its producer stopped filling reports an absence as a finding.
   AND THE LINE THAT CLOSED IT — "the engine-side equivalent that still has a producer is `dumpbundle`" —
   NAMED A COMMAND WITH NO PRODUCER EITHER, which is the same defect standing one command over from the
   note written to record it. `dumpbundle` and `srcloc` both opened IndexedDB `feDeepDB`; this extension
   opens `uasr_store` (lib/persistence.js) and `apiclient-frontier` (bridge.js) and no third database, so
   `indexedDB.open` CREATED an empty one, found no `code` store, and reported that as its answer — a
   diagnostic whose only output is a description of the database it just made.
   SIX MORE WENT WITH THEM, FOR ONE ROOT: `worker`, `profile`, `reachgap`, `learnstate` and `triage` all
   searched `offscreenPage.workers()` for `ast-thread.js`, and ast-worker.html's own policy forbids the
   search from ever succeeding — `default-src 'none'` with no `worker-src`, and the document says in
   words that nothing there calls `new Worker`. That is worse than a stale pointer for the three that
   returned a VERDICT: `learnstate` answered "is a finding MISSING or still being LOOKED FOR", `triage`
   answered HANG-vs-SPIN-vs-DONE, and `reachgap` answered "where did the fetch paths die" — each
   composed out of `self._whyRecords || []` and `self._learningState`, names no realm in this extension
   writes, so each `|| []` was a zero standing where the instrument's own absence belonged. The one that
   was READ downstream is rebuilt from the producer that does exist: see `netdiff`'s run census. */

// Evaluate an expression in the OFFSCREEN document (the learning brain lives
// there now, not the SW) — for DIAGNOSING a stalled learning pipeline (script
// buffers, swFetch results, analysis/resolver errors). Same role cmdSweval had
// for the old SW brain; results verification still goes through the popup UI.
async function cmdOffscreen(args) {
  const expr = args.join(" ");
  if (!expr) throw new Error("usage: offscreen <js-expression>");
  // Wrap in `return (...)` UNLESS the expr STARTS with a statement keyword — so
  // expressions that merely CONTAIN `{`/`;`/`return` inside nested functions (an
  // IIFE, `.map(fn).join()`, `JSON.stringify([...].map(...))`) still return their
  // value. The old `contains {/;/return` test ran those with no return -> undefined.
  const body = /^\s*(return|var|let|const|if|for|while|function|throw|do|switch)\b/.test(expr) ? expr : "return (" + expr + ");";
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
/* CAVEAT: a request absent from the learned set is only a definitive gap for a document whose run has
   RETURNED — until then it may still be learned, so the absence is the analysis being unfinished rather
   than a coverage hole. This caveat used to name `learnstate` and claim "the command prints the current
   learnstate alongside so the reader judges", and BOTH halves were false: the command printed no such
   thing, and `learnstate` read `self._learningState` off a worker ast-worker.html's own CSP forbids. So
   the one qualifier on the moat's headline diagnostic sent its reader to a verdict nobody can compute,
   at exactly the seam where "analysed and clean" and "never ran" are told apart. The census below is
   that qualifier answered from the producer that does write it — see `runs`. */
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
      // it's correctly not learned, and this diagnostic ASKS THE ANALYZER rather than
      // deciding for itself. Every captured HTTP response is classified in
      // lib/response-decode.js by `classifyResponseAsset` (lib/discovery.js, magic
      // bytes first and the declared type only as a weaker cross-check) and stamped on
      // the log entry as that classifier's LABEL, so non-null is the answer.
      // WHAT STOOD HERE WAS A SECOND CLASSIFIER, and it was reached on every entry:
      // it tested `r._assetKind`, a field whose writer had already been deleted, and
      // then fell through to its own content-type table — a declared-type guess, made
      // where the real answer needs the bytes, disagreeing with the one classifier
      // about exactly the mislabelled responses that classifier exists for. The
      // fallback is gone with the dead read; an entry with no classification is now
      // KEPT, which is the conservative direction this comment already asked for.
      const isAsset = (r) => !!r._assetReason;
      const learned = new Set(), learnedMap = new Map();
      /* THE KEY IS THE RECORD'S OWN ADDRESS, AND THREE SUBSTITUTES USED TO STAND IN IT. This diagnostic's
         whole subject is whether an address the solver learned matches one the page issued, so a field a
         consumer answers for is a field that decides an endpoint's IDENTITY — the concealment defect landing
         on the one number CLAUDE.md names (`netdiff --unused`). `e.method || "GET"` filed a verb-less record
         under GET, where it could match a live GET and be counted as covered. `e.path || e.url` was worse
         than a default: `path` is legitimately "" for a shape-origin address with no literal remainder
         (lib/callsite-url.js), and that file states the invariant this line is built on — "`host + path`
         reconstructs the address in both cases" — so falling through to `url` wrote the origin TWICE
         (`https://{origin}` + `{origin}`) and keyed that endpoint as one nothing can ever match. The trailing
         `|| ""` answered for a `url` that is written on every record. All three are gone and the record is
         asserted instead, at the door where it arrives. */
      if (typeof globalStore !== "undefined") {
        for (const e of globalStore.endpoints.values()) {
          checkEndpointRecord(e, "harness netdiff reading the cumulative store");
          const k = e.method + " " + norm((e.host ? "https://" + e.host : "") + e.path);
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
      // The request log is now GLOBAL (globalRequestLog), each entry tagged with
      // {tab,doc,frame} — the documentId re-key replaced the per-tab requestLog
      // (main 13205a4). Diff the live calls that FIRED against the learned set.
      if (typeof globalRequestLog !== "undefined" && Array.isArray(globalRequestLog)) {
        const _byTab = new Map();
        for (const r of globalRequestLog) {
          // url/method/service/callStack are asserted on every record `_pushGlobalLog` files, so the scheme
          // test is the only real question here — is this an HTTP round trip we can compare against a
          // learned endpoint — and the `|| "GET"` / `|| ""` copies were answers to a settled one.
          if (!/^https?:/.test(r.url)) continue;
          const k = r.method + " " + norm(r.url);
          if (seen.has(k)) continue; seen.add(k);
          if (learned.has(k) || matchedByTemplate(k)) continue;
          if (!showAssets && isAsset(r)) { assetFiltered++; continue; }
          // The bundle call-site that fired this live request (intercept.js captured
          // new Error().stack, wrapper frames stripped). A gap's FIRST frame IS the
          // function forced exec failed to drive — the actionable oracle signal.
          const site = r.callStack.split("\n").map((s) => s.trim()).filter(Boolean)[0] || "";
          gaps.push({ k, status: r.status, svc: r.service, site });
          /* `r.tab` HAS NO WRITER ANYWHERE, so this read always fell through to `r.tabId` and the first arm
             was a name nothing on this record has ever carried — concealed by a ternary rather than by a
             `||`, which is the same defect with a longer spelling. `tabId` is `number | null`, written on
             every record, and null is a capture with no tab to attribute it to. */
          const _t = String(r.tabId != null ? r.tabId : "?");
          _byTab.set(_t, (_byTab.get(_t) || 0) + 1);
        }
        for (const [t, n] of _byTab) byTab.push({ tab: t, gaps: n });
      }
      // Reached-but-opaque host edges: a fetch/XHR forced exec DID reach but
      // whose URL/method didn't resolve to a concrete string (an opaque
      // component reached the sink). On a 0-endpoint page this is THE deciding
      // signal — "reached but went opaque" (a driving/resolver gap to CLOSE) vs
      // "never reached" (a coverage gap). From each document's live _resolverErrors.
      const _reachedSet = new Set();
      const _moduleLinkSet = new Set();   // ESM "could not load module '/x'" — a DISCOVERY/LINK gap, not endpoint opacity
      // The per-document live state is the ONLY source now. It always was the richer one — the deep
      // orphan-residue drive surfaces opacity the first pass didn't (firebase's auth-instance config goes
      // opaque only once the init chain is force-driven) and lands here — and the replay cache that used to
      // be read alongside it is deleted, so there is no second, staler copy to reconcile against.
      /* WHICH RUNS THESE NUMBERS ARE MADE OF. Every count below — learned, live, gaps, unused — is a
         count over documents, and a document that has not returned a run contributes to the learned set
         whatever it had reached when this command looked. Without saying how many those are, a gap list
         and a finished analysis are the same JSON, which is the exact pair §Testing forbids averaging:
         "an absent count and a zero count are DIFFERENT facts".
         `_astRun` IS THE PRODUCER'S OWN WORD FOR IT, written once per document at the terminal return
         (offscreen-brain.js) out of the analysis record's `_run`, and DCHECKed there to be one of
         complete / crashed / nothing-to-run. Its ABSENCE is the fourth state and the one this command
         most needs, so it is counted as a state rather than defaulted into any of the three. An outcome
         outside that vocabulary is the producer having changed under this reader, and it says so here —
         this evaluate runs in ast-worker.html, which loads check.js first, so the assertion is the same
         mechanism the zone asserts with everywhere else. */
      const runs = { complete: 0, crashed: 0, "nothing-to-run": 0, "not-returned": 0 };
      if (typeof state !== "undefined" && state.docs) {
        for (const t of state.docs.values()) {
          const outcome = t ? t._astRun : undefined;
          if (outcome === undefined) runs["not-returned"]++;
          else {
            DCHECK(Object.prototype.hasOwnProperty.call(runs, outcome),
                   "a document carries the run outcome `" + outcome + "`, which is not one of the three " +
                   "offscreen-brain.js writes (complete / crashed / nothing-to-run) — netdiff's whole " +
                   "caveat is a census of that vocabulary, so a word it cannot name would be counted as " +
                   "a finished run and every gap below reported as definitive");
            runs[outcome]++;
          }
          /* AN ABSENT `_resolverErrors` IS "THE ENGINE RECORDED NO PAGE ERROR", not a document to skip.
             The `Array.isArray(re)` that stood alone here answered the same for that absence and for a
             present value of the wrong shape, and lib/serialize.js — the other reader of this field —
             already splits the two and DCHECKs the second. Split them the same way, so the reached-but-
             opaque count means the engine reported none rather than this reader having looked away. */
          const re = t ? t._resolverErrors : undefined;
          if (re === undefined) continue;
          DCHECK(Array.isArray(re),
                 "a document's _resolverErrors is present but is not an array — offscreen-brain.js pushes " +
                 "{context,message} rows onto it and nothing else creates the field, so a non-array is " +
                 "that writer broken and the reached-but-opaque count below is made of nothing");
          for (const r of re) {
            const msg = String((r && r.message) || JSON.stringify(r));
            if (msg.indexOf("could not load module") >= 0) {
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
        // THE MOAT here) — live in each document's _astResults[].fetchCallSites[].params
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
        if (typeof state !== "undefined" && state.docs) {
          for (const t of state.docs.values()) {
            for (const an of (t && t._astResults) || []) {
              const fcs = an && an.fetchCallSites;
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
        }
        // ALSO harvest per-field keys+values from the VDD (discoveryDocs). A
        // deep-grind-driven endpoint (an un-fired SDK method) has no fetchCallSite
        // at all, so its learned keys+values live ONLY in the VDD
        // (resources.learned.methods[].parameters + request $ref -> schemas) — e.g.
        // supabase's whole logged-out admin surface. Without this, --unused shows
        // bare URLs for exactly the moat's deep-grind wins though the values WERE
        // computed. The _fcsParams Map dedups, so the call sites + VDD merge cleanly.
        const _harvestVdd = (ddMap) => {
          if (!ddMap || typeof ddMap.values !== "function") return;
          for (const dd of ddMap.values()) {
            const doc = dd && dd.doc;
            const methods = doc && doc.resources && doc.resources.learned && doc.resources.learned.methods;
            if (!methods) continue;
            for (const mid in methods) {
              const m = methods[mid];
              if (!m || !m.httpMethod) continue;
              const pk = _paramKey(m.httpMethod, m.path || mid);
              let pm = _fcsParams.get(pk); if (!pm) { pm = new Map(); _fcsParams.set(pk, pm); }
              const addVals = (pe, o) => { const vv = o && (o._astValidValues || (o._exampleValue != null ? [o._exampleValue] : null)); if (Array.isArray(vv)) for (const v of vv) if (v != null && v !== "") pe.vals.add(v); };
              const params = m.parameters || {};
              for (const pn in params) { const p = params[pn]; let pe = pm.get(pn); if (!pe) { pe = { loc: (p && p.location) || "query", vals: new Set() }; pm.set(pn, pe); } addVals(pe, p); }
              let bodyProps = null; const req = m.request;
              if (req && req.$ref && doc.schemas && doc.schemas[req.$ref]) bodyProps = doc.schemas[req.$ref].properties;
              else if (req && req.properties) bodyProps = req.properties;
              if (bodyProps) for (const bk in bodyProps) { let pe = pm.get(bk); if (!pe) { pe = { loc: "body", vals: new Set() }; pm.set(bk, pe); } addVals(pe, bodyProps[bk]); }
            }
          }
        };
        if (typeof globalStore !== "undefined") _harvestVdd(globalStore.discoveryDocs);
        if (typeof state !== "undefined" && state.docs) for (const t of state.docs.values()) if (t) _harvestVdd(t.discoveryDocs);
        /* `e` IS AN ENDPOINT RECORD (every caller passes one out of `learnedMap`), asserted where it entered
           the loop above. `_paramKey` re-derives a pathname from whatever address it is handed, and `path` IS
           that pathname already — so `|| e.url || ""` was two substitutes for fields the record always
           carries, sitting on the key that decides which learned parameters get printed beside an endpoint. */
        const _fmtParams = (e) => {
          const pm = _fcsParams.get(_paramKey(e.method, e.path));
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
          /* `source` IS STATED ON EVERY RECORD (lib/endpoint-record.js asserts it is one of the two arms), so
             the `e.source &&` that guarded this test could only ever have admitted a record that had lost the
             field — and admitted it as a LITERAL-ORIGIN endpoint, which is the arm this list is for. A
             shape-origin address printed as an unused endpoint is an address nobody can fetch. */
          if (e.source !== "ast_analysis") continue;
          unusedList.push(k + _fmtParams(e));
        }
        // METRIC HONESTY: the same endpoint reached via opaque-POSITIONAL args ({arg0}) and NAMED args ({id})
        // is stored under two keys and double-counted. Collapse path-param placeholders ({...} / %7B..%7D) to a
        // canonical form so a placeholder-insensitive DISTINCT count is visible next to the raw count. Only the
        // {placeholder} SEGMENT normalizes — genuinely-distinct endpoints differ in non-placeholder segments,
        // so this cannot merge real endpoints (e.g. /users/{}/posts stays distinct from /users/{}/comments).
        const _normKey = (s) => s.replace(/%7B[^%]*?%7D/gi, "%7B%7D").replace(/\{[^}]*\}/g, "{}");
        const _unusedDistinct = new Set(unusedList.map((u) => _normKey(u.split("  [")[0]))).size;
        return { mode: "unused = learned-but-not-live (the unused API surface forced exec found — THE VALUE)",
                 runs: runs,
                 learnedCount: learnedMap.size, liveDistinct: seen.size, unusedCount: unusedList.length,
                 unusedDistinct: _unusedDistinct,   // placeholder-normalized (arg0/id collapsed) — the HONEST distinct-endpoint count
                 reachedButOpaque: _reached.length, reachedSamples: _reached.slice(0, 10),
                 moduleLinkFailures: _moduleLink.length, moduleLinkSamples: _moduleLink.slice(0, 10),
                 unused: unusedList.slice(0, 100) };
      }
      return { runs: runs,
               learnedCount: learned.size, liveDistinct: seen.size, gapCount: gaps.length,
               reachedButOpaque: _reached.length, reachedSamples: _reached.slice(0, 10),
               moduleLinkFailures: _moduleLink.length, moduleLinkSamples: _moduleLink.slice(0, 10),
               assetFiltered: assetFiltered + (showAssets ? " (shown; --assets)" : " (static-asset GETs excluded; --assets to show)"),
               gaps: gaps.slice(0, 60).map((g) => g.k + (g.status ? " [" + g.status + "]" : "") + (g.site ? "  ← " + g.site : "  ← (no stack)")) };
    }, { all, showAssets, unused }).catch((e) => ({ error: String(e && e.message || e) }));
    log(JSON.stringify(out, null, 2));
    /* THE NOTE IS DERIVED FROM THE CENSUS PRINTED DIRECTLY ABOVE IT, so a reader can check it against
       the same output instead of against a command that answers nothing. `runs` is built by the block
       in the evaluate and always carries all four keys, so it is read as written and never defaulted;
       a run that threw returns `{error}` instead and carries no census, which is a different fact and
       is said as one rather than being reported as zero documents. */
    if (out.error) { log("NOTE: this census did not run — the error above is the whole result, and no statement about gaps follows from it."); return; }
    log(out.runs["not-returned"]
      ? "NOTE: " + out.runs["not-returned"] + " document(s) have not returned a run, so an endpoint missing " +
        "from the learned set is NOT yet a gap for those — re-check once every document reports " +
        "complete / crashed / nothing-to-run."
      : "NOTE: every document has returned. A missing endpoint is a real gap for the " + out.runs.complete +
        " complete run(s); for the " + out.runs.crashed + " crashed one(s) it is unknowable, because a crash " +
        "invalidates the RUN and not the observations it had already made.");
  });
}


const CMDS = { start: cmdStart, restart: cmdRestart, "restart-keep": (a) => cmdRestart(a, true), page: cmdPage, popup: cmdPopup, pocrun: cmdPocRun, goto: cmdGoto, diag: cmdDiag, sweval: cmdSweval, offscreen: cmdOffscreen, capture: cmdCapture, multitab: cmdMultiTab, netdiff: cmdNetDiff };

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
