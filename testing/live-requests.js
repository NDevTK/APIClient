// What the TRUSTED ZONE actually put on the wire for a live page — the request column, which no
// other driver here can answer.
//
// WHY THIS EXISTS AS A SEPARATE INSTRUMENT. `testing/live-run.js` reports what the engine's result
// document says it DERIVED (`fetchCallSites.length`), and `harness netdiff` reports what the PAGE
// fetched (the content script's captured traffic, filed per document). Neither of those is "did the
// engine ISSUE this load". A run can derive an address and never fetch it, and a page can fetch an
// address the engine never saw — so the two existing columns are silent about the one question in
// between, and the shape of that silence is §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES: a zero that is a
// property of the instrument rather than of the run.
//
// IT IS NOT A HARNESS PRINT AND NOT A WRAPPER. Nothing here patches `safeFetch`, counts inside the
// extension, or scrapes a console. It reads the BROWSER's own report of the requests the offscreen
// document made — which is the definition of "issued" and is a fact no code in this repository
// authored. SECURITY.md puts every byte through `safe-fetch.js` in the trusted zone, and the
// renderer frames are sandboxed opaque origins that cannot fetch, so the offscreen page's request
// stream IS the engine's egress.
//
// THE HEADLINE IS A SET COMPARISON, NOT A COUNT. A total of issued requests answers almost nothing:
// the interesting question is whether the addresses the DOCUMENT ITSELF names as `<script src>` were
// loaded, because those are the bundle — the code that has to run before anything can be derived
// from it. So this driver reads the markup's own script list out of the tab and diffs it against
// what went out, and prints both halves: `markupIssued` and `markupMissed`. A page whose bundle was
// never fetched cannot have been analysed, whatever else the run reports.
//
// THREE CLASSES, KEPT APART DELIBERATELY. An extension-internal load (`chrome-extension://`, `blob:`)
// is the engine booting itself and says nothing about the page; a `#_internal_probe` is active
// discovery, which §Attacker-sources requires and which would otherwise swamp the count; everything
// else is a load made ON BEHALF OF the document. Summed into one number they read as one thing, and
// that is the averaging §MEASURE forbids.
//
//   node testing/live-requests.js <runs> <url> [url…]
//
"use strict";

const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");
const { artifactStamp } = require("./artifact_stamp.js");

const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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

/* THE SAME LIVENESS COLUMN live-run.js CARRIES, AND FOR THE SAME REASON: bridge.js sets `_hostDead`
   once and never clears it, so a run whose scheduler was already dead was never asked about this
   URL and its EMPTY request list is not a finding about the page. A probe that throws reports under
   its own token rather than folding into `alive`. */
function scheduler(pg) {
  return pg.evaluate(() => {
    try {
      const p = self.rendererPoolProbe();
      return { alive: p.scheduler.alive, driving: p.scheduler.driving,
               kicksRefused: p.scheduler.kicksRefused, diedOf: p.scheduler.diedOf };
    } catch (e) { return { PROBE_THREW: String((e && e.message) || e) }; }
  });
}

function engineRows(pg) {
  return pg.evaluate(() => (self._engineLog || []).map((r) => ({ run: r.run, endpoints: r.endpoints,
                                                                sinks: r.sinks, err: r.err })));
}

/* WHAT THE DOCUMENT ITSELF NAMES. Read from the TAB rather than from a fetch of our own, because a
   second fetch of the page can legitimately answer differently (cache, nonce, personalised SSR) and
   would then be a list of scripts the analysed document never carried. `src` is read as the resolved
   property, so it is the absolute URL the browser would request — the same form the request stream
   reports, which is what makes the diff below a comparison rather than a guess. */
function markupScripts(page) {
  return page.evaluate(() => ({
    title: document.title.slice(0, 60),
    htmlBytes: document.documentElement.outerHTML.length,
    scripts: Array.from(document.querySelectorAll("script[src]")).map((s) => s.src),
  })).catch((e) => ({ EVAL_THREW: String((e && e.message) || e) }));
}

const isInternal = (u) => u.startsWith("chrome-extension://") || u.startsWith("blob:") ||
                          u.startsWith("data:") || u.startsWith("about:");
const isProbe = (u) => u.includes("#_internal_probe");

