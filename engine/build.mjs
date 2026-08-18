/* APIClient v2 build — minimal emscripten WASM build of CLEAN quickjs-ng + the
 * host scheduler entry (engine/host/main.c). Deliberately small: the old build
 * (COW barrier post-processing, wasm64, JSPI, Lexbor/Z3 link) is gone with the
 * fresh fork. Re-add each capability ONLY when the scheduler needs it, verified.
 *
 *   node engine/build.mjs                  -> engine/host/out/qjs.mjs + qjs.wasm (node smoke test)
 *   node engine/build.mjs native [min]     -> the smoke fixture built and run NATIVELY (the memory series)
 *   node engine/build.mjs native leak      -> the same under LeakSanitizer
 *   node engine/build.mjs native address   -> the same under AddressSanitizer
 *   node engine/build.mjs native cold      -> the CROSS-SESSION round trip: session one parks its frontier to a
 *                                            file, session two (a second process) resumes from it
 *
 * Build success/failure is the milestone-0 signal (does clean quickjs-ng compile
 * + link + boot). Design-correctness verification stays on the live Chrome
 * harness once the browser target is wired.
 */
import { spawnSync, spawn } from "node:child_process";
import { mkdirSync, existsSync, copyFileSync, readdirSync, writeFileSync, statSync, readFileSync, rmSync, openSync, closeSync } from "node:fs";
import { dirname, join, resolve, relative, sep } from "node:path";
import { cpus } from "node:os";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { stampArtifact } from "./gate_revision.mjs";

/* A RUN THAT NEVER RETURNS IS NOT A VERDICT. Every program this file launches gets the same generous backstop
   and reports a hang through a DIFFERENT signal than a failure — `.signal` where a failure sets `.status` — so
   the two can never collapse into one verdict. Declared here because the native targets run ~300 lines before
   the wasm ones and a `const` below them would be a TDZ throw. */
const RUN_BACKSTOP_MS = 15 * 60 * 1000;
/* `.signal` IS NOT THE HANG TEST, AND USING IT AS ONE MADE THIS REPORTER LIE. `spawnSync` sets `signal` for a
   child killed by ANY signal, so an ABORT — a DCHECK doing its job, arriving as SIGABRT — was reported as
   "DID NOT FINISH within 15 min", with a load average beside it inviting the reader to blame the machine. That
   is exactly the conflation this backstop exists to prevent, committed inside the backstop: measured, a run
   that reached its completion moment and then aborted at `idl_args.c:2148` was filed as a hang.
   The discriminator is node's own: a timeout kill sets `error.code === "ETIMEDOUT"`, and nothing else does. A
   signal without it is a CRASH, and a crash's `@WHY` is the result — it must not be dressed as a timing
   artifact. Three outcomes, three reports, three exit codes.

   AND THE HUNG REPORT ASSERTED A CAUSE IT NEVER MEASURED. It said the kill meant "a defect in the fixture,
   not the engine" — a claim about which of two named causes held, made by a function whose children run
   `stdio: "inherit"`, so it has never seen a line of their output. Measured, it was backwards: the three runs
   it called HUNG reached `finished` 545-705 while every run it called FAILED died on an abort at 16-261, so
   it reported 20-40x MORE work completed as the worse verdict, and a reader who trusted it went looking for a
   stall in a frontier whose `blocked` and `owed` were 0 in every sample of every run. The kill cannot decide
   this — deciding it needs the child's census, which needs capturing stdout rather than inheriting it, and
   that is a real change and not this one. So it now names the discriminator and stops claiming the answer.

   AND THE VERDICT IS RETURNED RATHER THAN EXITED ON, BECAUSE A STAGE THAT EXITS IS A DOOR IN FRONT OF EVERY
   STAGE BEHIND IT — see the stage list at the bottom for the one that stood behind this exit.
   The `hint` is printed HERE, at the non-pass, which is also the only way it prints at all: the three call
   sites below each carried an `if (t.status !== 0)` block with a diagnostic in it, and every one of those
   blocks was DEAD CODE — this function had already exited on that exact condition. The lines naming the
   @COLDPARK census and the LeakSanitizer summary have never once reached a terminal. */
/* THE KILL NOW READS THE CENSUS, because the previous sentence here — "deciding it needs the child's census,
   which needs capturing stdout rather than inheriting it, and that is a real change and not this one" — was a
   gap DOCUMENTED where the fix belonged. It named the mechanism, named the obstacle, and left the reporter
   printing an instruction to a human about numbers the reporter itself could have read. Every child now runs
   through `runChild`, which gives the run a FILE instead of the terminal and hands the bytes here, so the two
   causes the paragraph above distinguishes are distinguished BY THIS FUNCTION.
   The samples compared are the LAST and the MIDDLE one rather than the last two, because the paragraph's own
   caveat — "a plateau that resolves is WFQ re-ranking, not a stall" — is exactly what two adjacent samples
   cannot see: one re-ranking pause between them reads as a stall, and a stall that happens to emit once reads
   as health. Half the run is the shortest window in which neither is true.
   THE VERDICT IS STILL A NON-PASS AND STILL CODE 2. A healthy frontier that wanted more budget is a REAL
   change in this fixture — this same smoke terminated inside the backstop until it did not — so naming the
   cause is diagnosis, never permission to call the run green. */
/* THE FIELDS ARE NAMED AND THEIR ABSENCE THROWS, because the paragraph this function replaced read a field the
   census does not have. It told the reader "`finished` flat with `live` rising is the stall" — and @COLD has no
   `live`: engine.c prints `flows` for the live count, and `live` is a @PROGRESS name. Written as a comparison
   that was the worst possible form of the mistake, `undefined > undefined`, which is FALSE for every input, so
   the stall arm could never once have fired and the discriminator would have answered "healthy" to a stall
   forever. A JS property read that answers `undefined` is this file's silent-fallback, and the fix is the same
   one C gets: name the contract and abort on the origin that breaks it. */
const COLD_FIELDS = ["finished", "flows", "blocked", "owed"];
function hungCause(out) {
  const s = [];
  for (const m of out.matchAll(/^@COLD (\{.*\})$/gm)) { try { s.push(JSON.parse(m[1])); } catch { /* truncated tail */ } }
  if (s.length < 2) return `only ${s.length} @COLD census line(s) — too few to say why, and a run that prints ` +
                           `none has not reached engine_sched_begin's first census at all.`;
  const b = s[s.length - 1], a = s[Math.floor((s.length - 1) / 2)];
  for (const f of COLD_FIELDS) for (const c of [a, b])
    if (typeof c[f] !== "number")
      throw new Error(`[build] the @COLD census has no numeric \`${f}\` — this discriminator reads ` +
                      `${COLD_FIELDS.join(", ")} and engine.c's printf is what decides they exist; a renamed ` +
                      `field must be renamed here rather than silently compared as undefined.`);
  const span = `over the last ${s.length - Math.floor((s.length - 1) / 2)} of ${s.length} censuses: ` +
               `finished ${a.finished}→${b.finished}, flows ${a.flows}→${b.flows}, ` +
               `blocked ${b.blocked}, owed ${b.owed}`;
  if (b.finished > a.finished && b.blocked === 0 && b.owed === 0)
    return `a HEALTHY FRONTIER THAT WANTED MORE BUDGET (${span}) — flows were still finishing when the kill ` +
           `landed and nothing was waiting on the host.`;
  if (b.finished === a.finished && b.flows > a.flows)
    return `a STALL (${span}) — no flow finished across half the run while the live flow count rose, so work ` +
           `is being admitted and not retired.`;
  return `NEITHER named cause (${span}) — the frontier is doing something this discriminator does not model, ` +
         `and the two censuses above are the measurement to start from.`;
}

