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
import { mkdirSync, existsSync, copyFileSync, readdirSync, writeFileSync, statSync, readFileSync, rmSync } from "node:fs";
import { dirname, join, resolve, relative, sep } from "node:path";
import { cpus } from "node:os";
import { fileURLToPath } from "node:url";
import { stampArtifact } from "./gate_revision.mjs";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const OUT = join(HOST, "out");
const EXT_QJS = join(ENGINE, "..", "extension", "lib", "qjs");   // where bridge.js imports the engine from

/* `--list-sources` answers WHAT THE PROGRAM IS and exits — check_recursion.sh shells back into this file to ask,
   and a list with build output in front of it is not a list.
   `--list-include-roots` answers WHERE ITS HEADERS COME FROM, for the same reason and to the same rule: the
   compiler is handed these roots and nobody else may restate them. gate_revision.mjs's dangling-include check
   had its own copy of the list — four roots, hand-written — and the browser process's `-I BPROC_DIR` made that
   copy wrong the day it landed, so the check declared a revision that BUILDS to be one that "cannot be built by
   anyone who checks it out". A confident false red is worse than a silent miss: it is the phantom §Testing
   describes, and the next real dangling include arrives in a report nobody believes. The answer is per SOURCE
   SET and not a flat union, because a union would ACCEPT a renderer unit including "network/corb.h" — a header
   its compiler is never given — and that is precisely the include this check exists to catch. */
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

// THE IDL GAP AUDIT IS NOT IN THIS BUILD, AND THAT ABSENCE IS THE WORK — it is not a note about history.
// engine/idlgen.mjs's own header says it "Runs at build time (best-effort)", and CLAUDE.md §Web-IDL says it
// DIFFS the spec member list against what each component installs at build time. Nothing in this tree calls
// it: grep answers this comment and idlgen.mjs itself. What stood here said the browser half had been deleted
// and there was therefore "nothing to generate" — that stopped being true when the components came back, and
// the sentence went on reading as authoritative while walkC(join(HOST,"browser")) below grew to 157 real
// components. The output is host/browser/platform_names.h (the [Exposed=Window] table solver/absent.c decides
// ReferenceError-vs-fork on), which is COMMITTED so the build works with no network — the long-gone
// idl_generated.h it named is not what idlgen writes. So a spec member no component installs is reported by
// NOBODY today. Wiring the audit in here is the next diff, and it must FAIL rather than warn, like every other
// gate — a best-effort step that is skipped when @webref/idl is absent is a gate that silently is not one.

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
   A THIRD ENTRY STOOD HERE AND WAS DELETED, AND THE REASON IS WHAT SHAPES THE ONE BELOW. It was a "trusted"
   program linked out of its own source subtree so that MIME Sniffing §7 would not link into the renderer, and
   A SEPARATE LINK IS NOT A PROCESS BOUNDARY: every object in THIS list was offered to every link, so the two
   artifacts were built from the same objects and the trusted one was in fact built from the whole engine and
   was the LARGER of the two — the 0.04 MB against 6.60 MB was wasm-ld dead-stripping, not isolation. Both
   Modules instantiated in the offscreen's own realm with the host holding an exported HEAPU8 over each, so
   nothing about the link kept either out of the other's memory. A real boundary is a REALM the host cannot
   reach into: for the untrusted renderer that is a sandboxed opaque-origin frame (extension/renderer.html +
   extension/renderer-host.js), and for the trusted network service it is a dedicated WORKER
   (extension/browser-process.js) — see BPROC_SOURCES below and browser_process/network/mime_sniff.h. */
const ENTRY_SMOKE = join(HOST, "test_forced.c");
const ENTRY_ABI   = join(HOST, "main.c");
/* THE THIRD PROGRAM IS BACK AND THE PARAGRAPH ABOVE IS WHY IT IS DIFFERENT THIS TIME. What was deleted was a
   second link out of THIS list; what is here is a program with its OWN source list, its OWN objects (compiled
   below into `bp_`-prefixed object files with their own include path) and its OWN runtime — a dedicated Worker
   of the offscreen document, `extension/browser-process.js`, holding its own realm, its own module instance and
   its own thread, reachable only by postMessage. The link is not the boundary and never was; the WORKER is, and
   the link is what keeps §7 out of the renderer's symbol table so a renderer-side caller fails to link rather
   than aborting at run time. Nothing in SHARED_SOURCES is offered to it: it links four files, one of which
   (core/mime/mime_type.c) is a SOURCE both programs compile, which is what a shared source is — the same
   algorithm in two programs, the way Chromium's net/ links into both of its. */
const BPROC_DIR = join(HOST, "browser_process");
const BPROC_SOURCES = walkC(BPROC_DIR).concat([join(HOST, "browser", "core", "mime", "mime_type.c")]).sort();

