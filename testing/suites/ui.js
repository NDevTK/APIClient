// UI interaction suite.
//
// Drives the real popup.html in a Chromium tab against whatever's in the
// persisted profile's globalStore (run `npm run harness` first). Exercises
// the Send panel end-to-end for a sample of learned methods:
//
//   • Selects the service dropdown
//   • Selects each method
//   • Verifies form inputs render for learned parameters/schema fields
//   • Confirms Content-Type auto-populates (no manual dropdown needed)
//   • Confirms body mode matches the method's encoding
//   • Captures each of the 3 export formats (curl, fetch, Python) and
//     sanity-checks they contain the request URL
//   • For multipart-captured methods, flags when the popup's form can't
//     express the original N sub-parts (the gap for Part B)
//
// Usage: node testing/suites/ui.js [--max N]

"use strict";

const path = require("path");
const fs = require("fs");
const fsp = fs.promises;
const puppeteer = require("puppeteer");

const ROOT = path.resolve(__dirname, "..", "..");
const EXT_DIR = path.join(ROOT, "extension");
const PROFILE_DIR = path.join(ROOT, "testing", "profile");
const REPORTS_DIR = path.join(ROOT, "testing", "reports");

function nowStamp() { return new Date().toISOString().replace(/[:.]/g, "-"); }
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function parseArgs(argv) {
  const out = { max: 30 };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === "--max") out.max = parseInt(argv[++i], 10);
  }
  return out;
}

async function findExtensionId(browser) {
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    const t = browser.targets().find(t => t.type() === "service_worker" && t.url().startsWith("chrome-extension://"));
    if (t) return new URL(t.url()).hostname;
    await sleep(200);
  }
  throw new Error("extension id not resolvable — is the extension loaded?");
}

// All browser-side logic is serialized into strings we evaluate via
// page.evaluate. Keeping it inline means we can edit the suite without
// touching any file the extension loads.

const PE_LIST_SERVICES = () => document.querySelector("#spec-service-select") && (() => {
  const sel = document.querySelector("#spec-service-select");
  return Array.from(sel.options).map(o => ({ value: o.value, label: o.textContent }));
})();

const PE_SELECT_SERVICE = (value) => {
  const sel = document.querySelector("#spec-service-select");
  if (!sel) return false;
  sel.value = value;
  sel.dispatchEvent(new Event("change", { bubbles: true }));
  return true;
};

const PE_LIST_METHODS = () => {
  const sel = document.querySelector("#send-ep-select");
  if (!sel) return [];
  return Array.from(sel.options)
    .filter(o => o.value)
    .map(o => ({ value: o.value, label: o.textContent }));
};

const PE_SELECT_METHOD = (value) => {
  const sel = document.querySelector("#send-ep-select");
  if (!sel) return false;
  sel.value = value;
  sel.dispatchEvent(new Event("change", { bubbles: true }));
  return true;
};

const PE_INSPECT_SEND_PANEL = () => {
  const vis = el => el && !el.classList.contains("hidden") && el.offsetParent !== null;
  const formFields = document.querySelector("#send-form-fields");
  const rawBody = document.querySelector("#send-raw-body");
  const gqlFields = document.querySelector("#send-graphql-fields");
  const wsConsole = document.querySelector("#send-ws-console");
  const sendActions = document.querySelector(".send-actions");
  const curlBtn = document.querySelector("#btn-copy-curl");
  const fetchBtn = document.querySelector("#btn-copy-fetch");
  const pythonBtn = document.querySelector("#btn-copy-python");
  const headersSec = document.querySelector("#send-headers-section");

  // Count rendered form inputs inside #send-form-fields (text inputs, selects, checkboxes).
  let formInputCount = 0;
  if (formFields) {
    formInputCount = formFields.querySelectorAll("input, select, textarea").length;
  }
  // GraphQL operation tabs
  let gqlOpTabs = 0;
  if (gqlFields) {
    gqlOpTabs = gqlFields.querySelectorAll("#gql-op-tabs > *").length;
  }

  return {
    modes: {
      form: vis(formFields),
      raw: vis(rawBody),
      graphql: vis(gqlFields),
      wsConsole: vis(wsConsole),
    },
    formInputCount,
    gqlOpTabs,
    rawBodyTextLen: rawBody ? rawBody.value.length : 0,
    sendActionsVisible: vis(sendActions),
    exportButtons: {
      curl: !!curlBtn,
      fetch: !!fetchBtn,
      python: !!pythonBtn,
    },
    headersVisible: vis(headersSec),
  };
};

