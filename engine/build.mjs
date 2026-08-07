/* APIClient v2 build — minimal emscripten WASM build of CLEAN quickjs-ng + the
 * host scheduler entry (engine/host/main.c). Deliberately small: the old build
 * (COW barrier post-processing, wasm64, JSPI, Lexbor/Z3 link) is gone with the
 * fresh fork. Re-add each capability ONLY when the scheduler needs it, verified.
 *
 *   node engine/build.mjs           -> engine/host/out/qjs.mjs + qjs.wasm (node smoke test)
 *
 * Build success/failure is the milestone-0 signal (does clean quickjs-ng compile
 * + link + boot). Design-correctness verification stays on the live Chrome
 * harness once the browser target is wired.
 */
import { spawnSync } from "node:child_process";
import { mkdirSync, existsSync, copyFileSync, readdirSync, writeFileSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const OUT = join(HOST, "out");
const EXT_QJS = join(ENGINE, "..", "extension", "lib", "qjs");   // where bridge.js imports the engine from

/* `--list-sources` answers WHAT THE PROGRAM IS and exits, before the gates run — one of those gates shells back
   into this file to ask, and a list with three lines of gate output in front of it is not a list. */
const LIST_SOURCES = process.argv.includes("--list-sources");

/* The recognizer ratchet runs BEFORE anything is compiled (CLAUDE.md §C-stack). The ban was written down and then
   violated four times in one session with the rule already in the file — so it is BUILT, not written. A detector
   added back under any name fails the build here rather than needing to be caught in review. */
if (!LIST_SOURCES) {
  const r = spawnSync(process.execPath, [join(ENGINE, "check_recognizers.mjs")], { stdio: "inherit" });
  if (r.status !== 0) process.exit(r.status ?? 1);
}
/* The step-machine ownership declarations are paired with their state structs by editing pattern, in batches.
   That pairing is a type error C cannot see — a visit attached to the wrong struct compiles and passes the
   fixture — so it is asserted before anything is compiled, for the reason the ratchet above is. */
if (!LIST_SOURCES) {
  const r = spawnSync(process.execPath, [join(ENGINE, "check_step_visits.mjs")], { stdio: "inherit" });
  if (r.status !== 0) process.exit(r.status ?? 1);
}
/* The DOM chokepoint. dom_cow.h claimed the raw Lexbor mutators were "poisoned in the component build" and
   nothing poisoned anything — the claim was what people read instead of looking, and two real bypasses were
   sitting in element.c the first time this ran. A write that misses the chokepoint is invisible to the per-flow
   delta, so a forked arm reads its sibling's write and the unapply cannot put the baseline back. */
if (!LIST_SOURCES) {
  const r = spawnSync(process.execPath, [join(ENGINE, "check_dom_chokepoint.mjs")], { stdio: "inherit" });
  if (r.status !== 0) process.exit(r.status ?? 1);
}
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

// (The old browser IDL codegen — idlgen.mjs -> idl_generated.h — drove the deleted Blink-mirroring browser
// components. With the browser half removed pending re-build against the rewritten fork, there is nothing to
// generate; re-add the idlgen step when those components are rebuilt.)

// THE NEW-WORLD SOLVER CORE. The OLD main.c scheduler + heap_cow + the entire browser half were deleted as
// legacy: they were built on the fork's PREVIOUS hook API (JS_SetCowCaptureHooks / JS_SetFlowYieldHook /
// JS_SetArrayIterHook / …), which the rewritten fork replaced with the time-travel hook system
// (JS_SetTimeTravelHooks + JS_FlowNew/Resume/Clone + JS_SetBranchHook/ForkHook/PreemptHook). These are the
// files that actually link against the current fork. There is no qjs_* extension ABI entry yet — test_forced.c
// is the node smoke-test main (build.mjs's milestone signal: does the new world compile + link + run + PASS its
// @H/@S fixture). Rebuilding the production ABI entry that wraps engine.c's scheduler for the offscreen
// document — and re-growing the browser components against the new fork — is the next keystone.
const ABI = process.argv.includes("abi");          // build the production qjs_* entry instead of the smoke main()
const SOLVER = (f) => join(HOST, "solver", f);     // the Time-Travel Solver (the novel half)
const sources = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c"]
  .map((f) => join(QJS, f))
  .concat([
    SOLVER("cow.c"), SOLVER("engine.c"), SOLVER("flow.c"), SOLVER("decide.c"),   // per-flow COW + interleaving scheduler + weight + fork
    SOLVER("world.c"),                                                           // the delta's CROSS-INSTANCE half: a flow's world spans documents
    SOLVER("concolic.c"), SOLVER("endpoint.c"), SOLVER("solve.c"),               // concolic value + @H surface + @S solver
    SOLVER("absent.c"),                                                          // absent global: unknown app state vs a component this engine owes
    SOLVER("result.c"),                                                          // the ONE result document the host reads
    SOLVER("dom_cow.c"), SOLVER("attr_shadow.c"),                                // DOM time-travel delta + DOM-attribute taint shadow
    join(HOST, "browser", "core", "loader", "document_scripts.c"),               // Lexbor <script> inventory + bundle identity
    join(HOST, "browser", "core", "fetch", "fetch.c"),
    join(HOST, "browser", "core", "encoding", "encoding.c"),                  // TextEncoder/TextDecoder: the Encoding Standard's labels and decoders
    join(HOST, "browser", "core", "encoding", "text_stream.c"),               // TextDecoderStream/TextEncoderStream: §7.5/§7.6 over a TransformStream
    join(HOST, "browser", "core", "streams", "stream_work.c"),                     // the plumbing §4 and §5 share
    join(HOST, "browser", "core", "streams", "readable_stream.c"),
    join(HOST, "browser", "core", "streams", "writable_stream.c"),                  // WritableStream: Streams §5                // ReadableStream: Streams §4.2-§4.5
    join(HOST, "browser", "core", "streams", "transform_stream.c"),               // TransformStream: Streams §6
    join(HOST, "browser", "core", "streams", "pipe.c"),                          // §4.2.4's pipeTo/pipeThrough: the algorithm that holds a reader on one stream and a writer on another
    join(HOST, "browser", "core", "streams", "queuing_strategy.c"),               // Count/ByteLengthQueuingStrategy: Streams §7
    join(HOST, "browser", "core", "byte_reader.c"),                            // reading a byte sequence as a promise: Fetch §5.2's readers and File API §3.3's, one machine
    join(HOST, "browser", "core", "fetch", "body.c"),
    join(HOST, "browser", "core", "file", "blob.c"),                           // Blob: File API §3's immutable byte sequence
    join(HOST, "browser", "core", "html", "form_data.c"),                      // FormData: XHR §5's entry list, and what .formData() answers with                          // §5.2's Body mixin: one implementation, included by Request and Response
    join(HOST, "browser", "core", "fetch", "response.c"),
    join(HOST, "browser", "core", "fetch", "request.c"),                       // Request: §5.3, and where the request guards become observable                       // Response: the reply a fetch promises
    join(HOST, "browser", "core", "fetch", "headers.c"),                        // Headers: the header list an endpoint requires
    join(HOST, "browser", "core", "loader", "module_loader.c"),                 // dynamic import: the lazy-chunk register                           // the Fetch API: every reached request funnels into the @H surface
    join(HOST, "browser", "core", "url", "url.c"),
    join(HOST, "browser", "core", "url", "idna.c"),                            // IDNA: UTS-46 domain-to-ASCII over RFC 3492 Punycode
    join(HOST, "browser", "core", "url", "url_search_params.c"),                // URLSearchParams: §5.1's urlencoded list, and the URL's live view of it                              // WHATWG URL: the record and the basic URL parser every address goes through
    join(HOST, "browser", "core", "frame", "location.c"),                       // Location: concrete principal + concolic search/hash
    join(HOST, "browser", "core", "frame", "window.c"),                         // Window: which browsing context this is, and window.name as attacker input
    join(HOST, "browser", "core", "idl_args.c"),
    join(HOST, "browser", "core", "idl_iter.c"),                                // Web IDL's sequence<T>: the ES iterator protocol as requests                                // the ONE coerce-then-call machine every DOMString member shares
    join(HOST, "browser", "core", "dom", "abort.c"),                            // AbortController/AbortSignal: the controller's flag real, a timeout's unknown
    join(HOST, "browser", "core", "frame", "navigator.c"),
    join(HOST, "browser", "core", "frame", "screen.c"),                         // Screen: every member the environment, so every member forks                      // Navigator: spec-fixed identity concrete, the gated environment concolic
    join(HOST, "browser", "core", "timing", "timer.c"),                         // Timers: the timer task source, each expiry a flow
    join(HOST, "browser", "core", "dom", "document.c"),                         // Document: parsed facts concrete, cookie/referrer input
    join(HOST, "browser", "core", "dom", "node.c"),                             // Node: identity, the tree, and the CharacterData nodes
    join(HOST, "browser", "core", "dom", "element.c"),
    join(HOST, "browser", "core", "idl_indexed.c"),                             // Web IDL's indexed property getter
    join(HOST, "browser", "core", "dom", "collections.c"),                      // NodeList + HTMLCollection, live
    join(HOST, "browser", "core", "dom", "attr.c"),                             // DOM 4.9.2 Attr + NamedNodeMap
    join(HOST, "browser", "core", "dom", "document_fragment.c"),                // DOM 4.7 DocumentFragment
    join(HOST, "browser", "core", "dom", "dom_token_list.c"),                   // DOMTokenList: §7.1, and classList
    join(HOST, "browser", "core", "css", "css_style_declaration.c"),           // CSSOM: element.style + getComputedStyle
    join(HOST, "browser", "core", "html", "html_element.c"),                    // HTMLElement + HTML's per-tag interface table
    join(HOST, "browser", "core", "html", "hyperlink.c"),                        // HTMLHyperlinkElementUtils: HTML 4.6.3
    join(HOST, "browser", "core", "html", "dom_string_map.c"),                  // HTML 3.2.2 dataset
    join(HOST, "browser", "core", "html", "html_form.c"),                       // §4.10: a control's value state + submission
    join(HOST, "browser", "core", "html", "custom_elements.c"),                 // §4.13: the registry, upgrade and reactions
    join(HOST, "browser", "core", "html", "unhandled_rejection.c"),             // §8.1.7.5: rejections nobody handled
    join(HOST, "browser", "core", "frame", "policy_container.c"),                 // HTML 7.2.6: the policy container an about:blank child clones
    join(HOST, "browser", "core", "frame", "window_proxy.c"),                    // WindowProxy: HTML 7.2.5.1, the per-flow navigable binding
    join(HOST, "browser", "core", "frame", "window_message.c"),                  // window.postMessage: HTML 9.4.4
    join(HOST, "browser", "core", "frame", "bar_prop.c"),                        // BarProp: HTML 7.2.5.3
    join(HOST, "browser", "core", "frame", "navigable.c"),                       // window.open: HTML 7.4, and the about:blank child's inherited policy
    join(HOST, "browser", "core", "structured_clone.c"),                       // HTML 2.7: StructuredSerialize/Deserialize
    join(HOST, "browser", "core", "events", "event.c"),                         // Event: §2.2, the object a listener receives
    join(HOST, "browser", "core", "events", "message_event.c"),
    join(HOST, "browser", "core", "events", "message_port.c"),                  // MessagePort/MessageChannel: HTML 9.4.2/9.4.3                 // MessageEvent: HTML 9.4.1, what every messaging path dispatches
    join(HOST, "browser", "core", "events", "broadcast_channel.c"),             // BroadcastChannel: HTML 9.5
    join(HOST, "browser", "core", "events", "event_target.c"),                  // EventTarget: listeners + the load event
    // THE ENTRY. `abi` builds the production qjs_* surface the extension bridge drives; the default builds
    // test_forced.c's main() as the node smoke test. They are alternatives, never both: test_forced.c owns
    // main() and runs on load, which a bridge-loaded module must not do.
    join(HOST, ABI ? "main.c" : "test_forced.c"),
  ]);

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

// The exports the bridge ccalls. Emscripten drops anything not named here, so a function missing from this
// list is a runtime "no such symbol" in the extension rather than a link error — the list IS the ABI.
const QJS_ABI = ["qjs_init", "qjs_bundle_id", "qjs_begin", "qjs_step", "qjs_result", "qjs_teardown",
                 "qjs_pending", "qjs_chunks", "qjs_provide", "qjs_top_weight", "qjs_set_yield_floor",
                 "qjs_request_park", "qjs_emit_partial",
                 "qjs_host_requests", "qjs_host_answer"];

const args = [
  ...sources,
  LEXBOR_LIB,                 // link the cached Lexbor DOM archive
  "-I", QJS,
  "-I", HOST, "-I", join(HOST, "browser"),   // include by FULL path from the host root: a browser component is "core/dom/dom_element.h", a solver component "solver/concolic.h" — the layer is always explicit (no bare-name -I solver shortcut, so a cross-layer include names its layer)
  "-I", LEXBOR_INC,           // <lexbor/html/html.h> etc for main.c's DOM host-edges
  /* -Werror on implicit declarations: -w would otherwise let a missing #include truncate a returned
     pointer to 32 bits, which is a segfault with no diagnostic. See the same note in test262.mjs. */
  "-O1", "-w", "-Werror=implicit-function-declaration",
  "-D_GNU_SOURCE", "-DENABLE_DUMPS",
  // Offensive-programming build mode (check.h): DEV (default) keeps every DCHECK live so a should-never-happen
  // aborts LOUD at its origin; a `release` arg compiles them out (the release exemption — the user is not
  // crashed on an unsupportable state). CHECK (OOM/security) stays fatal in both.
  "-DAPICLIENT_DEV=" + (process.argv.includes("release") ? "0" : "1"),
  // Opt-in `assert` build: emscripten ASSERTIONS=2 turns a bare terse `Aborted()` into an INFORMATIVE crash
  // (the failing C assert + file:line — e.g. a refcount/gc_obj_list leak), the offensive-programming ideal of a
  // LOUD *and* diagnosable dev failure. Off by default so normal dev builds stay fast; enable when debugging.
  ...(process.argv.includes("assert") ? ["-sASSERTIONS=2"] : []),
  // AddressSanitizer build (`asan` arg) — the FIRST-CLASS memory-debugging tool a browser engineer expects:
  // intercepts malloc/free (quickjs's js_free routes to system free), so a double-free / use-after-free is
  // reported DETERMINISTICALLY with the alloc stack + BOTH free stacks (function names via --profiling-funcs).
  // ASan's shadow memory is incompatible with memory GROWTH, so it pins a fixed 1 GiB heap instead.
  ...(process.argv.includes("asan")
      ? ["-fsanitize=address", "--profiling-funcs", "-sINITIAL_MEMORY=1073741824",
         ...(process.argv.includes("dwarf") ? ["-g"] : [])]
      /* THE ARCHITECTURE'S CEILING, not a budget. wasm32 addresses 4 GiB and emscripten stops the heap at 2 GiB
         unless told otherwise, so the growth flag alone was a 2 GiB cap wearing the word "growth". The smoke
         fixture's forced multi-path run measures 3.1 GiB of peak RSS natively BEFORE any of this session's
         changes — the frontier holds every flow's COW delta in RAM because the smoke has no IDB cold tier to
         page the low-value tail into — so the 2 GiB stop was already the thing about to fail, and a 12%
         increase from Fetch's clone-as-tee is what crossed it. Raising it to what the address space actually
         holds is the platform floor; the real answer for a frontier this size is the cold tier, which is the
         scheduler's work and not a flag. */
      : ["-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=4294967296"]),
  "-sSTACK_SIZE=8388608",
  // The smoke entry RUNS on load and exits with the @H/@S pass code; the ABI entry is driven by the bridge
  // through ccall, so its runtime must stay alive across qjs_step re-entries and be importable as an ES module.
  ...(ABI
      ? ["-sEXPORTED_FUNCTIONS=" + JSON.stringify(QJS_ABI.map((f) => "_" + f).concat(["_malloc", "_free"])),
         "-sEXPORTED_RUNTIME_METHODS=" + JSON.stringify(["ccall", "lengthBytesUTF8", "stringToUTF8"]),
         "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createQJS", "-sINVOKE_RUN=0"]
      : ["-sEXIT_RUNTIME=1"]),
  /* THE ABI ARTIFACT STAGES WHERE THE EXTENSION LOADS IT: bridge.js does import("./lib/qjs/qjs.mjs"), so that
     is the output path, not engine/host/out. It also keeps the two targets from colliding — emcc derives the
     .wasm name from the -o basename, so both emitting into out/qjs.* shared one qjs.wasm and overwrote each
     other, leaving a loader pair that was valid only until the next smoke build. Two artifacts, two homes. */
  "-o", ABI ? join(EXT_QJS, "qjs.mjs") : join(OUT, "qjs.js"),
];

console.log("[build] emcc " + sources.length + " sources -> engine/host/out/qjs." + (ABI ? "mjs (production ABI)" : "js (new-world smoke test)"));
const r = spawnSync(EMCC, args, { stdio: "inherit", shell: true, cwd: QJS });
if (r.status !== 0) { console.error("[build] FAILED rc=" + r.status); process.exit(r.status || 1); }
console.log("[build] OK -> " + resolve(join(OUT, "qjs.js")));

// Milestone smoke test: run test_forced.c's main (the @H merge + @S sink fire-verification on a fixture doc) —
// the design-correctness signal until the live-Chrome harness is re-wired to a rebuilt production ABI entry.
// (The old ES6-module + qjs.wasm staging into extension/lib/qjs served the deleted qjs_* entry; it returns when
// that entry is rebuilt.)
if (ABI) {
  console.log("[build] OK -> " + resolve(join(EXT_QJS, "qjs.mjs")) + " (no smoke run: the ABI entry has no main())");
} else {
  const t = spawnSync(process.execPath, [join(OUT, "qjs.js")], { stdio: "inherit", shell: false });
  if (t.status !== 0) { console.error("[build] smoke test FAILED rc=" + (t.status ?? "signal")); process.exit(t.status || 1); }
  console.log("[build] smoke test PASS (new-world @H + @S)");
}
