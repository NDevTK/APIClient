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
const { artifactStamp } = require("./artifact_stamp.js");

const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* THE ARTIFACT IS NAMED IN THE OUTPUT — see testing/artifact_stamp.js, which is where the refusal that
   makes that name trustworthy lives, and which testing/live-why.js reads through as well. It was a copy
   in each driver and only this one carried the refusal. */

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

/* IS THE ONE LEVEL-1 LOOP STILL ALIVE — a fact no counter in the row above can answer, and the one that
   decides whether a row is a measurement of the site named on it.
   bridge.js sets `_hostDead` ONCE and never clears it, and every document arriving afterwards is answered by
   `_hostKicksRefused++` and nothing else. So the FIRST crash that takes the scheduler down makes every later
   run in that browser produce no engine row at all — and this driver printed those under
   `budget-elapsed(no-row)`, whose own comment says it means "a document that was admitted and never
   provisioned an engine, which is NOT the same as an engine that ran and found nothing". That token was
   therefore carrying a THIRD nothing it does not name: a browser that died in an earlier run and was never
   asked about this site. §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES: an absent count and a zero count are
   different facts, and so are these.
   MEASURED, which is why this column exists: five runs across three app pages, artifact head 82c1a924. Run 0
   crashed all three sites, the third crash killed the scheduler, and the remaining TWELVE rows read
   `budget-elapsed(no-row)` — twelve rows about a dead loop, printed in the same column and the same shape as
   a finding about the engine. The driver takes a RUN COUNT precisely because one run is not a measurement,
   and it was silently returning one sample per site out of five.
   READ THROUGH `rendererPoolProbe` BECAUSE THAT IS THE ONLY SURFACE — `_hostDead`, `_hostDriving` and
   `_hostKicksRefused` are module-scoped bindings in bridge.js, not properties of `self`. The probe runs its
   own DCHECKs, so a throw is reported under its OWN token rather than folded into `alive`: a probe that could
   not answer and a scheduler that is answering are different facts, and defaulting one into the other is the
   exact shape this column was added to stop. */
function scheduler(pg) {
  return pg.evaluate(() => {
    try {
      const p = self.rendererPoolProbe();
      return { alive: p.scheduler.alive, driving: p.scheduler.driving,
               kicksRefused: p.scheduler.kicksRefused, diedOf: p.scheduler.diedOf };
    } catch (e) { return { PROBE_THREW: String((e && e.message) || e) }; }
  });
}

/* WHAT THE RUN COST, AND — the last four — WHAT ITS WORK MET. bridge.js forwards the @S arrival census onto
   every run record for the reason solver/result.c emits it: an empty `securitySinks` array has four readings
   that take opposite actions, and a probe that reports only `sinks: 0` cannot tell "no attacker source was
   ever read" from "taint arrived at a sink and the search was declined as unforgeable". This driver was
   reporting the cost columns and dropping the four that say whether there was anything to find. */
const COUNTERS = ["switches", "flows", "candidates", "jobsQueued", "jobsRun", "unitsDone",
                  "worldSegmentsHeld", "worldSegmentsMade", "worldSegmentsForked",
                  "sourceReads", "sinkReached", "sinkTainted", "sinkSuppressed",
                  /* AND THE ORPHAN PAIR, for the same sentence this list's own comment makes about the four
                     before it: they are the columns that say whether there was anything to find. §What-the-
                     tool-produces makes the drive of never-called code the headline surface, and `driven`
                     alone cannot tell a bundle with no uncalled code from a frontier that never reached the
                     question — `asked` is the one that separates them. */
                  "orphansDriven", "orphansAsked",
                  "endpoints", "sinks", "park", "resumed"];

