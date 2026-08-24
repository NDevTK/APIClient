// Drive a set of live URLs through the already-built extension and report, PER RUN,
// what the SHIPPED PATH WROTE — never what a harness printed.
//
// CLAUDE.md §Testing: "ONE RUN OF A LIVE SITE IS NOT A MEASUREMENT, AND A BEFORE/AFTER
// BUILT FROM TWO OF THEM IS AN ARTIFACT OF THE SITE." So this driver takes a RUN COUNT
// and reports every run's row plus the spread across them; it never averages, and it
// never collapses an ABSENT count into a zero — those are different facts and the two
// are printed with different tokens (`-` vs `0`).
//
// WHAT IT READS is `self._engineLog` in the offscreen document, which is the record
// `bridge.js` writes for every engine run (see `engineLogWrite`). That is the shipped
// path: the popup's GET_ENGINE_RUNS reads the same array. It is NOT a console scrape —
// the renderer deliberately does not tee its stdout, so a console scrape measures the
// instrument rather than the run.
//
// A row whose `run` is "crashed" carries NO counters at all, by bridge.js's own rule,
// and this driver preserves that: it prints `-` for each, because seven zeroes read as
// a run that explored nothing, which is a finding, and a crash is not one.
//
// Completion is polled off that row's own `run` word rather than off a wall clock,
// because §Testing's loaded-machine rule says a wall clock measures how busy the box
// was. The elapsed budget that remains is a BACKSTOP for a run that never reports at
// all, and it reports through a DIFFERENT token (`timeout`) so the two never collapse
// into one verdict.
//
//   node testing/live-run.js <runs> <url> [url…]
//
"use strict";

const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");

const ROOT = path.resolve(__dirname, "..");
const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* THE ARTIFACT IS NAMED IN THE OUTPUT, because §Testing requires the revision a number
   belongs to be reported WITH the result. A number quoted without it is not a
   measurement. This reads the build stamp the builder wrote, never a git command — the
   working tree advances under a live session and the tree's HEAD is not the artifact. */
function artifactStamp() {
  const p = path.join(ROOT, "extension", "lib", "qjs", "qjs.mjs.build.json");
  const j = JSON.parse(fs.readFileSync(p, "utf8"));
  return { head: j.head, qjsHead: j.qjsHead, dirty: j.dirty, at: j.at };
}

async function connect() {
  const lock = JSON.parse(fs.readFileSync(LOCK_FILE, "utf8"));
  return {
    extId: lock.extId,
    browser: await puppeteer.connect({
      browserURL: `http://127.0.0.1:${lock.port}`,
      defaultViewport: null,
      targetFilter: (t) => t.type() !== "browser",
    }),
  };
}

async function offscreenPage(browser, extId) {
  const url = `chrome-extension://${extId}/ast-worker.html`;
  for (let i = 0; i < 60; i++) {
    const t = browser.targets().find((t) => t.url().startsWith(url));
    if (t) { const pg = await t.page().catch(() => null); if (pg) return pg; }
    await sleep(200);
  }
  throw new Error("no offscreen document — is the extension loaded?");
}

// The offscreen's own view of this run. `rows` is bridge.js's per-run log; the store
// sizes are the cumulative moat, which is why the driver diffs them across the run
// rather than reporting the total (a total is every site this browser has ever seen).
function snapshot(pg) {
  return pg.evaluate(() => ({
    rows: (self._engineLog || []).map((r) => Object.assign({}, r)),
    endpoints: typeof globalStore !== "undefined" ? globalStore.endpoints.size : null,
    findings: typeof globalStore !== "undefined" ? globalStore.securityFindings.size : null,
  }));
}

const COUNTERS = ["switches", "flows", "candidates", "jobsQueued", "jobsRun",
                  "worldSegmentsHeld", "worldSegmentsMade", "worldSegmentsForked",
                  "endpoints", "sinks", "park", "resumed"];