/* ONE WAY TO RUN A CHILD, and it hands the run's own bytes to the reporter that judges it. The five call sites
   this replaced each open-coded `spawnSync(..., { stdio: "inherit", timeout })` and then asked `runOutcome`
   about a run it had never seen — five copies of the same three options, and every one of them a place for the
   backstop to go missing (it already had: the smoke's spawn carried no timeout at all until the paragraph
   below was written, and that is the shape a per-site option takes when it is forgotten).
   THE TERMINAL LOSES THE LIVE STREAM AND KEEPS EVERY BYTE. `stdio: "inherit"` cannot also capture, and a
   captured pipe (`spawnSync`'s own `stdout`) is a fixed buffer that TRUNCATES the tail — which is exactly the
   half a hang is diagnosed from. So the child writes to a FILE, its path is announced BEFORE the run so it is
   tailable while it runs, and the whole file is written to this process's stdout afterwards, in order, so a
   transcript of this build still contains the run in full. */
function runChild(label, prog, args, hint) {
  const log = join(OUT, "run-" + label.replace(/[^A-Za-z0-9]+/g, "-").replace(/^-|-$/g, "") + ".log");
  mkdirSync(OUT, { recursive: true });
  console.log(`[build] ${label} — live at ${log}`);
  const fd = openSync(log, "w");
  let t;
  try { t = spawnSync(prog, args, { stdio: ["inherit", fd, fd], shell: false, timeout: RUN_BACKSTOP_MS }); }
  finally { closeSync(fd); }
  t.captured = readFileSync(log, "utf8");
  process.stdout.write(t.captured);
  return runOutcome(label, t, hint);
}

function runOutcome(label, t, hint) {
  const bad = (verdict, code, why) => {
    console.error(`[build] ${label} ${why}`);
    if (hint) console.error(`[build]   ${hint}`);
    return { label, verdict, code };
  };
  if (t.error && t.error.code === "ETIMEDOUT") {
    let load = "unknown";
    try { load = readFileSync("/proc/loadavg", "utf8").trim().split(/\s+/).slice(0, 3).join(" "); } catch { /* not linux */ }
    const cause = hungCause(t.captured);
    return bad("HUNG — " + cause.split(" (")[0], 2,
      `DID NOT FINISH within ${RUN_BACKSTOP_MS / 60000} min — killed by the harness ` +
      `backstop, NOT a failing run and NOT a passing one.\n` +
      `[build]   load average at kill: ${load} (on ${cpus().length} cores)\n` +
      `[build]   the census says it was ${cause}`);
  }
  if (t.signal) {
    return bad("CRASHED on " + t.signal, 3,
      `DIED ON ${t.signal} — read the @WHY above it; an abort is a DCHECK naming ` +
      `either an invariant to fix at its root or a capability to build, and it is the RESULT of this ` +
      `run rather than an interruption of it.`);
  }
  if (t.status !== 0) return bad("FAILED rc=" + t.status, t.status || 1, `FAILED rc=${t.status}`);
  return { label, verdict: "PASS", code: 0 };
}

/* A STAGE THAT CANNOT RUN IS REPORTED AS SKIPPED WITH ITS REASON, AND IT CARRIES A NON-ZERO CODE. Absorbing it
   into a pass is the excluded test again — a run that did not ask the question must never read like one that
   asked and liked the answer. The code is non-zero BY CONSTRUCTION rather than by the argument that whatever
   caused the skip already failed: that argument is true today and is exactly the kind of thing a later edit
   makes quietly false. */
function skipped(label, why) {
  console.error(`[build] ${label} SKIPPED — ${why}`);
  return { label, verdict: "SKIPPED (" + why + ")", code: 1 };
}

/* PER STAGE, IN ONE REPORT — §Testing: a gate reports PER AREA, never one number in which one area drowns the
   rest. Every stage this run reached is named with its own verdict, so "the smoke failed" and "the two-instance
   drive was never asked" can never again be the same line. The exit code is the FIRST non-zero in stage order,
   which is exactly what each single-stage target exited with before. */
function report(stages) {
  const w = Math.max(...stages.map((s) => s.label.length));
  console.log("[build] ── stages ──");
  for (const s of stages) console.log("[build]   " + s.label.padEnd(w) + "  " + s.verdict);
  const bad = stages.find((s) => s.code !== 0);
  if (bad) { console.error("[build] BUILD FAILED — " + bad.label + ": " + bad.verdict); process.exit(bad.code); }
  process.exit(0);
}

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const OUT = join(HOST, "out");
const EXT_QJS = join(ENGINE, "..", "extension", "lib", "qjs");   // where bridge.js imports the engine from

/* `--list-sources` answers WHAT THE PROGRAM IS and exits — check_recursion.sh shells back into this file to ask,
   and a list with build output in front of it is not a list.
   `--list-include-roots` answers WHERE ITS HEADERS COME FROM, for the same reason and to the same rule: the
   compiler is handed these roots and nobody else may restate them. gate_revision.mjs's dangling-include check
   had its own copy of the list — four roots, hand-written — and a second program's extra `-I` made that copy
   wrong the day it landed, so the check declared a revision that BUILDS to be one that "cannot be built by
   anyone who checks it out". A confident false red is worse than a silent miss: it is the phantom §Testing
   describes, and the next real dangling include arrives in a report nobody believes. The answer stays a LIST
   OF SOURCE SETS rather than a flat union even while there is one set, because that is what stops the check
   answering "fine" to a unit including a header its own compiler is never given — which is precisely the
   include it exists to catch — and a shape that degenerates the moment the program count drops to one is a
   shape that has to be rebuilt the moment it rises again. */
const LIST_SOURCES = process.argv.includes("--list-sources");
const LIST_INCLUDE_ROOTS = process.argv.includes("--list-include-roots");

const WORK = join(ENGINE, ".work");
const EMSDK = join(WORK, "emsdk");
const EMCC = join(EMSDK, "upstream", "emscripten", process.platform === "win32" ? "emcc.bat" : "emcc");

if (!existsSync(EMCC)) {
  /* A fresh container has no toolchain, and "emcc not found" on its own sends whoever hits it hunting for the
     version that was used. Say exactly what to run — the same reason the lexbor tag below is pinned here. */
  console.error("[build] emcc not found at " + EMCC + "\n" +
                "[build] provision it with:\n" +
                "  git clone --depth 1 https://github.com/emscripten-core/emsdk.git " + EMSDK + "\n" +
                "  " + join(EMSDK, "emsdk") + " install latest && " + join(EMSDK, "emsdk") + " activate latest");
  process.exit(1);
}
mkdirSync(OUT, { recursive: true });
mkdirSync(EXT_QJS, { recursive: true });

/* ── Lexbor DOM (HTML5 parser + DOM + CSS selectors) ─────────────────────────────
   The moat runs the page's real bundle against a real DOM. Lexbor is pure C, compiles
   to wasm, and links in the same module as quickjs. It's slow to compile (213 files),
   so build it ONCE into a cached static archive (liblexbor.a) and link that; rebuilds
   of the engine (quickjs + main.c) then stay fast. Rebuild the archive with
   `node engine/build.mjs lexbor`. */
/* The vendored checkout lives beside its build product in .work, which is where it actually is and where
   liblexbor.o was compiled from. It used to be named at engine/lexbor/source — a second, git-ignored location
   that is empty in a fresh container, so the build died on a missing <lexbor/html/html.h> while the headers sat
   in .work. One location, no fallback. */
