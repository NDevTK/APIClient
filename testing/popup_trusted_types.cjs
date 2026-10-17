// testing/popup_trusted_types.cjs — the popup's no-HTML-string guard, checked in the shipped pages.
//
// popup.html and dev.html carry `require-trusted-types-for 'script'; trusted-types 'none'`, so every HTML
// sink throws when handed a string and page-derived text can reach those documents only through
// lib/dom-builder.js. This loads the unpacked extension in a headless Chrome of its own (no harness lock, no
// engine build needed) and, in each page:
//   armed controls     every HTML sink handed a string must throw a TypeError naming TrustedHTML;
//   negative controls  text and attribute writes and dom-builder.js must still work;
// and in an about:blank tab, which carries no policy, the same sinks must succeed, so a probe that could
// only ever report "threw" is caught. It also fails on any Trusted Types violation the page's own load
// raised. Exit status 0 is every expectation met.
//
//   node testing/popup_trusted_types.cjs [extension-dir]
"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const { launchChrome } = require("./harness.js");

const EXT = path.resolve(process.argv[2] || path.join(__dirname, "..", "extension"));
const PAGES = ["popup.html", "dev.html"];

// Runs in the page: each sink's outcome, "allowed" or the thrown error's name and message.
function probeSinks() {
  const out = {};
  const probe = (name, fn) => {
    try { fn(); out[name] = "allowed"; } catch (e) { out[name] = e.name + ": " + e.message; }
  };
  const host = document.createElement("div");
  probe("innerHTML", () => { host.innerHTML = "<b>x</b>"; });
  probe("innerHTML empty", () => { host.innerHTML = ""; });
  probe("outerHTML", () => { const c = document.createElement("span"); host.appendChild(c); c.outerHTML = "<b>x</b>"; });
  probe("insertAdjacentHTML", () => { host.insertAdjacentHTML("beforeend", "<b>x</b>"); });
  probe("DOMParser", () => { new DOMParser().parseFromString("<b>x</b>", "text/html"); });
  probe("createContextualFragment", () => { document.createRange().createContextualFragment("<b>x</b>"); });
  probe("document.write", () => { document.implementation.createHTMLDocument("").write("<b>x</b>"); });
  return out;
}

function probeText() {
  const out = {};
  const probe = (name, fn) => {
    try { fn(); out[name] = "allowed"; } catch (e) { out[name] = e.name + ": " + e.message; }
  };
  const host = document.createElement("div");
  probe("textContent", () => { host.textContent = "<b>x</b>"; });
  probe("setAttribute", () => { host.setAttribute("title", "<b>x</b>"); });
  probe("domElement", () => { host.appendChild(domElement("span", { class: "c" }, ["<b>x</b>"])); });
  return out;
}

(async () => {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "popup-tt-"));
  // Chrome refuses to start its sandbox as root, which is how the cloud containers run.
  const root = typeof process.getuid === "function" && process.getuid() === 0;
  const browser = await launchChrome({ headless: true, pipe: true, enableExtensions: [EXT], userDataDir: profile,
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

    for (const p of PAGES) {
      const page = await browser.newPage();
      const violations = [];
      page.on("console", (m) => { if (/TrustedHTML|Trusted Type/.test(m.text())) violations.push(m.text()); });
      page.on("pageerror", (e) => { if (/TrustedHTML|Trusted Type/.test(String(e.message))) violations.push(String(e.message)); });
      await page.goto(`chrome-extension://${extId}/${p}`, { waitUntil: "load" });
      await new Promise((r) => setTimeout(r, 1500));   // let the page's own first render run
      const loadViolations = violations.slice();
      const sinks = await page.evaluate(probeSinks);
      const text = await page.evaluate(probeText);
      for (const [k, v] of Object.entries(sinks))
        if (!/^TypeError: .*TrustedHTML/.test(v)) failures.push(`${p}: armed control ${k} was not refused (${v})`);
      for (const [k, v] of Object.entries(text))
        if (v !== "allowed") failures.push(`${p}: negative control ${k} failed (${v})`);
      for (const v of loadViolations) failures.push(`${p}: its own load raised a Trusted Types violation: ${v}`);
      console.log(p + " sinks " + JSON.stringify(sinks));
      console.log(p + " text  " + JSON.stringify(text));
      console.log(p + " load violations " + loadViolations.length);
      await page.close();
    }

    const blank = await browser.newPage();
    const open = await blank.evaluate(probeSinks);
    for (const [k, v] of Object.entries(open))
      if (v !== "allowed") failures.push(`about:blank: invented negative control ${k} did not succeed (${v})`);
    console.log("about:blank sinks " + JSON.stringify(open));
  } finally {
    await browser.close();
    fs.rmSync(profile, { recursive: true, force: true });
  }
  for (const f of failures) console.log("FAIL " + f);
  console.log(failures.length ? `popup_trusted_types: ${failures.length} failure(s)` : "popup_trusted_types: ok");
  process.exit(failures.length ? 1 : 0);
})().catch((e) => { console.error("popup_trusted_types: FATAL", e); process.exit(2); });
