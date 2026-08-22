// Close non-github tabs via puppeteer.connect to the running harness.
// Lock file path: ~/.harness-lock per the harness convention. The
// browser exposes a CDP port read from the lock.
var puppeteer = require("puppeteer");
var fs = require("fs");
var path = require("path");
var os = require("os");

async function readLock() {
  var lockPath = path.join(__dirname, "harness.lock");
  try { return JSON.parse(fs.readFileSync(lockPath, "utf8")); }
  catch (_) { return null; }
}

(async () => {
  var lock = await readLock();
  var port = (lock && lock.port) || 9222;
  var browser = await puppeteer.connect({
    browserURL: "http://127.0.0.1:" + port,
    defaultViewport: null,
    targetFilter: function (target) { return target.type() !== "browser"; },
    protocolTimeout: 60000,
  });
  var pages = await browser.pages();
  for (var i = 0; i < pages.length; i++) {
    var url = pages[i].url();
    if (url.startsWith("chrome-extension://") || url.startsWith("devtools://")) continue;
    if (url.startsWith("https://github.com/") || url === "about:blank") continue;
    console.log("closing: " + url);
    try { await pages[i].close(); } catch (e) { console.log("close failed: " + e.message); }
  }
  await browser.disconnect();
  console.log("done");
})();