/* PINNED, and provisioned here. Lexbor's C API moves between releases — a container that cloned its default
   branch instead got a css_declaration_list_parse with a different arity and the build died in the CSSOM. The
   version is part of the build, so it is stated in the build rather than in whoever's shell history. */
const LEXBOR_TAG = "v2.7.0";
const LEXBOR_DIR = join(WORK, "lexbor-src");
if (!existsSync(join(LEXBOR_DIR, "source", "lexbor"))) {
  console.log("[build] lexbor " + LEXBOR_TAG + " not present — cloning it");
  const r = spawnSync("git", ["clone", "--depth", "1", "--branch", LEXBOR_TAG,
                              "https://github.com/lexbor/lexbor.git", LEXBOR_DIR], { stdio: "inherit" });
  if (r.status !== 0) { console.error("[build] could not clone lexbor " + LEXBOR_TAG); process.exit(1); }
}
const LEXBOR_SRC = join(LEXBOR_DIR, "source");
const LEXBOR_LIB = join(WORK, "liblexbor.o");   // relocatable partial-link object (emcc -o .a doesn't archive from .c)
const LEXBOR_INC = LEXBOR_SRC;
function findC(dir, out) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) { if (p.includes("windows_nt")) continue; findC(p, out); }  // posix port on emscripten
    else if (e.name.endsWith(".c")) out.push(p);
  }
  return out;
}
function buildLexbor(force) {
  if (!force && existsSync(LEXBOR_LIB)) return;
  const srcs = findC(join(LEXBOR_SRC, "lexbor"), []);
  console.log("[build] lexbor: compiling " + srcs.length + " sources -> liblexbor.a (once, ~minutes)");
  const rsp = join(WORK, "lexbor.rsp");
  const fwd = (s) => s.replace(/\\/g, "/");   // response-file backslashes are clang escapes -> forward-slash paths
  writeFileSync(rsp, [...srcs.map(fwd), "-I", fwd(LEXBOR_INC), "-O2", "-w", "-D_GNU_SOURCE", "-DENABLE_DUMPS", "-r", "-o", fwd(LEXBOR_LIB)].join("\n"));
  const r = spawnSync(EMCC, ["@" + rsp], { stdio: "inherit", shell: true, cwd: ENGINE });
  if (r.status !== 0) { console.error("[build] lexbor FAILED rc=" + r.status); process.exit(r.status || 1); }
  console.log("[build] lexbor OK -> " + LEXBOR_LIB);
}
buildLexbor(process.argv[2] === "lexbor");
if (process.argv[2] === "lexbor") { console.log("[build] lexbor archive rebuilt; re-run without arg to build the engine."); process.exit(0); }

// THE IDL GAP AUDIT IS A STAGE OF THIS BUILD — see the stage list at the bottom. It was in nobody's build,
// which is §Testing's excluded gate: its whole subject (does each component install the members its IDL
// declares) was unmeasured while the report ended in a complete-looking total. It is a stage on the SAME terms
// as every other one — it fails the run, it is not best-effort, and it is not skipped when @webref/idl is
// absent, because that package is a declared devDependency of this repo and a gate that skips itself when its
// input is missing is a gate that silently is not one.

// THE SOLVER CORE AND THE BROWSER HALF ARE BOTH IN THE PROGRAM, and both entries are BUILT. The note that
// stood here — "there is no qjs_* extension ABI entry yet" — was false in the way CLAUDE.md §DFAIL describes:
// main.c IS that entry, QJS_ABI below is checked against its own QJS_EXPORT bodies in both directions, and
// link() emits both artifacts (test_forced.c -> out/qjs.js, main.c -> extension/lib/qjs/qjs.mjs). Neither may
// be dropped from the compile: §Testing's "a translation unit no gate compiles is outside the gate" is exactly
// how the shipped ABI entry rotted the last time only one of the two was linked.
/* Every .c under a directory, sorted — the same rule engine/wpt.mjs uses to decide what the gate links, and
   for the same reason its comment gives: a list picked per component only ever describes what was needed the
   last time someone remembered to edit it. */
function walkC(dir, out = []) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) walkC(p, out);
    else if (e.name.endsWith(".c")) out.push(p);
  }
  return out.sort();
}

/* WHICH DOCUMENT THE SMOKE DRIVES. `min` selects test_forced.c's minimal clone/COW/verify fixture and its probe
   subset — the per-change MEMORY gate, seconds instead of minutes, and the one to pair with `sanitize` (that
   fixture exists precisely to avoid the full document's fork-tree blowup under a sanitizer).
   IT IS PASSED AS AN ARGUMENT, and that is the whole fix: the selection used to be getenv("APICLIENT_ASAN_MIN"),
   and emscripten's ENV is a fixed default set that never merges process.env — so the minimal document, its probe
   subset and every statement only it carried were unreachable in EVERY mode of this script. A fixture no mode of
   the build can run is an excluded test, and an excluded test is a failure (CLAUDE.md, Testing). */
const MIN = process.argv.includes("min");
const SOLVER = (f) => join(HOST, "solver", f);     // the Time-Travel Solver (the novel half)
/* THE TWO ENTRIES, NAMED — they are alternatives at the LINK and identical everywhere else, which is the whole
   reason both can be verified for the price of one. test_forced.c owns main() and runs on load; main.c owns the
   qjs_* ABI the bridge drives through ccall and must not run on load.
   THERE HAVE TWICE BEEN THREE, AND BOTH THIRDS ARE GONE FOR TWO DIFFERENT REASONS WORTH KEEPING APART. The
   first was a "trusted" program linked out of a subtree of THIS list so that MIME Sniffing §7 would not link
   into the renderer, and A SEPARATE LINK IS NOT A PROCESS BOUNDARY: every object here was offered to every
   link, both Modules instantiated in the offscreen's own realm with the host holding an exported HEAPU8 over
   each, and the "trusted" artifact was in fact the LARGER of the two. The second was a real second program —
   its own source list, its own objects, its own dedicated Worker — and it went because of WHAT WAS IN IT
   rather than how it was linked. It held WHATWG MIME Sniffing §7 and Chromium's CORB analyzer, which
   CLAUDE.md §Architecture rules belong in `extension/lib/safe-fetch.js` where SECURITY.md's threat model
   puts the CORB gate; and it held the RENDERER REGISTRY, which is a `Map` from an agent cluster key to an
   integer. Neither is what a FLOW needs mid-execution, which is this project's whole test for what the engine
   owns — and the registry is the component that arbitrates between renderers of DIFFERENT ORIGINS, so a
   memory-corruption bug in it is a cross-origin boundary failure and it is the last thing that should be C.
   It is `extension/render-process-host.js`. There are two programs and both are the renderer's. */
const ENTRY_SMOKE = join(HOST, "test_forced.c");
const ENTRY_ABI   = join(HOST, "main.c");

/* THE HEADER ROOTS THE COMPILER IS GIVEN, DECLARED ONCE. CFLAGS below is BUILT from these rather than spelling
   them again, and `--list-include-roots` reports them, so the compiler, the build and any checker are reading
   one statement.
   Include by FULL path from the host root — a browser component is "core/dom/dom_element.h", a solver component
   "solver/concolic.h" — so a cross-layer include always names its layer and no bare-name shortcut hides one. */
