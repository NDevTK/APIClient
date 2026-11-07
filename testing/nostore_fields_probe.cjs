// Where Chrome adds the `Pragma` and `Cache-Control` fields of a "no-store" fetch (Fetch §4.6
// "HTTP-network-or-cache fetch" step 8.18), and whether a declarativeNetRequest rule can remove them. An
// extension of its own fetches from an offscreen document, as lib/wire.js does, while its service worker records
// webRequest's onBeforeSendHeaders and onSendHeaders and a local server records what arrived.
//
//   NODE_PATH=<checkout>/node_modules node testing/nostore_fields_probe.cjs <out.json>
//
// Rows, each `cache` mode × whether a session rule removing both fields matches the URL:
//   nostore        cache "no-store", no rule: the positive control, both fields arrive.
//   nostore.rule   cache "no-store", the rule: does the removal reach the wire?
//   listed.rule    cache "default" with both fields in the fetch's own header list, the rule: the control that
//                  the rule removes a field it can see.
//   default        cache "default", no rule: the negative control, neither arrives.
//   nostore.cdp    cache "no-store", with the extension's chrome.debugger session on the offscreen document
//                  continuing the request (Fetch.continueRequest) with a header list naming neither field.
//   cachedisabled  cache "default" under that session's Network.setCacheDisabled, fetched twice from a reply
//                  fresh for 600 s: whether the extension's cache is bypassed (two hits) and with which fields.
"use strict";
const path = require("path");
const fs = require("fs");
const os = require("os");
const http = require("http");
const { launchChrome } = require("./harness.js");

const outPath = process.argv[2];
if (!outPath) throw new Error("usage: node testing/nostore_fields_probe.cjs <out.json>");
const PORT = 18761, BASE = `http://127.0.0.1:${PORT}`;
const seen = {}, hits = {};

const SW = `
self.WR = {};
const keep = (ev) => (d) => {
  if (!d.url.includes(":${PORT}/")) return;
  const k = new URL(d.url).searchParams.get("id");
  (WR[k] = WR[k] || {})[ev] = d.requestHeaders.map((h) => [h.name, h.value]);
};
chrome.webRequest.onBeforeSendHeaders.addListener(keep("onBeforeSendHeaders"), { urls: ["<all_urls>"] },
                                                  ["requestHeaders", "extraHeaders"]);
chrome.webRequest.onSendHeaders.addListener(keep("onSendHeaders"), { urls: ["<all_urls>"] },
                                            ["requestHeaders", "extraHeaders"]);
self.rule = (on) => chrome.declarativeNetRequest.updateSessionRules({ removeRuleIds: [7001], addRules: on ? [{
  id: 7001, priority: 1, condition: { urlFilter: "rule=1", resourceTypes: ["xmlhttprequest", "other"] },
  action: { type: "modifyHeaders", requestHeaders: [{ header: "pragma", operation: "remove" },
                                                    { header: "cache-control", operation: "remove" }] } }] : [] });
self.cdpOn = async (on) => {
  const t = (await chrome.debugger.getTargets()).find((x) => x.url.endsWith("/off.html"));
  if (!on) return chrome.debugger.detach({ targetId: t.id });
  await chrome.debugger.attach({ targetId: t.id }, "1.3");
  await chrome.debugger.sendCommand({ targetId: t.id }, "Fetch.enable", { patterns: [{ urlPattern: "*cdp=1*" }] });
};
chrome.debugger.onEvent.addListener((src, method, p) => {
  if (method !== "Fetch.requestPaused") return;
  const headers = Object.entries(p.request.headers).filter(([n]) => !/^(pragma|cache-control)$/i.test(n))
    .map(([name, value]) => ({ name, value }));
  self.CDP_SEEN = Object.keys(p.request.headers);
  chrome.debugger.sendCommand(src, "Fetch.continueRequest", { requestId: p.requestId, headers });
});
self.cacheDisabled = async (on) => {
  const t = (await chrome.debugger.getTargets()).find((x) => x.url.endsWith("/off.html"));
  if (!on) return chrome.debugger.detach({ targetId: t.id });
  await chrome.debugger.attach({ targetId: t.id }, "1.3");
  await chrome.debugger.sendCommand({ targetId: t.id }, "Network.enable", {});
  await chrome.debugger.sendCommand({ targetId: t.id }, "Network.setCacheDisabled", { cacheDisabled: true });
};
self.offFetch = (m) => chrome.runtime.sendMessage(Object.assign({ to: "off" }, m));
chrome.offscreen.createDocument({ url: "off.html", reasons: ["WORKERS"], justification: "probe" });
`;
const OFF_JS = `
chrome.runtime.onMessage.addListener((m, s, reply) => {
  if (m.to !== "off") return;
  fetch(m.url, { cache: m.cache, headers: m.headers, credentials: "omit" })
    .then((r) => reply({ status: r.status }), (e) => reply({ error: String(e) }));
  return true;
});`;

