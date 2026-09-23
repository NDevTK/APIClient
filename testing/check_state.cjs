// Quick state dump: connect to the running harness, find any github
// tab, dump its DIAG_TAB + endpoint list.
"use strict";
const fs = require("fs");
const path = require("path");
const puppeteer = require("puppeteer");
const LOCK = path.resolve(__dirname, "harness.lock");
(async () => {
  const h = JSON.parse(fs.readFileSync(LOCK, "utf8"));
  const browser = await puppeteer.connect({
    browserURL: `http://127.0.0.1:${h.port}`,
    defaultViewport: null,
    targetFilter: (t) => t.type() !== "browser",
    protocolTimeout: 60000,
  });
  const p = await browser.newPage();
  await p.goto(`chrome-extension://${h.extId}/popup.html`, { waitUntil: "domcontentloaded", timeout: 15000 });
  const allGh = await p.evaluate(async () => new Promise(r => chrome.tabs.query({}, ts => r(ts.filter(x => x.url && x.url.startsWith("https://github.com/")).map(x => ({ id: x.id, url: x.url }))))));
  console.log("github tabs:", JSON.stringify(allGh));
  for (const t of allGh) {
    const d = await p.evaluate(async (tid) => new Promise(r => chrome.runtime.sendMessage({ type: "DIAG_TAB", tabId: tid }, x => r(x))), t.id);
    console.log(`-- tab ${t.id} DIAG --`);
    console.log(JSON.stringify(d, null, 2));
    /* THE RECORD IS ASSERTED AT THE DOOR AND NOT DEFAULTED AT THE READ, and the assert is the extension's
       OWN — `lib/endpoint-record.js`'s `checkEndpointRecord`, run IN THE PAGE, because that file is the one
       description of an endpoint and a second copy of its field list here would be the ninth transcription
       its own header is about. This driver navigates to `popup.html`, which loads `check.js` and then
       `lib/endpoint-record.js` as classic scripts, so both names are globals in the frame `evaluate` runs in.
       WHAT THE THREE `||`s HERE WERE CONCEALING. The contract guarantees `url` and `method` are NON-EMPTY
       strings on every record, so `e.method || "?"` and `e.url || … || "?"` could not fire for any record the
       producer made — they could only fire for a BROKEN one, which is exactly the sighting they turned into a
       plausible dash. The middle arm was worse than a default and `testing/harness.js` had already paid for
       it: `path` is legitimately "" for a shape-origin address with no literal remainder, and the record's own
       assert says the address is `host` + `path` and "never `url` as a fallback" — so a chain that reads one
       for the other prints an address that was never learned. All three are gone; the fields are read plainly
       because the assert above has already refused every record that could not answer them.
       LEFT AS IT WAS, AND SAID RATHER THAN QUIETLY KEPT: the `reply?.endpoints` shape tolerance below. That is
       a claim about the GET_STATE REPLY and not about an endpoint, it has no stated contract this driver could
       assert against, and inventing one here would be the guess this file exists to refuse. */
    const st = await p.evaluate(async (tid) => {
      const reply = await new Promise(r => chrome.runtime.sendMessage({ type: "GET_STATE", tabId: tid }, x => r(x)));
      const list = Array.isArray(reply?.endpoints) ? reply.endpoints
                 : (reply?.endpoints ? Object.values(reply.endpoints) : []);
      if (typeof checkEndpointRecord !== "function")
        throw new Error("popup.html did not supply lib/endpoint-record.js's checkEndpointRecord — this " +
                        "driver asserts every endpoint against the producer's own contract and will not " +
                        "read one it could not check");
      for (const ep of list) checkEndpointRecord(ep, `check_state.cjs reading GET_STATE for tab ${tid}`);
      return { ...reply, endpoints: list };
    }, t.id);
    const eps = st.endpoints;
    console.log(`-- tab ${t.id} endpoints (${eps.length}) --`);
    for (const e of eps.slice(0, 20)) console.log(`  ${e.method} ${e.url}  source=${e.source}`);
    const sf = Array.isArray(st?.securityFindings) ? st.securityFindings : [];
    console.log(`-- tab ${t.id} security findings (${sf.length}) --`);
    for (const f of sf.slice(0, 5)) {
      const sinks = f.securitySinks || [];
      for (const s of sinks.slice(0, 3)) console.log(`  ${s.type} sink=${s.sink} verdict=${s.verdict}`);
    }
  }
  await p.close();
  browser.disconnect();
})();