const ENGINE_INCLUDE_ROOTS = [QJS, HOST, join(HOST, "browser"), LEXBOR_INC];
const dashI = (roots) => roots.flatMap((r) => ["-I", r]);
const SHARED_SOURCES = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c"]
  .map((f) => join(QJS, f))
  .concat([
    /* EVERY BROWSER AND SOLVER SOURCE, WALKED — not a hand-picked list. The list that stood here named each
       component with a comment, and it was wrong in the way a hand list always becomes wrong: a component
       added and not added to it is simply NOT IN THE ENGINE. remote_object.c is what proved it — the file
       existed, compiled, was tested through the WPT runner (which walks the tree, and has for exactly this
       reason) and did not link into the shipped wasm, because one place knew about it and the other did not.
       The failure was a link error here, which is the lucky version; the unlucky version is a component whose
       symbols happen to be unreferenced and which silently ships absent.
       ORDER IS STABLE (sorted), so a build is reproducible and a diff of this list is a diff of the tree. */
    ...walkC(join(HOST, "solver")),
    ...walkC(join(HOST, "browser")),
  ]);
/* WHAT THE PROGRAM IS — BOTH entries, because both are compiled and both are linked. This used to be whichever
   single entry the `abi` argument selected, and that argument is gone: it chose which of the two programs a run
   produced, and a run now produces both, so nothing is left for it to select. check_recursion.sh shells in here
   for this list, and an answer naming one entry excluded the other from the recursion check too. */
/* DEDUPED, because check_recursion.sh reads this list to decide what to analyse and a file named twice is a
   unit analysed twice. The `new Set` earned itself when core/mime/mime_type.c was in two programs and one
   file; it stays because the property it guarantees is about this list and not about how many programs
   happen to exist. */
const sources = [...new Set(SHARED_SOURCES.concat([ENTRY_SMOKE, ENTRY_ABI]))];

/* WHAT THE PROGRAM IS, asked rather than copied. check_recursion.sh needs exactly this list — its header says
   "the unit list mirrors engine/build.mjs" — and it was a second copy that had drifted to a THIRD of it: every
   browser component (the DOM tree walks, the HTML serialiser, custom elements, fetch, Headers) was outside the
   check entirely, so its zero was a zero about quickjs and the solver and nothing else. A checker that covers a
   fraction is worse than none, which is what that script's own header warns; the fix is that there is one list
   and the checker reads it. */
if (LIST_SOURCES) {
  console.log(sources.join("\n"));
  process.exit(0);
}

/* THE HEADER ROOTS, PER SOURCE SET, EMITTED FROM THE ONE PLACE THAT HANDS THEM TO THE COMPILER. There is one
   set today and the shape is still a LIST OF SETS, which is not hedging: a set is a compiler invocation with
   its own `-I` list, so the day a second program exists again the answer is one more entry rather than a
   reader that has to learn a new shape — and a flat union, which is what a single set collapses to if anyone
   "simplifies" it, would answer "fine" to a unit including a header its own compiler is never given, which is
   exactly the include the consumer of this manifest exists to catch.
   The roots are repo-relative because the consumer resolves them against a git revision rather than a path on
   this disk, and they are derived from the same CFLAGS array the link uses rather than restated — this file
   may not hold a second copy either, or it becomes the thing it is fixing. */
if (LIST_INCLUDE_ROOTS) {
  const rel = (p) => relative(join(ENGINE, ".."), p).split(sep).join("/");
  console.log(JSON.stringify([
    { name: "engine", roots: ENGINE_INCLUDE_ROOTS.map(rel), sources: sources.map(rel) },
  ], null, 1));
  process.exit(0);
}

/* THE NATIVE SMOKE TARGET, and the sanitized builds that are only possible on it.
 *
 *   node engine/build.mjs native            -> the smoke fixture as a native binary (the MEMORY series: its
 *                                              @HEAP/@PROGRESS stream is the only place this engine's live
 *                                              allocation is reported, and a wasm link per iteration is a tool
 *                                              nobody reaches for)
 *   node engine/build.mjs native leak       -> LeakSanitizer (which allocation is still live at exit)
 *   node engine/build.mjs native address    -> AddressSanitizer (UAF / double-free / overflow, leaks included)
 *   … plus `min` to drive the minimal fixture, as the wasm smoke takes it.
 *
 * It used to be a `-fsanitize=address` FLAG ON THE EMCC LINK, and that target could not run: wasm32 addresses
 * 4 GiB total, and this fixture's forced multi-path frontier measures 5.2 GiB of live allocation NATIVE and
 * unsanitized. `node engine/build.mjs asan` therefore aborted `Aborted(OOM)` inside the FIRST context switch,
 * with 46 KiB of JS heap on it — refusing to grow past 1.29 GB — and raising INITIAL_MEMORY only moved the wall
 * (MAXIMUM_MEMORY clamps it; lifting both reached 3.1 GiB still inside switch 1). A sanitizer target that cannot
 * reach switch two measures nothing, so the flag is DELETED rather than kept as a mode nobody can use.
 *
 * Native is not a workaround for that: it is where every other gate in this project already runs its C —
 * engine/wpt.mjs builds this same source list natively for the same reason (an eight-minute wasm link per
 * iteration is a gate nobody runs), and a native ASan run over that runner is what named the attribute-lifetime
 * SEGV in one go. The flags below are wpt.mjs's, with the SMOKE entry instead of the WPT one, and DEV on so a
 * DCHECK stays live — a sanitized build with the asserts compiled out reports faults the engine's own
 * invariants would have caught first, at the wrong site. */