async function main() {
  const server = http.createServer((req, res) => {
    const id = new URL(req.url, BASE).searchParams.get("id");
    seen[id] = req.rawHeaders;
    hits[id] = (hits[id] || 0) + 1;
    res.writeHead(200, { "content-type": "text/plain", "cache-control": "max-age=600" });
    res.end("ok");
  }).listen(PORT, "127.0.0.1");
  const ext = fs.mkdtempSync(path.join(os.tmpdir(), "nostore-probe-ext-"));
  fs.writeFileSync(path.join(ext, "manifest.json"), JSON.stringify({ manifest_version: 3, name: "nostore probe",
    version: "1", permissions: ["offscreen", "webRequest", "declarativeNetRequestWithHostAccess", "debugger"],
    host_permissions: ["<all_urls>"], background: { service_worker: "sw.js" } }));
  fs.writeFileSync(path.join(ext, "sw.js"), SW);
  fs.writeFileSync(path.join(ext, "off.html"), `<!doctype html><script src="off.js"></script>`);
  fs.writeFileSync(path.join(ext, "off.js"), OFF_JS);
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "nostore-probe-prof-"));
  const root = typeof process.getuid === "function" && process.getuid() === 0;
  const browser = await launchChrome({ headless: true, pipe: true, enableExtensions: [ext], userDataDir: profile,
    waitForInitialPage: false, targetFilter: (t) => t.type() === "service_worker" || t.type() === "browser",
    args: ["--no-first-run", "--no-default-browser-check", ...(root ? ["--no-sandbox"] : [])] });
  const out = { rows: {} };
  try {
    let swT = null;
    for (let i = 0; i < 100 && !swT; i++) {
      swT = browser.targets().find((t) => t.type() === "service_worker" && t.url().endsWith("/sw.js"));
      if (!swT) await new Promise((r) => setTimeout(r, 100));
    }
    if (!swT) throw new Error("the probe's service worker never started");
    const sw = await swT.worker();
    await new Promise((r) => setTimeout(r, 1000));
    const listed = [["Pragma", "no-cache"], ["Cache-Control", "no-cache"]];
    for (const [id, cache, headers, rule] of [["nostore", "no-store", [], false], ["nostore.rule", "no-store", [], true],
                                              ["listed.rule", "default", listed, true], ["default", "default", [], false],
                                              ["nostore.cdp", "no-store", [], false],
                                              ["cachedisabled", "default", [], false]]) {
      const cdp = id === "nostore.cdp";
      if (id === "cachedisabled") {
        await sw.evaluate(() => cacheDisabled(true));
        for (let i = 0; i < 2; i++) await sw.evaluate((m) => offFetch(m), { url: `${BASE}/x?id=${id}`, cache, headers });
        await sw.evaluate(() => cacheDisabled(false));
        const raw = seen[id];
        out.rows[id] = { hits: hits[id], server: raw.filter((v, i) => i % 2 === 0 && /^(pragma|cache-control)$/i.test(v))
                                                    .map((v) => [v, raw[raw.indexOf(v) + 1]]) };
        continue;
      }
      await sw.evaluate((on) => rule(on), rule);
      if (cdp) await sw.evaluate(() => cdpOn(true));
      const reply = await sw.evaluate((m) => offFetch(m), { url: `${BASE}/x?id=${id}&rule=${rule ? 1 : 0}&cdp=${cdp ? 1 : 0}`,
                                                          cache, headers });
      if (cdp) { out.cdpPausedHeaderNames = await sw.evaluate(() => self.CDP_SEEN || null); await sw.evaluate(() => cdpOn(false)); }
      await new Promise((r) => setTimeout(r, 300));
      const wr = await sw.evaluate((k) => WR[k] || null, id);
      const pick = (list) => list && list.filter(([n]) => /^(pragma|cache-control)$/i.test(n));
      const raw = seen[id] || null;
      out.rows[id] = { reply, onBeforeSendHeaders: pick(wr && wr.onBeforeSendHeaders), onSendHeaders: pick(wr && wr.onSendHeaders),
                       server: raw && pick(raw.reduce((a, v, i) => (i % 2 ? a : a.concat([[v, raw[i + 1]]])), [])) };
    }
  } finally {
    fs.writeFileSync(outPath, JSON.stringify(out, null, 1));
    await browser.close();
    server.close();
    fs.rmSync(profile, { recursive: true, force: true });
    fs.rmSync(ext, { recursive: true, force: true });
  }
  for (const [id, r] of Object.entries(out.rows)) console.log(id.padEnd(14), JSON.stringify(r));
}
main().catch((e) => { console.error(e); process.exit(1); });
