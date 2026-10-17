// testing/psl_agreement.mjs — whether this tree's Public Suffix List answers agree with the browser's.
//
// The engine (engine/host/browser/core/url/public_suffix.c) and the native host's site oracle (engine/psl.mjs)
// answer registrable domains from Chromium's list at the tag engine/pslgen.mjs pins, with Chromium's matching.
// The extension's zone asks the running browser through chrome.publicSuffix (extension/background.js). This
// tool launches harness.js's Chrome with a throwaway extension holding the `publicSuffix` permission, asks
// it for every host built from both lists' rules (each rule, one label under it and two under it) and from a
// fixed list of hard hosts, and compares the engine's answer with the browser's. The host is serialized by the
// URL parser first, which is what every caller hands a site question.
//
// A disagreement is skew when the engine's matching over the browser's own list (Chromium's file at the
// browser's version tag) gives the browser's answer: the lists differ and the matching does not. Every other
// disagreement is a matching disagreement, and the tool exits 1 when there is one.
//
// Usage: node testing/psl_agreement.mjs [--json <out>]
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import * as table from "../engine/public_suffix_table.mjs";
import * as engine from "../engine/psl.mjs";
import { parseList } from "../engine/psl_list.mjs";

const require = createRequire(import.meta.url);
const { launchChrome } = require("./harness.js");
const REPO = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const LIST_PATH = "net/base/registry_controlled_domains/effective_tld_names.dat";
const jsonAt = process.argv.indexOf("--json");
if (process.argv.length !== 2 && !(process.argv.length === 4 && jsonAt === 2)) {
  console.error("usage: node testing/psl_agreement.mjs [--json <out>]");
  process.exit(2);
}

const HARD = ["www.example.co.uk", "co.uk", "EXAMPLE.COM", "example.com.", "github.io", "a.user.github.io",
  "kawasaki.jp", "www.city.kawasaki.jp", "www.ck", "a.b.ck", "s3.amazonaws.com", "a.compute.amazonaws.com",
  "192.168.1.1", "[::1]", "127.0.0.1", "localhost", "printer.internal-network", "foo.local", "foo.invalid",
  "a.b.c.example", "例え.テスト", "münchen.de", "bücher.münchen.de", "a.中国", "_dmarc.example.com"];

const engineHeader = fs.readFileSync(path.join(REPO, "engine/host/browser/core/url/public_suffix_table.h"), "utf8");
if (!engineHeader.includes("sha256 " + table.PSL_SHA256)) {
  console.error("the engine's C table and engine/public_suffix_table.mjs are not one list — re-run engine/pslgen.mjs");
  process.exit(1);
}

/* Chromium's list at the browser's version tag, parsed as pslgen parses the engine's. */
async function browserList(version) {
  const url = `https://chromium.googlesource.com/chromium/src/+/refs/tags/${version}/${LIST_PATH}?format=TEXT`;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const bytes = Buffer.from(await res.text(), "base64");
  return { sha256: createHash("sha256").update(bytes).digest("hex"), ...parseList(bytes.toString("utf8")) };
}

/* background.js's `_registrableDomain`, which is the answer the zone memoizes. */
const BROWSER_ANSWER = (hs) => hs.map((h) => {
  const ps = chrome.publicSuffix;
  if (ps.getKnownSuffix(h) === h) return null;
  const d = ps.getDomain(h, { allowUnknownSuffix: true });
  return d === null || (d === h && h.indexOf(".") < 0) ? null : d;
});

const ext = fs.mkdtempSync(path.join(os.tmpdir(), "psl-agreement-"));
fs.writeFileSync(path.join(ext, "manifest.json"), JSON.stringify({
  manifest_version: 3, name: "psl agreement", version: "1", permissions: ["publicSuffix"],
  background: { service_worker: "sw.js" } }));
fs.writeFileSync(path.join(ext, "sw.js"), "");

const browser = await launchChrome({ headless: true, pipe: true, enableExtensions: [ext], args: ["--no-sandbox"] });
let failed = false;
try {
  // This extension's worker, by its script: another chrome-extension:// worker target can come first.
  const swt = await browser.waitForTarget((t) => t.type() === "service_worker" &&
                                                 /^chrome-extension:\/\/[a-p]{32}\/sw\.js$/.test(t.url()));
  const sw = await swt.worker();
  const has = await sw.evaluate(() => typeof chrome.publicSuffix);
  const version = await sw.evaluate(async () =>
    (await navigator.userAgentData.getHighEntropyValues(["uaFullVersion"])).uaFullVersion);
  if (has !== "object") {
    console.error(version + " has no chrome.publicSuffix (typeof " + has + "); it exists from Chrome 153");
    process.exit(1);
  }
  const theirs = await browserList(version);
  const overTheirs = engine.suffixerOver(theirs);

  const hosts = new Set();
  const serialize = (h) => { try { return new URL("https://" + h + "/").hostname; } catch { return null; } };
  for (const h of HARD) { const s = serialize(h); if (s) hosts.add(s); }
  for (const t of [{ N: table.PSL_NORMAL, W: table.PSL_WILDCARD, E: table.PSL_EXCEPTION }, theirs])
    for (const r of [...t.N, ...t.W, ...t.E])
      for (const h of [r, "x." + r, "y.x." + r]) { const s = serialize(h); if (s) hosts.add(s); }
  const list = [...hosts];
  const answers = await sw.evaluate(`(${BROWSER_ANSWER})(${JSON.stringify(list)})`);

  const skew = [], matching = [];
  list.forEach((h, i) => {
    const ours = engine.registrableDomainOf(h);
    if (ours === answers[i]) return;
    const row = { host: h, engine: ours, browser: answers[i], engineOverBrowserList: overTheirs.registrableDomainOf(h) };
    (row.engineOverBrowserList === answers[i] ? skew : matching).push(row);
  });
  const out = { browser: version, engineList: table.PSL_VERSION, engineSha256: table.PSL_SHA256,
                browserList: "Chromium " + version, browserSha256: theirs.sha256, hosts: list.length,
                agree: list.length - skew.length - matching.length, skew, matching };
  console.log(`Chrome ${version}: ${out.agree} of ${list.length} hosts agree with the engine`);
  console.log(`  ${skew.length}  list-version skew: engine list ${table.PSL_VERSION} (sha256 ${table.PSL_SHA256}), ` +
              `browser list Chromium ${version} (sha256 ${theirs.sha256})`);
  console.log(`  ${matching.length}  matching disagreements (the engine's matching over the browser's own list ` +
              `disagrees with the browser)`);
  for (const r of [...matching, ...skew].slice(0, 60))
    console.log(`  ${r.host}: engine ${r.engine}, browser ${r.browser}, engine over the browser's list ` +
                `${r.engineOverBrowserList}`);
  if (jsonAt > 0) fs.writeFileSync(process.argv[jsonAt + 1], JSON.stringify(out, null, 1));
  failed = matching.length > 0;
} finally {
  await browser.close();
  fs.rmSync(ext, { recursive: true, force: true });
}
process.exit(failed ? 1 : 0);
