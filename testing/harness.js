// Extension quality harness.
//
// Launches a real Chrome with the unpacked extension loaded, drives a list of
// complex popular sites, then dumps extension state per site to a timestamped
// report directory. No extension code is modified — we evaluate against the
// service-worker target to read existing module-level state and serializer
// functions.
//
// Usage:
//   npm run harness                     # all sites
//   npm run harness -- --only github_home,reddit_home
//   npm run harness -- --dwell 15       # seconds per site
//
// Output:
//   testing/reports/<ISO>/
//     run.json            meta + per-site status
//     <site>.json         full state dump for that site
//     <site>.scripts/*    script sources referenced by security findings
//
// Requires:  npm install  (to fetch puppeteer devDep)

"use strict";

const path = require("path");
const fs = require("fs");
const fsp = fs.promises;

const puppeteer = require("puppeteer");
const { DUMP_TAB, DUMP_GLOBAL, FETCH_SCRIPT_SOURCE } = require("./extractors");

const ROOT = path.resolve(__dirname, "..");
const EXT_DIR = path.join(ROOT, "extension");
const PROFILE_DIR = path.join(__dirname, "profile");
const REPORTS_DIR = path.join(__dirname, "reports");
const SITES_PATH = path.join(__dirname, "sites.json");

function parseArgs(argv) {
  const out = { only: null, dwell: 20, nav_timeout: 45 };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--only") out.only = String(argv[++i] || "").split(",").map(s => s.trim()).filter(Boolean);
    else if (a === "--dwell") out.dwell = Number(argv[++i]);
    else if (a === "--nav-timeout") out.nav_timeout = Number(argv[++i]);
    else if (a === "--help" || a === "-h") {
      console.log("usage: node harness.js [--only name1,name2] [--dwell 20] [--nav-timeout 45]");
      process.exit(0);
    }
  }
  return out;
}

function nowStamp() {
  return new Date().toISOString().replace(/[:.]/g, "-");
}

async function mkdirp(p) {
  await fsp.mkdir(p, { recursive: true });
}

// Sleep without relying on node timers/promises on older node.
function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}

// Locate the extension's service-worker target. It may appear a bit after
// launch; poll for up to 20s.
async function findExtensionServiceWorker(browser) {
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    const targets = browser.targets();
    const sw = targets.find(t => t.type() === "service_worker" && t.url().startsWith("chrome-extension://"));
    if (sw) return sw;
    await sleep(200);
  }
  throw new Error("could not find extension service_worker target — is the extension loaded?");
}

