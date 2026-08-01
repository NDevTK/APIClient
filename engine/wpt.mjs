/* WPT — the BROWSER half's oracle, the twin of engine/test262.mjs.
 *
 * WHY THIS EXISTS. test262 is self-validating for the JS half: every file carries its own assertions, so a
 * failure means the engine got the SPEC answer wrong and nothing has to be maintained alongside it. The browser
 * half had no such thing — its only checks were hand-written probes and test_forced.c's fixture, which measure
 * what someone thought to ask. The Web Platform Tests are the same self-validating shape for DOM, Fetch, URL and
 * HTML: testharness.js carries the oracle, and a failure is a fidelity bug with the spec text attached.
 *
 * HOW A TEST IS RUN. A WPT file is an HTML document that loads /resources/testharness.js and then declares
 * tests. This harness INLINES those resources into the document rather than serving them, because a
 * parser-inserted <script src> is not fetched by this engine yet — that is a real gap, recorded here rather
 * than worked around invisibly: when it is built, the inlining goes away.
 *
 * HOW RESULTS COME BACK. The engine has one output channel a page can reach without any new API: the endpoints
 * it reports. The shim below turns each test result into a fetch of /wpt-result, so the results arrive in the
 * same document qjs_result already produces. Nothing is added to the ABI for testing. */
import { existsSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join, dirname, basename } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const WPT = join(ENGINE, ".work", "wpt");
const ABI = join(ENGINE, "..", "extension", "lib", "qjs", "qjs.mjs");

if (!existsSync(WPT)) {
  console.error("[wpt] checkout missing — clone it:\n" +
    "  git clone --filter=blob:none --sparse --depth 1 https://github.com/web-platform-tests/wpt.git engine/.work/wpt\n" +
    "  cd engine/.work/wpt && git sparse-checkout set resources dom/nodes dom/events url fetch/api/basic html/dom");
  process.exit(1);
}
if (!existsSync(ABI)) { console.error("[wpt] build the ABI first: node engine/build.mjs abi"); process.exit(1); }

const sub = process.argv[2] || "dom/nodes";
const dir = join(WPT, sub);
if (!existsSync(dir)) { console.error("[wpt] no such subdir: " + sub); process.exit(1); }

/* Inlined script text must not contain a literal `</script`, or the HTML parser ends the element in the middle
   of it and the rest of the file becomes markup — which is what a browser does too, and is why WPT serves these
   as separate files. Escaping is the inlining's business, not a workaround for the parser. */

/* The reporting shim, SERVED in place of /resources/testharnessreport.js. The engine has one output channel a
   page can reach without any new API — the endpoints it reports — so each result becomes a fetch and arrives in
   the document qjs_result already produces. Nothing is added to the ABI for testing. */
const REPORT = `
add_completion_callback(function(tests, status){
  for (var i = 0; i < tests.length; i++) {
    fetch("/wpt-result?i=" + i + "&s=" + tests[i].status + "&n=" + encodeURIComponent(tests[i].name) +
          "&m=" + encodeURIComponent(String(tests[i].message).slice(0, 200)));
  }
  fetch("/wpt-done?n=" + tests.length + "&s=" + status.status + "&m=" + encodeURIComponent(String(status.message).slice(0,200)));
});
`;

/* THE SERVER. WPT documents load their resources by URL, and this engine now fetches a <script src> the way a
   browser does — so the harness serves the checkout instead of inlining anything. Inlining was a workaround and
   it was also WRONG: testharness.js contains the sequences that put an HTML tokenizer into its script-data
   escaped states, so pasting it into a <script> element changes where the element ends. */
const serve = (url, testFile) => {
  const path = url.split("?")[0];
  if (path === "/resources/testharnessreport.js") return REPORT;
  const abs = path.startsWith("/") ? join(WPT, path.slice(1)) : join(WPT, dirname(testFile), path);
  try { return readFileSync(abs, "utf8"); } catch { return ""; }
};

