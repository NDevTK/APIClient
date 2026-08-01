/* WPT — the BROWSER half's oracle, the twin of engine/test262.mjs.
 *
 * WHY THIS EXISTS. test262 is self-validating for the JS half: every file carries its own assertions, so a
 * failure means the engine got the SPEC answer wrong and nothing has to be maintained alongside it. The browser
 * half had no such thing — its only checks were hand-written probes and test_forced.c's fixture, which measure
 * what someone thought to ask. The Web Platform Tests are the same self-validating shape for DOM, Fetch, URL and
 * HTML: testharness.js carries the oracle, and a failure is a fidelity bug with the spec text attached.
 *
 * HOW A TEST IS RUN. A WPT file is an HTML document that loads /resources/testharness.js and then declares
 * tests. Each document runs in its OWN PROCESS, holding its OWN engine — the architecture's "one WASM instance
 * per DOCUMENT" taken literally. That is not a convenience: a shared process accumulated one never-released
 * 1 GiB-capable instance per file (2.3 GB resident by the end of one directory, most of the wall clock spent in
 * GC), and any document that aborted — a wasm trap, a DCHECK, a sanitizer report — took the whole run with it.
 * One process per document frees the engine with the process, isolates every abort to the file that caused it,
 * and lets documents run concurrently.
 *
 * HOW RESULTS COME BACK. The engine has one output channel a page can reach without any new API: the endpoints
 * it reports. The shim below turns each test result into a fetch of /wpt-result, so the results arrive in the
 * same document qjs_result already produces. Nothing is added to the ABI for testing. */
import { existsSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { fork } from "node:child_process";
import { cpus } from "node:os";

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
/* How many qjs_step calls the harness pumps before it gives up on a document. This is the HARNESS's limit, not
   the engine's — the engine has none, and a document that hits this is REPORTED as unfinished rather than
   counted as silent. Raise it when a document legitimately needs more; never read it as "the engine stops here". */
const STEP_LIMIT = 4000;
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

/* ---- THE CHILD: one document, one engine, one process. It prints ONE @WPT line and exits. ---- */
const ONE = process.argv[3] === "--one" ? process.argv[4] : null;
if (ONE) {
  const html = readFileSync(join(WPT, ONE), "utf8");
  const createQJS = (await import(ABI)).default;
  let out = "", aborted = "", unfinished = 0;
  const emit = () => {
    let doc = null;
    try { doc = JSON.parse(out); } catch { /* no document at all */ }
    const results = [];
    for (const site of (doc && doc.fetchCallSites) || []) {
      if (site.url !== "/wpt-result") continue;
      const byName = (k) => (site.params.find((p) => p.name === k) || { validValues: [] }).validValues;
      const st = byName("s"), nm = byName("n"), ms = byName("m");
      for (let i = 0; i < st.length; i++)
        results.push({ status: st[i], name: decodeURIComponent(nm[i] || ""), message: decodeURIComponent(ms[i] || "") });
    }
    process.stdout.write("@WPT " + JSON.stringify({ results, pageErrors: (doc && doc.pageErrors) || [], aborted, unfinished }) + "\n");
  };
  try {
    const M = await createQJS({ noInitialRun: true, print: () => {}, printErr: (l) => { if (!aborted) aborted = String(l); } });
    const cstr = (s2) => { const n = M.lengthBytesUTF8(s2 || "") + 1, p2 = M._malloc(n); M.stringToUTF8(s2 || "", p2, n); return p2; };
    M.ccall("qjs_init", "number", ["number","number","number","number","number"],
            [cstr(""), cstr(html), cstr("https://wpt.test/" + ONE), cstr(""), cstr("")]);
    M.ccall("qjs_begin", "void", ["string"], [""]);
    /* The ENGINE decides when a document is finished: qjs_step returns 0 when its frontier has drained. The
       harness still has to terminate, so it stops pumping after STEP_LIMIT — and when it does, it SAYS SO. A
       document that was still working reads identically to one that produced nothing, and reporting the two the
       same way is how a stalled document hid inside "no result, no abort and no page error". */
    let n = 0, r;
    while ((r = M.ccall("qjs_step", "number", [], [])) !== 0 && ++n < STEP_LIMIT) {
      const pend = String(M.ccall("qjs_pending", "string", [], [])).split("\n").filter(Boolean);
      for (const u of pend) M.ccall("qjs_provide", "void", ["string","string"], [u, serve(u, ONE)]);
    }
    if (r !== 0) unfinished = n;
    out = M.ccall("qjs_result", "string", [], []);
  } catch (e) {
    aborted = aborted || String((e && e.message) || e);
  }
  emit();
  process.exit(0);
}

/* ---- THE PARENT: fan the documents out, aggregate what comes back. ---- */
const JOBS = Math.max(1, Math.min(cpus().length - 1, 8));
const runOne = (rel) => new Promise((resolve) => {
  const ch = fork(fileURLToPath(import.meta.url), [sub, "--one", rel], { stdio: ["ignore", "pipe", "pipe", "ipc"] });
  let sout = "", serr = "";
  ch.stdout.on("data", (d) => { sout += d; });
  ch.stderr.on("data", (d) => { serr += d; });
  ch.on("exit", (code, sig) => {
    const line = sout.split("\n").find((l) => l.startsWith("@WPT "));
    if (line) { try { return resolve(JSON.parse(line.slice(5))); } catch { /* fall through */ } }
    /* The child DIED before it could report — its own stderr is the only account of why, and it is exactly the
       kind of failure that used to kill the whole run. Report it as this document's gap. */
    resolve({ results: [], pageErrors: [], unfinished: 0,
              aborted: (serr.split("\n").find(Boolean) || ("child exited " + (sig || code) + " with no output")) });
  });
});

let ran = 0, pass = 0, fail = 0, noresult = 0;
const gaps = new Map();          // distinct failure/abort message -> count
const noResultFiles = [];
const note = (msg) => gaps.set(String(msg).slice(0, 160), (gaps.get(String(msg).slice(0, 160)) || 0) + 1);

/* A CONTINUOUS pool, not batches. A batch is a barrier: the slowest document in it holds every other slot idle,
   and WPT documents differ by orders of magnitude in how long they run. Each slot takes the next file the moment
   it is free. */
let next = 0;
const takeNext = async () => {
  for (;;) {
    const i = next++;
    if (i >= files.length) return;
    const f = files[i];
    const res = await runOne(join(sub, f));
    if (!res.results.length) {
      noresult++;
      noResultFiles.push(f);
      /* The page's OWN uncaught errors are the real reason a document produced nothing: each names a capability
         the harness needed. They are the gap list. */
      if (res.pageErrors.length) for (const e of res.pageErrors) note(e);
      else note(res.aborted
                || (res.unfinished ? "the document was STILL WORKING after " + res.unfinished + " qjs_step calls "
                                     + "— the harness stopped pumping, the engine did not finish"
                                   : "no result, no abort and no page error"));
      continue;
    }
    ran++;
    for (const t of res.results) {
      if (t.status === "0") pass++;
      else { fail++; note(t.message || "(no message)"); }
    }
  }
};
await Promise.all(Array.from({ length: JOBS }, takeNext));

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
