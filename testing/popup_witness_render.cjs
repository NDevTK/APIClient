// testing/popup_witness_render.cjs — the Endpoints panel's witness cell, checked by a render differential in
// the shipped popup.
//
// It loads the unpacked extension in a headless Chrome of its own, opens popup.html (Trusted Types enforced
// with no policy), and renders lib/popup-endpoints.js's `endpointRow` for method records that differ only in
// `_astWitnesses`. Each row is serialized as its element tree (tag, class, text). Expectations:
//   differential       two records differing only in the pool differ in the Assumed cell and nowhere else;
//   expected text      each case's Assumed cell reads exactly as stated below, verified and unverified apart;
//   invented negative  one record rendered twice serializes identically;
//   armed control      a decision spelled as markup becomes text and creates no element;
//   Trusted Types      the renders raise no violation.
// Exit status 0 is every expectation met.
//
//   node testing/popup_witness_render.cjs [extension-dir]
"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const puppeteer = require("puppeteer");

const EXT = path.resolve(process.argv[2] || path.join(__dirname, "..", "extension"));
const ASSUMED_CELL = 4;   // Method, Address, Razor, Provenance, Assumed, Parameters, Send

const MARKUP = "<img src=x onerror=alert(1)>";
const CASES = [
  { name: "verified forced pin", pool: [{ provenance: "forced", verification: "verified",
      assumed: [{ source: "location.hash", value: "#admin", kind: "string" }] }],
    text: 'forced verified found by assuming location.hash = "#admin"' },
  { name: "candidate minted, not yet observed", pool: [{ provenance: "forced", verification: "candidate",
      assumed: [{ source: "location.hash", value: "#admin", kind: "string" }] }],
    text: 'forced unverified found by assuming location.hash = "#admin"' },
  { name: "non-string pin is not quoted", pool: [{ provenance: "forced", verification: "no-candidate",
      assumed: [{ source: "{c}.n", value: "0", kind: "number" }] }],
    text: "forced unverified found by assuming {c}.n = 0" },
  { name: "decision and completion", pool: [{ provenance: "derived", verification: "no-candidate", assumed: [
      { decision: '{location.hash}.startsWith("#x")', taken: true }, { decision: "JSON.parse({r})", completion: 1 }] }],
    text: 'derived unverified found by assuming {location.hash}.startsWith("#x") \u2192 true; JSON.parse({r}) \u2192 completion 1' },
  { name: "empty forced sighting is the residual", pool: [{ provenance: "forced", verification: "no-candidate", assumed: [] }],
    text: "forced assumption not spellable" },
  { name: "sightings never merged, verified kept apart", pool: [
      { provenance: "forced", verification: "verified", assumed: [{ decision: "{a}>3", taken: true }] },
      { provenance: "forced", verification: "no-candidate", assumed: [{ decision: "{b}>3", taken: false }] }],
    text: "forced verified found by assuming {a}>3 \u2192 true|forced unverified found by assuming {b}>3 \u2192 false" },
  { name: "unforced empty sighting assumed nothing", pool: [{ provenance: "derived", verification: "no-candidate", assumed: [] }],
    text: "nothing assumed" },
  { name: "unasked sighting", pool: [{ provenance: "derived", verification: null, assumed: null }],
    text: "derived no flow stood at this sighting; not asked" },
  { name: "no pool from an older engine", pool: undefined, text: "unknown" },
  { name: "markup is text", pool: [{ provenance: "forced", verification: "no-candidate", assumed: [{ decision: MARKUP, taken: true }] }],
    text: "forced unverified found by assuming " + MARKUP + " \u2192 true", markup: true },
];