async function oneRun(browser, pg, url, budgetMs) {
  const before = await snapshot(pg);
  const baseRows = before.rows.length;
  /* READ BEFORE THE NAVIGATION, because "the scheduler was already dead when this URL arrived" is the only
     reading under which this row is not about this URL at all. */
  const schedBefore = await scheduler(pg);

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

  /* EVERY RUN GETS ITS OWN DOCUMENT, AND THE RUN BEFORE IT IS WHY THAT NEEDS SAYING. A `goto` to the
     URL the tab is ALREADY at, when that URL carries a fragment, is a same-document navigation: no
     request, no response, no new Document, so nothing admits an engine — and the run then reports no
     engine row, which is indistinguishable in the table from a document that was admitted and never
     provisioned. That is the silent-zero-run shape, manufactured by the instrument: three runs of one
     fixture read as crash / nothing / nothing, and the two nothings were the browser declining to
     navigate. Going through about:blank first makes the next goto a real cross-document navigation
     whatever the previous run left in the tab. */
  try { await page.goto("about:blank", { waitUntil: "domcontentloaded", timeout: 15000 }); } catch (e) {}
  let nav = null;
  const t0 = Date.now();
  try {
    const resp = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
    /* A NULL RESPONSE IS NOT A STATUS, AND IT IS NOT A RUN. puppeteer returns null when no navigation
       actually happened, so this is reported under its own token rather than folded into the status
       column — an absent document and a document that produced nothing are different facts. */
    nav = resp ? resp.status() : "nav-noop(no document created)";
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
  /* READ AFTER THE POLL LOOP for the reason testing/live-why.js states at its own probe read: a scheduler
     that died mid-run must be seen dead, not seen alive one poll before it died. */
  const schedAfter = await scheduler(pg);
  const elapsed = Date.now() - t0;
  const mine = (terminal || last.rows.slice(baseRows));

  return {
    url, nav, elapsedMs: elapsed,
    // ABSENT and ZERO are different facts. No row at all is `rows: 0` with a null
    // outcome — a document that was admitted and never provisioned an engine, which is
    // NOT the same as an engine that ran and found nothing.
    /* THE VERDICT NAMES WHICH OF THE THREE NOTHINGS THIS IS. A run with no engine row is only a finding
       about the ENGINE when a document actually arrived for it to run on; when the navigation itself
       produced nothing, the run measured the browser, not the engine, and says so. */
    /* THE FOURTH NOTHING IS NAMED FIRST, because it is the one that makes the row not a row about this site:
       a scheduler already dead when this URL arrived was never asked about it, so `budget-elapsed(no-row)`
       would be reporting an engine that was never offered the document as an engine that produced nothing. */
    verdict: schedBefore.alive === false && !mine.length
             ? "scheduler-dead-before-this-run(NOT a sample of this site)"
           : typeof nav !== "number" ? "no-navigation"
           : terminal ? "terminal"
           : (mine.length ? "budget-elapsed(partial)" : "budget-elapsed(no-row)"),
    /* CARRIED ON EVERY ROW, not only the refused ones: a run whose scheduler was alive before and dead after
       is where the death happened, and that is a fact about THIS site. `kicksRefusedDelta` is how many
       documents were answered by nothing while this row was being taken. */
    scheduler: { before: schedBefore, after: schedAfter,
                 kicksRefusedDelta:
                   (typeof schedBefore.kicksRefused === "number" && typeof schedAfter.kicksRefused === "number")
                     ? schedAfter.kicksRefused - schedBefore.kicksRefused : null },
    rows: mine.length,
    outcomes: mine.map((r) => r.run),
    counters: mine.map((r) => {
      const o = { run: r.run };
      for (const k of COUNTERS) o[k] = (k in r) ? r[k] : null;   // null = the crash arm carries none
      /* THE CRASH ARM CARRIES NO COUNTERS AND IT DOES CARRY ITS CAUSE, which is the whole reason a live site
         is worth running: the ROOT @WHY on it names the capability that is missing. Read off the run record
         rather than scraped from a console — the renderer does not tee its stdout, so a console scrape is
         the wrong surface by construction. */
      if (r.run === "crashed") o.err = r.err;
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
    /* SAID ONCE, LOUDLY, AT THE INSTANT THE RUN COUNT STOPS MEANING ANYTHING. A driver that takes a RUN COUNT
       does so because §Testing says one run is not a measurement — so the moment the scheduler dies, every
       remaining run is a refused document and the count in the header is a promise this driver can no longer
       keep. Per-row tokens say it too, but they say it once per row in a stream the reader scrolls; this says
       it where the reader is still deciding what the run means. The run is NOT aborted: the remaining rows are
       what a wedged browser does, which is itself worth seeing, and stopping would hide the shape. */
    let announcedDead = false;
    for (let i = 0; i < runs; i++) {
      for (const url of urls) {
        const r = await oneRun(browser, pg, url, budgetMs);
        bySite.get(url).push(r);
        console.log(JSON.stringify(Object.assign({ runIndex: i }, r)));
        if (!announcedDead && r.scheduler.after.alive === false) {
          announcedDead = true;
          console.log("# ── SCHEDULER DEAD from run " + i + " of " + url + ". bridge.js sets `_hostDead` once " +
                      "and never clears it, so every run after this one is a REFUSED document, not a sample of " +
                      "the site on its row. Restart the browser between runs to get independent samples.");
        }
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
        /* THE DENOMINATOR OF EVERY SPREAD ON THIS LINE. §a-coverage-figure-states-what-it-is-a-fraction-of:
           a range across five runs of which four were refused is a range across ONE, and the reader cannot
           see that from the range. */
        runsThatWereSamples: rs.filter((r) => r.scheduler.before.alive !== false).length + "/" + rs.length,
        verdicts: rs.map((r) => r.verdict + ":" + (r.outcomes.join("+") || "no-row")),
        endpoints: spread(rs, first("endpoints")),
        sinks: spread(rs, first("sinks")),
        candidates: spread(rs, first("candidates")),
        flows: spread(rs, first("flows")),
        switches: spread(rs, first("switches")),
        jobsQueued: spread(rs, first("jobsQueued")),
        jobsRun: spread(rs, first("jobsRun")),
        unitsDone: spread(rs, first("unitsDone")),
        sourceReads: spread(rs, first("sourceReads")),
        sinkReached: spread(rs, first("sinkReached")),
        sinkTainted: spread(rs, first("sinkTainted")),
        sinkSuppressed: spread(rs, first("sinkSuppressed")),
        park: spread(rs, first("park")),
        storeEndpointsDelta: spread(rs, (r) => r.storeEndpointsDelta),
      }));
    }
  } finally { browser.disconnect(); }
}

main().catch((e) => { console.error(e.stack || e.message || e); process.exit(1); });
