// The scheduler's own order census, SAMPLED ACROSS A LIVE PAGE'S WINDOW.
//
// WHY IT IS A SERIES AND NOT A READING. `solver/result.c` composes `_wfq` and states which of its
// rows may be differenced: `picksLifetime`, `starvedPicks`, `topForgiven`, `workDone` and the scan
// counters are LIFETIME COUNTERS; `picksLive`, `picksMax`, `neverPicked`, `neverPickedAtTop` and
// every `svc*` notch are GAUGES over the members standing NOW and CAN FALL between two samples. A
// single census therefore cannot answer the question anybody asks of it — "is the tail being
// reached" — because that is a claim about a trajectory. bridge.js's `streamPartial` rewrites the
// run's row every 750 ms, so the shipped path already produces the series; nothing here computes it.
//
// SO THIS DRIVER POLLS AND PRINTS ROWS, AND DOES NO ARITHMETIC ON THEM. It deliberately does not
// emit `starvedPicks / picksLifetime`, which is the one number a reader wants: result.c says that
// fraction is read "never raw", and it sums re-dispatches that CONTINUE a framed program — which are
// necessary work — with those that pass over a never-run member, which are the defect. A quotient of
// both is a reading of neither, so printing it here would manufacture the authority the contract
// withholds. The columns are printed side by side and the reader does the division knowing what is
// in it.
//
// WHAT IT CHECKS RATHER THAN ASSUMES. result.c states one identity that is checkable from outside:
// `picksLifetime` must EQUAL `_switches` on the same document, because flow_credit_pick has one
// caller and engine.c raises the switch count beside it. That is printed as `switchDelta` on every
// row — a non-zero value is the census disagreeing with the run record it rides on, which would make
// every other row here a number about nothing.
//
// AND IT SAYS WHICH RUN EACH ROW BELONGS TO. A row is only a sample of the site named on it while
// the scheduler is alive; bridge.js sets `_hostDead` once and never clears it, so the same liveness
// column the other two drivers carry is here too, for the same reason.
//
//   node testing/live-wfq.js <url> [url…]        (one navigation per url, in order)
//
"use strict";

const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");
const { artifactStamp } = require("./artifact_stamp.js");

const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const WINDOW = Number(process.env.LIVE_WFQ_WINDOW_MS || 45000);
const EVERY = Number(process.env.LIVE_WFQ_EVERY_MS || 1500);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* THE ROWS THIS DRIVER PRINTS, and the kind of each, taken from result.c's own statements rather
   than from the names — because the names do not say, and result.c records a wrong relay that was
   caused by exactly that (`svcMax` read as a dispatch count when it is a quotient of thread time by
   one cooperative quantum). Only the COUNTER rows may be differenced across samples. */
/* `arrivals`/`departures` are here and not among the gauges because solver/flow.h states their kind: both
   are lifetime counts with exactly one writer each, so both may be differenced — which is what turns
   `arrivals / picksLifetime` (members minted per dispatch) into a RATE over the window this driver samples
   rather than an average over the session. `members` beside them is a gauge and is not the same question.
   Printed, never divided here, for the reason the header gives about `starvedPicks / picksLifetime`. */
const COUNTERS = ["picksLifetime", "starvedPicks", "workDone", "rankChanges", "topForgiven",
                  "arrivals", "departures"];
const GAUGES = ["members", "unrun", "neverPicked", "neverPickedGap", "neverPickedAtTop",
                "picksLive", "picksMax", "families", "jobsReady", "jobsFramed", "jobsOwed",
                "delivReady", "delivFramed", "delivOwed", "valTop", "valMin", "valMax"];

async function connect() {
  const lock = JSON.parse(fs.readFileSync(LOCK_FILE, "utf8"));
  return { extId: lock.extId,
           browser: await puppeteer.connect({ browserURL: `http://127.0.0.1:${lock.port}`,
                                              defaultViewport: null,
                                              targetFilter: (t) => t.type() !== "browser" }) };
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

/* READ OFF THE RUN RECORD, which is the shipped path: bridge.js asserts `_wfq`'s presence and shape
   on every result document it accepts, and the popup's GET_ENGINE_RUNS reads the same array.
   A row with NO `wfq` at all and a row carrying `{members:0}` are DIFFERENT FACTS and are returned
   as different things — bridge.js's own contract for this field is that a missing census is a broken
   contract while `{members:0}` is an empty frontier, and collapsing them would report a scheduler
   that published nothing as one whose frontier had drained. */
function sample(pg) {
  return pg.evaluate(() => {
    const rows = (self._engineLog || []);
    if (!rows.length) return { NO_ROW: true };
    const r = rows[rows.length - 1];
    let sched = null;
    try { const p = self.rendererPoolProbe(); sched = { alive: p.scheduler.alive }; }
    catch (e) { sched = { PROBE_THREW: String((e && e.message) || e) }; }
    if (!("wfq" in r)) return { run: r.run, NO_WFQ: true, switches: r.switches, sched: sched };
    return { run: r.run, wfq: r.wfq, switches: r.switches, sched: sched };
  });
}

async function main() {
  const urls = process.argv.slice(2);
  if (!urls.length) { console.error("usage: node testing/live-wfq.js <url> [url…]"); process.exit(2); }
  console.log("# artifact " + JSON.stringify(artifactStamp()));
  console.log("# windowMs=" + WINDOW + " everyMs=" + EVERY +
              " — COUNTERS (may be differenced): " + COUNTERS.join(",") +
              " | GAUGES (may FALL; never difference): " + GAUGES.join(","));

  const { browser, extId } = await connect();
  try {
    const pg = await offscreenPage(browser, extId);
    const pages = await browser.pages();
    const nonExt = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                       !p.url().startsWith("devtools://"));
    const page = nonExt.length ? nonExt[nonExt.length - 1] : await browser.newPage();

    for (const url of urls) {
      try { await page.goto("about:blank", { waitUntil: "domcontentloaded", timeout: 15000 }); } catch (e) {}
      const t0 = Date.now();
      let nav;
      try { const r = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
            nav = r ? r.status() : "nav-noop"; }
      catch (e) { nav = "navfail:" + String((e && e.message) || e).split("\n")[0]; }
      console.log("\n═══ " + url + "  nav=" + nav);

      let n = 0;
      while (Date.now() - t0 < WINDOW) {
        await sleep(EVERY);
        const s = await sample(pg);
        n++;
        if (s.NO_ROW) { console.log(JSON.stringify({ n, atMs: Date.now() - t0, NO_ROW: true })); continue; }
        if (s.NO_WFQ) { console.log(JSON.stringify({ n, atMs: Date.now() - t0, run: s.run, NO_WFQ: true })); continue; }
        const w = s.wfq;
        const out = { n, atMs: Date.now() - t0, run: s.run,
                      alive: s.sched && s.sched.alive };
        for (const k of COUNTERS) out[k] = (k in w) ? w[k] : null;   // null = absent, never 0
        for (const k of GAUGES) out[k] = (k in w) ? w[k] : null;
        /* THE IDENTITY result.c STATES, CHECKED RATHER THAN TRUSTED. Non-zero means the census and
           the run record it rides on disagree about how many dispatches happened, and then every
           other row on this line is a number about nothing. */
        out.switchDelta = (typeof w.picksLifetime === "number" && typeof s.switches === "number")
                            ? w.picksLifetime - s.switches : null;
        console.log(JSON.stringify(out));
      }
    }
  } finally { browser.disconnect(); }
}

main().catch((e) => { console.error(e.stack || e.message || e); process.exit(1); });
