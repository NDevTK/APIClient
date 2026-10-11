// Sends one request several ways from a page on origin A and prints the header list the server received for
// each, so a send path that claims to speak as the page can be scored against the page's own request.
//
//   python3 -I testing/page_identity_echo.py 18431 18432 18433 &
//   HARNESS_PROFILE=/home/chromerun/<lane>-prof HARNESS_LOCK=/tmp/<lane>/harness.lock \
//     node testing/harness.js restart <port>
//   node testing/page_identity_probe.cjs <port> <extId> <out.json>
//
// Paths: `page` is the page's own fetch (the oracle) and `xhr` its XMLHttpRequest; `iso` is a fetch in the
// extension's isolated world; `main` is a fetch in the page's main world through the reference intercept.js
// captured; `off` is a fetch in the offscreen document; `dnr` is an offscreen fetch under a declarativeNetRequest
// session rule installed by the service worker. A `dnr` row whose rule is scoped to another initiator, another
// tab or another method is a negative control: it must arrive exactly as the rule-less `off` row does.
// `send` is the Send panel's SEND_REQUEST from the popup document, and `zone` rows are the zone's own fetch
// speaking as an identity through lib/request-identity.js and lib/speak-as.js.
"use strict";
const path = require("path");
const http = require("http");
const fs = require("fs");
const puppeteer = require(require.resolve("puppeteer", { paths: [path.join(__dirname, "..")] }));

const [port, extId, outPath] = process.argv.slice(2);
if (!port || !extId || !outPath) throw new Error("usage: node testing/page_identity_probe.cjs <port> <extId> <out.json>");
const A = "http://localhost:18431", B = "http://127.0.0.1:18432", C = "http://127.0.0.1:18433";
const T = { A, B, C };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const get = (u) => new Promise((res, rej) =>
  http.get(u, (r) => { let b = ""; r.on("data", (c) => (b += c)); r.on("end", () => res(b)); }).on("error", rej));

async function target(browser, pred, what) {
  for (let i = 0; i < 80; i++) {
    const t = browser.targets().find(pred);
    if (t) return t;
    await sleep(250);
  }
  throw new Error("no " + what + " target");
}

