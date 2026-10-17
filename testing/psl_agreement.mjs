// testing/psl_agreement.mjs — whether this tree's Public Suffix List answers agree with the browser's.
//
// The engine (engine/host/browser/core/url/public_suffix.c) and the native host's site oracle (engine/psl.mjs)
// answer public suffixes and registrable domains over one bundled list, which engine/pslgen.mjs writes into
// engine/host/browser/core/url/public_suffix_table.h and engine/public_suffix_table.mjs. The extension's zone
// asks the browser instead (extension/lib/site.js), which answers from its own list through chrome.publicSuffix
// (Chrome 153 and later, the `publicSuffix` permission, privileged extension contexts only). This tool launches
// the Chrome it is given with a throwaway extension holding that permission, asks both for every host it builds
// from the bundled list's own rules (each rule, one label under it and two under it) and from a fixed list of
// hard hosts, and prints each disagreement with the class it falls in. The host is serialized by the URL parser
// first, which is what every caller hands a site question.
//
// Usage: node testing/psl_agreement.mjs <chrome binary> [--json <out>]
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import * as table from "../engine/public_suffix_table.mjs";
import * as zone from "../engine/psl.mjs";

const require = createRequire(import.meta.url);
const puppeteer = require("puppeteer");
const REPO = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const chrome = process.argv[2];
const jsonAt = process.argv.indexOf("--json");
if (!chrome || !fs.existsSync(chrome)) {
  console.error("usage: node testing/psl_agreement.mjs <chrome binary> [--json <out>]");
  process.exit(2);
}

const HARD = ["www.example.co.uk", "co.uk", "EXAMPLE.COM", "example.com.", "github.io", "a.user.github.io",
  "kawasaki.jp", "www.city.kawasaki.jp", "www.ck", "a.b.ck", "s3.amazonaws.com", "a.compute.amazonaws.com",
  "192.168.1.1", "[::1]", "127.0.0.1", "localhost", "printer.internal-network", "foo.local", "foo.invalid",
  "a.b.c.example", "例え.テスト", "münchen.de", "bücher.münchen.de", "a.中国", "_dmarc.example.com"];

const engineHeader = fs.readFileSync(path.join(REPO, "engine/host/browser/core/url/public_suffix_table.h"), "utf8");
if (!engineHeader.includes("commit " + table.PSL_COMMIT)) {
  console.error("the engine's C table and engine/public_suffix_table.mjs are not one PSL commit — re-run " +
                "engine/pslgen.mjs");
  process.exit(1);
}

const hosts = new Set();
const serialize = (h) => { try { return new URL("https://" + h + "/").hostname; } catch { return null; } };
for (const h of HARD) { const s = serialize(h); if (s) hosts.add(s); }
for (const r of [...table.PSL_NORMAL, ...table.PSL_WILDCARD, ...table.PSL_EXCEPTION])
  for (const h of [r, "x." + r, "y.x." + r]) { const s = serialize(h); if (s) hosts.add(s); }
const list = [...hosts];

const ext = fs.mkdtempSync(path.join(os.tmpdir(), "psl-agreement-"));
fs.writeFileSync(path.join(ext, "manifest.json"), JSON.stringify({
  manifest_version: 3, name: "psl agreement", version: "1", permissions: ["publicSuffix"],
  background: { service_worker: "sw.js" } }));
fs.writeFileSync(path.join(ext, "sw.js"), "");

const browser = await puppeteer.launch({ executablePath: chrome, headless: true, pipe: true,
  enableExtensions: [ext], args: ["--no-sandbox"] });
let failed = false;
try {
  const swt = await browser.waitForTarget((t) => t.type() === "service_worker" && t.url().startsWith("chrome-extension://"));
  const sw = await swt.worker();
  const version = await browser.version();
  const has = await sw.evaluate(() => typeof chrome.publicSuffix);
  if (has !== "object") {
    console.error(version + " has no chrome.publicSuffix (typeof " + has + "); it exists from Chrome 153");
    process.exit(1);
  }
  const browserAnswers = await sw.evaluate((hs) => hs.map((h) => [
    chrome.publicSuffix.getKnownSuffix(h), chrome.publicSuffix.getDomain(h),
    chrome.publicSuffix.getDomain(h, { allowUnknownSuffix: true })]), list);
  // Which way each disagreement goes, so a reader sees the shapes rather than a count.
  const wildcards = new Set(table.PSL_WILDCARD);
  const classes = {};
  const rows = [];
  list.forEach((h, i) => {
    const [known, domain, domainUnknown] = browserAnswers[i];
    const zs = zone.publicSuffixOf(h), zd = zone.registrableDomainOf(h);
    if (zd === domain) return;
    const cls = known !== null && wildcards.has(known) && h === known
                ? "the browser holds a wildcard rule's parent as a public suffix (the PSL Format's match needs " +
                  "\"as many or more labels than the rule\")"
              : known === null && zd === domainUnknown ? "unknown-tld (the PSL algorithm's \"*\" rule; the browser's default " +
                                                         "is Note 2's reading, no registrable domain)"
              : known !== null && known.length > (zs || "").length ? "the browser matches a longer rule than this snapshot"
              : known !== null && known.length < (zs || "").length ? "this snapshot matches a longer rule than the browser (a rule the browser's list lacks)"
              : "other";
    classes[cls] = (classes[cls] || 0) + 1;
    rows.push({ host: h, zoneSuffix: zs, zoneDomain: zd, browserSuffix: known, browserDomain: domain, cls });
  });
  const out = { version, pslVersion: table.PSL_VERSION, pslCommit: table.PSL_COMMIT, hosts: list.length,
                agree: list.length - rows.length, disagree: rows.length, classes, rows };
  console.log(`${version} against PSL ${table.PSL_VERSION}: ${out.agree} of ${list.length} hosts agree, ` +
              `${rows.length} disagree`);
  for (const [c, n] of Object.entries(classes)) console.log(`  ${n}  ${c}`);
  for (const r of rows.filter((x) => !x.cls.startsWith("unknown-tld")).slice(0, 60))
    console.log(`  ${r.host}: zone ${r.zoneSuffix} / ${r.zoneDomain}, browser ${r.browserSuffix} / ${r.browserDomain}`);
  if (jsonAt > 0) fs.writeFileSync(process.argv[jsonAt + 1], JSON.stringify(out, null, 1));
  failed = rows.length > 0;
} finally {
  await browser.close();
  fs.rmSync(ext, { recursive: true, force: true });
}
process.exit(failed ? 1 : 0);