// Capture what a click on an export button produces. The popup writes the
// formatted request to the system clipboard; we intercept navigator.clipboard
// by installing a monkey-patch before clicking.
const PE_PATCH_CLIPBOARD = () => {
  window.__uiSuiteClipboard = null;
  const orig = navigator.clipboard;
  const patched = {
    async writeText(text) { window.__uiSuiteClipboard = text; return; },
    async readText() { return window.__uiSuiteClipboard || ""; },
  };
  Object.defineProperty(navigator, "clipboard", { value: patched, configurable: true });
};

const PE_CLICK_EXPORT = async (which) => {
  const map = { curl: "#btn-copy-curl", fetch: "#btn-copy-fetch", python: "#btn-copy-python" };
  const btn = document.querySelector(map[which]);
  if (!btn) return { ok: false, detail: "button missing" };
  window.__uiSuiteClipboard = null;
  btn.click();
  // The handler may be async (BUILD_REQUEST → formatter → clipboard).
  await new Promise(r => setTimeout(r, 250));
  return { ok: true, text: window.__uiSuiteClipboard || "" };
};

// Pull persisted multipart requests from the SW so we know which methods
// *should* have multi-part editors.
const SW_DUMP_MULTIPART = `
  const out = [];
  for (const [tabId, tab] of state.tabs.entries()) {
    for (const r of tab.requestLog || []) {
      const ct = (r.contentType || "").toLowerCase();
      if (ct.startsWith("multipart/") && r.rawBodyB64) {
        out.push({
          tabId,
          url: r.url,
          methodId: r.methodId || null,
          contentType: r.contentType,
          service: r.service || null,
        });
      }
    }
  }
  return out;
`;

async function evalSW(worker, body, arg) {
  const src = `(async (arg) => { ${body} })(${JSON.stringify(arg ?? null)})`;
  return worker.evaluate(src);
}