async function oneRun(browser, pg, url, budgetMs) {
  const before = await snapshot(pg);
  const baseRows = before.rows.length;

  let page;
  const pages = await browser.pages();
  const nonExt = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                     !p.url().startsWith("devtools://"));
  page = nonExt.length ? nonExt[nonExt.length - 1] : await browser.newPage();

  // A page error the ENGINE never sees is still part of what this run met, so it is
  // collected — but separately from the engine's own pageErrors, which ride the result
  // document. Conflating them would report a site's own console noise as engine output.
  const pageConsole = [];
  const onErr = (e) => pageConsole.push("pageerror: " + String(e && e.message || e));
  page.on("pageerror", onErr);

  let nav = null;
  const t0 = Date.now();
  try {
    const resp = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
    nav = resp ? resp.status() : null;
  } catch (e) {
    nav = "navfail:" + String(e && e.message || e).split("\n")[0];
  }

  // Poll the row this run owns. `partial` is a snapshot of a run still going; only
  // `complete`/`crashed` are terminal, and a run that never reports at all falls out
  // through the backstop below with its own distinct token.
  let terminal = null, last = before;
  while (Date.now() - t0 < budgetMs) {
    await sleep(2000);
    last = await snapshot(pg);
    const mine = last.rows.slice(baseRows);
    if (mine.length && mine.every((r) => r.run === "complete" || r.run === "crashed")) {
      terminal = mine; break;
    }
  }
  page.off("pageerror", onErr);
  const elapsed = Date.now() - t0;
  const mine = (terminal || last.rows.slice(baseRows));

  return {
    url, nav, elapsedMs: elapsed,
    // ABSENT and ZERO are different facts. No row at all is `rows: 0` with a null
    // outcome — a document that was admitted and never provisioned an engine, which is
    // NOT the same as an engine that ran and found nothing.
    verdict: terminal ? "terminal" : (mine.length ? "budget-elapsed(partial)" : "budget-elapsed(no-row)"),
    rows: mine.length,
    outcomes: mine.map((r) => r.run),
    counters: mine.map((r) => {
      const o = { run: r.run };
      for (const k of COUNTERS) o[k] = (k in r) ? r[k] : null;   // null = the crash arm carries none
      return o;
    }),
    storeEndpointsDelta: (last.endpoints === null || before.endpoints === null)
      ? null : last.endpoints - before.endpoints,
    storeFindingsDelta: (last.findings === null || before.findings === null)
      ? null : last.findings - before.findings,
    pageConsole: pageConsole.slice(0, 8),
  };
}

function spread(runs, pick) {
  const vals = runs.map(pick).filter((v) => typeof v === "number");
  if (!vals.length) return "-";                       // absent, not zero
  const lo = Math.min(...vals), hi = Math.max(...vals);
  return lo === hi ? String(lo) : lo + "–" + hi;  // an en dash: a RANGE, never a mean
}

async function main() {
  const runs = parseInt(process.argv[2], 10);
  const urls = process.argv.slice(3);
  if (!runs || !urls.length) {
    console.error("usage: node testing/live-run.js <runs> <url> [url…]");
    process.exit(2);
  }
  const budgetMs = Number(process.env.LIVE_RUN_BUDGET_MS || 90000);
  const stamp = artifactStamp();
  console.log("# artifact " + JSON.stringify(stamp));
  console.log("# runs=" + runs + " budgetMs=" + budgetMs + " (budget is a BACKSTOP, not the verdict)");

  const { browser, extId } = await connect();
  try {
    const pg = await offscreenPage(browser, extId);
    const bySite = new Map();
    for (const url of urls) bySite.set(url, []);
    // Interleave the runs rather than repeating one site N times back to back: a site's
    // own run-to-run drift and a monotone drift in the browser (a growing moat, a warm
    // code cache) are otherwise indistinguishable in the spread.
    for (let i = 0; i < runs; i++) {
      for (const url of urls) {
        const r = await oneRun(browser, pg, url, budgetMs);
        bySite.get(url).push(r);
        console.log(JSON.stringify(Object.assign({ runIndex: i }, r)));
      }
    }
    console.log("\n# ── spread across " + runs + " run(s); a range, never a mean ──");
    for (const [url, rs] of bySite) {
      const first = (pickKey) => (r) => {
        const c = r.counters[0];
        return c && typeof c[pickKey] === "number" ? c[pickKey] : undefined;
      };
      console.log(JSON.stringify({
        url,
        verdicts: rs.map((r) => r.verdict + ":" + (r.outcomes.join("+") || "no-row")),
        endpoints: spread(rs, first("endpoints")),
        sinks: spread(rs, first("sinks")),
        candidates: spread(rs, first("candidates")),
        flows: spread(rs, first("flows")),
        switches: spread(rs, first("switches")),
        jobsQueued: spread(rs, first("jobsQueued")),
        jobsRun: spread(rs, first("jobsRun")),
        park: spread(rs, first("park")),
        storeEndpointsDelta: spread(rs, (r) => r.storeEndpointsDelta),
      }));
    }
  } finally { browser.disconnect(); }
}

main().catch((e) => { console.error(e.stack || e.message || e); process.exit(1); });