const NATIVE = process.argv.includes("native");
if (NATIVE) {
  /* WHICH SANITIZER, IF ANY — named, because the plain native build is the one the memory series comes from and
     a sanitizer changes both the numbers and the wall-clock by an order of magnitude. */
  const kind = process.argv.includes("address") ? "address"
             : process.argv.includes("leak")    ? "leak" : "none";
  const bin = join(OUT, "qjs-native-" + kind);
  mkdirSync(OUT, { recursive: true });
  /* LEXBOR, NATIVELY, exactly as wpt.mjs provisions it — the same vendored source and the same cached archive,
     because a second copy of that provisioning is a second thing to keep in step with the pinned tag. */
  const LEXBOR_NATIVE = join(WORK, "lexbor-native", "liblexbor_static.a");
  if (!existsSync(LEXBOR_NATIVE)) {
    console.error("[build] the native lexbor archive is not built: " + LEXBOR_NATIVE + "\n" +
                  "[build] `node engine/wpt.mjs` builds it once (cmake + make) — run that first.");
    process.exit(1);
  }
  /* The quiet list and -Werror=implicit-function-declaration are the SHIPPED build's, taken from the same place
     rather than restated, so the sanitized program is the program. */
  const quiet = ["-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses",
                 "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow",
                 "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer",
                 "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration"];
  const cc = spawnSync("clang", [
    "-O1", "-g", "-fno-omit-frame-pointer",
    ...(kind === "none" ? [] : ["-fsanitize=" + kind]),
    ...quiet,
    "-D_GNU_SOURCE", "-DENABLE_DUMPS", '-DCONFIG_VERSION="native"', "-DAPICLIENT_DEV=1",
    "-I" + QJS, "-I" + HOST, "-I" + join(HOST, "browser"), "-I" + join(WORK, "lexbor-src", "source"),
    /* THE SMOKE ENTRY, always: this target BUILDS AND RUNS a fixture, and main.c has no main() to run. */
    ...SHARED_SOURCES, ENTRY_SMOKE, LEXBOR_NATIVE, "-o", bin, "-lm", "-lpthread",
  ], { stdio: "inherit" });
  if (cc.status !== 0) { console.error("[build] native build FAILED rc=" + cc.status); process.exit(cc.status || 1); }
  console.log("[build] OK -> " + bin);
  /* THE CROSS-SESSION ROUND TRIP: TWO INVOCATIONS OVER ONE SHELF.
   *
   * §Time-travel-resume's whole claim is that the frontier persists as suspended snapshots ACROSS SESSIONS, and
   * until this target existed nothing in this tree could run both halves of it: the residue the engine writes
   * and the residue cold_resume reads were produced and consumed in different processes, and the only host that
   * held both ends was a browser with an IndexedDB. So the read half — the segment rebuild, park_unhex,
   * solve_resume_candidate, the probe address — had never executed in ANY process.
   * A SESSION BOUNDARY IS A PROCESS BOUNDARY, which is why this is two spawns and not one binary doing both. A
   * single process would leave the first session's endpoint surface, sink searches and world namespace standing
   * behind the second, so "the resumed session found it" and "the previous session had already found it" would
   * be the same observation and the round trip would prove nothing.
   * IT IS NOT A DRIVER. The shelf is a file, the resume is engine_sched_begin's own choice between a residue
   * and a boot flow, and everything between the two spawns is the store — which is exactly what the trusted
   * zone is to the shipped engine. */
  if (process.argv.includes("cold")) {
    const store = join(OUT, "park.recipes");
    /* THE SHELF IS EMPTY BEFORE SESSION ONE. A residue left by an earlier run of a DIFFERENT tree would resume
       flows standing on segments this build never wrote — and it would look like a pass. */
    rmSync(store, { force: true });
    const v1 = runChild("session ONE (--cold-park)", bin, ["--cold-park", store],
      "read its `@H park-*` rows and the @COLDPARK census: a 0 there names which record kind the park did " +
      "not write, and the moment it was taken at is `fixture_want_park` in engine/host/test_forced.c.");
    /* SESSION TWO IS SKIPPED AND NOT MERELY UNREPORTED. This is a real data dependency and not a door — the
       resume reads the residue session ONE writes, so with no residue there is nothing for it to be a test OF
       — and it is stated as a skip with that reason so the report never has a silent hole in it. */
    const v2 = v1.code
      ? skipped("session TWO (--cold-resume)", "session ONE wrote no residue for it to resume from")
      : runChild("session TWO (--cold-resume)", bin, ["--cold-resume", store],
                 "`@RESUMED <n>` and the @COLDRESUME census say what it rebuilt out of the residue; a kind " +
                 "session one wrote and this one did not rebuild is the arm to look at.");
    if (!v1.code && !v2.code) console.log("[build] cold round trip (" + kind + ") — residue at " + store);
    report([v1, v2]);
  }
  /* AND IT IS RUN, because a target that is only built is the excluded test one layer down: the whole point is
     the stream it prints and the report it ends with, and nothing else in the tree produces either. */
  report([runChild("the native run (" + kind + (MIN ? ", minimal document" : "") + ")", bin, MIN ? ["--min"] : [],
                   "a LeakSanitizer summary above is a real leak, and an AddressSanitizer report a real fault")]);
}

/* THE EXPORTS THE BRIDGE ccalls — and the previous sentence here ("emscripten drops anything not named") is
   DELETED, because it is not the mechanism and stating a wrong one is how a check gets skipped as redundant.
   `QJS_EXPORT` is `EMSCRIPTEN_KEEPALIVE`, which is `__attribute__((used))`, and EXPORT_KEEPALIVE defaults to 1
   outside MINIMAL_RUNTIME — so an entry left off this list may well reach `Module` anyway, by the accident of a
   default that another setting flips. AN ABI REACHED BY ACCIDENT IS NOT AN ABI: what this list decides is
   `--export=` on wasm-ld and `Module._x` on purpose, and the entry that is only there when the toolchain feels
   like it is the entry that vanishes in the build nobody re-checked. So the list is ENFORCED rather than
   asserted about — `qjs_perform` and `qjs_host_answer_remote` (the peer's half of the cross-instance seam, the
   only entries by which one instance is ASKED to perform another's operation) were written, linked and left
   off it, and no gate said a word. */
const QJS_ABI = ["qjs_init", "qjs_join", "qjs_bundle_id", "qjs_begin", "qjs_step", "qjs_result", "qjs_teardown",
                 "qjs_pending", "qjs_chunks", "qjs_provide", "qjs_top_weight", "qjs_set_yield_floor",
                 "qjs_request_park", "qjs_emit_partial",
                 "qjs_host_requests", "qjs_host_answer", "qjs_host_notices", "qjs_route",
                 "qjs_perform", "qjs_host_answer_remote", "qjs_world_gone"];

/* THE LIST IS THE ABI, SO THE ENTRY POINT AND THE LIST ARE ONE FACT AND ARE CHECKED AGAINST EACH OTHER. Both
   directions are a real defect and neither has a symptom at build time: an entry main.c defines and this omits
   is a capability the extension cannot call (or can, until a setting changes); a name here that main.c does not
   define is `--export=` of a symbol that does not exist, which wasm-ld reports as an undefined export only
   because ERROR_ON_UNDEFINED_SYMBOLS happens to be on. Read from the source rather than restated: `QJS_EXPORT`
   is exactly the marker main.c puts on every ABI body.
   IT IS A HELPER WITH ONE CALLER, WHICH IT HAS BEEN TWICE BEFORE AND IN OPPOSITE DIRECTIONS. It was inlined
   once on the argument that "a helper kept for a caller that no longer exists is scaffolding", then made a
   helper again when a second program with its own entry arrived, and that program is deleted. It stays a
   helper because the argument for inlining it was wrong even when it was true: a second copy of these lines is
   the hand-maintained list this file spends its length warning about, and the shape it fails in is silence —
   an entry added to one program's list with the copy for the other left unedited exports nothing and says
   nothing. A function whose parameters are exactly the four facts that differ per program costs nothing to
   keep and is what makes the next program's check one line rather than a transcription. */
function abiCheck(program, entrySrc, marker, prefix, list) {
  const src = readFileSync(entrySrc, "utf8");
  const re = new RegExp(marker + "\\s+[\\w \\t*]+?\\b(" + prefix + "\\w+)\\s*\\(", "g");
  const defined = [...new Set([...src.matchAll(re)].map((m) => m[1]))];
  const missing = defined.filter((f) => !list.includes(f));
  const phantom = list.filter((f) => !defined.includes(f));
  if (missing.length || phantom.length) {
    console.error("[build] the " + program + " ABI list and its entry's " + marker + " bodies disagree — the\n" +
                  "[build] list IS the ABI, so a disagreement is either an entry nothing can call or an export\n" +
                  "[build] of nothing (" + entrySrc + "):\n" +
                  (missing.length ? "[build]   defined in the entry, missing from the list: " + missing.join(", ") + "\n" : "") +
                  (phantom.length ? "[build]   named in the list, defined nowhere:          " + phantom.join(", ") + "\n" : ""));
    return { label: program + " ABI list", verdict: "FAILED (list vs " + marker + " bodies)", code: 1 };
  }
  return { label: program + " ABI list", verdict: "PASS", code: 0 };
}
/* IT FAILS THE RUN AND IT IS NOT A DOOR EITHER. This check is about ONE program's export list, and it used to
   exit before anything was compiled — so a name added to main.c and not to QJS_ABI took the SMOKE gate, which
   has no export list and does not know main.c exists, out of the run with it. It reports its own verdict; what
   it withholds is the one thing it is actually about, the ABI link (an `--export=` of a name defined nowhere is
   what wasm-ld would report, less clearly and only by the accident of ERROR_ON_UNDEFINED_SYMBOLS). */