async function wakeServiceWorker(browser, extensionId) {
  // Opening an extension page forces the SW to wake if it was dormant.
  const page = await browser.newPage();
  try {
    await page.goto(`chrome-extension://${extensionId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 10000 });
  } catch (_) {
    // popup.html may throw in some load states; ignore
  }
  await sleep(500);
  await page.close();
}

async function evalInWorker(worker, fnBody, arg) {
  // Puppeteer evaluates a string as an expression in the target and returns
  // the resolved value. We IIFE the extractor body with the arg inlined via
  // JSON so it lands inside the SW as a single expression.
  const src = `(async (arg) => { ${fnBody} })(${JSON.stringify(arg ?? null)})`;
  return worker.evaluate(src);
}

async function dumpSite(worker, tabId) {
  const tab = await evalInWorker(worker, DUMP_TAB, tabId);
  const global = await evalInWorker(worker, DUMP_GLOBAL, null);
  return { tab, global };
}

// Poll the SW until AST has finished buffering + analyzing for this tab, or
// we hit the timeout. "Done" = debounce timer cleared AND astResults length
// has been stable for two consecutive polls.
async function waitForASTIdle(worker, tabId, timeoutMs = 30000) {
  const body = `
    const tabId = arg;
    const buf = _scriptBuffers.get(tabId);
    const tab = state.tabs.get(tabId);
    return {
      bufferedScripts: buf ? buf.scripts.length : 0,
      timerPending: buf ? !!buf.timer : false,
      astResults: tab ? (tab._astResults || []).length : 0,
    };
  `;
  const deadline = Date.now() + timeoutMs;
  let last = null;
  let stableTicks = 0;
  while (Date.now() < deadline) {
    const s = await evalInWorker(worker, body, tabId);
    if (!s.timerPending && last && s.astResults === last.astResults && s.bufferedScripts === last.bufferedScripts) {
      stableTicks++;
      if (stableTicks >= 2) return s;
    } else {
      stableTicks = 0;
    }
    last = s;
    await sleep(1000);
  }
  return last;
}

async function triggerLazyLoads(page) {
  try {
    await page.evaluate(async () => {
      const steps = 6;
      for (let i = 0; i < steps; i++) {
        window.scrollBy(0, Math.max(300, Math.floor(window.innerHeight / 2)));
        await new Promise(r => setTimeout(r, 400));
      }
      window.scrollTo(0, 0);
    });
  } catch (_) {
    // navigations during eval throw; fine
  }
}

function uniqueSourceUrls(findings) {
  const set = new Set();
  for (const key of Object.keys(findings || {})) {
    const f = findings[key];
    if (f.sourceUrl) set.add(f.sourceUrl);
  }
  return [...set];
}

async function saveScriptSources(worker, dirPath, sourceUrls) {
  await mkdirp(dirPath);
  const index = [];
  for (const url of sourceUrls) {
    try {
      const res = await evalInWorker(worker, FETCH_SCRIPT_SOURCE, url);
      if (!res || !res.ok) {
        index.push({ url, ok: false, error: res && res.error });
        continue;
      }
      // Filename based on a short hash of the URL to stay fs-safe on Windows.
      const fname = "s_" + Math.abs(hashStr(url)).toString(36) + ".txt";
      await fsp.writeFile(path.join(dirPath, fname), res.text, "utf8");
      index.push({ url, ok: true, file: fname, length: res.length });
    } catch (e) {
      index.push({ url, ok: false, error: String(e) });
    }
  }
  await fsp.writeFile(path.join(dirPath, "index.json"), JSON.stringify(index, null, 2), "utf8");
}

function hashStr(s) {
  let h = 2166136261 >>> 0;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h | 0;
}

async function run() {
  const args = parseArgs(process.argv);
  const allSites = JSON.parse(await fsp.readFile(SITES_PATH, "utf8"));
  const sites = args.only ? allSites.filter(s => args.only.includes(s.name)) : allSites;
  if (!sites.length) {
    console.error("no sites matched --only filter");
    process.exit(2);
  }

  await mkdirp(PROFILE_DIR);
  const runDir = path.join(REPORTS_DIR, nowStamp());
  await mkdirp(runDir);
  console.log("[harness] run dir:", runDir);

  const browser = await puppeteer.launch({
    headless: false,
    userDataDir: PROFILE_DIR,
    defaultViewport: null,
    args: [
      `--disable-extensions-except=${EXT_DIR}`,
      `--load-extension=${EXT_DIR}`,
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-features=Translate,BackForwardCache",
    ],
  });

  const runMeta = { startedAt: new Date().toISOString(), sites: [], errors: [] };

  try {
    const swTarget = await findExtensionServiceWorker(browser);
    const extensionId = new URL(swTarget.url()).hostname;
    console.log("[harness] extension id:", extensionId);
    await wakeServiceWorker(browser, extensionId);
    const worker = await swTarget.worker();
    if (!worker) throw new Error("service worker target has no worker handle");

    for (const site of sites) {
      const t0 = Date.now();
      const report = { name: site.name, url: site.url, startedAt: new Date().toISOString() };
      console.log(`[harness] → ${site.name}  (${site.url})`);

      let page;
      try {
        page = await browser.newPage();
        // Wake SW each loop in case it went dormant.
        await wakeServiceWorker(browser, extensionId).catch(() => {});

        await page.goto(site.url, {
          waitUntil: "domcontentloaded",
          timeout: args.nav_timeout * 1000,
        });

        // Let intercepts and AST buffer up.
        await sleep(Math.max(3, Math.floor(args.dwell / 2)) * 1000);
        await triggerLazyLoads(page);
        await sleep(Math.max(3, Math.floor(args.dwell / 2)) * 1000);

        // The tab id Chrome exposes to extensions ≠ CDP target id. We reach
        // it by asking the SW which tab is hosting the current URL, with a
        // fallback to whichever tab has had the most recent request activity.
        const resolveBody = `
          const baseUrl = arg.split("#")[0];
          for (const [tid, m] of _tabMeta.entries()) {
            if (m && m.url && (m.url === arg || m.url.startsWith(baseUrl))) return tid;
          }
          let best = null, bestTs = 0;
          for (const [tid, tab] of state.tabs.entries()) {
            const last = tab.requestLog.length ? tab.requestLog[tab.requestLog.length - 1].timestamp : 0;
            if (last > bestTs) { bestTs = last; best = tid; }
          }
          return best;
        `;
        const tabId = await evalInWorker(worker, resolveBody, site.url);

        if (tabId == null) {
          report.error = "no extension-side tab id resolved (requests may not have been captured)";
        } else {
          // Give AST analysis time to debounce + process before dumping.
          const astState = await waitForASTIdle(worker, tabId, 45000);
          report.astState = astState;
          const dump = await dumpSite(worker, tabId);
          report.tabId = tabId;
          report.dump = dump;

          const sourceUrls = uniqueSourceUrls(dump.global.findings);
          if (sourceUrls.length) {
            const scriptsDir = path.join(runDir, `${site.name}.scripts`);
            await saveScriptSources(worker, scriptsDir, sourceUrls);
            report.scriptsDir = path.basename(scriptsDir);
          }
        }
      } catch (e) {
        report.error = String(e && e.stack || e);
      } finally {
        try { if (page) await page.close(); } catch (_) {}
      }

      report.elapsedMs = Date.now() - t0;
      await fsp.writeFile(
        path.join(runDir, `${site.name}.json`),
        JSON.stringify(report, null, 2),
        "utf8",
      );
      runMeta.sites.push({
        name: site.name,
        url: site.url,
        elapsedMs: report.elapsedMs,
        ok: !report.error,
        error: report.error || null,
        requests: report.dump?.tab?.requestLog?.length ?? 0,
        services: Object.keys(report.dump?.global?.discovery || {}).length,
        findings: Object.keys(report.dump?.global?.findings || {}).length,
      });
      console.log(
        `[harness]   done ${site.name} — reqs=${runMeta.sites.at(-1).requests} services=${runMeta.sites.at(-1).services} findings=${runMeta.sites.at(-1).findings} (${report.elapsedMs}ms)`,
      );
    }
  } catch (e) {
    runMeta.errors.push(String(e && e.stack || e));
    console.error("[harness] fatal:", e);
  } finally {
    runMeta.finishedAt = new Date().toISOString();
    await fsp.writeFile(path.join(runDir, "run.json"), JSON.stringify(runMeta, null, 2), "utf8");
    await browser.close().catch(() => {});
    console.log(`[harness] done → ${runDir}`);
  }
}

run().catch(err => {
  console.error(err);
  process.exit(1);
});