/* THE HEADER ROOTS EACH SET'S COMPILER IS GIVEN, DECLARED ONCE. CFLAGS and BPCFLAGS below are BUILT from these
   rather than spelling them again, and `--list-include-roots` reports them, so the compiler, the build and any
   checker are reading one statement. The two lists differ deliberately: only a browser_process unit is given
   BPROC_DIR, which is what makes `#include "network/corb.h"` legal there and a build failure anywhere else.
   Include by FULL path from the host root — a browser component is "core/dom/dom_element.h", a solver component
   "solver/concolic.h" — so a cross-layer include always names its layer and no bare-name shortcut hides one. */
const ENGINE_INCLUDE_ROOTS = [QJS, HOST, join(HOST, "browser"), LEXBOR_INC];
const BPROC_INCLUDE_ROOTS = [HOST, join(HOST, "browser"), BPROC_DIR];
const dashI = (roots) => roots.flatMap((r) => ["-I", r]);
const BPROC_OUT = join(ENGINE, "..", "extension", "lib", "bproc");
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
/* DEDUPED, because core/mime/mime_type.c is in two PROGRAMS and is one FILE — check_recursion.sh reads this
   list to decide what to analyse, and a file named twice is a unit analysed twice. */
const sources = [...new Set(SHARED_SOURCES.concat([ENTRY_SMOKE, ENTRY_ABI], BPROC_SOURCES))];

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

/* THE HEADER ROOTS, PER SOURCE SET, EMITTED FROM THE ONE PLACE THAT HANDS THEM TO THE COMPILER. The two sets
   differ and the difference is the whole point: a browser_process unit is given `-I BPROC_DIR` so it may write
   `#include "network/corb.h"`, and a renderer unit is NOT, so the same line in a renderer file is a build
   failure that a consumer of this manifest must be able to SEE. Emitting a flat union would answer "fine" to
   both and turn the check into the diagnostic that always says yes.
   The roots are repo-relative because the consumer resolves them against a git revision rather than a path on
   this disk, and they are derived from the same CFLAGS/BPCFLAGS arrays the links use rather than restated —
   this file may not hold a second copy either, or it becomes the thing it is fixing. */
if (LIST_INCLUDE_ROOTS) {
  const rel = (p) => relative(join(ENGINE, ".."), p).split(sep).join("/");
  console.log(JSON.stringify([
    { name: "engine", roots: ENGINE_INCLUDE_ROOTS.map(rel),
      sources: sources.filter((f) => !BPROC_SOURCES.includes(f)).map(rel) },
    { name: "browser_process", roots: BPROC_INCLUDE_ROOTS.map(rel), sources: BPROC_SOURCES.map(rel) },
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
    const one = spawnSync(bin, ["--cold-park", store], { stdio: "inherit" });
    if (one.status !== 0) {
      console.error("[build] session ONE (--cold-park) reported rc=" + (one.status ?? "signal") +
                    " — read its `@H park-*` rows and the @COLDPARK census: a 0 there names which record kind " +
                    "the park did not write, and the moment it was taken at is `fixture_want_park` in " +
                    "engine/host/test_forced.c.");
      process.exit(one.status || 1);
    }
    const two = spawnSync(bin, ["--cold-resume", store], { stdio: "inherit" });
    if (two.status !== 0) {
      console.error("[build] session TWO (--cold-resume) reported rc=" + (two.status ?? "signal") +
                    " — `@RESUMED <n>` and the @COLDRESUME census say what it rebuilt out of the residue; a " +
                    "kind session one wrote and this one did not rebuild is the arm to look at.");
      process.exit(two.status || 1);
    }
    console.log("[build] cold round trip PASS (" + kind + ") — residue at " + store);
    process.exit(0);
  }
  /* AND IT IS RUN, because a target that is only built is the excluded test one layer down: the whole point is
     the stream it prints and the report it ends with, and nothing else in the tree produces either. */
  const t = spawnSync(bin, MIN ? ["--min"] : [], { stdio: "inherit" });
  if (t.status !== 0) {
    console.error("[build] the native run reported rc=" + (t.status ?? "signal") +
                  " — a LeakSanitizer summary above is a real leak, and an AddressSanitizer report a real fault");
    process.exit(t.status || 1);
  }
  console.log("[build] native run PASS (" + kind + (MIN ? ", minimal document" : "") + ")");
  process.exit(0);
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
                 "qjs_perform", "qjs_host_answer_remote"];

/* THE LIST IS THE ABI, SO THE ENTRY POINT AND THE LIST ARE ONE FACT AND ARE CHECKED AGAINST EACH OTHER. Both
   directions are a real defect and neither has a symptom at build time: an entry main.c defines and this omits
   is a capability the extension cannot call (or can, until a setting changes); a name here that main.c does not
   define is `--export=` of a symbol that does not exist, which wasm-ld reports as an undefined export only
   because ERROR_ON_UNDEFINED_SYMBOLS happens to be on. Read from the source rather than restated: `QJS_EXPORT`
   is exactly the marker main.c puts on every ABI body.
   IT IS A HELPER AGAIN, AND THE PARAGRAPH THAT SAID IT MUST NOT BE IS WHY THIS ONE SAYS SO. That paragraph
   read "there is ONE ABI … a helper kept for a caller that no longer exists is scaffolding", and it was true of
   the tree that deleted the second wasm-ld link. There are two PROGRAMS again — the renderer's qjs_* entry and
   the browser process's bp_* entry, in different source lists, behind a real boundary — so the second caller
   exists and the reason for a block is gone with it. The check is what must not be per-program: a second copy
   of these five lines is the hand-maintained list this file spends its length warning about, and the shape it
   fails in is silence — an entry added to one program's list with the copy for the other left unedited exports
   nothing and says nothing. */
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
    process.exit(1);
  }
}
abiCheck("renderer", join(HOST, "main.c"), "QJS_EXPORT", "qjs_", QJS_ABI);
/* THE BROWSER PROCESS'S ABI. Every entry is a question about BYTES answered with no state kept between calls,
   which is what a network service is and why there is no scheduler behind them. It is enforced by the same call
   for the same reason: an entry reaching `Module` because EXPORT_KEEPALIVE happens to be on is not an ABI, it
   is an accident that survives until a setting changes. */