const ABI_LIST = abiCheck("renderer", join(HOST, "main.c"), "QJS_EXPORT", "qjs_", QJS_ABI);
/* A SECOND ABI LIST STOOD HERE, five entries of a second program, and it is deleted with that program. What
   it exported was the RENDERER REGISTRY: which agent clusters have an instance, what routing id each was
   given, and the refusal of a second instance for one cluster. That is `extension/render-process-host.js`
   again, in the trusted zone, in JavaScript, because the component that arbitrates between renderers of
   DIFFERENT ORIGINS is the one where a memory bug is a cross-origin boundary failure — and because a `Map`
   from a string to an integer is not what a FLOW needs mid-execution, which is this build's test for what the
   engine owns. There is one ABI list again and it is the renderer's. */

/* COMPILE FLAGS AND LINK FLAGS ARE SEPARATED, and that separation is what lets both entries be verified.
   They used to be one list handed to one emcc invocation that compiled and linked together, which forced two
   things: every build recompiled all 130-odd sources from scratch, and only ONE entry could be produced per
   run. The consequence was that the shipped ABI entry — the one the extension actually loads — was never
   LINKED by any default gate. It was compiled to a throwaway object, which catches a missing #include and a
   bad declaration and catches NOTHING about an undefined symbol; a qjs_* body calling a function deleted from
   the tree passed that check and would have failed only in the extension. §Testing's rule is that the shipped
   entry is the one that rots, and half a check is how it rots quietly.
   The two programs differ ONLY in which entry object enters the link, so compiling once into shared objects
   and linking twice costs one extra link and closes that hole completely. */
const CFLAGS = [
  ...dashI(ENGINE_INCLUDE_ROOTS),   // declared once beside the source sets; see ENGINE_INCLUDE_ROOTS
  /* -Werror ON IMPLICIT DECLARATIONS, and the reason `-w` is NOT here beside it. A missing #include makes C
     assume `int (...)`, so a returned 64-bit POINTER comes back TRUNCATED — a segfault with no diagnostic, and
     it happened: window.c called window_proxy_name without its header and the whole corpus segfaulted inside
     strcmp. This line used to read `-w -Werror=implicit-function-declaration`, which does NOT work: `-w`
     suppresses the diagnostic outright, so the -Werror= promotion has nothing left to promote. The quiet list
     is explicit for that reason, and the moment it went in the gate found three more missing includes. */
  "-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow", "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer", "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration",
  "-D_GNU_SOURCE", "-DENABLE_DUMPS",
  // Offensive-programming build mode (check.h): DEV (default) keeps every DCHECK live so a should-never-happen
  // aborts LOUD at its origin; a `release` arg compiles them out (the release exemption — the user is not
  // crashed on an unsupportable state). CHECK (OOM/security) stays fatal in both.
  "-DAPICLIENT_DEV=" + (process.argv.includes("release") ? "0" : "1"),
];

const LDFLAGS_COMMON = [
  LEXBOR_LIB,                 // link the cached Lexbor DOM archive
  // Opt-in `assert` build: emscripten ASSERTIONS=2 turns a bare terse `Aborted()` into an INFORMATIVE crash
  // (the failing C assert + file:line — e.g. a refcount/gc_obj_list leak), the offensive-programming ideal of a
  // LOUD *and* diagnosable dev failure. Off by default so normal dev builds stay fast; enable when debugging.
  ...(process.argv.includes("assert") ? ["-sASSERTIONS=2"] : []),
  /* THE ARCHITECTURE'S CEILING, not a budget. wasm32 addresses 4 GiB and emscripten stops the heap at 2 GiB
     unless told otherwise, so the growth flag alone was a 2 GiB cap wearing the word "growth". The smoke
     fixture's forced multi-path run measures gigabytes of live allocation — the frontier holds every flow's COW
     delta in RAM because the smoke has no IDB cold tier to page the low-value tail into — so the 2 GiB stop was
     already the thing about to fail. Raising it to what the address space actually holds is the platform floor;
     the real answer for a frontier this size is the cold tier, which is the scheduler's work and not a flag.
     THE SANITIZED TARGET IS NOT HERE, and that is measured rather than assumed — see `sanitize` above: a wasm
     ASan link cannot reach the second context switch inside 4 GiB, so the flag that used to sit on this line
     was a mode that had never run. */
  "-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=4294967296",
  "-sSTACK_SIZE=8388608",
];
// The smoke entry RUNS on load and exits with the @H/@S pass code; the ABI entry is driven by the bridge
// through ccall, so its runtime must stay alive across qjs_step re-entries and be importable as an ES module.
const LDFLAGS_SMOKE = ["-sEXIT_RUNTIME=1"];
const LDFLAGS_ABI = [
  "-sEXPORTED_FUNCTIONS=" + JSON.stringify(QJS_ABI.map((f) => "_" + f).concat(["_malloc", "_free"])),
  /* HEAPU8 IS AN EXPORT, and it is the one every byte that crosses this ABI travels through. Fetch §2.2.5's
     body is a BYTE SEQUENCE, so the zone places it with `M.HEAPU8.set(bytes, p)` and hands `qjs_provide`
     a (ptr, len) — text cannot carry one and every way of making it able to is an encode by a zone with
     no business performing one. Emscripten does not export the heap views by default and answers a read
     of an unexported one by ABORTING the module, so omitting it here is not a missing convenience, it is
     the whole byte path failing at its first use. */
  "-sEXPORTED_RUNTIME_METHODS=" + JSON.stringify(["ccall", "lengthBytesUTF8", "stringToUTF8", "HEAPU8"]),
  "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createQJS", "-sINVOKE_RUN=0",
];

/* ── OBJECTS, COMPILED ONCE AND CACHED ────────────────────────────────────────────────────────────────────
   An object is rebuilt when it is missing, when its source is newer, or when any HEADER it included is newer —
   the last of which is the one a hand-rolled cache always gets wrong, so it is not hand-rolled: clang emits the
   real dependency list with -MMD and this reads it back. A cache that misses a header edit is worse than no
   cache, because it reports a stale binary as a fresh one. */
const OBJDIR = join(WORK, "obj");
mkdirSync(OBJDIR, { recursive: true });
/* THE FLAGS ARE PART OF THE OBJECT'S IDENTITY, NOT A THING THE CACHE COMPARES. The comment above says a cache
   that misses a header edit reports a stale binary as a fresh one; this cache missed a FLAG edit, which is the
   same defect with a wider blast radius. `CFLAGS` carries `-DAPICLIENT_DEV=0` under `release` and `=1`
   otherwise — the switch that compiles out every DCHECK — and `objIsStale` compared only mtimes, so
   `node engine/build.mjs release` after a dev build found every object fresh, relinked the DEV objects, and
   reported a green release build of a program that was never built. That is §Testing's "a number about
   NOTHING" in the build system itself, and it is why release mode had to be verified with `-fsyntax-only`
   rather than by running the target.
   Hashing the flags into the PATH rather than storing them for comparison is what makes the failure
   impossible instead of detected: two flag sets are two files, so neither can masquerade as the other and
   both stay cached across switches. The compiler binary is in the hash for the same reason a header is —
   changing it changes the output. */
const FLAG_ID = createHash("sha256").update(EMCC + " " + CFLAGS.join(" ")).digest("hex").slice(0, 12);
const objPath = (src) => join(OBJDIR, resolve(src).replace(/[\\/:]/g, "_") + "." + FLAG_ID + ".o");

function objIsStale(src, obj) {
  if (!existsSync(obj)) return true;
  const objTime = statSync(obj).mtimeMs;
  if (statSync(src).mtimeMs > objTime) return true;
  const dep = obj.replace(/\.o$/, ".d");
  if (!existsSync(dep)) return true;   /* no recorded deps is not "no deps" — it is no information */
  for (const f of readFileSync(dep, "utf8")
                    .replace(/\\\r?\n/g, " ").split(/\s+/).slice(1).filter(Boolean)) {
    if (!existsSync(f)) return true;   /* a header that vanished changes the compile */
    if (statSync(f).mtimeMs > objTime) return true;
  }
  return false;
}