async function main() {
  const browser = await puppeteer.connect({ browserURL: "http://127.0.0.1:" + port, defaultViewport: null,
                                            protocolTimeout: 120000 });
  const page = await browser.newPage();
  for (const o of [A, B, C]) await page.goto(o + "/setcookies", { waitUntil: "load" });
  await page.goto(A + "/page?x=secret", { waitUntil: "load" });
  const controlled = await page.evaluate(() => window.swReady);
  await page.evaluate((b) => fetch(b + "/setpart", { credentials: "include" }).then((r) => r.status), B);
  await sleep(1500);   // content.js runs at document_idle

  const cdp = await page.createCDPSession();
  const ctxs = [];
  cdp.on("Runtime.executionContextCreated", (e) => ctxs.push(e.context));
  await cdp.send("Runtime.enable");
  await sleep(300);
  const frameId = page.mainFrame()._id;
  const iso = ctxs.find((c) => c.auxData && c.auxData.type === "isolated" && c.auxData.frameId === frameId &&
                               c.origin.includes(extId));
  const mainCtx = ctxs.find((c) => c.auxData && c.auxData.isDefault && c.auxData.frameId === frameId);
  if (!iso || !mainCtx) throw new Error("no isolated or main world for the page's main frame");
  const evalIn = async (ctx, expr) => {
    const r = await cdp.send("Runtime.evaluate", { contextId: ctx.id, expression: expr, awaitPromise: true,
                                                    returnByValue: true });
    return r.exceptionDetails ? { error: r.exceptionDetails.text } : r.result.value;
  };
  const sw = await (await target(browser, (t) => t.type() === "service_worker" && t.url().includes(extId),
                                 "extension service worker")).worker();
  const off = await (await target(browser, (t) => t.url().startsWith("chrome-extension://" + extId +
                                  "/ast-worker.html"), "offscreen")).page();
  // The probe page's own tab, by its exact URL: another tab on A would lend the sends a different frame.
  const tabs = await sw.evaluate(async (a) => (await chrome.tabs.query({ url: a + "/page?x=secret" })).map((t) => t.id), A);
  if (tabs.length !== 1) throw new Error("expected one tab on " + A + "/page?x=secret, found " + tabs.length);
  const tabId = tabs[0];
  const route = await sw.evaluate(async (t) => ({ tabId: t,
    documentId: (await chrome.webNavigation.getFrame({ tabId: t, frameId: 0 })).documentId }), tabId);
  const popup = await browser.newPage();
  await popup.goto("chrome-extension://" + extId + "/popup.html", { waitUntil: "domcontentloaded" });
  await page.bringToFront();
  const hasDnr = await sw.evaluate(() => typeof chrome.declarativeNetRequest === "object");

  const setRule = (cond, headers) => sw.evaluate(async (c, h) => {
    const old = (await chrome.declarativeNetRequest.getSessionRules()).map((r) => r.id);
    const addRules = c ? [{ id: 9001, priority: 1, condition: c, action: { type: "modifyHeaders",
      requestHeaders: Object.entries(h).map(([header, value]) => ({ header, operation: "set", value })) } }] : [];
    await chrome.declarativeNetRequest.updateSessionRules({ removeRuleIds: old, addRules });
  }, cond, headers);
  const fetchExpr = (u, init) => `fetch(${JSON.stringify(u)}, ${JSON.stringify(init)})` +
                                 `.then(r => ({status: r.status}), e => ({error: String(e)}))`;

  await get(A + "/clear");
  const results = {};
  async function fire(label, t, how, init, rule) {
    const id = label + "." + t;
    const url = T[t] + "/echo?id=" + encodeURIComponent(id);
    if (how === "page") results[id] = await evalIn(mainCtx, fetchExpr(url, init));
    else if (how === "xhr") results[id] = await page.evaluate((u) => window.pageXhr(u, true), url);
    else if (how === "main") results[id] = await evalIn(mainCtx, fetchExpr(url, init));
    else if (how === "iso") results[id] = await evalIn(iso, fetchExpr(url, init));
    // The Send panel's command, from the popup document, through lib/send.js and safe-fetch.js's operator tier.
    else if (how === "send") results[id] = await popup.evaluate((rt, u, o) => chrome.runtime.sendMessage({
      type: "SEND_REQUEST", tabId: rt.tabId, documentId: rt.documentId, endpointKey: "", service: null,
      methodId: null, url: u, httpMethod: o.method || "GET", contentType: "", headers: o.headers || {},
      body: o.body === undefined ? null : { mode: "raw", rawBody: o.body, formData: null },
      frameId: 0, apiKeyOverride: null, requestInit: o.requestInit })
      .then((x) => (x && x.error ? { error: x.error } : { status: x && x.status })), route, url, init);
    else if (how === "off" || how === "dnr") {
      if (how === "dnr") await setRule(rule(url), { "Origin": A, "Referer": A + "/page?x=secret",
                                                    "Cookie": "Bnone=1; Cnone=1; Bpart=1",
                                                    "Sec-Fetch-Site": "cross-site" });
      // A function, not an expression: the offscreen document's CSP has no 'unsafe-eval'.
      results[id] = await off.evaluate((u, i) => fetch(u, i).then((r) => ({ status: r.status }),
                                                                 (e) => ({ error: String(e) })), url, init);
      if (how === "dnr") await setRule(null, {});
    } else throw new Error("unknown path " + how);
  }
  const so = { credentials: "same-origin" }, inc = { credentials: "include" };
  const post = { method: "POST", credentials: "include", body: "{}",
                 headers: { "Content-Type": "application/json", "X-Probe": "1" } };
  for (const t of ["A", "B", "C"]) {
    await fire("page.so", t, "page", so);
    await fire("page.inc", t, "page", inc);
    await fire("xhr.wc", t, "xhr");
    await fire("iso.so", t, "iso", so);
    await fire("iso.inc", t, "iso", inc);
    await fire("main.so", t, "main", so);
    await fire("main.inc", t, "main", inc);
    await fire("off.so", t, "off", so);
    await fire("off.inc", t, "off", inc);
  }
  await fire("page.post", "B", "page", post);
  await fire("iso.post", "B", "iso", post);
  await fire("main.post", "B", "main", post);
  await fire("off.post", "B", "off", post);
  for (const t of ["A", "B"]) {
    const sameRef = { credentials: "include", referrer: A + "/other/path?r=1" };
    await fire("ref.same.page", t, "page", sameRef);
    await fire("ref.same.iso", t, "iso", sameRef);
    await fire("ref.cross.page", t, "page", { credentials: "include", referrer: "http://evil.example/x" });
    await fire("ref.nopol.page", t, "page", { credentials: "include", referrerPolicy: "no-referrer" });
  }
  // The Send panel, replaying the record of the page's own request it is scored against.
  const ri = (credentials, extra) => ({ requestInit: Object.assign({ credentials, mode: "cors", referrer: null,
                                                                     referrerPolicy: null }, extra) });
  const riPost = (credentials) => Object.assign(ri(credentials), { method: "POST", body: "{}",
                                                headers: { "Content-Type": "application/json", "X-Probe": "1" } });
  for (const how of ["send"]) {
    for (const t of ["A", "B", "C"]) {
      await fire(how + ".so", t, how, ri("same-origin"));
      await fire(how + ".inc", t, how, ri("include"));
    }
    await fire(how + ".unstated", "B", how, ri(null));
    await fire(how + ".post", "B", how, riPost("include"));
    for (const t of ["A", "B"]) {
      await fire(how + ".ref.same", t, how, ri("include", { referrer: A + "/other/path?r=1" }));
      await fire(how + ".ref.cross", t, how, ri("include", { referrer: "http://evil.example/x" }));
      await fire(how + ".ref.nopol", t, how, ri("include", { referrerPolicy: "no-referrer" }));
    }
  }
  await fire("send.forbidden", "B", "send", Object.assign(ri("include"), { headers: { "Cookie": "x=1" } }));
  if (hasDnr) {
    const cond = (extra) => (url) => Object.assign({ urlFilter: "|" + url + "|", tabIds: [-1],
      initiatorDomains: [extId], requestMethods: ["get"], resourceTypes: ["xmlhttprequest", "other"] }, extra);
    await fire("dnr.ours", "B", "dnr", inc, cond({}));
    await fire("dnr.otherinitiator", "B", "dnr", inc, cond({ initiatorDomains: ["a".repeat(32)] }));
    await fire("dnr.realtab", "B", "dnr", inc, cond({ tabIds: [tabId] }));
    await fire("dnr.wrongmethod", "B", "dnr", inc, cond({ requestMethods: ["post"] }));
    await fire("dnr.post", "B", "dnr", post, cond({ requestMethods: ["post"] }));
  }
  if (hasDnr) await zoneRows({ browser, page, sw, off, extId, route, results });
  await sleep(1500);
  const log = JSON.parse(await get(A + "/log"));
  fs.writeFileSync(outPath, JSON.stringify({ controlled, hasDnr, results, log }, null, 1));
  await page.close();
  await popup.close();
  browser.disconnect();
  printTable(results, log);
}