const files = readdirSync(dir).filter((f) => f.endsWith(".html") && !f.includes("-ref.") && !f.includes("manual"));

const createQJS = (await import(ABI)).default;

let ran = 0, pass = 0, fail = 0, noresult = 0;
const gaps = new Map();          // distinct failure/abort message -> count
const noResultFiles = [];

for (const f of files) {
  const rel = join(sub, f);
  const html = readFileSync(join(WPT, rel), "utf8");

  let out = "", aborted = "";
  try {
    const M = await createQJS({ noInitialRun: true, print: () => {}, printErr: (l) => { if (!aborted) aborted = l; } });
    const cstr = (s) => { const n = M.lengthBytesUTF8(s || "") + 1, p = M._malloc(n); M.stringToUTF8(s || "", p, n); return p; };
    M.ccall("qjs_init", "number", ["number","number","number","number","number"],
            [cstr(""), cstr(html), cstr("https://wpt.test/" + rel), cstr(""), cstr("")]);
    M.ccall("qjs_begin", "void", ["string"], [""]);
    let n = 0, r;
    while ((r = M.ccall("qjs_step", "number", [], [])) !== 0 && ++n < 4000) {
      const pend = String(M.ccall("qjs_pending", "string", [], [])).split("\n").filter(Boolean);
      for (const u of pend) M.ccall("qjs_provide", "void", ["string","string"], [u, serve(u, rel)]);
    }
    out = M.ccall("qjs_result", "string", [], []);
  } catch (e) {
    aborted = aborted || String(e && e.message || e);
  }

  let doc = null;
  try { doc = JSON.parse(out); } catch { /* no document at all */ }
  const results = [];
  for (const site of (doc && doc.fetchCallSites) || []) {
    if (site.url !== "/wpt-result") continue;
    const byName = (k) => (site.params.find((p) => p.name === k) || { validValues: [] }).validValues;
    const st = byName("s"), nm = byName("n"), ms = byName("m");
    for (let i = 0; i < st.length; i++) results.push({ status: st[i], name: decodeURIComponent(nm[i] || ""), message: decodeURIComponent(ms[i] || "") });
  }
  if (!results.length) {
    noresult++;
    noResultFiles.push(f);
    /* The page's OWN uncaught errors are the real reason a document produced nothing: each names a capability
       the harness needed. They are the gap list. */
    const perrs = (doc && doc.pageErrors) || [];
    if (perrs.length) for (const e of perrs) gaps.set(e.slice(0, 160), (gaps.get(e.slice(0, 160)) || 0) + 1);
    else gaps.set((aborted || "no result, no abort and no page error").slice(0, 160),
                  (gaps.get((aborted || "no result, no abort and no page error").slice(0, 160)) || 0) + 1);
    continue;
  }
  ran++;
  for (const t of results) {
    if (t.status === "0") pass++;
    else { fail++; const key = (t.message || "(no message)").slice(0, 160); gaps.set(key, (gaps.get(key) || 0) + 1); }
  }
}

console.log("\n==================== WPT (" + sub + ") ====================");
console.log("  files: " + files.length + "  |  produced results: " + ran + "  |  produced NONE: " + noresult);
console.log("  assertions: " + pass + " pass, " + fail + " fail");
console.log("\n  THE GAP LIST — distinct reasons, most common first (this is the work queue):");
for (const [msg, n] of [...gaps.entries()].sort((a, b) => b[1] - a[1]).slice(0, 25))
  console.log("   " + String(n).padStart(4) + "x  " + msg);
console.log("===========================================================");
writeFileSync(join(ENGINE, ".work", "wpt-gaps.json"),
              JSON.stringify({ sub, files: files.length, ran, noresult, pass, fail,
                               gaps: [...gaps.entries()].sort((a,b)=>b[1]-a[1]), noResultFiles }, null, 1));