// Runs in the page: each case's row serialized, the Assumed cell's line texts, and the element count of img.
function renderCases(cases, cellIndex) {
  const ser = (n) => n.nodeType === 3 ? JSON.stringify(n.data)
    : "<" + n.tagName.toLowerCase() + (n.className ? "." + n.className.replace(/ /g, ".") : "") + ">[" +
      Array.from(n.childNodes).map(ser).join(",") + "]";
  tabData = { endpoints: {}, discoveryDocs: {} };
  const record = (pool) => {
    const m = { id: "svc.probe.get", httpMethod: "GET", path: "api/probe", request: null,
                _astInferred: true, _astProvenance: "forced" };
    if (pool !== undefined) m._astWitnesses = pool;
    return m;
  };
  const out = [];
  for (const c of cases) {
    const row = endpointRow("svc", { schemas: {} }, record(c.pool));
    const again = endpointRow("svc", { schemas: {} }, record(c.pool));
    const bare = endpointRow("svc", { schemas: {} },
                             record([{ provenance: "derived", verification: "no-candidate", assumed: [] }]));
    const cells = Array.from(row.children).map(ser);
    const bareCells = Array.from(bare.children).map(ser);
    const cell = row.children[cellIndex];
    const lines = cell.querySelector("ul") ? Array.from(cell.querySelectorAll("li")).map((li) => li.textContent)
                                           : [cell.textContent];
    out.push({ name: c.name, lines: lines.join("|"), same: ser(row) === ser(again),
               differing: cells.map((s, i) => s === bareCells[i] ? -1 : i).filter((i) => i >= 0),
               imgs: row.querySelectorAll("img").length });
  }
  return out;
}

(async () => {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "popup-witness-"));
  const root = typeof process.getuid === "function" && process.getuid() === 0;
  const browser = await puppeteer.launch({ headless: true, pipe: true, enableExtensions: [EXT], userDataDir: profile,
                                           args: ["--no-first-run", "--no-default-browser-check",
                                                  ...(root ? ["--no-sandbox"] : [])] });
  const failures = [];
  try {
    let sw = null;
    for (let i = 0; i < 100 && !sw; i++) {
      sw = browser.targets().find((t) => t.type() === "service_worker" && t.url().startsWith("chrome-extension://"));
      if (!sw) await new Promise((r) => setTimeout(r, 100));
    }
    if (!sw) throw new Error("the extension's service worker never started from " + EXT);
    const extId = new URL(sw.url()).host;
    const page = await browser.newPage();
    const violations = [], errors = [];
    page.on("console", (m) => { if (/TrustedHTML|Trusted Type/.test(m.text())) violations.push(m.text()); });
    page.on("pageerror", (e) => errors.push(String(e.message)));
    await page.goto(`chrome-extension://${extId}/popup.html`, { waitUntil: "load" });
    await new Promise((r) => setTimeout(r, 1500));
    const before = violations.length;
    const res = await page.evaluate(renderCases, CASES.map((c) => ({ name: c.name, pool: c.pool })), ASSUMED_CELL);
    for (let i = 0; i < CASES.length; i++) {
      const c = CASES[i], r = res[i];
      console.log(JSON.stringify(r));
      if (r.lines !== c.text) failures.push(`${c.name}: Assumed cell reads ${JSON.stringify(r.lines)}, expected ${JSON.stringify(c.text)}`);
      if (!r.same) failures.push(`${c.name}: invented negative control — one record rendered twice differs`);
      const expectDiff = c.text === "nothing assumed" ? [] : [ASSUMED_CELL];
      if (JSON.stringify(r.differing) !== JSON.stringify(expectDiff))
        failures.push(`${c.name}: differential — cells ${JSON.stringify(r.differing)} differ from the bare record, expected ${JSON.stringify(expectDiff)}`);
      if (r.imgs !== 0) failures.push(`${c.name}: armed control — markup in a witness created ${r.imgs} img element(s)`);
    }
    for (const v of violations.slice(before)) failures.push("render raised a Trusted Types violation: " + v);
    // renderCases is synchronous, so a DCHECK in the render rejects the evaluate above. The popup's own load,
    // opened outside a tab, raises unrelated asserts of its own; only a witness one is this test's.
    for (const e of errors) if (/witness/i.test(e)) failures.push("page error: " + e);
    await page.close();
  } finally {
    await browser.close();
    fs.rmSync(profile, { recursive: true, force: true });
  }
  for (const f of failures) console.log("FAIL " + f);
  console.log(failures.length ? `popup_witness_render: ${failures.length} failure(s)` : "popup_witness_render: ok");
  process.exit(failures.length ? 1 : 0);
})().catch((e) => { console.error("popup_witness_render: FATAL", e); process.exit(2); });