const BP_ABI = ["bp_corb_check", "bp_classify"];
abiCheck("browser process", join(BPROC_DIR, "main.c"), "BP_EXPORT", "bp_", BP_ABI);

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
const objPath = (src) => join(OBJDIR, resolve(src).replace(/[\\/:]/g, "_") + ".o");

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
        p.on("exit", (code) => {
          if (code !== 0) failed++;
          running--;
          if (next >= stale.length && running === 0) done();
          else pump();
        });
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
   in the offscreen's own realm with the host holding an exported HEAPU8 over each. */
function link(what, entryObj, ldflags, out) {
  const l = spawnSync(EMCC, [...OBJS_SHARED, entryObj, ...LDFLAGS_COMMON, ...ldflags, "-o", out],
                      { stdio: "inherit", shell: true, cwd: QJS });
  if (l.status !== 0) { console.error("[build] " + what + " LINK FAILED rc=" + l.status); process.exit(l.status || 1); }
  console.log("[build] OK -> " + out);
}
link("smoke", objPath(ENTRY_SMOKE), LDFLAGS_SMOKE, join(OUT, "qjs.js"));
link("production ABI", objPath(ENTRY_ABI), LDFLAGS_ABI, join(EXT_QJS, "qjs.mjs"));

/* ── THE BROWSER PROCESS ──────────────────────────────────────────────────────────────────────────────────
   A SECOND PROGRAM, NOT A SECOND LINK OF THE FIRST. Its objects are compiled here, from BPROC_SOURCES only,
   under `bp_`-prefixed paths so that a file both programs contain (core/mime/mime_type.c) cannot be handed
   from one compile's cache to the other's link — the include path differs, and an object cache keyed on the
   source path alone would silently reuse an object built with the wrong `-I`. It links no lexbor, no quickjs
   and nothing under host/solver or host/browser except the MIME record, which is what makes a renderer-side
   call to §7 an undefined symbol.
   IT IS SEQUENTIAL because it is four translation units; the parallel pump above exists for 130. */
{
  mkdirSync(BPROC_OUT, { recursive: true });
  const bpObj = (src) => join(OBJDIR, "bp_" + resolve(src).replace(/[\\/:]/g, "_") + ".o");
  const BPCFLAGS = [
    ...dashI(BPROC_INCLUDE_ROOTS),   // declared once beside the source sets; "network/corb.h" resolves ONLY here
    "-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses",
    "-Werror=implicit-function-declaration",
    "-D_GNU_SOURCE",
    "-DAPICLIENT_DEV=" + (process.argv.includes("release") ? "0" : "1"),
  ];
  for (const src of BPROC_SOURCES) {
    const obj = bpObj(src);
    if (!objIsStale(src, obj)) continue;
    const c = spawnSync(EMCC, [...BPCFLAGS, "-MMD", "-MF", obj.replace(/\.o$/, ".d"), "-c", src, "-o", obj],
                        { stdio: "inherit", shell: true, cwd: QJS });
    if (c.status !== 0) { console.error("[build] browser process: " + src + " did not compile"); process.exit(1); }
  }
  const l = spawnSync(EMCC, [
    ...BPROC_SOURCES.map(bpObj),
    /* THERE IS NO `main()` IN THIS PROGRAM AND THE LINK SAYS SO RATHER THAN LEAVING IT TO BE INFERRED. A
       network service is entered by its callers; browser_process/main.c owns the ABI the way host/main.c owns
       qjs_*, and neither runs on load. emcc would reach the same conclusion on its own (no `_main` in
       EXPORTED_FUNCTIONS turns EXPECT_MAIN off), and that is precisely the shape the ABI check above refuses to
       depend on: a property held by the accident of a default is a property the next setting change takes away
       with no diagnostic. */
    "--no-entry",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sEXPORTED_FUNCTIONS=" + JSON.stringify(BP_ABI.map((f) => "_" + f).concat(["_malloc", "_free"])),
    "-sEXPORTED_RUNTIME_METHODS=" + JSON.stringify(["ccall", "HEAPU8"]),
    "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createBrowserProcess", "-sINVOKE_RUN=0",
    "-o", join(BPROC_OUT, "bproc.mjs"),
  ], { stdio: "inherit", shell: true, cwd: QJS });
  if (l.status !== 0) { console.error("[build] browser process LINK FAILED rc=" + l.status); process.exit(l.status || 1); }
  console.log("[build] OK -> " + join(BPROC_OUT, "bproc.mjs"));
}

