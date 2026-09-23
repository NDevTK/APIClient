// Drive live URLs and capture the ENGINE'S OWN stderr, which is the only place a crash
// names its cause. renderer-host.js `absorbStdio` tees fd 2 to this zone's console live
// (stdout is deliberately not teed — a hot engine's @H traffic would bury it), so the
// offscreen document's console IS the engine's stderr and a puppeteer console listener
// on it is reading the shipped path, not a harness print.
//
// It reports, per navigation, the pool probe's scheduler liveness beside the @WHY —
// because "the scheduler died" and "the scheduler died OF this" are different facts and
// rendererPoolProbe only carries the first as a wasm frame list with no text in it.
//
//   node testing/live-why.js <url> [url…]      (one navigation per url, in order)
//
"use strict";
const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");
const { artifactStamp, absentAbortIsEvidence } = require("./artifact_stamp.js");

const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const BUDGET = Number(process.env.LIVE_WHY_BUDGET_MS || 60000);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// A line worth keeping is one the ENGINE wrote about itself. Everything else on that
// console is this zone's own console.debug traffic, which would bury the cause exactly
// as stdout would.
const KEEP = /@WHY|@E\b|Aborted|abort\(\)|assert|DCHECK|CHECK|Error:|RuntimeError|out of memory|OOM/i;

async function main() {
  const urls = process.argv.slice(2);
  if (!urls.length) { console.error("usage: node testing/live-why.js <url> [url…]"); process.exit(2); }
  /* PRINTED FIRST BECAUSE EVERY @WHY BELOW IS FILED UNDER IT — and read through
     artifact_stamp.js, which REFUSES a stamp older than the bytes beside it. This used to
     be a bare JSON.parse here: it printed three fields as fact, so the one failure that
     cannot be seen in the output (a wasm copied in over an older stamp) was the one this
     tool published. An unreadable stamp is a refusal for the same reason and not an ENOENT. */
  const stamp = artifactStamp();
  console.log("# artifact " + JSON.stringify(stamp));

  const lock = JSON.parse(fs.readFileSync(LOCK_FILE, "utf8"));
  const browser = await puppeteer.connect({
    browserURL: `http://127.0.0.1:${lock.port}`, defaultViewport: null,
    targetFilter: (t) => t.type() !== "browser",
  });
  try {
    const oUrl = `chrome-extension://${lock.extId}/ast-worker.html`;
    let pg = null;
    for (let i = 0; i < 60 && !pg; i++) {
      const t = browser.targets().find((t) => t.url().startsWith(oUrl));
      if (t) pg = await t.page().catch(() => null);
      if (!pg) await sleep(200);
    }
    if (!pg) throw new Error("no offscreen document — is the extension loaded?");

    const eng = [];
    pg.on("console", (m) => { const t = m.text(); if (KEEP.test(t)) eng.push(t); });
    pg.on("pageerror", (e) => eng.push("OFFSCREEN pageerror: " + String(e && e.message || e)));

    const pages = await browser.pages();
    const nonExt = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                       !p.url().startsWith("devtools://"));
    const page = nonExt.length ? nonExt[nonExt.length - 1] : await browser.newPage();

    for (const url of urls) {
      const mark = eng.length;
      let nav;
      try {
        const r = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
        nav = r ? r.status() : "nav-noop";
      } catch (e) { nav = "navfail:" + String(e && e.message || e).split("\n")[0]; }
      await sleep(BUDGET);
      // The probe is read AFTER the budget so a scheduler that died mid-run is seen dead
      // rather than seen alive one poll before it died.
      const probe = await pg.evaluate(() => {
        try {
          const p = self.rendererPoolProbe();
          return {
            alive: p.scheduler.alive, diedOf: p.scheduler.diedOf,
            kicksRefused: p.scheduler.kicksRefused, driving: p.scheduler.driving,
            waiting: p.waiting, runs: p.runs, crashes: p.crashes,
            navigated: p.reservations.navigated, made: p.reservations.made,
            pool: p.pool.map((e) => ({ name: e.name, docId: e.docId, topDocId: e.topDocId,
                                       state: e.state, joined: e.joined, joinedDocIds: e.joinedDocIds })),
            recent: p.recent.slice(-2),
          };
        } catch (e) { return { PROBE_THREW: String(e && e.message || e) }; }
      });
      console.log("\n═══ " + url + "  nav=" + nav);
      console.log(JSON.stringify(probe, null, 1));
      const mine = eng.slice(mark);
      console.log("── engine stderr for this navigation (" + mine.length + " line(s)) ──");
      /* WHAT A ZERO HERE IS WORTH, SAID WHERE THE ZERO IS PRINTED. This tool exists to capture the @WHY, and
         a @WHY is a DCHECK or a DFAIL, both of which check.h compiles OUT at -DAPICLIENT_DEV=0 — so against a
         release artifact a navigation that violated every invariant in the engine prints exactly what a clean
         one prints. That is the absence CLAUDE.md's reachability witness cannot separate: the path WAS taken
         and the check was simply not in the program. The regime is in the `# artifact` line above, and it is
         also SAID HERE, because the header is the line a reader files the run under and this is the line they
         are looking at when they conclude nothing went wrong.
         ONLY THE ZERO IS QUALIFIED. A non-empty list is the tool working whatever the regime, and a paragraph
         under every navigation would be the noise that gets scrolled past on the way to the one that matters. */
      if (!mine.length)
        console.log(absentAbortIsEvidence(stamp)
          ? "  (nothing matched, and this absence is SCORED: " + stamp.assertsAtBuild + ")"
          : "  (nothing matched, and THIS ABSENCE IS NOT EVIDENCE OF A CLEAN RUN: " + stamp.assertsAtBuild +
            ". Every @WHY is a DCHECK or a DFAIL, and the reading above is what says whether this " +
            "program contains them; it does not say yes. So this zero rules out a CHECK (@E, fatal in both " +
            "regimes) and an offscreen page error and NOTHING ELSE. Re-read against a `dev` artifact " +
            "before concluding anything about an invariant.)");
      for (const l of mine.slice(0, 60)) console.log("  " + l);
    }
  } finally { browser.disconnect(); }
}
main().catch((e) => { console.error(e.stack || e.message || e); process.exit(1); });