/* The zone send: the offscreen document's own fetch speaking as an identity, through lib/request-identity.js
   and lib/speak-as.js. `spec` names the identity (a frame by documentId, or an engine origin and URL), the
   request, and the identity's document referrer policy. The page sets `Referrer-Policy: unsafe-url`. */
function zoneSend(off, spec) {
  return off.evaluate(async (s) => {
    const id = s.engine ? engineIdentity(s.engine.origin, s.engine.docUrl) : await frameIdentity(s.tabId, s.documentId);
    const facts = identityFor(id, { url: s.url, requestInit: s.requestInit }, s.documentPolicy);
    const c = await cookieHeaderFor(facts, s.url);
    const mode = s.requestInit.mode === null ? "cors" : s.requestInit.mode;
    const rules = [{ method: s.method, headers: speakAsHeaders(facts, s.method, mode, "", s.url, c) }];
    // A preflighted send: the OPTIONS rule rides with the request's, as Fetch §4.8 sends it first.
    if (s.preflightUnsafe) rules.push({ method: "OPTIONS",
      headers: speakAsPreflightHeaders(facts, s.method, "", s.url, s.preflightUnsafe) });
    return withSpeakAsRules(facts.origin, s.url, rules, async () => {
      if (s.pageFetchDuring) await new Promise((r) => setTimeout(r, 1500));   // the tab's own fetch lands here
      if (s.preflightUnsafe) await fetch(s.url, { method: "OPTIONS", credentials: "omit" });
      const r = await fetch(s.url, { method: s.method, credentials: c.wire, headers: s.headers, body: s.body });
      return { status: r.status };
    });
  }, spec).catch((e) => ({ error: String(e) }));
}