/* THE ARTIFACT RECORDS THE REVISION IT WAS BUILT FROM, because engine/solvergate.mjs runs this file and
   never compiles anything, so without a stamp the only question it could ask about the program was how old
   the FILE was. That answer is wrong in exactly the mode CLAUDE.md §Testing mandates: `git worktree add`
   writes every tracked file at the checkout instant, so in a frozen snapshot every source is newer than
   any artifact and the check reported a build OF that revision as stale against 600 sources, three minutes
   after the checkout. The stamp is computed by gate_revision.mjs itself rather than re-derived here, so
   what is written and what is checked are the same answer by construction. The cone is what this link
   actually compiled — the host and the submodule — and not the whole tree, for the reason that file gives:
   another agent's popup edit is not a reason to distrust a JS-engine number. */
stampArtifact(join(EXT_QJS, "qjs.mjs"), ["engine/host", "engine/qjs"]);

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

/* The trailing arguments reach main()'s argv — the channel getenv could not be, since emscripten's ENV never
   merges the launching process's environment. */
{
  const t = spawnSync(process.execPath, [join(OUT, "qjs.js"), ...(MIN ? ["--min"] : [])],
                      { stdio: "inherit", shell: false });
  if (t.status !== 0) { console.error("[build] smoke test FAILED rc=" + (t.status ?? "signal")); process.exit(t.status || 1); }
  console.log("[build] smoke test PASS (new-world @H + @S" + (MIN ? ", minimal document" : "") + ")");
}

/* THE SHIPPED ENTRY, DRIVEN THROUGH THE SURFACE THE BRIDGE ACTUALLY CALLS. "The ABI entry has no main()" was
   true and was not a reason: the entry is DRIVEN, so its smoke test is a driver, and engine/route.mjs is one —
   it boots the module just linked, provisions a SECOND instance from the create notice, and routes a post
   between them. That is the shipped qjs_init/qjs_begin/qjs_step/qjs_pending/qjs_host_notices/qjs_route path,
   end to end. It is also the only thing in the tree that provisions two instances at all, which §SECURITY makes
   the precondition for believing any cross-instance mechanism has ever run — and it used to run only when
   somebody remembered to pass `abi`, which is to say almost never. */
{
  const t = spawnSync(process.execPath, [join(ENGINE, "route.mjs")], { stdio: "inherit", shell: false });
  if (t.status !== 0) {
    console.error("[build] the two-instance ABI drive FAILED rc=" + (t.status ?? "signal"));
    process.exit(t.status || 1);
  }
  console.log("[build] two-instance ABI drive PASS");
}

/* AND THE BROWSER PROCESS IS DRIVEN TOO, for the reason every other target here is: a program that is only
   built is the excluded test one layer down. engine/bproc.mjs loads the module just linked and puts the
   MISLABELLED cases through it — the only cases §7 and CORB exist for — so the C is exercised in a process
   with no browser in it, and the live-Chrome probe (self.browserProcessProbe) is then measuring the WORKER
   boundary rather than the algorithm. */
{
  const t = spawnSync(process.execPath, [join(ENGINE, "bproc.mjs")], { stdio: "inherit", shell: false });
  if (t.status !== 0) {
    console.error("[build] the browser-process drive FAILED rc=" + (t.status ?? "signal"));
    process.exit(t.status || 1);
  }
  console.log("[build] browser-process drive PASS");
}