/* Both entries are compiled every time, because both are LINKED every time. */
const TO_COMPILE = SHARED_SOURCES.concat([ENTRY_SMOKE, ENTRY_ABI]);
const stale = TO_COMPILE.filter((s) => objIsStale(s, objPath(s)));
console.log("[build] " + TO_COMPILE.length + " sources, " + stale.length + " to compile" +
            (stale.length < TO_COMPILE.length ? " (rest cached)" : ""));

if (stale.length) {
  /* IN PARALLEL, bounded by the cores actually present. A cold build is ~130 translation units and the machine
     is otherwise idle while each one runs. */
  const JOBS = Math.max(1, cpus().length - 1);
  let next = 0, failed = 0, running = 0;
  await new Promise((done) => {
    const pump = () => {
      while (running < JOBS && next < stale.length) {
        const src = stale[next++], obj = objPath(src);
        running++;
        const p = spawn(EMCC, [...CFLAGS, "-MMD", "-MF", obj.replace(/\.o$/, ".d"), "-c", src, "-o", obj],
                        { stdio: "inherit", shell: true, cwd: QJS });
        let settled = false;
        const settle = (why) => {          /* exit and error are not mutually exclusive; the pump must run once */
          if (settled) return;
          settled = true;
          if (why) { failed++; console.error("[build] FAILED " + src + " — " + why); }
          running--;
          if (next >= stale.length && running === 0) done();
          else pump();
        };
        p.on("exit", (code) => settle(code === 0 ? null : "compiler exited " + code));
        /* A SPAWN THAT NEVER STARTS MUST BE A FAILED TU, NOT AN UNHANDLED THROW. With no `error` listener node
           raises the event as an exception, so the build died mid-run with no line naming a source — and
           `running--` never ran either, so the promise it was inside could not have settled had the throw been
           caught. Observed twice under fork pressure as `spawn /bin/sh ENOENT`: the machine was saturated, not
           the code wrong, and the build reported neither. That is the loaded-machine defect §Testing names —
           an artifact of HOW it ran presented as a fact about WHAT ran — so it is reported as what it is, with
           the source named. */
        p.on("error", (e) => settle("could not start the compiler: " + e.message));
      }
      if (next >= stale.length && running === 0) done();
    };
    pump();
  });
  if (failed) { console.error("[build] FAILED — " + failed + " source(s) did not compile"); process.exit(1); }
}

const OBJS_SHARED = SHARED_SOURCES.map(objPath);

/* ── LINK BOTH PROGRAMS ───────────────────────────────────────────────────────────────────────────────────
   THE ABI ARTIFACT STAGES WHERE THE EXTENSION LOADS IT: bridge.js does import("./lib/qjs/qjs.mjs"), so that is
   the output path, not engine/host/out. Two artifacts, two homes — emcc derives the .wasm name from the -o
   basename, so both emitting into out/qjs.* would share one qjs.wasm and overwrite each other.
   AND THAT IS ALL A SEPARATE LINK IS — a separate FILE. There is no `extra` object list here selecting which
   components a program may reach, because a link boundary is not a trust boundary: the objects are the same
   objects, what wasm-ld leaves out is only what the exports do not reach, and both artifacts are instantiated
   in the offscreen's own realm with the host holding an exported HEAPU8 over each.
   AND NEITHER LINK IS A DOOR IN FRONT OF THE OTHER: a failing smoke link used to take the production ABI link
   — and with it the only two-instance drive in the tree — out of the run entirely. Both are attempted, both
   are reported, and a program that did not link makes its OWN drive a SKIP with that reason. */
function link(what, entryObj, ldflags, out) {
  const l = spawnSync(EMCC, [...OBJS_SHARED, entryObj, ...LDFLAGS_COMMON, ...ldflags, "-o", out],
                      { stdio: "inherit", shell: true, cwd: QJS });
  if (l.status !== 0) {
    console.error("[build] " + what + " LINK FAILED rc=" + l.status);
    return { label: what + " link", verdict: "FAILED rc=" + l.status, code: l.status || 1 };
  }
  console.log("[build] OK -> " + out);
  return { label: what + " link", verdict: "PASS", code: 0 };
}
const SMOKE_LINK = link("smoke", objPath(ENTRY_SMOKE), LDFLAGS_SMOKE, join(OUT, "qjs.js"));
const ABI_LINK = ABI_LIST.code
  ? skipped("production ABI link", "the renderer ABI list and main.c's QJS_EXPORT bodies disagree, so this "
                                 + "link's --export= list is known wrong")
  : link("production ABI", objPath(ENTRY_ABI), LDFLAGS_ABI, join(EXT_QJS, "qjs.mjs"));

/* THE ARTIFACT RECORDS THE REVISION IT WAS BUILT FROM, because engine/solvergate.mjs runs this file and
   never compiles anything, so without a stamp the only question it could ask about the program was how old
   the FILE was. That answer is wrong in exactly the mode CLAUDE.md §Testing mandates: `git worktree add`
   writes every tracked file at the checkout instant, so in a frozen snapshot every source is newer than
   any artifact and the check reported a build OF that revision as stale against 600 sources, three minutes
   after the checkout. The stamp is computed by gate_revision.mjs itself rather than re-derived here, so
   what is written and what is checked are the same answer by construction. The cone is what this link
   actually compiled — the host and the submodule — and not the whole tree, for the reason that file gives:
   another agent's popup edit is not a reason to distrust a JS-engine number.
   ONLY WHEN THAT LINK PRODUCED THE ARTIFACT: stamping after a failed link would mark whatever qjs.mjs a
   PREVIOUS build left on disk as belonging to this revision, which is §Testing's number about nothing with the
   stamp itself doing the lying. */
if (ABI_LINK.code === 0) stampArtifact(join(EXT_QJS, "qjs.mjs"), ["engine/host", "engine/qjs"]);

// Milestone smoke test: run test_forced.c's main (the @H merge + @S sink fire-verification on a fixture doc) —
// the design-correctness signal until the live-Chrome harness is re-wired to a rebuilt production ABI entry.
// (The old ES6-module + qjs.wasm staging into extension/lib/qjs served the deleted qjs_* entry; it returns when
// that entry is rebuilt.)
/* BOTH PROGRAMS ARE RUN, in the build that produced them. A target that is only built is the excluded test one
   layer down: the whole point of each is the stream it prints and the report it ends with.
   The compile-only check that used to stand here — the unlinked entry compiled to a throwaway object — is
   DELETED rather than kept beside this. It existed only because one invocation could produce one program, and
   it answered a strictly weaker question than the link above now answers: it caught a missing #include and a
   bad declaration, and nothing at all about an undefined symbol. Leaving it would be a second, worse check of
   the same thing (CLAUDE.md: a superseded system is deleted in the same diff, never kept as a fallback). */

/* A RUN THAT NEVER RETURNS IS NOT A VERDICT, AND UNTIL THIS BACKSTOP IT WAS NOT EVEN AN EVENT. `spawnSync`
   here carried no timeout, so `node engine/build.mjs` — the command CLAUDE.md names as THE build — did not
   terminate whenever the fixture's frontier did not drain, and it consumed a core while not terminating. That
   is not hypothetical and it was not rare: four abandoned `out/qjs.js` processes were found at 84-97% CPU
   having run 1.9-4.1 hours, in the scratch trees of agents that had long since finished. They WERE the machine
   load, and the load was then read by six separate lanes as "the machine is saturated, a measurement now would
   be a loaded-machine artifact" — so they each declined to run their gates. One missing timeout suppressed
   every gate in the project, and nothing anywhere said the word "hang".
   THE BACKSTOP IS NOT A CAP, AND THE DISTINCTION IS §Testing's: the real measure is the fixture reporting its
   own stream, this is the case that measure cannot see, so it is GENEROUS and it reports through a DIFFERENT
   SIGNAL — a `signal` where a failure sets a `status` — and the two never collapse into one verdict. A hang is
   named as a hang, with the load average that is the first thing to suspect, and it is NOT reported as a
   failing smoke test. §NO BOUNDS is about the FRONTIER: it forbids the engine capping its own exploration, and
   it has never had anything to say about a harness refusing to wait forever for a process it launched. */