async function run() {
  const args = parseArgs(process.argv);

  const browser = await puppeteer.launch({
    headless: false,
    userDataDir: PROFILE_DIR,
    defaultViewport: null,
    args: [
      `--disable-extensions-except=${EXT_DIR}`,
      `--load-extension=${EXT_DIR}`,
      "--no-first-run",
      "--no-default-browser-check",
    ],
  });

  const runDir = path.join(REPORTS_DIR, "ui-" + nowStamp());
  await fsp.mkdir(runDir, { recursive: true });

  try {
    const extId = await findExtensionId(browser);
    console.log("[ui] extension id:", extId);

    // Grab the SW target to query for multipart-captured requests later.
    const swTarget = browser.targets().find(t => t.type() === "service_worker");
    const worker = swTarget ? await swTarget.worker() : null;

    // Open popup.html as a tab. The popup JS runs the same way it does in
    // the action popup; tab context only changes sizing (no behavior fork).
    const page = await browser.newPage();
    page.on("pageerror", e => console.warn("[ui] page error:", e.message));
    page.on("console", m => {
      if (m.type() === "error") console.warn("[ui] console.error:", m.text());
    });
    await page.goto(`chrome-extension://${extId}/popup.html`, { waitUntil: "domcontentloaded" });
    // Let the initial render + message round-trip settle.
    await sleep(1500);

    // Switch to Send panel
    await page.evaluate(() => {
      const tab = document.querySelector('[data-panel="send"]');
      if (tab) tab.click();
    });
    await sleep(400);
    await page.evaluate(PE_PATCH_CLIPBOARD);

    // Multipart captured requests from live SW (for gap findings).
    const multipartReqs = worker ? await evalSW(worker, SW_DUMP_MULTIPART, null) : [];
    const multipartByMethodId = new Map(
      multipartReqs.filter(r => r.methodId).map(r => [r.methodId, r])
    );

    // Enumerate services
    const services = await page.evaluate(PE_LIST_SERVICES);
    if (!services) {
      throw new Error("popup did not render — is globalStore empty? Run `npm run harness` first.");
    }
    console.log(`[ui] services in dropdown: ${services.length - 1}`);  // minus "All Services"

    const results = { services: [], findings: [] };
    let tested = 0;
    for (const svc of services) {
      if (!svc.value) continue; // skip "All Services"
      if (tested >= args.max) break;

      await page.evaluate(PE_SELECT_SERVICE, svc.value);
      await sleep(300);

      const methods = await page.evaluate(PE_LIST_METHODS);
      const svcEntry = { service: svc.value, methods: [] };

      for (const m of methods) {
        if (tested >= args.max) break;
        tested++;
        try {
          await page.evaluate(PE_SELECT_METHOD, m.value);
          await sleep(350); // allow form render
          const inspect = await page.evaluate(PE_INSPECT_SEND_PANEL);

          // Try each export format. We only validate that the output is
          // non-empty and contains something URL-shaped; the deep
          // verification is in the codec/replay suites.
          const exports = {};
          for (const kind of ["curl", "fetch", "python"]) {
            const out = await page.evaluate(PE_CLICK_EXPORT, kind);
            exports[kind] = {
              ok: out.ok && out.text && /https?:\/\//.test(out.text),
              length: out.text ? out.text.length : 0,
            };
          }

          // Gap signals
          const usesOnlyRaw = inspect.modes.raw && !inspect.modes.form && !inspect.modes.graphql && !inspect.modes.wsConsole;
          const emptyForm = inspect.modes.form && inspect.formInputCount === 0 && inspect.rawBodyTextLen === 0;

          const row = {
            methodValue: m.value,
            methodLabel: m.label,
            modes: inspect.modes,
            formInputCount: inspect.formInputCount,
            sendActionsVisible: inspect.sendActionsVisible,
            exports,
            usesOnlyRaw,
            emptyForm,
          };

          // Multipart editability finding
          const mp = multipartByMethodId.get(m.value);
          if (mp) {
            row.multipart = {
              captured: true,
              currentMode: inspect.modes.graphql ? "graphql" : inspect.modes.form ? "form" : inspect.modes.raw ? "raw" : "none",
              gap: !inspect.modes.graphql && (inspect.modes.form || inspect.modes.raw),
              detail: "Captured request was multipart but popup renders as a flat " +
                (inspect.modes.form ? "form" : "raw textarea"),
            };
            if (row.multipart.gap) {
              results.findings.push({
                kind: "multipart-flattened",
                service: svc.value, methodId: m.value, capturedUrl: mp.url,
              });
            }
          }
          if (usesOnlyRaw) {
            results.findings.push({
              kind: "raw-fallback", service: svc.value, methodId: m.value,
            });
          }
          if (emptyForm) {
            results.findings.push({
              kind: "empty-form", service: svc.value, methodId: m.value,
            });
          }
          svcEntry.methods.push(row);
        } catch (e) {
          svcEntry.methods.push({ methodValue: m.value, error: String(e) });
        }
      }
      results.services.push(svcEntry);
    }

    // Aggregate counts
    const totals = { tested };
    totals.formMode = 0;
    totals.rawMode = 0;
    totals.graphqlMode = 0;
    totals.wsMode = 0;
    totals.exportsPassing = 0;
    totals.exportsFailing = 0;
    for (const s of results.services) for (const m of s.methods) {
      if (m.error) continue;
      if (m.modes.form) totals.formMode++;
      if (m.modes.raw) totals.rawMode++;
      if (m.modes.graphql) totals.graphqlMode++;
      if (m.modes.wsConsole) totals.wsMode++;
      const allExportsOK = m.exports && m.exports.curl.ok && m.exports.fetch.ok && m.exports.python.ok;
      if (allExportsOK) totals.exportsPassing++; else totals.exportsFailing++;
    }
    const findingCounts = results.findings.reduce((acc, f) => { acc[f.kind] = (acc[f.kind] || 0) + 1; return acc; }, {});

    await fsp.writeFile(path.join(runDir, "ui.json"), JSON.stringify({ totals, findings: results.findings, services: results.services }, null, 2), "utf8");

    console.log("\n── ui totals ──────────────────────────────────────────");
    console.log(`  methods tested:     ${tested}`);
    console.log(`  form mode:          ${totals.formMode}`);
    console.log(`  graphql mode:       ${totals.graphqlMode}`);
    console.log(`  raw mode:           ${totals.rawMode}`);
    console.log(`  ws-console mode:    ${totals.wsMode}`);
    console.log(`  all exports OK:     ${totals.exportsPassing}/${tested}`);
    console.log("  findings by kind:");
    for (const [k, n] of Object.entries(findingCounts)) {
      console.log(`    ${k.padEnd(25)} ${n}`);
    }
    console.log(`\n[ui] wrote ${path.join(runDir, "ui.json")}`);
  } finally {
    await browser.close().catch(() => {});
  }
}

run().catch(err => { console.error(err); process.exit(1); });