async function oneRun(browser, pg, url, budgetMs) {
  const schedBefore = await scheduler(pg);
  const base = (await engineRows(pg)).length;

  const issued = [];
  const onReq = (r) => { try { issued.push(r.method() + " " + r.url()); } catch (e) {} };
  pg.on("request", onReq);

  const pages = await browser.pages();
  const nonExt = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                     !p.url().startsWith("devtools://"));
  const page = nonExt.length ? nonExt[nonExt.length - 1] : await browser.newPage();

  /* about:blank FIRST, for the reason live-run.js states at its own goto: a `goto` to the URL the tab
     already holds is a same-document navigation, which creates no Document and admits no engine, and
     the empty request list that follows is the browser declining to navigate rather than the engine
     issuing nothing. */
  try { await page.goto("about:blank", { waitUntil: "domcontentloaded", timeout: 15000 }); } catch (e) {}
  const t0 = Date.now();
  let nav = null;
  try {
    const resp = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
    nav = resp ? resp.status() : "nav-noop(no document created)";
  } catch (e) { nav = "navfail:" + String((e && e.message) || e).split("\n")[0]; }

  const markup = await markupScripts(page);

  /* POLLED OFF THE ENGINE'S OWN ROW, not a wall clock — but unlike live-run.js this driver does NOT
     stop at the terminal row, because requests keep being issued after a renderer dies (the pool
     provisions another, discovery continues) and stopping at the first terminal row would report a
     truncated wire. The budget is spent in full and is a MEASUREMENT WINDOW here rather than a
     backstop, which is why it is printed on every row: a request count is only comparable against
     another count taken over the same window. */
  await sleep(budgetMs);

  pg.off("request", onReq);
  const schedAfter = await scheduler(pg);
  const rows = (await engineRows(pg)).slice(base);

  const net = issued.filter((r) => !isInternal(r.split(" ")[1]));
  const probes = net.filter((r) => isProbe(r.split(" ")[1]));
  const loads = net.filter((r) => !isProbe(r.split(" ")[1]));
  const loadUrls = new Set(loads.map((r) => r.split(" ")[1]));

  /* THE HEADLINE. An issued markup script is matched by its exact resolved URL; nothing is normalised
     away, because a near-match is not a load. */
  const scripts = Array.isArray(markup.scripts) ? markup.scripts : [];
  const markupIssued = scripts.filter((s) => loadUrls.has(s));
  const markupMissed = scripts.filter((s) => !loadUrls.has(s));

  return {
    url, nav, windowMs: budgetMs, elapsedMs: Date.now() - t0,
    /* THE SAME FOUR NOTHINGS live-run.js NAMES, because an empty request list has the same readings
       as an empty counter set and must not collapse them. */
    verdict: schedBefore.alive === false ? "scheduler-dead-before-this-run(NOT a sample of this site)"
           : typeof nav !== "number" ? "no-navigation"
           : markup.EVAL_THREW ? "tab-unreadable(" + markup.EVAL_THREW + ")"
           : "sample",
    scheduler: { before: schedBefore, after: schedAfter },
    outcomes: rows.map((r) => r.run),
    engineEndpoints: rows.length ? rows.map((r) => (r.endpoints === undefined ? null : r.endpoints)) : null,
    doc: { title: markup.title, htmlBytes: markup.htmlBytes, markupScripts: scripts.length },
    issued: { total: issued.length, internal: issued.length - net.length,
              probes: probes.length, loads: loads.length },
    markupIssued, markupMissed,
    loadSample: loads.slice(0, 30),
    crashSites: rows.filter((r) => r.err).map((r) => {
      const m = /"at":"([^"]+)"/.exec(r.err) || /(engine\/host\/[\w/]+\.c:\d+)/.exec(r.err) ||
                /(engine\/qjs\/quickjs\.c:\d+)/.exec(r.err);
      return m ? m[1] : "?";
    }),
  };
}

function spread(vals) {
  const v = vals.filter((x) => typeof x === "number");
  if (!v.length) return "-";                       // absent, not zero
  const lo = Math.min(...v), hi = Math.max(...v);
  return lo === hi ? String(lo) : lo + "–" + hi;   // a RANGE, never a mean
}

async function main() {
  const runs = parseInt(process.argv[2], 10);
  const urls = process.argv.slice(3);
  if (!runs || !urls.length) {
    console.error("usage: node testing/live-requests.js <runs> <url> [url…]");
    process.exit(2);
  }
  const budgetMs = Number(process.env.LIVE_REQ_WINDOW_MS || 45000);
  console.log("# artifact " + JSON.stringify(artifactStamp()));
  console.log("# runs=" + runs + " windowMs=" + budgetMs +
              " (a MEASUREMENT WINDOW, not a backstop — request counts compare only across equal windows)");

  const { browser, extId } = await connect();
  try {
    const pg = await offscreenPage(browser, extId);
    const bySite = new Map();
    for (const u of urls) bySite.set(u, []);
    for (let i = 0; i < runs; i++) {
      for (const u of urls) {
        const r = await oneRun(browser, pg, u, budgetMs);
        bySite.get(u).push(r);
        console.log(JSON.stringify(Object.assign({ runIndex: i }, r)));
      }
    }
    console.log("\n# ── spread across " + runs + " run(s); a range, never a mean ──");
    for (const [u, rs] of bySite) {
      console.log(JSON.stringify({
        url: u,
        /* THE DENOMINATOR, stated on the same line as every range drawn from it. */
        runsThatWereSamples: rs.filter((r) => r.verdict === "sample").length + "/" + rs.length,
        markupScripts: spread(rs.map((r) => r.doc.markupScripts)),
        markupIssued: spread(rs.map((r) => r.markupIssued.length)),
        markupMissed: spread(rs.map((r) => r.markupMissed.length)),
        issuedLoads: spread(rs.map((r) => r.issued.loads)),
        issuedProbes: spread(rs.map((r) => r.issued.probes)),
        issuedInternal: spread(rs.map((r) => r.issued.internal)),
        crashSites: Array.from(new Set([].concat(...rs.map((r) => r.crashSites)))),
      }));
    }
  } finally { browser.disconnect(); }
}

main().catch((e) => { console.error(e.stack || e.message || e); process.exit(1); });