async function zoneRows({ browser, page, sw, off, extId, route, results }) {
  const echo = (t, id) => ({ A, B }[t]) + "/echo?id=" + id;
  const ri = (credentials, extra) => Object.assign({ credentials, mode: null, referrer: null, referrerPolicy: null }, extra);
  const asTop = (t, id, extra) => Object.assign({ tabId: route.tabId, documentId: route.documentId, url: echo(t, id),
    method: "GET", requestInit: ri("include"), documentPolicy: "unsafe-url" }, extra);
  for (const t of ["A", "B"]) {
    for (const cr of ["same-origin", "include"]) {
      const id = "zone." + (cr === "include" ? "inc" : "so") + "." + t;
      results[id] = await zoneSend(off, asTop(t, id, { requestInit: ri(cr) }));
    }
    let id = "zone.ref.same." + t;
    results[id] = await zoneSend(off, asTop(t, id, { requestInit: ri("include", { referrer: A + "/other/path?r=1" }) }));
    id = "zone.ref.nopol." + t;
    results[id] = await zoneSend(off, asTop(t, id, { requestInit: ri("include", { referrerPolicy: "no-referrer" }) }));
  }
  results["zone.post.B"] = await zoneSend(off, asTop("B", "zone.post.B", { method: "POST", body: "{}",
    headers: { "Content-Type": "application/json", "X-Probe": "1" }, preflightUnsafe: ["content-type", "x-probe"] }));
  results["zone.put.B"] = await zoneSend(off, asTop("B", "zone.put.B", { method: "PUT", body: "{}",
    headers: { "Content-Type": "application/json" }, preflightUnsafe: ["content-type"] }));
  results["page.put.B"] = await page.evaluate((u) => fetch(u, { method: "PUT", credentials: "include", body: "{}",
    headers: { "Content-Type": "application/json" } }).then((r) => r.status), echo("B", "page.put.B"));
  // An engine send whose stated referrer is another origin's URL arrives with the document's URL.
  results["zone.engine.crossref.B"] = await zoneSend(off, { engine: { origin: A, docUrl: A + "/page?x=secret" },
    url: echo("B", "zone.engine.crossref.B"), method: "GET",
    requestInit: ri("include", { referrer: "http://evil.example/x" }), documentPolicy: "unsafe-url" });
  // Two concurrent sends of one URL as two identities: each must arrive with its own Origin.
  const same = B + "/echo?id=zone.concurrent";
  const [r1, r2] = await Promise.all([
    zoneSend(off, Object.assign(asTop("B", "x"), { url: same })),
    zoneSend(off, { engine: { origin: "http://localhost:18433", docUrl: "http://localhost:18433/doc" }, url: same,
                    method: "GET", requestInit: ri("include"), documentPolicy: "strict-origin-when-cross-origin" })]);
  results["zone.concurrent"] = { first: r1, second: r2 };
  // The tab's own fetch of a URL while a rule for it is installed must arrive untouched.
  const during = echo("B", "zone.duringrule");
  const zp = zoneSend(off, Object.assign(asTop("B", "x"), { url: during, pageFetchDuring: true }));
  await sleep(500);
  results["zone.duringrule.page"] = await page.evaluate((u) => fetch(u, { credentials: "include" })
    .then((r) => r.status), during).catch((e) => String(e));
  results["zone.duringrule"] = await zp;
  // A rule a killed offscreen document left: installed directly, then the offscreen document reloads and
  // its first send must remove it.
  await sw.evaluate(() => chrome.declarativeNetRequest.updateSessionRules({ addRules: [{ id: 1500000, priority: 1,
    condition: { urlFilter: "|http://127.0.0.1:18432/echo?id=zone.leftover|", tabIds: [-1] },
    action: { type: "modifyHeaders", requestHeaders: [{ header: "x-leftover", operation: "set", value: "1" }] } }] }));
  await off.reload({ waitUntil: "load" });
  await sleep(1500);
  results["zone.leftover"] = await zoneSend(off, asTop("B", "zone.leftover"));
  results["zone.leftover.rulesAfter"] = await sw.evaluate(async () =>
    (await chrome.declarativeNetRequest.getSessionRules()).map((r) => r.id));
}

const SHOWN = ["origin", "referer", "sec-fetch-site", "sec-fetch-mode", "sec-fetch-dest", "sec-fetch-storage-access",
               "cookie"];
function printTable(results, log) {
  const sw = new Set(log.filter((r) => r.kind === "swlog").map((r) => r.q.id));
  for (const id of Object.keys(results)) {
    const rows = log.filter((r) => r.id === id && (r.kind === "echo" || r.kind === "preflight"));
    if (rows.length === 0) { console.log(id.padEnd(24), "NOT RECEIVED", JSON.stringify(results[id])); continue; }
    for (const r of rows) {
      const h = new Map(r.headers.map(([k, v]) => [k.toLowerCase(), v]));
      console.log(id.padEnd(24), (r.kind === "preflight" ? "OPTIONS" : r.method).padEnd(7),
                  SHOWN.map((k) => k.replace("sec-fetch-", "sf-") + "=" + (h.has(k) ? h.get(k) : "-")).join(" | "),
                  "| sw=" + (sw.has(id) ? "yes" : "no"));
    }
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