function runProgram(label, argv, hint) {
  return runChild(label, process.execPath, argv, hint);
}

/* THE TWO PROGRAMS ARE TWO AREAS AND BOTH ARE ASKED, EVERY RUN.
 *
 * The smoke drives test_forced.c's fixture document (its @H probe stream: any row 0 and the process exits
 * non-zero — the trailing arguments reach main()'s argv, the channel getenv could not be, since emscripten's
 * ENV never merges the launching process's environment). The second drives the SHIPPED entry through the
 * surface the bridge actually calls: engine/route.mjs boots the module just linked, provisions a SECOND
 * instance from the create notice, routes posts and a synchronous cross-origin `length` read between them,
 * and parks one of them on an outstanding read.
 *
 * IT IS THE ONLY THING IN THE TREE THAT PROVISIONS TWO INSTANCES, which §SECURITY makes the precondition for
 * believing any cross-instance mechanism has ever run — the world registry, the nearest-first ancestry, the
 * lazy segment materialization, the peer that answers by running a program. It stood BEHIND the smoke's exit,
 * so the run that would first show a cross-instance regression is precisely the run that never asked: any
 * probe row 0, in any unrelated area of the fixture, and the seam went unexercised while the report named only
 * the smoke. That is §Testing's excluded test wearing a complete-looking total, and the fix is that neither
 * stage gates the other and BOTH numbers are in one report. */
const STAGES = [ABI_LIST, SMOKE_LINK, ABI_LINK];
STAGES.push(SMOKE_LINK.code
  ? skipped("smoke test", "the smoke program did not link")
  : runProgram("smoke test" + (MIN ? " (minimal document)" : ""),
               [join(OUT, "qjs.js"), ...(MIN ? ["--min"] : [])],
               "the @H row printed 0 above names the statement the fixture document makes and this run did " +
               "not answer — engine/host/test_forced.c's probe table is where that row is declared."));
/* A STALE ARTIFACT IS NOT A SUBJECT. route.mjs imports extension/lib/qjs/qjs.mjs off disk, so running it after
   a failed ABI link would measure whatever a PREVIOUS build left there and report the number under this
   revision — which is worse than not running it, and is why this is a SKIP rather than an attempt. */
STAGES.push(ABI_LINK.code
  ? skipped("two-instance ABI drive", "the production ABI program did not link")
  : runProgram("two-instance ABI drive", [join(ENGINE, "route.mjs")],
               "this is the cross-instance seam: the world registry, the nearest-first ancestry fork, the " +
               "synchronous cross-origin read, and the park on an outstanding one. Nothing else in this tree " +
               "provisions a second instance, so a failure here is unobserved by every other gate."));
/* AND THE SAME SEAM ONE LAYER UP — the BROWSER-PROCESS half, which route.mjs plays the part of rather than
   runs. route.mjs calls `makeEngine` itself, so what it proves about the transport says nothing about the two
   components that decide an instance exists and materialize it in the shipped extension:
   `extension/render-process-host.js` (the registry: which agent clusters have a renderer, the routing-id mint,
   and the refusal of a second for one cluster) and `extension/renderer-host.js` (the RenderFrameHost). This
   stage loads both by their own bytes and drives them, so the admission decision and the CHECK-class refusal
   SECURITY.md's one-instance-per-agent-cluster rule is made of are taken by the shipped code.
   IT SKIPS ON THE SAME CONDITION AND FOR THE SAME REASON as the drive above: it boots the renderer program the
   ABI link just produced, so running it after a failed link would measure whatever a previous build left in
   `extension/lib/qjs/` and report that number under this revision. */
STAGES.push(ABI_LINK.code
  ? skipped("browser-process layer", "the production ABI program did not link")
  : runProgram("browser-process layer", [join(ENGINE, "renderer_host_gate.mjs")],
               "this is the renderer registry and the RenderFrameHost, driven by their own bytes: two " +
               "cross-origin renderers provisioned into one browsing-context group, the duplicate-cluster " +
               "refusal that is SECURITY.md's one-instance-per-cluster rule, and the routing-id accounting. " +
               "Until this stage existed neither file was compiled, imported or run by anything."));
/* THE THIRD AREA: does each component install the surface its Web IDL declares. It gates nothing and nothing
   gates it — it compiles no C and reads no artifact, so it asks its question of the SOURCES whatever the two
   programs above did, and a link failure can never take the member census out of the run with it.
   IT IS LAST IN STAGE ORDER, AND THAT IS THE ONE THING THIS PLACEMENT DECIDES: report() exits with the first
   non-zero code, so a program that does not build or does not run keeps its own exit code and the member gap
   does not stand in front of it. Every stage still reports, which is the whole point of the list. */
STAGES.push(runProgram("Web IDL gap audit", [join(ENGINE, "idlgen.mjs")],
                       "each category above is a spec member no component installs, a stub where a real value " +
                       "belongs, or an install construct the audit cannot resolve — implement it at the root " +
                       "in its real component. There is no baseline to update: the count IS the gap."));
report(STAGES);

/* A THIRD DRIVE STOOD HERE — the driver for the deleted second program, which put the RENDERER REGISTRY's
   transitions through it — and it is deleted with the program it drove. THE COVERAGE IT HELD IS NAMED RATHER
   THAN QUIETLY DROPPED, because a gate that vanishes with its subject is only honest if what it was measuring
   is stated: it exercised the duplicate-cluster refusal, the two keys that are equal up to `clusterKeyOf`'s
   NUL separator, and the reported-dead-twice and never-minted-id refusals.
   THAT GAP IS THE `browser-process layer` STAGE ABOVE AND THIS PARAGRAPH NO LONGER DESCRIBES THIS TREE. It
   stood here saying "this build compiles no JavaScript" and "the REFUSALS have no caller that fires them",
   which was true and then was read as an instruction — the exact way a `DFAIL` outlives the absence it names
   and starts to lie. `engine/renderer_host_gate.mjs` loads render-process-host.js and renderer-host.js by
   their own bytes into a realm carrying only the browser surface they read, provisions two cross-origin
   renderers through the registry, asks it for a second renderer for a cluster that already has one, and asks
   it to bury a routing id it never minted and a renderer it has already buried — so all THREE of the
   registry's CHECK-class refusals now have a caller that fires them on every build, which is the whole of the
   coverage this paragraph was written to record the loss of.
   WHAT NO GATE IN THIS FILE ASKS AT ALL, stated here rather than left for a reader to discover by grep:
   whether `clusterKeyOf` decides that two LIVE documents are two clusters. Both gates above write their own
   keys, and SECURITY.md requires both halves of a real one to be BROWSER-STATED — so that claim belongs to the
   live harness (`harness offscreen "return await self.rendererPoolProbe()"`) and is made nowhere here. Nor is
   a message routed BETWEEN two renderers: this stage speaks the ABI to each of two instances and compares
   their answers, while renderer-to-renderer routing is bridge.js's and is exercised by route.mjs one layer
   down. */
