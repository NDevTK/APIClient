/* WEB-PLATFORM-TESTS — the browser half's correctness gate.
 *
 * WHY: test262 is the JS half's oracle, and the browser half had nothing equivalent. Its only checks were the
 * IDL audit, which measures COVERAGE and can only say what is ABSENT, and a fixture of probes written by
 * whoever wrote the component — which tests what that person already thought of. Both missed the same things:
 * the audit had no row for Headers at all, then reported it COMPLETE with four members missing, and the fixture
 * agreed because I wrote both. WPT is written by the people who wrote the spec, so it disagrees.
 *
 * ONE PROCESS PER TEST FILE, deliberately. A DCHECK is an abort — that is the whole point of it — so a runner
 * that ran the corpus in one process would report the first unbuilt capability and nothing after it. Per-file
 * isolation makes an abort a RESULT for that file and leaves the rest of the picture intact, which is the
 * difference between a gate and a bisect.
 *
 * The corpus is pinned, like lexbor and test262: a moving corpus turns a regression into an argument.
 *
 * Usage:  node engine/wpt.mjs [subdir-or-file]
 */
import { spawnSync, spawn } from "node:child_process";
import { existsSync, readdirSync, mkdtempSync, mkdirSync, openSync, readFileSync, statSync,
         writeFileSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
import { tmpdir, cpus, loadavg } from "node:os";
import { gateRevision, revisionLines, revisionMoved } from "./gate_revision.mjs";

function walk(dir) {
  const out = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...walk(p));
    else if (e.name.endsWith(".c")) out.push(p);
  }
  return out;
}

const ENGINE = import.meta.dirname;
const WORK = join(ENGINE, ".work");
const WPT = join(WORK, "wpt");
/* PINNED. The corpus is an oracle, and an oracle that changes under you turns "this regressed" into "did it?".
   Re-pin deliberately, as a commit of its own, so the diff in results has one cause. */
const WPT_REV = "bf4714d";

/* A test's budget is the CPU it may CONSUME, plus a wall backstop for a test that consumes none because it is
   deadlocked on I/O. See the spawn below for why elapsed time alone cannot be the verdict. */
/* The HARD limit sits above the soft one so the kernel raises SIGXCPU at the soft limit and only escalates to
   SIGKILL if the process ignores it. Set as one value, dash makes soft == hard and the first notification IS the
   kill — measured: the same busy loop reports SIGKILL with `ulimit -t 1` and SIGXCPU with the pair. Both are
   read as the CPU cause below; the pair just makes the usual path say so in its own signal. */
const CPU_BUDGET_S = 60;
const WALL_BACKSTOP_MS = 600_000;
/* The kernel's own tick, asked for rather than assumed: /proc's CPU fields are in clock ticks and a hardcoded
   100 is a constant this file would have no way of noticing had gone wrong. */
const CLK_TCK = Number(spawnSync("getconf", ["CLK_TCK"], { encoding: "utf8" }).stdout.trim()) || 0;
if (!CLK_TCK) { console.error("[wpt] getconf CLK_TCK answered nothing, so a child's CPU cannot be read"); process.exit(1); }
/* WHAT IS CHECKED OUT. A sparse list rather than the whole 1 GB tree, and it grows as areas are covered — an
   area absent here is honestly untested, which is a different statement from "passes". */
/* `tools` is not a test area — it is WPT'S OWN SERVER and its vendored dependencies. The corpus is SERVED by
   it rather than read off disk, because a `.py` path is a handler the server imports and calls, and because
   the rewrites and content types are then wptserve's own rather than a table copied into this file. */
/* A PATH THAT CONTAINS ANOTHER LISTED PATH IS REPORTED AT THE FINER ONE. `dom` is one standard and eight
   COMPONENTS — Node/Document/Element, Event/EventTarget, AbortController, HTMLCollection, DOMTokenList, Range,
   NodeIterator/TreeWalker, Observable — which fail in DIFFERENT ways and at different depths, so a single
   `dom` row would fold each component's answer into one number in which none of them is visible. That is the
   sentence this file already applies to the TOTAL, one level down. So the list carries
   both: `dom` is what is CHECKED OUT and RUN (its ten root-level test files belong to no subdirectory and would
   otherwise be silently dropped), and `dom/nodes` and its siblings are REPORTING refinements — a path with a
   listed ancestor is never walked a second time, and byArea takes the LONGEST match. */
const WPT_PATHS = ["resources", "fetch/api/headers", "fetch/api/response", "fetch/api/resources", "url", "common",
                   /* THE STANDARD, THEN ITS COMPONENTS — the same two-level shape as `dom` below, and for the
                      same reason twice over. `FileAPI/blob` and three siblings were listed and the STANDARD was
                      not, so ten test files living at FileAPI's own level — fileReader, unicode, idlharness —
                      were checked out by cone mode and collected by nothing, and FileReader's and BlobURL's
                      directories were not measured at all. */
                   "FileAPI", "FileAPI/blob", "FileAPI/file", "FileAPI/support", "FileAPI/url",
                   "FileAPI/BlobURL", "FileAPI/FileReader", "FileAPI/filelist-section",
                   "FileAPI/reading-data-section",
                   "encoding", "tools",
                   /* THE WHATWG DOM. Eighteen DOM/HTML step machines were converted to rest at exact spec steps
                      — createElement, setAttribute, the mutation algorithms, innerHTML/outerHTML — and the gate
                      that would have judged them did not collect a single one of their tests, because this
                      standard's directory had never been checked out. It was verified against html/browsers
                      instead, which is a different component.
                      THIS ROW USED TO SAY `dom/ranges`, `dom/traversal` and `dom/observable` "have no
                      implementation at all", AND THAT IS NO LONGER TRUE: range.c and abstract_range.c,
                      node_iterator.c and tree_walker.c, observable.c with observable_ops.c — 4257 lines. The
                      claim was written when it was true and was never revisited, which is the stale-DFAIL
                      failure this project names: accurate about the SPEC, wrong about THIS TREE. It also made
                      those three rows unreadable in the worst way, by pre-declaring their numbers as expected
                      — a row nobody would read a regression out of. They stay listed, for the reason every
                      other row is listed rather than that one: an area absent from this list is untested,
                      which is a different statement from "passes". */
                   "dom", "dom/abort", "dom/collections", "dom/events", "dom/lists", "dom/nodes",
                   "dom/observable", "dom/ranges", "dom/traversal",
                   /* HTML §8.5.4/§8.5.5 — innerHTML, outerHTML, insertAdjacentHTML and the fragment parsing
                      algorithm, which element.c implements and which SPEC_STEPS.md §4 is the conversion target
                      for. THIS ROW USED TO SAY DOMParser AND XMLSerializer "live here too and are absent",
                      and half of it has stopped being true: §8.5.1's DOMParser is core/html/domparser.c, whose
                      `text/html` arm is the engine's own document parse and whose XML arm CRASHES by name.
                      So what this directory measures now is three different things and the row says which:
                      the `text/html` files are a real number where they were a missing global, §8.5.8's
                      XMLSerializer is still honestly absent (engine/idlgen.mjs's UNBUILT names why), and every
                      file that reaches an XML type ABORTS on a capability neither interface can have until an
                      XML parser exists — including the two that do both, which abort partway. A count is not
                      quoted here on purpose: this row says what is measured, not what it measures. */
                   "domparsing",
                   /* THE CSS STANDARDS THIS ENGINE HAS COMPONENTS FOR, and until now `grep '"css'` over this
                      file answered ZERO. That is the excluded-test failure at its largest here: core/css holds
                      the cascade, the computed and used value paths, CSSOM's style sheets, rules and
                      declaration blocks, and core/layout holds the containing-block chain — and not one line of
                      any of it had a spec directory checked out. A component whose spec directory is not
                      collected is a component whose gate cannot fail.
                      NAMED PER STANDARD, and only the three whose components exist, because `css` is the
                      largest tree in the corpus and most of it is REFTESTS — files that render two documents
                      and compare pixels. This gate collects a test by whether it loads
                      `/resources/testharness.js`, so a reftest is materialized on disk and collected by
                      nothing: adding `css` whole would cost tens of thousands of files to measure the same
                      subtests these three already name. `css/cssom` is CSSStyleDeclaration, CSSStyleSheet,
                      CSSRule and serialize-a-declaration-block; `css/cssom-view` is clientTop/clientLeft,
                      clientWidth/clientHeight and the scrolling area; `css/css-values` is the viewport units
                      and the length grammar. The standards whose components are NOT built — css-backgrounds
                      beyond the border longhands, css-sizing, css-cascade, css-logical — are deliberately
                      absent rather than silently included, and each becomes a row the day its component does.
                      Expect bad first numbers, including aborts: a `width: auto` box with a real border is a
                      joint function of the ICB and the device pixel ratio, and css_px_combine crashes on that
                      multi-fact domain by design. That crash's frequency IS the size of the next subproblem,
                      which is what a first measurement is for. */
                   "css/cssom", "css/cssom-view", "css/css-values",
                   /* AND THE HELPERS THOSE THREE STANDARDS' TESTS LOAD, which is the missing-META-script defect
                      this file already calls an EXCLUDED TEST one screen up — measured, not anticipated: 111
                      collected files aborted with "a <script src> the corpus does not serve" naming a file
                      under `css/support`, and 107 of them are in `css/css-values` alone, out of the 268 that
                      area runs. TWO FIFTHS of the area was reporting a fact about THIS LIST rather than about
                      the engine, and the abort NAMED the path every time, which is exactly the widening those
                      abort lines exist to ask for. Five of the nine helpers here answer all 111:
                      parsing-testcommon.js (38), serialize-testcommon.js (21), numeric-testcommon.js (19),
                      interpolation-testcommon.js (18) and computed-testcommon.js (11).
                      IT COSTS NO TEST AND NO SUBTREE, which is the whole reason it is safe to name and is
                      CHECKED rather than assumed: at the pinned revision `css/support` is FLAT — 61 files, zero
                      subdirectories — and not one of them loads `/resources/testharness.js`, so it is checked
                      out to BE USED, with the same standing as `html/resources`, `wai-aria/scripts` and
                      `service-workers/service-worker/resources`, and it adds nothing to the stray census below.
                      Its only ancestor is `css` itself, whose own level cone mode has already materialized for
                      the three standards above and which holds two blobs, neither a test.
                      THE OTHER HELPERS THOSE ABORTS NAME ARE DELIBERATELY NOT HERE, because they are not the
                      same shape: `css/mediaqueries/resources`, `css/css-scroll-snap/support` and
                      `css/css-fonts/support` each sit under a standard whose OWN LEVEL cone mode would then
                      materialize — 106, 92 and 680 blobs respectively — so each is a decision about running
                      that standard, not a helper that costs nothing, and each belongs to whoever takes it. */
                   "css/support",
                   /* THE COMPONENTS' OWN SPEC DIRECTORIES, each named because the component exists in
                      engine/host/browser/core and its tests were not being collected: request.c and body.c
                      (fetch/api/headers and .../response were checked out and their two siblings were not),
                      timer.c, structured_clone.c, form_data.c. */
                   "fetch/api/request", "fetch/api/body", "fetch/api/abort",
                   "html/webappapis/system-state-and-capabilities/the-navigator-object",
                   "html/webappapis/timers", "html/webappapis/microtask-queuing",
                   /* EVERYTHING ROOTED AT `navigator`, which this gate could not have measured whatever it
                      checked out: wpt_runner.c never called navigator_init, so its realm had no `navigator`
                      at all. The-navigator-object above WAS collected and was therefore failing on a missing
                      global rather than on the component — and the three standards that hang off partial
                      interfaces of Navigator were not checked out either, so their components (core/
                      permissions, core/file's virtual filesystem and StorageManager, core/html/
                      user_activation) had a gate that could not fail. Both halves are fixed together,
                      because either one alone still reports a number about the other's absence.
                      Permissions §6.2 is `navigator.permissions`; File System Access is reached through
                      Storage §3's `navigator.storage.getDirectory()`, which is why `fs` and `storage` are one
                      surface and not two; HTML §6.4.4 is `navigator.userActivation`. First numbers here are
                      expected to be bad, and a bad first number is the honest measurement this gate exists to
                      take rather than a reason to leave the directories out. */
                   "permissions", "storage", "fs", "html/user-activation",
                   /* HTML §8.1.7.3's rendering loop and §8.9's animation frames — core/rendering. The
                      component did not exist, so neither did this row; `requestAnimationFrame` was one of the
                      ~1300 names browser/platform_names.h had the engine reporting as honestly ABSENT. Now
                      that the rendering task source, "update the rendering" and §8.9's map are built, a
                      directory that is not collected is an EXCLUDED TEST — which this file calls a failure
                      one paragraph up. */
                   "html/webappapis/animation-frames",
                   "html/webappapis/structured-clone",
                   /* HTML §8.1.4.2's TWO SCRIPT-FETCHING ALGORITHMS AND §4.12.3's TEMPLATE, whose
                      components now exist and whose standard directory has never been checked out. This is
                      the excluded-test failure this file names twice above, and both rows are load-bearing
                      right now rather than prospectively.
                      `the-script-element` is 814 files, 304 of them under `module/`. core/loader/
                      script_fetch.c is fetch-a-classic-script and fetch-a-single-module-script — two
                      DIFFERENT decodes, Encoding §6.1's `decode` with a fallback for the classic entry and
                      §6's UTF-8 decode for the module one — and the corpus's own `charset-*` and
                      `external-script-utf8.js` files are what distinguish them. Neither was materialized on
                      disk, so the difference between the two algorithms had no test in this tree at all and
                      the component was verified only by fixtures its own author wrote.
                      `the-template-element` is 57 files, and it is the directory for the assert that is
                      FIRING as this row is written: node_heap.c's teardown check names §4.12.3's template
                      contents as the allocations nothing walks, because the contents fragment is not a child
                      of the inert owner document and lexbor's template destructor frees the fragment without
                      what is in it. A standard whose absence is already crashing the build is the clearest
                      possible case that an uncollected directory is an untested one.
                      NAMED PER COMPONENT, not `html/semantics` whole, for the reason `css` is: the parent
                      holds forms, embedded content and tabular data, whose components do not exist. Files at
                      `scripting-1/`'s own level are META.yml only, so the two-level shape FileAPI needed is
                      not needed here — checked rather than assumed, because that lesson cost ten files once.
                      Expect bad first numbers: async/defer ordering, `document.write`'d scripts and dynamic
                      import are all in there. A bad first number is the measurement. */
                   "html/semantics/scripting-1/the-script-element",
                   "html/semantics/scripting-1/the-template-element",
                   /* XHR §3 and §5 — core/xhr. Its two SUPPORT directories were listed and the standard's own
                      was not, so the 305 test files of the standard this engine had no component for were the
                      largest single uncollected population in the checkout. `xhr` is the standard; the two
                      below are its fixtures and the FormData tests that live under it. */
                   "xhr", "xhr/formdata", "xhr/resources",
                   /* custom_elements.c — HTML §4.13, the reactions that every [CEReactions] operation in
                      SPEC_STEPS.md runs. */
                   "custom-elements",
                   /* shadow_root.c — DOM §4.8 and §4.2.2's slots, which did not exist in this engine at all
                      until they did: no `attachShadow`, no ShadowRoot interface, no `<slot>`. The directory is
                      the standard's own and its first number is expected to be bad, which is the honest first
                      measurement rather than a reason not to take it. */
                   "shadow-dom",
                   /* Web IDL §3.2's ECMAScript binding — SPEC_STEPS.md §7's own directory, and the spec
                      idl_args.c, idl_iter.c and idl_indexed.c implement: what an interface object is, how a
                      DOMString/sequence/record argument is converted, what `has instance` and `toString`
                      answer. The IDL audit in build.mjs measures which members EXIST; this measures whether
                      the binding around them behaves. */
                   /* AND THE STANDARD ITSELF, for the reason FileAPI and streams each carry above and this one
                      makes three: `webidl/current-realm.html` and `webidl/idlharness.any.js` sit at Web IDL's
                      own level, were checked out by cone mode, and were collected by nothing. Naming the
                      standard adds no file to disk — `ecmascript-binding` is its only subdirectory — and adds
                      the two tests that were being excluded. */
                   "webidl", "webidl/ecmascript-binding",
                   /* location.c — HTML §7.10's Location, whose own directory was never checked out even though
                      the component is named in the project's own architecture. */
                   "html/browsers/history/the-location-interface",
                   /* THE COMPONENT'S OWN SPEC TESTS. readable_stream.c was written, and then measured against
                      fetch and FileAPI — which use a stream but assert almost nothing ABOUT one. A component
                      whose spec directory is not checked out is a component whose gate cannot fail, which is
                      the same defect as a gate that only reads the spelling that existed when it was written. */
                   /* And the standard itself, for the reason FileAPI carries above: queuing_strategy.c's own
                      three test files sit at `streams`'s own level and no listed path reached them. */
                   "streams", "streams/readable-streams", "streams/resources", "streams/writable-streams",
                   "streams/piping", "streams/transform-streams", "streams/readable-byte-streams",
                   "streams/transferable",
                   /* HTML §9.4's MESSAGING. Cross-document messaging is where popups, iframes and this
                      engine's one-instance-per-ORIGIN-KEYED-AGENT-CLUSTER rule meet, and it is also where the
                      solver's `message.origin` attacker source comes from.
                      WHAT THIS ROW USED TO SAY WAS "None of it exists yet", AND THAT HAD STOPPED BEING TRUE.
                      It is the stale-DFAIL failure exactly: accurate about the SPEC, wrong about THIS TREE,
                      and it reads as authoritative while sending the next reader to build what is already
                      here. Five components implement this standard — events/message_port.c,
                      events/message_event.c, events/broadcast_channel.c, frame/window_message.c and
                      structured_clone.c, 2674 lines between them — installing MessageChannel, port1/port2,
                      postMessage, start, close, and MessageEvent's data/origin/source/ports/lastEventId, with
                      no js_noop among them. So this directory is not measuring an absence; it is measuring
                      components that exist, which is the stronger reason to collect it.
                      AND IT HAS ALREADY CAUGHT ONE: `window.postMessage(msg)` was reading an UNDEFINED
                      targetOrigin where §9.4.4 reads `"/"`, because the conversion never visited an omitted
                      dictionary argument's position. That was a live defect in shipped code, found by reading
                      rather than by this gate — which is the argument for pointing the gate at it.
                      WHAT IS GENUINELY ABSENT is the WORKER half: there is no worker component in
                      browser/core at all, so every cross-worker file here measures that honestly. */
                   "webmessaging",
                   /* INDEXED DATABASE — core/indexeddb. The reason this was held back is gone: every file in
                      this directory opens with `indexedDB.open`, which until now had no code at all, so the
                      whole standard would have reported one abort and said nothing about anything. §5.1 is
                      now three step machines and the round trip runs, which means the directory stops being
                      "no result" and becomes a real fraction — and §Testing's rule is that a directory which
                      ABORTS is not better than one reporting errors, it is the same failures with the count
                      hidden. Expect a bad first number: most files reach `db.transaction(…)` in their second
                      act, which is absent, and a large share reach §5.5 step 2's unbuilt REVERT. That is the
                      honest first measurement of this area rather than a reason to leave it out.
                      TWO LEVELS, the same shape `FileAPI` and `dom` carry above and for the identical reason:
                      the standard's own level holds `idlharness.any.js` and `bindings-inject-key.any.js`,
                      which cone mode checks out and a subdirectory-only listing collects with nothing. */
                   "IndexedDB", "IndexedDB/resources",
                   /* HTML §7.2.5.1 AND §7.4 — WindowProxy and popups. `window.open`, `opener`, `parent`,
                      `top`, `frames`, named access, and what a cross-origin WindowProxy may expose. This
                      engine has just grown a WindowProxy member surface and §7.4's open(), and neither had a
                      spec directory: a component whose spec directory is not checked out is a component whose
                      gate cannot fail, which is the same defect as asserting only the spelling that existed
                      when the assertion was written. */
                   "html/browsers/windows", "html/browsers/the-window-object",
                   /* THE HELPERS A CHECKED-OUT TEST'S META BLOCK NAMES, which are not areas and are checked out
                      to BE USED — the same standing as `resources` and `common`. Four webmessaging files were
                      reported as "META script not checked out", which is an EXCLUDED TEST wearing a reason:
                      the gate had decided what it measures and then not measured four of them. Naming the
                      helper's own `resources` directory checks out no test file at all, so nothing is added to
                      the total except the four files that can now run. */
                   "html/browsers/browsing-the-web/remote-context-helper/resources",
                   "html/browsers/browsing-the-web/back-forward-cache/resources",
                   /* `/html/resources/common.js` is named by a `<script src>` in dom/ranges, and a document
                      whose script 404s runs a test nobody wrote. Its last segment is `resources`, so it is
                      checked out to BE USED and contributes no test of its own. */
                   "html/resources",
                   /* The same, for `shadow-dom/reference-target/`: ten of its files name
                      `/wai-aria/scripts/aria-utils.js`, and the gate reported all ten as ABORTED with "a
                      <script src> the corpus does not serve" — which is an EXCLUDED TEST wearing a reason.
                      The directory holds helper scripts and no testharness document, so it adds ten runnable
                      files and no test of its own. */
                   "wai-aria/scripts",
                   /* `/service-workers/service-worker/resources/test-helpers.sub.js` is named by five
                      webmessaging files — three broadcastchannel documents and two message-channels scripts —
                      and by five of the back-forward-cache tests this gate now runs. Checked out to BE USED; it
                      contributes no test of its own. */
                   "service-workers/service-worker/resources"];

/* AND THE DIRECTORIES WHOSE OWN LEVEL CONE MODE HAS ALREADY PUT ON DISK. A cone-mode checkout materializes every
   file of every directory ON THE PATH to a listed one, so naming one helper's `resources` lands its standard's
   own level too: 265 test files sat in this checkout, collected by nothing, while the census at the foot of this
   file printed the number every run and named no decision about a single one. A count nobody acts on is the
   excluded-test failure this gate exists to catch, one level out from the corpus.
   AN ENTRY HERE ADDS NOTHING TO THE CHECKOUT — that is the whole difference from a WPT_PATHS entry, which pulls
   the SUBTREE as well. At the pinned revision those subtrees are `html/semantics` 3970 files,
   `service-workers/service-worker` 766, `fetch/api` 284 and `wai-aria` 273 (234 of them under `manual/`).
   Running an own level costs none of that, and each subtree stays absent — every unlisted path's standing
   statement, which is untested rather than passing.
   NOTHING IS PREDICTED HERE ABOUT WHAT THEY SCORE. Not one of these files has ever been run in this tree, so a
   sentence claiming which capability they will name would be a guess sitting where the next reader takes it for
   a fact. What each abort NAMES is the work queue, and it is read off the run.
   BOTH DIRECTIONS ARE ASSERTED. An entry whose own level holds no test file fails this gate — a reason that
   outlives the absence it describes lies about the tree — and so does a test file on disk that NEITHER list
   accounts for, which is what makes silence impossible rather than merely discouraged. */
const WPT_OWN_LEVEL = [
  /* The standard's own `idlharness.https.any.js`, for a standard five WPT_PATHS entries already measure — the
     FileAPI/streams/webidl/IndexedDB shape a fifth time. Its unlisted subtree is basic/, cors/, redirect/,
     policies/, credentials/ and crashtests/: 121 files, and a decision for whoever takes it. */
  "fetch/api",
  /* `interfaces.html` and `rellist-feature-detection.html`. Naming the standard instead collects 3970 files
     across forms, embedded content and tabular data. */
  "html/semantics",
  /* The twelve files at back-forward-cache's own level, on disk because their own `resources` is listed above —
     five files under IndexedDB, fs and webmessaging name its helpers. Its unlisted sibling directories are
     broadcastchannel/ and eligibility/. */
  "html/browsers/browsing-the-web/back-forward-cache",
  /* `accessibility_properties_basic.tentative.html` and `idlharness.window.js`, on disk because
     `wai-aria/scripts` is listed for shadow-dom/reference-target. */
  "wai-aria",
  /* The standard's own `idlharness.https.any.js`, and the 247 test files at service-worker's own level — the
     largest population this gate had on disk and did not run. Its five subdirectories
     (ServiceWorkerGlobalScope/, multi-globals/, navigation-preload/, tentative/ and the listed resources/) are
     otherwise not on disk and are claimed by nothing here. */
  "service-workers", "service-workers/service-worker"];

if (!existsSync(join(WPT, "resources", "testharness.js"))) {
  /* NO --depth 1. The corpus is PINNED, and a depth-1 clone has only the tip — `git checkout bf4714d` in it
     fails with "reference is not a tree", so the instructions this gate printed could not be followed. The
     blob filter is what keeps a full-history clone cheap: it fetches commits and trees, and file contents only
     for the paths the sparse checkout names. */
  console.error("[wpt] corpus missing — provision it with:\n" +
    `  git clone --filter=blob:none --sparse https://github.com/web-platform-tests/wpt.git ${WPT}\n` +
    `  cd ${WPT} && git sparse-checkout set ${WPT_PATHS.join(" ")} && git checkout ${WPT_REV}`);
  process.exit(1);
}

/* THE ORACLE IS SHARED, AND THE ENFORCEMENT BELOW MUTATES IT. `sparse-checkout set` is a WRITE to a checkout every
   concurrent run of this gate also writes — and WPT_PATHS is read from whichever tree the run was launched
   from, so two runs at two revisions apply two different cones to ONE corpus and each removes what the other
   put there. It is not hypothetical and it is not rare: this repository currently has worktrees at revisions
   whose WPT_PATHS differ by ten entries (`permissions`, `fs`, `storage`, `xhr`, `webidl`, `shadow-dom`,
   `html/user-activation`, `wai-aria/scripts`, `html/webappapis/animation-frames`), and 660 corpus files carry
   the timestamp of the most recent re-materialization. Between this gate's classification pass and its run loop
   sits the whole runner link — minutes — so a file collected as a test can be gone by the time it is run.
   That is §Testing's frozen-snapshot rule one level out: the gate must not report a number about a corpus that
   no longer exists, and it CANNOT freeze this one, because the enforcement above exists precisely to make the
   checkout agree with WPT_PATHS. What it can do is know when the corpus moved under it. So the corpus's
   IDENTITY is captured here — its HEAD and the exact cone that was applied — and re-read wherever a file turns
   out to be unreadable, so the report distinguishes "another run re-sparsified this corpus mid-run" from "this
   file is unreadable and the checkout never moved", which are different diagnoses and must not read alike. */
/* ASKED OF GIT, NOT READ OFF A PATH THIS FILE GUESSES. `.git` is a directory in a clone and a FILE in a
   worktree, so `.git/info/sparse-checkout` is a path that exists for one provisioning and not the other — and a
   read that quietly failed would make the cone compare equal to itself forever, which is a diagnostic that
   always answers "nothing moved". A failure is kept AS the identity, so a run that could not ask and a run that
   asked and got a different answer are both changes rather than both silence. */
function corpusIdentity() {
  const ask = (...a) => { const r = spawnSync("git", a, { cwd: WPT, encoding: "utf8" });
                          return r.status === 0 ? r.stdout : `<git ${a.join(" ")} failed: ${r.stderr}>`; };
  return { head: ask("rev-parse", "HEAD").trim(), cone: ask("sparse-checkout", "list") };
}

/* THE CHECKOUT IS ENFORCED, NOT ASSUMED. WPT_PATHS is this gate's statement of what it measures, and until now
   nothing made the working tree agree with it: the presence of testharness.js was the whole test, so a corpus
   missing an entire directory answered "provisioned" and every file in that directory silently stopped being
   run. That is the same defect the list's own comment names one line up — a directory that is not checked out
   is a directory whose gate cannot fail — except that here the gate LOOKED green rather than looking absent,
   which is worse. It happened: `webmessaging` was added to this list, measured at 0/52, and then reverted out
   of the working tree by an unrelated restore; the next run reported the same total as before and nothing said
   the area had gone. `sparse-checkout set` is idempotent and takes about a second when the list already
   matches, so it runs every time rather than being guarded by a check that can itself be wrong. */
/* AND WHAT IT CHANGED IS SAID OUT LOUD, because this is the write that makes the corpus shared-mutable and
   nothing named it. `set` is idempotent when the list already matches — and when it does NOT match, it is a
   worktree update that ADDS and REMOVES files, applied to a checkout other runs of this gate are reading. Two
   trees at two revisions of this file therefore fight over one corpus: measured, this repository has worktrees
   whose WPT_PATHS differ by ten entries, and 660 corpus files carry the timestamp of the most recent
   re-materialization. Printing the delta at the moment it is applied is what turns "a file vanished mid-run"
   from a mystery into a line the reader already has: a run whose first output is `-xhr -webidl` is a run about
   to invalidate somebody else's numbers, and a run that prints nothing here changed nothing. */
{
  const before = corpusIdentity();
  const set = spawnSync("git", ["sparse-checkout", "set", ...WPT_PATHS], { cwd: WPT, encoding: "utf8" });
  if (set.status !== 0) {
    console.error("[wpt] could not apply the sparse checkout this gate measures:\n" + (set.stderr || set.stdout));
    process.exit(1);
  }
  const after = corpusIdentity();
  if (before.cone !== after.cone) {
    const cut = (s) => new Set(s.split("\n").map((l) => l.trim()).filter(Boolean));
    const b = cut(before.cone), a = cut(after.cone);
    console.log("[wpt] THIS RUN RE-SPARSIFIED THE SHARED CORPUS — any other run reading it now has a corpus " +
                "that no longer matches what it collected:" +
                [...[...a].filter((p) => !b.has(p)).map((p) => " +" + p),
                 ...[...b].filter((p) => !a.has(p)).map((p) => " -" + p)].join(""));
  }
  const missing = WPT_PATHS.filter((p) => !existsSync(join(WPT, p)));
  if (missing.length) {
    console.error(`[wpt] the corpus has no ${missing.join(", ")} — the pinned revision ${WPT_REV} does not ` +
                  "contain it, so this gate would report a total that silently excludes it");
    process.exit(1);
  }
  /* AN OWN-LEVEL ENTRY IS ON DISK ONLY BECAUSE A LISTED PATH BELOW IT PUT IT THERE, so that puller is part of the
     entry's contract: remove the puller and the entry stops collecting anything, which is an excluded test
     wearing a green row. Asked here beside the same question for WPT_PATHS — is this the corpus the gate says it
     measures — and the overlap check with it, because a directory in both lists would be run twice and counted
     twice. */
  for (const d of WPT_OWN_LEVEL) {
    const over = WPT_PATHS.find((p) => d === p || d.startsWith(p + "/"));
    if (over) {
      console.error(`[wpt] ${d} is in WPT_OWN_LEVEL and also under the WPT_PATHS entry ${over}, so its files ` +
                    "would be walked twice and the total would count them twice");
      process.exit(1);
    }
    if (!WPT_PATHS.some((p) => p.startsWith(d + "/"))) {
      console.error(`[wpt] ${d} is in WPT_OWN_LEVEL with no WPT_PATHS entry below it — nothing checks its own ` +
                    "level out, so the entry names files that are not there");
      process.exit(1);
    }
    if (!existsSync(join(WPT, d))) {
      console.error(`[wpt] the corpus has no ${d}, whose own level this gate runs`);
      process.exit(1);
    }
  }
}

/* THE IDENTITY THIS RUN'S NUMBERS BELONG TO, taken AFTER the cone above is applied — that is the corpus the
   collection below is about, and every later "did it move?" question is asked against it. */
const CORPUS_AT_START = corpusIdentity();

/* AND THE OTHER HALF OF THE SAME QUESTION, WHICH THIS GATE HAD NEVER ASKED. Everything above proves which
   ORACLE ran; nothing proved which ENGINE ran. That asymmetry is not academic: a whole-corpus run of this file
   reported 1393 files aborting on nine leaked atoms whose three roots superproject 26a34533 had already fixed,
   and the only surviving evidence of WHICH tree it measured was the wording of the DCHECK it printed —
   submodule 88701bd's "still interned", superseded by 2268e0f twenty-three minutes later. The engine is a
   superproject plus a submodule and this checkout is edited continuously, so the revision is asked for BOTH
   halves and for the dirtiness of exactly the cone linked below, and it is printed BEFORE the build so the
   reader has it even for a run that never reaches a summary. See engine/gate_revision.mjs. */
const REV_AT_START = gateRevision(["engine/host", "engine/qjs", "engine/wpt.mjs", "engine/gate_revision.mjs"]);
for (const l of revisionLines(REV_AT_START)) console.log(l);

/* WHICH FILES TO RUN — AND THE ANSWER IS THE CORPUS'S OWN, PORTED, not a set of endings this file grew one
   defect at a time. Four times now the rule here has been a habit rather than the definition, and each time the
   cost was invisible from inside: `.any.js` only, so html/browsers's twenty `.window.js` files made checking
   those directories out change no total at all; then no documents, so 523 of 778 checked-out files — most of
   the corpus — were never run and never counted while the total LOOKED complete; then `.html` alone, so 24 of
   webmessaging's 26 `.htm` postMessage tests and url/a-element-xhtml.xhtml were collected by nothing; then the
   harness path as a bare SUBSTRING, so eight human-driven `-manual` tests were run as if automated.
   `tools/manifest/sourcefile.py` is what decides every file's type for WPT's own runs, it is checked out
   (WPT_PATHS names `tools`), and every rule below is one of its properties, named after it. It has no holes
   left to find, because it IS the definition.
   TWO QUESTIONS, BOTH NEEDED, AND THE PORT KEEPS THEM APART. The NAME decides document-vs-script and
   test-vs-manual — which is what a name genuinely determines — and the CONTENT decides test-vs-support for a
   document, which no name can say. Reading the content of everything instead would take the corpus's own `.py`
   handlers and `.md` notes as tests, since they name testharness.js too. */

/* sourcefile.py's `type_flag` / `meta_flags`. `os.path.splitext` cuts at the LAST dot, and the flags are then
   the dotted parts of the base name's last HYPHEN-separated chunk (`send-authentication-prompt-manual.htm` ⇒
   type flag `manual`) or, with no hyphen, the dotted parts after the first (`historical.any.js` ⇒ meta flag
   `any`). No suffix test can tell those apart: `.any.js` and `-manual.htm` are the same shape of name and mean
   opposite things. */
function fileFlags(filename) {
  const dot = filename.lastIndexOf(".");
  const name = dot <= 0 ? filename : filename.slice(0, dot);
  const hyphen = name.lastIndexOf("-");
  const parts = (hyphen >= 0 ? name.slice(hyphen + 1) : name).split(".");
  return { ext: dot <= 0 ? "" : filename.slice(dot),
           typeFlag: hyphen >= 0 ? parts[0] : null,
           meta: parts.slice(1) };
}
/* sourcefile.py's `name_is_non_test` + `in_non_test_dir`. A SUPPORT PATH IS ANY PART OF THE PATH, which is
   what this file had hand-copied as `p !== "common" && !p.endsWith("/resources")` — a list applied to the
   WPT_PATHS ENTRY rather than to the file, so `FileAPI/support` was listed as an area and its support documents
   were counted as this gate's tests. One rule, applied to the file, and the entry list needs no exclusions. */
function nameIsNonTest(rel) {
  const parts = rel.split("/");
  const filename = parts[parts.length - 1];
  if (parts.length === 1) return true;            /* a file at the corpus root: sourcefile.py's `dir_path == ""` */
  if (parts[0] === "common") return true;         /* `root_dir_non_test` */
  if (parts.some((p) => p === "resources" || p === "support" || p === "tools")) return true;  /* `dir_non_test` */
  return filename.startsWith("MANIFEST") || filename === "META.yml" || filename === "WEB_FEATURES.yml" ||
         filename.startsWith(".") || filename.endsWith(".headers") || filename.endsWith(".ini");
}
const DOC_EXT = /\.(html|htm|xhtml|xht|xml|svg)$/;   /* sourcefile.py's `markup_type` */
function isDocument(p) { return DOC_EXT.test(p); }
/* sourcefile.py's `content_is_testharness`: a markup file is a testharness TEST when it holds a
   `<script src=/resources/testharness.js>` ELEMENT — an element, not the string anywhere in the bytes, which
   also counts a support page that merely NAMES the harness in a comment or a template.
   THE ELEMENT IS THE XHTML-NAMESPACED `script`, WHICH IS NOT ALWAYS SPELLED `script`. sourcefile.py matches
   `{http://www.w3.org/1999/xhtml}script`, and an SVG test reaches that namespace through a PREFIX: the
   thirteen `dom/nodes/*-svg.svg` tests declare `xmlns:h` and write `<h:script src="/resources/testharness.js"/>`.
   A pattern anchored on the literal tag name dropped all thirteen — a narrowing that reads as tidier and
   silently excludes real tests, which is the exact defect this collector exists to prevent. */
/* EVERY READ OF THE CORPUS GOES THROUGH HERE, AND AN UNREADABLE FILE IS A RESULT OF ITS OWN KIND — neither a
   test that failed, nor a DCHECK naming a missing capability, nor a kill. There were TWO reads of each collected
   file and they handled the same failure in opposite, equally wrong, ways. At COLLECT time `isTestDocument`
   wrapped its read in `catch { return false; }`, so a document the process could not open was classified "not a
   test" and vanished from the total — the silent exclusion this file's own header calls a failure. At RUN time
   `scriptMetadata` read the same file with no containment at all, so the same condition threw out of the driver
   and an ENTIRE AREA produced NO NUMBER: measured, as `node engine/wpt.mjs html/browsers` dying on
   `ENOENT … back-forward-cache/focus.html`, a file that both collected and classified seconds earlier.
   The gap between those two reads is the whole runner build — minutes of clang — and the corpus is SHARED and
   MUTATED IN PLACE by every run (the `sparse-checkout set` above), so the window is real and the file that
   disappears is real. Neither answer was allowed: dropping it is an excluded test, and dying is that area's
   number thrown away over one file. So the read RECORDS, and what it records is reported, counted and named.
   It records by PATH, so the same file discovered twice is one fact, and it keeps the errno — ENOENT (the
   checkout moved) and EACCES (it did not) are different diagnoses and must not read alike. */
const g_unreadable = new Map();
function readCorpus(p) {
  try { return readFileSync(p, "utf8"); }
  catch (e) {
    /* THE ERRNO IS THE DIAGNOSIS, and where there is none that is itself the fact rather than a hole to fill:
       `readFileSync` throws a SystemError carrying `code`, so anything else came from somewhere this file does
       not expect and must arrive in the report saying so, not wearing a plausible errno. */
    const why = typeof e.code === "string" ? e.code : `non-errno: ${e.message}`;
    g_unreadable.set(relative(WPT, p).split(sep).join("/"), why);
    return null;
  }
}
/* `null` — NOT `false` — WHEN THE FILE CANNOT BE READ, because "this is not a testharness test" is a claim about
   its CONTENT and there is no content to make it from. The caller propagates the third answer rather than
   collapsing it into the negative one. */
/* A `<script src>` SPELLED INSIDE A SCRIPT'S OWN TEXT IS NOT AN ELEMENT, and until this stripped it the
   pattern above was structural in shape and textual in fact. HTML §13.2.5.4 script data state: once the
   tokenizer is inside a `script` element the content is TEXT, so markup written there becomes an element only
   if something later parses it. `html/semantics/scripting-1/the-script-element/execution-timing/102.html` is
   the worked example and it is not an edge case — it `document.write`s a whole harness document, so the bytes
   contain a literal `<script src='/resources/testharness.js'>` that no parser ever sees as an element at parse
   time. sourcefile.py parses and says "not a test"; this gate matched the substring and said TEST; and because
   the collector-vs-sourcefile.py disagreement is a HARD FAIL, that one file made `node engine/wpt.mjs` exit 1
   for EVERY path, so the browser half had no WPT gate at all while each run still printed a clean revision
   banner above the refusal.
   The strip is the same correction twice, and this file's own header already names it: a comment that NAMES
   the harness, and now a script that WRITES it. Both are content the parser does not read as markup, so both
   come out before the question is asked — the opening tag is kept, because the opening tag is the thing being
   matched. This mirrors `codeOnly()`'s rule for JS: strip what is not code before reading it as code. */
function markupOnly(src) {
  return src.replace(/<!--[\s\S]*?-->/g, "")
            .replace(/(<(?:[A-Za-z][\w.-]*:)?script\b[^>]*>)[\s\S]*?<\/(?:[A-Za-z][\w.-]*:)?script\s*>/gi, "$1");
}
function isTestDocument(p) {
  const src = readCorpus(p);
  return src === null ? null
       : /<(?:[A-Za-z][\w.-]*:)?script[^>]*\ssrc\s*=\s*(["']?)\/resources\/testharness\.js\1[\s/>]/i
           .test(markupOnly(src));
}
/* WHAT KIND OF TEST A FILE IS, or null when it is not one. The order is sourcefile.py's `manifest_items`
   cascade, and the order is load-bearing: every NAME-based type is asked BEFORE the content, so a file that
   loads testharness.js is still a manual test / a visual test / a crashtest if its name says so. A human clicks
   a manual test and this gate cannot; a crashtest asserts only that the renderer survived, and its subtests are
   not this corpus's oracle. It was collecting twenty-seven `-manual` files and `dom/svg-insert-crash.html` as
   if they were ordinary automated tests.
   THE PORT IS CHECKED AGAINST THE ORIGINAL, not asserted to match it: running sourcefile.py over the same
   checkout and diffing the two classifications is one command, and it is what found the crashtest and the
   thirteen prefixed-`script` SVG tests. Zero files where they disagree is the standard. */
function testKind(rel) {
  if (nameIsNonTest(rel)) return null;
  const parts = rel.split("/");
  const f = fileFlags(parts[parts.length - 1]);
  if (f.typeFlag === "manual" || f.typeFlag === "visual") return null;
  /* `name_is_crashtest` / `name_is_print_reftest`, both of which sourcefile.py gates on markup — a `.js` has no
     markup type, so neither can claim one. Each is a type FLAG or a directory. */
  if (isDocument(rel) && (f.typeFlag === "crash" || f.typeFlag === "print" ||
                          parts.some((p) => p === "crashtests" || p === "print"))) return null;
  if (f.ext === ".js")
    /* `name_is_multi_global` / `name_is_window` / `name_is_worker`. A `.worker.js` needs a
       DedicatedWorkerGlobalScope, which this engine does not have — so it fails loud on `importScripts`, which
       NAMES the missing capability. Leaving it out named nothing and counted nothing. */
    return ["any", "window", "worker"].some((g) => f.meta.includes(g)) ? "script" : null;
  if (!isDocument(rel)) return null;
  /* THE THIRD ANSWER IS CARRIED, NOT COLLAPSED. A `.js` is classified by its NAME and needs no read, so an
     unreadable one reaches the runner and dies there; a document is classified by its CONTENT, so an unreadable
     one used to be quietly demoted here. One kind for both: the file is COLLECTED (it can never silently leave
     the total) and it is reported as unreadable rather than run. */
  const t = isTestDocument(join(WPT, rel));
  return t === null ? "unreadable" : t ? "document" : null;
}
function collect(dir, out) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    /* A DOTTED NAME IS NOT A TEST — sourcefile.py's `name_is_non_test`. It is also what keeps a walk of the
       corpus ROOT out of `.git`, which is most of the 318 MB on disk. */
    if (e.name.startsWith(".")) continue;
    const p = join(dir, e.name);
    if (e.isDirectory()) collect(p, out);
    else if (testKind(relative(WPT, p).split(sep).join("/"))) out.push(p);
  }
  return out;
}
/* ONE LEVEL, NO RECURSION — a WPT_OWN_LEVEL entry's subtree is not in the checkout and is not this gate's, so
   descending would either find nothing or find a listed path's files a second time. `testKind` still answers what
   is a test, so this shares that one rule with the walk rather than restating it. */
function collectOwnLevel(dir, out) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    if (e.isDirectory()) continue;
    const p = join(dir, e.name);
    if (testKind(relative(WPT, p).split(sep).join("/"))) out.push(p);
  }
  return out;
}
/* COLLECTED ONCE. The emptiness check and the run set are the same fact, and computing them twice is how they
   come to disagree. An entry that collects nothing is a decision about a directory that is not there — the
   stale-claim failure, in a list whose whole job is to say what is measured. */
const g_ownLevel = new Map(WPT_OWN_LEVEL.map((d) => [d, collectOwnLevel(join(WPT, d), [])]));
for (const [d, found] of g_ownLevel) {
  if (!found.length) {
    console.error(`[wpt] ${d} is in WPT_OWN_LEVEL and its own level holds no test file — the entry reads as a ` +
                  "decision and measures nothing");
    process.exit(1);
  }
}

const arg = process.argv[2] || "";
/* A NO-ARGUMENT RUN IS EVERY PATH THAT IS CHECKED OUT, not one hard-coded directory. It said "fetch", so
   widening WPT_PATHS to `url` checked the corpus out and then never ran it — the gate reported the same number
   as before and looked like the new component had changed nothing. The default is derived from WPT_PATHS for
   the same reason the META scripts are read from the file: the two must not be able to disagree. */
const root = arg ? join(WPT, arg) : WPT;
/* A SINGLE-FILE ARGUMENT IS WHATEVER NAMES A FILE, not whatever matches an extension this file lists. The
   extensions decide what a WALK collects; naming one path is an explicit instruction and needs no vote. */
const argIsFile = Boolean(arg) && existsSync(root) && !statSync(root).isDirectory();
/* AN ARGUMENT NAMING AN OWN-LEVEL AREA IS THAT AREA, which is its own level and not its subtree. The argument
   selects WHAT to measure; the two lists above decide what each area IS, and re-deciding it here would make
   `wpt.mjs html/semantics` a different set of files from the same area in a full run. */
const argOwnLevel = g_ownLevel.get(arg.replace(/\/+$/, ""));
const files = argIsFile ? [root]
            : argOwnLevel ? [...argOwnLevel].sort()
            : arg ? collect(root, []).sort()
            /* A PATH WITH A LISTED ANCESTOR IS NOT WALKED TWICE. `dom/nodes` is listed so that its 331 files
               get a row of their own; `dom` is listed because ten test files live at that directory's own level
               and belong to no subdirectory. Walking both would run every dom file twice and report a total in
               which the DOM standard counted double.
               THAT IS THE WHOLE FILTER NOW. The support-path exclusions that used to sit here — `common`,
               `tools`, `resources` and any entry ending in one — were the same rule spelled a second time
               against the ENTRY, and being a second spelling it disagreed with the first: `FileAPI/support`
               matched none of them, so its support documents were counted as this gate's tests.
               `nameIsNonTest` answers for the FILE, so a listed support path now simply collects nothing and
               needs no name here. */
            : [...WPT_PATHS.filter((p) => !WPT_PATHS.some((q) => p.startsWith(q + "/")))
                           .flatMap((p) => collect(join(WPT, p), [])),
               ...[...g_ownLevel.values()].flat()].sort();
if (!files.length) { console.error(`[wpt] no test files under ${root}`); process.exit(1); }

/* A `// META:` BLOCK IS PARSED ONCE, HERE — sourcefile.py's `script_metadata`. It is the leading run of comment
   lines, and past it the file is code. Two consumers read it (the scripts a test must be given, and the
   variants it declares) and a second parser for the second consumer is how the two come to disagree about
   where the block ends. */
/* IT ANSWERS `null` WHEN THE FILE CANNOT BE READ. This is the line the whole area died on — an unguarded
   `readFileSync` of a collected file, three frames below a `for` loop with no catch around it. */
function scriptMetadata(file) {
  const src = readCorpus(file);
  if (src === null) return null;
  const out = [];
  for (const line of src.split("\n")) {
    if (!line.startsWith("// META:")) {
      if (line.startsWith("//") || line.trim() === "") continue;
      break;
    }
    const m = line.match(/^\/\/ META:\s*([^=]+)=(.*)$/);
    if (m) out.push([m[1].trim(), m[2].trim()]);
  }
  return out;
}
/* A VARIANT IS A SEPARATE TEST RUN, and sourcefile.py says so in one line: `test_url + variant` for each
   declared variant, and `[""]` — exactly one bare run — when a file declares none. So a file declaring four
   variants is FOUR tests at four addresses, and the bare address is not among them; this gate ran the bare one
   and none of the four. That is an excluded test wearing a file that IS collected, which is the hardest kind to
   notice: the file appears in the count, passes, and three quarters of what it was written to check never
   happened.
   THE CORPUS'S VALIDATION COMES WITH THE RULE. sourcefile.py raises on a non-empty variant that is neither a
   query nor a fragment, so this does too — a malformed declaration is a corpus fact this gate must not paper
   over by silently running the file bare. */
function testVariants(file, kind) {
  const rel = relative(WPT, file).split(sep).join("/");
  /* AN UNREADABLE FILE IS EXACTLY ONE RUN, and that run's whole result is that it could not be read. Its
     variant declarations live in the bytes nobody can see, so claiming any number other than one would be
     inventing a count — the same fabrication as defaulting a producer's absent field. */
  if (kind === "unreadable") return [""];
  const src = kind === "document" ? readCorpus(file) : null;
  const md = kind === "document" ? null : scriptMetadata(file);
  if (src === null && md === null) return [""];   /* readCorpus/scriptMetadata already recorded WHY */
  const rv = kind === "document"
    ? [...src.matchAll(/<meta\s[^>]*name=(["']?)variant\1[^>]*>/gi)]
        .map((m) => m[0].match(/\scontent=(["']?)([^"'>]*)\1/i))
        .filter(Boolean).map((m) => m[2])
    : md.filter(([k]) => k === "variant").map(([, v]) => v);
  for (const v of rv) {
    if (v === "") continue;
    if (v[0] !== "?" && v[0] !== "#")
      throw new Error(`[wpt] ${rel} declares the variant ${JSON.stringify(v)}, which is neither a query nor a ` +
                      "fragment — sourcefile.py rejects it and so does this gate");
    if (v.length === 1 || (v[0] === "?" && v[1] === "#"))
      throw new Error(`[wpt] ${rel} declares the empty variant ${JSON.stringify(v)}`);
  }
  return rv.length ? rv : [""];
}
/* THE UNIT OF A RUN IS (FILE, VARIANT), which is what WPT's manifest calls a test. Every count below — the
   area rows, the total, the abort tally — is over these, because a file that declares four variants is four
   things that can pass or fail and reporting it as one is reporting three of them as nothing. */
const runs = files.flatMap((f) => {
  const kind = testKind(relative(WPT, f).split(sep).join("/")) ||
               (isDocument(f) ? "document" : "script");   /* an explicitly named single file is a run by fiat */
  return testVariants(f, kind).map((variant) => ({ file: f, kind, variant }));
});

/* AND THE PORT IS CHECKED AGAINST THE ORIGINAL, EVERY RUN. Everything above is a port of sourcefile.py, and a
   port nothing compares against its source is a copy that drifts — this one has drifted four times, and each
   time the drift EXCLUDED tests while the total looked complete, which is the exact failure this gate exists to
   catch. So sourcefile.py itself is run over the same checkout and the two classifications are diffed: which
   files are testharness tests, and how many runs each is. A disagreement is FATAL and names every file, because
   a collector that is not the corpus's own answer is a measurement of something else.
   IT COVERS EVERY FILE ON DISK, not only the walked areas, so the stray census below is checked by the same
   comparison. It costs about fifteen seconds against a gate measured in tens of minutes, and it runs BEFORE the
   runner build so a collector defect is reported in seconds rather than after a link. */
{
  const cls = spawnSync("python3", [join(ENGINE, "wpt_classify.py"), WPT], { encoding: "utf8", maxBuffer: 1 << 26 });
  if (cls.status !== 0) {
    console.error("[wpt] sourcefile.py could not classify the corpus, so this gate cannot check its own " +
                  "collector against WPT's:\n" + (cls.stderr || cls.stdout));
    process.exit(1);
  }
  const oracle = new Map();
  for (const line of cls.stdout.split("\n")) {
    if (!line) continue;
    const [rel, ...variants] = line.split("\t");
    oracle.set(rel, variants);
  }
  const disagree = [];
  const everyFile = (dir) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      if (e.name.startsWith(".")) continue;
      const p = join(dir, e.name);
      if (e.isDirectory()) { everyFile(p); continue; }
      const rel = relative(WPT, p).split(sep).join("/");
      const mine = testKind(rel);
      const theirs = oracle.get(rel);
      /* A FILE NEITHER SIDE COULD READ IS NOT A CLASSIFIER DISAGREEMENT, and reporting it as one would exit
         here with a message about sourcefile.py — a confident wrong diagnosis of a corpus fact. It is carried
         to the unreadable report, which is the one place that fact belongs. */
      if (mine === "unreadable") continue;
      if (Boolean(mine) !== Boolean(theirs)) {
        disagree.push(`  ${rel}: this gate says ${mine ? "TEST" : "not a test"}, sourcefile.py says ` +
                      `${theirs ? "TEST" : "not a test"}`);
      } else if (mine) {
        const a = testVariants(p, mine).filter(Boolean).join(" ");
        const b = theirs.join(" ");
        if (a !== b) disagree.push(`  ${rel}: variants [${a}] here, [${b}] in sourcefile.py`);
      }
    }
  };
  everyFile(WPT);
  if (disagree.length) {
    console.error(`[wpt] this gate's collector disagrees with WPT's own tools/manifest/sourcefile.py about ` +
                  `${disagree.length} file(s):\n` + disagree.join("\n"));
    process.exit(1);
  }
}

/* THE RUNNER IS BUILT NATIVELY, like run-test262 and for the same reason: the gate is run per change, and an
   eight-minute wasm link per iteration is a gate nobody runs. The flags mirror the shipped build where they
   change the engine — ENABLE_DUMPS alters the interpreter's dispatch macros, and building the oracle without it
   tested a different interpreter once already. */
/* EVERY BROWSER AND SOLVER SOURCE, not a hand-picked list. The list was picked per component, and each new one
   arrived by chasing undefined symbols until the link succeeded — which is a list that only ever describes what
   was needed last time. Streams §5.4's AbortSignal is what made that untenable: it is an EventTarget, which
   reaches the solver's decision hook, which reaches the scheduler, whose COW covers the DOM. The gate links
   what the project SHIPS, and a component that cannot be linked is a component that cannot be tested. */
const SRCS = [
  "qjs/quickjs.c", "qjs/libregexp.c", "qjs/libunicode.c", "qjs/dtoa.c",
  ...walk(join(ENGINE, "host", "browser")).map((f) => relative(ENGINE, f)),
  ...walk(join(ENGINE, "host", "solver")).map((f) => relative(ENGINE, f)),
  "host/wpt_runner.c",
].map((f) => join(ENGINE, f));

console.log("[wpt] building the native runner…");
const bin = join(mkdtempSync(join(tmpdir(), "wpt-")), "wpt.exe");
/* `-w` SILENCES STYLE, NEVER A MISSING PROTOTYPE. C89's implicit-declaration rule assumes `int (...)`, so a
   function whose header was not included returns a 32-bit value — and a 64-bit POINTER comes back TRUNCATED.
   That is not a warning-shaped problem: it segfaulted the whole corpus with no output, and it read as a crash
   in the engine rather than as a missing #include, which is the most expensive shape a diagnostic can take.
   -Werror on that one diagnostic makes it a build failure naming the function. */
/* LEXBOR, BUILT NATIVELY ONCE. Streams §5.4 gives every writable controller a real AbortSignal, which is an
   EventTarget, which reaches the solver's decision hook and through it the scheduler — and the scheduler's COW
   covers the DOM, so the gate needs the same tree the shipped build has. Vendored and built on first use, like
   the corpus itself; the .a is committed to nothing and rebuilt if the source moves. */
const LEXBOR_SRC = join(WORK, "lexbor-src");
const LEXBOR_BUILD = join(WORK, "lexbor-native");
const LEXBOR_LIB = join(LEXBOR_BUILD, "liblexbor_static.a");
if (!existsSync(LEXBOR_LIB)) {
  console.log("[wpt] building lexbor natively (once)…");
  mkdirSync(LEXBOR_BUILD, { recursive: true });
  for (const [cmd, args] of [["cmake", ["-DCMAKE_BUILD_TYPE=Release", "-DLEXBOR_BUILD_SHARED=OFF",
                                        "-DLEXBOR_BUILD_STATIC=ON", "-DLEXBOR_BUILD_TESTS=OFF",
                                        "-DLEXBOR_BUILD_EXAMPLES=OFF", LEXBOR_SRC]],
                             ["make", ["-j" + (cpus().length || 4)]]]) {
    const b = spawnSync(cmd, args, { cwd: LEXBOR_BUILD, encoding: "utf8" });
    if (b.status !== 0) { console.error(`[wpt] lexbor ${cmd} FAILED\n` + (b.stderr || "")); process.exit(1); }
  }
}

const cc = spawnSync("clang", ["-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow", "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer", "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration", "-DNDEBUG", "-D_GNU_SOURCE", '-DCONFIG_VERSION="wpt"',
  "-DAPICLIENT_DEV=1", "-DENABLE_DUMPS",
  "-I" + join(ENGINE, "qjs"), "-I" + join(ENGINE, "host"), "-I" + join(ENGINE, "host", "browser"),
  "-I" + join(LEXBOR_SRC, "source"),
  ...SRCS, LEXBOR_LIB, "-o", bin, "-lm", "-lpthread"], { encoding: "utf8" });
if (cc.status !== 0) { console.error("[wpt] runner build FAILED\n" + (cc.stderr || "")); process.exit(1); }


/* The AREA a file belongs to: the checked-out path it lives under, which is what the two lists name — the LONGEST
   such path, so that a standard listed alongside its components reports at the component.
   BOTH LISTS, because an own-level area is an area: `service-workers/service-worker`'s 247 files would otherwise
   fall back to the first path segment and report inside `service-workers` beside that standard's one file, which
   is the folded number this gate reports per area to avoid. */
const AREA_PATHS = [...WPT_PATHS, ...WPT_OWN_LEVEL];
const areas = new Map();
function byArea(rel) {
  const p = AREA_PATHS.filter((d) => rel === d || rel.startsWith(d + "/"))
                      .reduce((a, b) => (b.length > a.length ? b : a), rel.split("/")[0]);
  let a = areas.get(p);
  if (!a) areas.set(p, (a = { name: p, expected: 0, done: 0, runs: 0, pass: 0, fail: 0, aborted: 0,
                              unread: 0, errored: 0, abPass: 0, abFail: 0, abFiles: 0, lines: [] }));
  return a;
}

/* A REPORT THAT ONLY EXISTS AT EXIT IS A REPORT YOU CANNOT GET ON A SLOW CORPUS. This driver printed everything
   — every failure and every area row — after the last file, so a run that died at ninety percent yielded
   NOTHING AT ALL: an hour of real measurement thrown away by where a print statement sits. It happened, on the
   run that was supposed to produce this file's own numbers, and the corpus has just tripled in size, so it will
   happen again. It is the same sentence CLAUDE.md applies to the engine's cost reporting — emitting them as the
   run goes is the host's own job — and the same distinction the uncollected-file list draws: an area that
   printed is measured, an area that did not is VISIBLY ABSENT rather than silently missing.
   AN AREA IS FINISHED WHEN ITS LAST FILE IS, WHICH IS NOT WHEN THE NEXT AREA STARTS. The file list is sorted by
   path, and a standard listed beside its own components interleaves with them — `dom/attributes-are-nodes.html`
   sorts between `dom/abort/` and `dom/collections/` — so "the area changed" is not the same question as "the
   area is done". Every area's file count is known before the first file runs, so the answer is exact. */
for (const r of runs) byArea(relative(WPT, r.file)).expected++;
const AREA_W = Math.max(...[...areas.keys()].map((n) => n.length));
/* FIVE COLUMNS, AND `aborted` USED TO BE TWO OF THEM ADDED TOGETHER. The rule below is this file's own and it
   was broken by this file: `aborted` counted the ERROR population too — a file that RAN, registered no subtest
   and threw, which is a result about the page rather than a capability the engine lacks — so the column was a
   sum over two different diagnoses. It read 332 in an area with 331 runs and 319 ABORT lines, which is the
   shape of the defect: a per-file count that exceeds the file count is not a per-file count. Worse than the
   13 it was over by, the SUM is what two runs get compared on, and two different mixes total the same number —
   so "the abort column did not move" was read as "the same aborts fired", which it does not mean and cannot.
   FOUR COLUMNS, NOT THREE, AND THE FOURTH IS NOT A KIND OF ABORT. §Testing insists a wrong answer, a missing
   capability, a kill and an artifact of HOW the run happened stay distinguishable — this is that rule applied
   to the corpus itself. `unread` is "the gate collected this file and then could not read it": nothing about
   the engine was measured, so folding it into `aborted` would report a checkout fact as a missing capability
   and send the next reader to build something. It is per AREA because an area losing files to a shared-corpus
   mutation and an area whose component is absent look identical in a total. */
function areaRow(a) {
  console.log(`  ${a.name.padEnd(AREA_W)}  runs ${String(a.runs).padStart(5)}  pass ${String(a.pass).padStart(7)}` +
              `  fail ${String(a.fail).padStart(7)}  aborted ${String(a.aborted).padStart(3)}` +
              `  errored ${String(a.errored).padStart(3)}  unread ${String(a.unread).padStart(3)}`);
  /* WHICH PART OF THE ROW ABOVE IS WORK THAT DID NOT FINISH — see the accumulation for the incident. Printed
     only when there IS such a part, because a row where every counted subtest came from a file that ran to
     completion is a row with nothing extra to say, and a line that reads `0` every time is a line nobody reads
     the one time it does not. */
  if (a.abFail || a.abPass)
    console.log(`  ${" ".repeat(AREA_W)}    └─ of which ${a.abFail} fail / ${a.abPass} pass came from ` +
                `${a.abFiles} file(s) that ABORTED — counted, but not a finished measurement of anything`);
}
/* An area's failure lines are held only until that area finishes, for the same reason: buffered to the end of
   the RUN they are lost with it; buffered to the end of the AREA they arrive with the row they belong to. */
function areaFinish(a) {
  for (const l of a.lines) console.log(l);
  a.lines.length = 0;
  areaRow(a);
}

const HARNESS = join(WPT, "resources", "testharness.js");

/* WPT'S OWN SERVER, started once for the run. Everything the corpus fetches goes through it, so the handlers
   are the real ones and the rewrites are the real ones.
   ITS OUTPUT GOES TO A FILE, AND A PIPE HERE IS A DEADLOCK — not a style preference. The server was spawned
   with `stdio: ["ignore","pipe","pipe"]` and nothing ever read the stderr pipe, so wptserve's own logger filled
   the 64 KiB socketpair and the next handler thread to log blocked in `write(2, …)` FOREVER, inside
   `sock_alloc_send_pskb`, holding the connection it was answering. The runner then sat in `read()` in
   `wpt_http` with zero CPU until the 600 s wall backstop killed it — measured, on the live process: server
   thread blocked writing 1342 bytes to fd 2, runner blocked reading fd 3, both socket queues empty. Five `xhr`
   files hung that way and cost ~50 minutes of every run, and WHICH five was deterministic: the ones served
   after the pipe filled whose requests make the server log at all (a 500, a 400), while a request answered 200
   logs nothing and sailed past.
   DRAINING IT FROM NODE WOULD NOT FIX IT. The run is a sequence of `spawnSync` calls, each of which blocks this
   process's event loop for as long as the test takes — up to the whole backstop — so a `data` listener runs
   only between tests and the buffer fills within one. The fix has to be at the file descriptor: a real file
   cannot exert back-pressure, so the server can never block on it whatever it writes and however busy this
   driver is. READY is then read back from that file rather than off a pipe. The log is KEPT, not discarded —
   it is what named the missing stash — and its path is printed so the next reader can look. */
/* It lives beside THIS RUN'S binary, not in .work: the log is an artifact of one run, and a fixed path in a
   shared work directory is one two concurrent runs would interleave and then read as one server's story. */
const SERVER_LOG = join(dirname(bin), "wptserve.log");
writeFileSync(SERVER_LOG, "");
const serverLogFd = openSync(SERVER_LOG, "a");
const server = spawn("python3", [join(ENGINE, "wptserve.py"), WPT, "0"],
                     { stdio: ["ignore", serverLogFd, serverLogFd] });
const serverAddr = await new Promise((resolve, reject) => {
  const fail = setTimeout(() => reject(new Error("wptserve did not report READY within 60s")), 60_000);
  const poll = setInterval(() => {
    const m = /READY (\d+)/.exec(readFileSync(SERVER_LOG, "utf8"));
    if (m) { clearTimeout(fail); clearInterval(poll); resolve("127.0.0.1:" + m[1]); }
  }, 50);
  server.on("exit", (c) => { clearTimeout(fail); clearInterval(poll); reject(new Error("wptserve exited with " + c)); });
}).catch((e) => { console.error("[wpt] " + e.message); process.exit(1); });
console.log(`[wpt] wptserve on ${serverAddr}; its log: ${SERVER_LOG}`);
process.on("exit", () => server.kill());

/* WPT's server does not serve every path from a file of that name. This is its rewrite table (tools/serve's
   `rewrites`), reproduced for the entries the corpus actually asks for: /resources/WebIDLParser.js is the
   webidl2 library under its historical name. A driver that skipped this would report the file as missing —
   which is what it did — rather than as served from somewhere else. */
const SERVER_REWRITES = {
  "/resources/WebIDLParser.js": "/resources/webidl2/lib/webidl2.js",
};

/* `// META: script=` IS PART OF THE FILE. WPT's server reads those lines and emits a wrapper that loads each
   named script before the test — a file whose META names idlharness.js is not a file that happens to want it,
   it is a file that does not run without it. Reading them here is what makes the corpus run AS WRITTEN; the
   runner keeps its one job of executing a list of programs in order.
   A `/`-rooted path is WPT-root-relative and anything else is relative to the test's own directory, which is
   the server's own resolution rule. */
function metaScripts(file) {
  const md = scriptMetadata(file);
  if (md === null) return null;   /* the file itself is gone; the run loop reports that, it does not die of it */
  return md.filter(([k]) => k === "script").map(([, v]) => {
    /* THE REWRITE APPLIES HERE AND NOWHERE ELSE. A META script is a PROGRAM INPUT — the driver hands the runner
       its file to execute before the test — so this path is resolved on disk and needs wptserve's rewrite table
       to find /resources/WebIDLParser.js, which is the webidl2 library under its historical name. Everything the
       test FETCHES goes through the real server, which applies its own rewrites; this table is not a second copy
       of those, it is the one entry the driver itself must resolve. */
    const ref = SERVER_REWRITES[v] || v;
    return ref.startsWith("/") ? join(WPT, ref.slice(1)) : join(dirname(file), ref);
  });
}
/* A `.sub.js` META SCRIPT IS NOT ITS BYTES ON DISK. wptserve SUBSTITUTES `{{host}}`, `{{ports[http][0]}}`
   and friends when it serves one, and the whole point of common/get-host-info.sub.js is to hand a test the
   REAL alternate hosts and ports of the server it is running against. Read off disk it hands back the
   placeholders instead, so `get_host_info().HTTP_REMOTE_ORIGIN` is the literal string `http://{{hosts[alt][]}}`
   — which is not a URL, which is why every cross-origin `window.open` in the corpus failed with "the URL to
   open is not a URL" and every test built on one timed out. That is a GATE defect reported as an engine one.
   Everything a test FETCHES already goes through the real server; a META script is the one input the driver
   hands over itself, so it is the one that has to be fetched here. Memoized: the same helper is named by many
   files and substitution is the server's work, not this loop's. */
/* AND A SERVER THAT WILL NOT SERVE ONE IS A THIRD ANSWER, NEVER THE BYTES ON DISK. This line was
   `if (!r.ok) return dep;`, excused by "the missing-dep report below names it" — and that claim is FALSE for
   the only case that arises: the report below is `existsSync`, the file IS checked out, so it passed, and the
   driver handed the runner THE TEMPLATE. That is not a degraded result, it is the exact defect the paragraph
   above this function exists to have fixed, re-created silently on the failure path — `get_host_info()` reading
   back `http://{{hosts[alt][]}}`, which is not a URL.
   MEASURED, NOT SUSPECTED: `service-workers/service-worker/resources/test-helpers.sub.js` answers HTTP 500,
   because wptserve's `sub` pipe raised IndexError on `{{ports[wss][0]}}` against a config that declared no
   `wss` — engine/wptserve.py now declares it, and this branch is what kept that invisible while ten collected
   runs named the helper. A FALLBACK IS WHY THE ROOT SURVIVED, so it goes with the root fix rather than after
   it: with nothing to fall back to, the next scheme the config forgets is an ABORT naming the file and its
   status instead of a family of tests quietly measuring a template.
   The failure is MEMOIZED like the success — the same helper is named by many files, and re-fetching a 500 per
   file made one server fault look like many. */
const g_subbed = new Map();
const g_unserved = new Map();
async function substituted(dep) {
  if (!/\.sub\.[a-z]+$/.test(dep)) return dep;
  if (g_subbed.has(dep)) return g_subbed.get(dep);
  const path = "/" + relative(WPT, dep).split(sep).join("/");
  const r = await fetch("http://" + serverAddr + path);
  if (!r.ok) { g_unserved.set(path, r.status); g_subbed.set(dep, null); return null; }
  const out = join(dirname(bin), relative(WPT, dep).split(sep).join("__"));
  writeFileSync(out, await r.text());
  g_subbed.set(dep, out);
  return out;
}

let pass = 0, fail = 0, aborted = 0, unread = 0, errored = 0;
/* THE CENSUS BELOW IS A VERDICT, so it needs a home outside its own block: a test file on disk that neither list
   accounts for is an excluded test, and an excluded test is a failure — not a row a reader may skip. */
let g_undecided = 0;

console.log("\n==================== web-platform-tests ====================");
for (const { file: f, kind, variant } of runs) {
  /* THE RUN'S NAME CARRIES ITS VARIANT, because that is what distinguishes it from its siblings — four lines
     reading `url/url-constructor.any.js` with four different failures name nothing a reader can act on. The
     AREA is asked for the file's own path, since a variant does not live somewhere else. */
  const path = relative(WPT, f);
  const rel = path + variant;
  const area = byArea(path);
  const failures = area.lines;   /* held until this AREA finishes, not until the run does */
  area.runs++;
  try {
    /* THE FILE ITSELF, BEFORE ANYTHING IT NAMES. This is where the driver used to die: `metaScripts` read the
       collected file with no containment, so one file the corpus no longer had took the whole area's number
       with it. Both halves of that are wrong and both are fixed here — it is REPORTED (never dropped, which
       would be the excluded test) and it is reported AS ITS OWN KIND (never an abort, never a fail), and it
       costs exactly one run rather than an area. */
    const meta = kind === "unreadable" ? null : metaScripts(f);
    if (meta === null) {
      /* THE REASON IS ASSERTED, NOT DEFAULTED. `readCorpus` is the only producer of a null here and it always
         records the errno first, so an absent entry means THIS DRIVER is wrong about its own contract — a
         should-never-happen that must crash at its origin, not become the string "unknown" sitting in a report
         as a plausible datum. */
      const key = path.split(sep).join("/");
      if (!g_unreadable.has(key))
        throw new Error(`[wpt] ${key} read as unreadable with no recorded reason — readCorpus records every ` +
                        "failure it returns null for, so this driver's own accounting is broken");
      unread++; area.unread++;
      failures.push(`  UNREAD ${rel}\n         the corpus file could not be read: ${g_unreadable.get(key)} — ` +
                    "nothing about the engine was measured by this run");
      continue;
    }
    const deps = (await Promise.all(meta.map(substituted)));
    /* ASKED BEFORE `existsSync`, AND NOT ONLY BECAUSE A NULL WOULD THROW THERE. The two are different
       diagnoses and must not read alike: a META script the sparse checkout does not have is fixed by widening
       WPT_PATHS, and one the SERVER would not serve is on disk already and is fixed in engine/wptserve.py.
       Reporting the second as the first would send the reader to add a directory that is present. */
    const unserved = deps.map((d, i) => (d === null ? meta[i] : null)).filter(Boolean)
                         .map((d) => "/" + relative(WPT, d).split(sep).join("/"));
    if (unserved.length) {
      aborted++; area.aborted++;
      failures.push(`  ABORT  ${rel}\n         a .sub META script the corpus server would not serve: ` +
                    unserved.map((p) => `${p} (HTTP ${g_unserved.get(p)})`).join(", ") +
                    "\n         it is a TEMPLATE, so its bytes on disk are not the file this test is written " +
                    "against — see the wptserve log named above for the traceback");
      continue;
    }
    const missing = deps.filter((d) => !existsSync(d));
    if (missing.length) {
      /* A META script the sparse checkout does not have is a GATE defect, not a test result: the file would run
         against a corpus it was not written for. Name the paths so WPT_PATHS can be widened. */
      aborted++; area.aborted++;
      failures.push(`  ABORT  ${rel}\n         META script not checked out: ${missing.map((d) => relative(WPT, d)).join(", ")}`);
      continue;
    }
    /* WHETHER THE TEST IS A DOCUMENT IS DECIDED HERE AND NOWHERE ELSE. The runner used to re-derive it from the
       file name — `.html`, and only `.html` — which is a second copy of the rule above and drifted from it the
       moment the first `.htm` was collected: the driver would have handed the runner a document and the runner
       would have executed the markup as JavaScript. One authority, passed as a flag; the runner carries no
       extension list at all. */
    /* AND THE VARIANT GOES WITH IT, for the same reason: the driver decided what this run IS, so the runner is
       told rather than left to re-derive it. It becomes the run's address — what the runner GETs and what
       `location.search` answers — which is the whole of what a variant is. */
    /* THE BUDGET IS CPU, NOT WALL CLOCK, AND THAT IS THE WHOLE DIFFERENCE BETWEEN A RESULT AND AN ARTIFACT.
       This was `timeout: 60_000` — sixty seconds of ELAPSED time — so a healthy test running on a loaded box was
       killed and reported in the same column as a DCHECK naming a missing capability. It is not hypothetical: at
       a load average of 13-18 on four cores, `dom/ranges` read 15743 pass against a 25056 baseline with an
       IDENTICAL fail count, because one file was killed part-way and the subtests it had not yet printed simply
       ceased to exist. The reader cannot tell that from a regression, which is the same shape as the aborted
       files this gate was built to expose.
       RLIMIT_CPU is the honest measure: it counts the CPU SECONDS this process consumed, so a test starved by
       other work is never killed for waiting, however long it waits. The kernel raises SIGXCPU at the soft
       limit, which is a DIFFERENT signal from the wall backstop's SIGTERM — so the two causes stay
       distinguishable in the report rather than collapsing into "signal SIGTERM".
       The wall backstop stays, generously, because RLIMIT_CPU cannot see a test BLOCKED on I/O — a deadlock
       against wptserve burns no CPU at all. Two measures, two signals, and the report says which fired. */
    /* AND THE BACKSTOP'S REPORT CARRIES THE CPU THE KILLED TEST ACTUALLY CONSUMED, because that number is the
       whole verdict and it was missing. A SIGTERM at the backstop is TWO different facts wearing one signal: a
       real test crawling on a saturated box (seconds of CPU per second of wall), and a test blocked on a socket
       that will never answer (zero). This driver reported both as "load average N — re-run this file alone",
       which is a claim about the BOX, and it sent three separate diagnoses in one day hunting a phantom before
       anyone measured the process: 0.03 s of CPU over 600 s of wall, blocked in `read()`, against a wptserve
       whose logging thread was itself blocked writing to a pipe nobody read. `cutime`/`cstime` in
       /proc/self/stat accumulate the CPU of children this process has REAPED, and spawnSync reaps before it
       returns, so the delta across the call IS that child's CPU — no wrapper, no accounting the kill can
       destroy. */
    const childCpu = () => {
      const f = readFileSync("/proc/self/stat", "utf8");
      const v = f.slice(f.lastIndexOf(")") + 2).split(" ");   /* comm may hold spaces; fields resume after it */
      return (Number(v[11]) + Number(v[12])) / CLK_TCK;       /* stat(3) fields 16,17: cutime, cstime */
    };
    const cpu0 = childCpu();
    const r = spawnSync("/bin/sh",
                        ["-c", `ulimit -H -t ${CPU_BUDGET_S + 10} 2>/dev/null; ulimit -S -t ${CPU_BUDGET_S}; exec "$@"`, "sh", bin,
                         ...(kind === "document" ? ["--test-document"] : []),
                         ...(variant ? ["--variant", variant] : []), HARNESS, ...deps, f],
                        { encoding: "utf8", maxBuffer: 1 << 28, timeout: WALL_BACKSTOP_MS,
                          env: { ...process.env, WPT_SERVER: serverAddr } });
    const cpuUsed = childCpu() - cpu0;
    const out = (r.stdout || "") + (r.stderr || "");
    /* An ABORT is a result about this file, not an accident: it is a DCHECK naming a capability the browser half
       does not have, which is exactly what this gate is for. It is counted apart from a FAIL because the two ask
       for different work — a fail is a wrong answer, an abort is a missing one. */
    /* AN ABORT DOES NOT ERASE WHAT THE FILE ALREADY REPORTED. A DCHECK ends the process, but the results printed
       before it are results — and discarding them here counted a file that had failed four subtests and then
       leaked as a file that produced NOTHING, which is the "same failures with the count hidden" shape this gate
       exists to avoid. It hid them from me, too: I diagnosed that file as ending with no results and went looking
       for what it was waiting on. The abort is reported AND the subtests are counted; they are different facts. */
    /* THE TWO @WHY SHAPES, because there are two emitters and losing one loses the name. The HOST's check.h
       prints a machine-readable JSON line whose `reason` carries the message; the ENGINE's quickjs-check.h prints
       `@WHY <msg> (file:line)` as plain text. Matching only the first reported the second as a bare
       "signal SIGABRT" — an abort with the name thrown away, which is the one thing an abort is FOR. It cost a
       round trip per diagnosis and taught nothing on its own. */
    const why = out.match(/@WHY .*"reason":"([^"]*)/) || out.match(/^@WHY (.+)$/m);
    const abortedHere = Boolean(why || r.signal);
    if (abortedHere) {
      aborted++; area.aborted++;
      /* NAME WHICH OF THE THREE THIS IS. A DCHECK abort is a missing capability and is work; a CPU-budget kill
         is a real statement about this test's cost and is also work; a wall-backstop kill is a fact about the
         BOX, and reporting it as either of the others sends the next reader hunting a phantom. The load average
         rides along with the third because it is the number that explains it. */
      const cause = why ? why[1].slice(0, 160)
                  : (r.signal === "SIGXCPU" || r.signal === "SIGKILL") ? `exceeded the ${CPU_BUDGET_S}s CPU budget (not wall time — this test really is that expensive)`
                  : r.signal === "SIGTERM"
                      ? (cpuUsed < 1
                          ? `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuUsed.toFixed(2)}s of CPU — ` +
                            `that is a BLOCK, not load: the process was asleep waiting for something that never came. ` +
                            `Attach to it (gdb -p, /proc/PID/syscall) and look at the OTHER end`
                          : `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuUsed.toFixed(1)}s of CPU, ` +
                            `load average ${loadavg()[0].toFixed(1)} on ${cpus().length} cores — starved, not blocked; RE-RUN THIS FILE ALONE`)
                  : "signal " + r.signal;
      /* AND WHAT THE WALK ACTUALLY FOUND, because "the runtime went down holding live objects" without saying
         WHICH is a number about nothing — the one abort cause in this gate that names a defect and then throws
         away its own evidence. quickjs's JS_FreeRuntime already prints its censuses and this driver already has
         them in `out`: `[gcleak]` counts the survivors per KIND with the class NAME (the atom table is still
         alive there, which is what makes naming possible at all), `[gcroot]` re-lists after gc_decref so it
         shows only what is rooted from OUTSIDE the heap — the only line that names a CULPRIT, since every
         object in a leaked cycle is innocent — `[stepleak]` names each continuation-holding builtin that was
         dropped rather than finished, and `[atomleak]` names each atom still interned, which is the one leak
         class the object walk structurally cannot see: an atom is not a GC object, so a component's private
         Symbol survives every other census on this list.
         THEY RIDE THE ABORT rather than being aggregated, because they are a fact about THIS file: two files
         leaking one Array each and one file leaking a realm are three different pieces of work, and a total
         would say "1552 objects" and send the reader to the wrong one. Discarding them cost a full measure-and-
         report round trip per diagnosis: the count sat at 128 → 107 → 104 across three revisions while nobody
         could see that it is a per-FILE boolean over SEVERAL universal leaks, so fixing one of them could not
         move it at all. This is what tells the difference. */
      const census = out.split("\n").filter((l) => /^\[(gcleak|gcroot|stepleak|atomleak)\]/.test(l));
      failures.push(`  ABORT  ${rel}\n         ${cause}` + census.map((l) => `\n         ${l}`).join(""));
    }
    /* THE `@WPTHANDLER` BRANCH IS GONE, not disabled. It excused a test that asks for a wptserve `.py` handler
       back when this driver served the corpus off disk — and engine/wptserve.py now runs WPT'S OWN server, which
       imports those handlers and calls their `main`, so nothing has emitted that marker since. A branch whose
       emitter no longer exists is not a safety net, it is a reader's mistake waiting to happen: it says this gate
       cannot run handler tests, and this gate can. */
    /* A `<script src>` THE CORPUS DOES NOT HAVE is the missing-META-script defect one layer in, and it was the
       only one of the two that went uncounted: the document RAN, without a file it asked for, and whatever it
       then reported was counted as an ordinary result unless the file happened to report nothing at all. It is
       the same fact — this file ran against a corpus it was not written for — so it is the same ABORT, and it
       names the path so WPT_PATHS can be widened. Its partial subtests are dropped for the reason the count
       exists at all: they are numbers from a test that is not the test. */
    /* `!abortedHere` — A FILE IS COUNTED ONCE. The block above does not `continue` (deliberately: an abort does
       not erase what the file already reported), so this fired IN ADDITION to it and a file that both aborted
       and asked for a script the corpus lacks was counted TWICE, in a column whose whole meaning is one per
       file. Measured: moveBefore/fullscreen-preserve.html, which is why the area read 332 over 331 runs. Its
       abort already names the file and its cause; the missing path rides that row rather than opening a second. */
    const noscript = out.match(/^@WPTERR .*<script src> did not load: (.*)$/m);
    if (noscript && !abortedHere) {
      aborted++; area.aborted++;
      failures.push(`  ABORT  ${rel}\n         a <script src> the corpus does not serve: ${noscript[1]}`);
      continue;
    }
    /* WHAT THE RUNNER STREAMED, AND IT IS TWO POPULATIONS BECAUSE THE DIFFERENCE BETWEEN THEM IS THE DIAGNOSIS.
       `@WPTSTART` is a subtest testharness REGISTERED; `@WPT` is one whose RESULT is known. Registered-minus-
       resolved is exactly "the subtests this run left hanging", which is the sentence the no-@WPTDONE report used
       to have no way to write.
       BOTH ARE KEYED ON THE HARNESS'S OWN `tests` INDEX, which is what makes a subtest seen twice one subtest —
       the harness notifies test state at PUSH and again at START, and the completion callback re-emits any
       subtest forced complete without a result. Keying on the NAME would also fold two DIFFERENT subtests that
       share one, which the harness itself reports as an error and this gate must not hide. */
    const started = new Map(), settled = new Map();
    const keyOf = (t) => (typeof t.i === "number" ? "#" + t.i : "@" + t.name);
    for (const line of out.split("\n")) {
      const s = line.match(/^@WPTSTART (\{.*\})$/);
      if (s) { let t; try { t = JSON.parse(s[1]); } catch { continue; } started.set(keyOf(t), t.name); continue; }
      const m = line.match(/^@WPT (\{.*\})$/);
      if (!m) continue;
      let t; try { t = JSON.parse(m[1]); } catch { continue; }
      settled.set(keyOf(t), t);
    }
    let filePass = 0, fileFail = 0;
    for (const t of settled.values()) {
      if (t.status === 0) { filePass++; }
      else { fileFail++; failures.push(`  FAIL   ${rel} :: ${t.name}\n         ${(t.message || "").slice(0, 200)}`); }
    }
    const err = out.match(/^@WPTERR (.*)$/m);
    if (!abortedHere && err && !filePass && !fileFail) {
      /* NOT AN ABORT. The file ran and threw before registering a subtest — a result about the PAGE, where an
         abort is a capability the engine does not have. Folding it in is the very thing the column rule above
         forbids, and it is what made that column exceed the run count. */
      errored++; area.errored++;
      failures.push(`  ERROR  ${rel}\n         ${err[1].slice(0, 200)}`);
      continue;
    }
    /* A FILE THAT NEVER COMPLETED IS NOT A FILE WITH NOTHING IN IT — and the two used to read alike, which is a
       number about nothing. testharness reports through its completion callback, so a run that ends without
       @WPTDONE contributed ZERO to every column and looked exactly like a file that asserts nothing.
       IT IS NOW SPLIT BY CAUSE, because "no @WPTDONE" is at least four different pieces of work and they are not
       the same finding: a subtest awaiting something that never settles is an ENGINE gap and the message NAMES
       the subtests, which is what names the awaited thing; every subtest settled and still no completion is the
       harness's completion path not reaching this runner's hook, which is THIS gate's bug; nothing registered at
       all with a throw on the way is the FILE failing before it had a test, and the throw is the report. The
       fourth — a queued completion callback that never got a turn — cannot arrive here silently: the scheduler
       DCHECKs that no flow holds a job when it declares the frontier exhausted, so it aborts by name instead. */
    if (!abortedHere && !/^@WPTDONE /m.test(out)) {
      aborted++; area.aborted++;
      const hanging = [...started.keys()].filter((k) => !settled.has(k)).map((k) => started.get(k));
      const cause =
        hanging.length
          ? `${settled.size} of ${started.size} subtest(s) reached a result; ${hanging.length} never did, so ` +
            `something they await never settled: ${hanging.slice(0, 8).map((n) => JSON.stringify(n)).join(", ")}` +
            (hanging.length > 8 ? `, +${hanging.length - 8} more` : "")
        : started.size
          ? `all ${started.size} of its subtest(s) reached a result and the harness STILL never completed — ` +
            "testharness's completion path did not reach this runner's report hook, which is this gate's own bug"
        : err
          ? `it registered no subtest at all and threw on the way: ${err[1].slice(0, 200)}`
          : "it registered no subtest and never completed — testharness.js itself never ran, or the report hook " +
            "is not installed in this run's programs";
      failures.push(`  ABORT  ${rel}\n         the harness never completed — no @WPTDONE. ${cause}`);
      continue;
    }
    /* THE FILE-LEVEL VERDICT, WHICH THIS DRIVER USED TO PARSE AND THROW AWAY. @WPTDONE carries testharness's own
       harness status and it is not always OK: ERROR is an uncaught exception, a duplicate subtest name, a failing
       `cleanup` or `done()` called with no test defined; TIMEOUT is the file's own 10 s harness timeout firing.
       Matching only the PRESENCE of the line reported every one of those as a clean file — a file whose report
       the harness itself calls untrustworthy, counted green. It is a real result about the file (the file ran),
       so it is a FAIL rather than an abort, and it is counted.
       ASKED OF THE LINE, NOT OF `abortedHere`: a file can abort AFTER the harness completed — a teardown leak is
       the usual one — and that file's harness status is as real as any other's. */
    {
      const d = out.match(/^@WPTDONE (\{.*\})$/m);
      let h = null;
      if (d) { try { h = JSON.parse(d[1]); } catch { h = null; } }
      if (d && !h) {
        aborted++; area.aborted++;
        failures.push(`  ABORT  ${rel}\n         @WPTDONE carried no readable JSON: ${d[1].slice(0, 200)}`);
        continue;
      }
      const NAME = { 0: "OK", 1: "ERROR", 2: "TIMEOUT", 3: "PRECONDITION_FAILED" };
      if (h && h.status !== 0) {
        fileFail++;
        failures.push(`  FAIL   ${rel} :: <harness>\n         testharness status ` +
                      `${NAME[h.status] || h.status}: ${(h.message || "").slice(0, 200)}`);
      }
      /* `count` IS HOW MANY SUBTESTS THE HARNESS HOLDS, cross-checked because losing a line is the one failure
         mode a streamed report adds and it would make every number above wrong. Not asked of a file that
         ABORTED: a process killed between two @WPT lines really does hold more than arrived, and the abort
         line already says so. */
      if (h && !abortedHere && h.count !== settled.size) {
        fileFail++;
        failures.push(`  FAIL   ${rel} :: <harness>\n         the harness holds ${h.count} subtest(s) and ` +
                      `${settled.size} reached this driver — the report and the run disagree about the file`);
      }
    }
    pass += filePass;
    fail += fileFail;
    area.pass += filePass;
    area.fail += fileFail;
    /* AND HOW MUCH OF THAT COLUMN CAME FROM A FILE THAT DID NOT FINISH — the fact whose absence turned this
       area's row into a number about nothing. Counting an aborted file's subtests is right and stays (see the
       paragraph at the abort above); what was missing is that the row never said it had done so, and the two
       populations do not decompose the same way. `css/css-values` read `pass 82 fail 15072`, which was taken as
       an area failing 99.5% of its subtests and sent a reader hunting one wrong serialization convention across
       the CSS value machinery. 14134 of those 15072 came from TWO files, each killed by the CPU budget while
       re-registering ONE subtest name — 6469 and 7663 times — so the column was one runaway counted per
       registration, and the area's real surface was 938 failures against 82 passes: a completely different
       diagnosis, in a completely different component.
       IT HIDES NOTHING AND SUBTRACTS NOTHING. Both totals are printed exactly as they were; this is one extra
       line saying which part of them belongs to files whose own row already says they did not finish. A number
       that cannot be decomposed into finished and unfinished work is the shape §Testing warns about — an
       artifact of HOW the run went, wearing the shape of a measurement of WHAT ran. */
    if (abortedHere) { area.abPass += filePass; area.abFail += fileFail; area.abFiles++; }
  } finally {
    /* THE ONE PLACE THE AREA'S PROGRESS IS COUNTED, so that every `continue` above — a missing META script, a
       missing <script src>, an abort, a harness that never completed — still advances it. A count kept at the
       bottom of the body would be skipped by exactly the paths that matter most. */
    if (++area.done === area.expected) areaFinish(area);
  }
}

/* PER-AREA, NOT ONE NUMBER. `encoding` alone answers three quarters of a million subtests — one per code point
   per legacy encoder — so a single total is a number in which every other area is invisible: a component that
   lost a hundred subtests and one that gained them read identically. The areas are the checked-out paths, which
   is the same list that decides what runs, so the breakdown cannot drift from the corpus.
   THE ROWS ARE REPEATED HERE because the streamed ones are interleaved with their failure lines and a reader
   wants them together; the streaming is what makes a killed run report, the summary is what makes a finished
   run readable. Every area must have finished — each file passes through the accounting above exactly once —
   so an unflushed area at this point is this driver miscounting, and it says so rather than printing a table
   that quietly disagrees with the rows above it. */
console.log("  ---- summary");
{
  const names = [...areas.keys()].sort();
  for (const n of names) {
    const a = areas.get(n);
    if (a.done !== a.expected || a.lines.length)
      throw new Error(`[wpt] area ${n} finished ${a.done} of ${a.expected} runs with ${a.lines.length} ` +
                      "unreported line(s) — the per-area accounting is wrong, so this table cannot be trusted");
    areaRow(a);
  }
}

/* AND WHAT IS SITTING IN THE CHECKOUT THAT NOTHING RAN — WHICH IS NOW A FAILURE, NOT A ROW. A sparse checkout in
   CONE MODE gives you the files of every directory on the path to a listed one, so naming
   `service-workers/service-worker/resources` as a support path also puts 247 service-worker TESTS on disk, and
   naming `FileAPI/blob` put ten FileAPI tests one level up. They were present, walked by nothing, counted by
   nothing, and NOTHING SAID SO — this gate's own version of the defect it exists to catch, one level out from the
   corpus.
   PRINTING THE COUNT WAS THE HALF-FIX, AND THIS PARAGRAPH USED TO ARGUE IT WAS THE WHOLE ONE. It said naming them
   with counts made widening the list "a decision someone makes rather than a discovery someone stumbles into" —
   and then 265 files sat here for as long as that sentence did, because a count carries no decision and nothing
   made anyone take one. The decision is now the mechanism: a file on disk is either RUN — WPT_PATHS for a
   subtree, WPT_OWN_LEVEL for a level cone mode has already materialized, which costs no checkout — or it fails
   this gate by name. Both populations this census used to distinguish are decided the same way and the
   distinction is still what tells you WHICH: a group whose puller is a SUPPORT path is collateral, and a group
   with no puller is a standard nobody named (`webidl/idlharness.any.js` beside the listed
   `webidl/ecmascript-binding` was that, twice, for FileAPI and streams). So the puller is still printed, and the
   number to read is ZERO. */
{
  const named = (rel) => WPT_PATHS.some((d) => rel === d || rel.startsWith(d + "/")) ||
    /* RUN BY AN OWN-LEVEL ENTRY: directly in that directory, which is exactly what `collectOwnLevel` walks. */
    WPT_OWN_LEVEL.some((d) => rel.startsWith(d + "/") && !rel.slice(d.length + 1).includes("/"));
  /* WHICH ENTRY PUT THIS DIRECTORY ON DISK. Cone mode checks out every file of every directory ON THE PATH to a
     listed one, so a stray file's puller is any listed entry that lives BELOW its directory. */
  const pullerOf = (dir) => WPT_PATHS.filter((d) => d.startsWith(dir + "/")).sort()[0];
  const stray = new Map();
  for (const f of collect(WPT, [])) {
    const rel = relative(WPT, f).split(sep).join("/");
    if (named(rel)) continue;
    /* GROUPED BY DIRECTORY, at most two segments deep — the granularity a WPT_PATHS entry is written at. The
       filename is not part of the key; taking the first two SEGMENTS made one row per file for everything
       sitting at a standard's own level, which is a list of 300 rows saying "xhr". */
    if (g_unreadable.has(rel)) continue;   /* it is reported below, by name, as unreadable — not as a stray test */
    const k = rel.split("/").slice(0, -1).slice(0, 2).join("/") || ".";
    stray.set(k, (stray.get(k) || 0) + 1);
  }
  const total = [...stray.values()].reduce((a, b) => a + b, 0);
  g_undecided = total;
  if (!total) {
    console.log("  ---- every test file in the checkout is named by WPT_PATHS or WPT_OWN_LEVEL");
  } else {
    console.log(`  ---- UNDECIDED — ${total} test file(s) are checked out and run by nothing. Each needs a ` +
                "decision at WPT_PATHS: list the directory to check out and run its subtree, or list it in " +
                "WPT_OWN_LEVEL to run the level cone mode already materialized. This FAILS the gate.");
    for (const [k, n] of [...stray.entries()].sort((a, b) => b[1] - a[1])) {
      const puller = pullerOf(k);
      console.log(`       ${String(n).padStart(4)}  ${k}` +
                  (puller ? `   (on disk as an ancestor of the listed ${puller})` : "   (NAMED BY NOTHING)"));
    }
  }
}

/* AND WHAT THE VARIANTS CAME TO. A WPT `variant` is a SEPARATE TEST RUN of the same file at a different address
   — `<meta name="variant" content="?wrapper">`, or `// META: variant=` — and WPT runs one per declaration,
   never the bare file. This gate used to run the bare file and none of the variants, and reported the shortfall
   here as a work item. It is now the work: the run unit is (file, variant), so this line states the SIZE of
   what was being skipped rather than the fact that it was. */
{
  const vfiles = new Set(runs.filter((r) => r.variant).map((r) => r.file));
  if (vfiles.size)
    console.log(`  ---- ${vfiles.size} collected file(s) declare variants, run as ` +
                `${runs.filter((r) => r.variant).length} distinct runs at their own addresses`);
}
/* COLLECTED VERSUS READ, EVERY RUN, WHETHER OR NOT ANYTHING WENT WRONG. A total that LOOKS complete and is not
   is the defect this gate exists to catch, and until now the only number printed was the collected one — so a
   corpus that lost files under the run reported a smaller total with nothing to say why, which is
   indistinguishable from a regression. Two numbers make the difference visible without anyone having to
   suspect it.
   AND WHEN THEY DIFFER, THE CORPUS IS ASKED WHETHER IT MOVED. `sparse-checkout set` at the top of this file is
   a write to a checkout every concurrent run also writes, from whichever tree it was launched from, so "the
   file vanished mid-run" and "the file was never readable" are both real and are different work: the first is
   a statement about how this gate is being RUN and is fixed by not running two cones against one corpus; the
   second is a statement about the checkout and is fixed by provisioning it. Reporting them alike is the
   collapsed-verdict failure §Testing names. */
{
  const collected = new Set(files.map((f) => relative(WPT, f).split(sep).join("/")));
  const readable = files.length - [...g_unreadable.keys()].filter((k) => collected.has(k)).length;
  console.log(`  ---- corpus: ${files.length} file(s) collected, ${readable} readable` +
              (g_unreadable.size ? `; ${g_unreadable.size} unreadable path(s) seen in total` : ""));
  if (g_unreadable.size) {
    /* THREE ANSWERS, ALL WRITTEN OUT — no `||` standing in for the third. "The corpus did not move" is a
       POSITIVE finding and the most expensive one to misread: it says the checkout is genuinely short of what
       this gate collected, which is a provisioning bug, whereas the other two say the measurement was taken
       while somebody else moved the ground. */
    const now = corpusIdentity();
    const moved = now.head !== CORPUS_AT_START.head
                    ? `its HEAD changed (${CORPUS_AT_START.head} → ${now.head})`
                : now.cone !== CORPUS_AT_START.cone
                    ? "its sparse cone was rewritten — ANOTHER RUN OF THIS GATE applied a different WPT_PATHS " +
                      "to this shared corpus while this run was in flight, so these files are an artifact of " +
                      "HOW this ran, not of what ran"
                    : "did not move during this run — these files were never readable, so the checkout itself " +
                      "is short of what this gate collected";
    console.log(`       the corpus ${moved}`);
    for (const [k, why] of [...g_unreadable.entries()].sort()) console.log(`       ${why.padEnd(8)} ${k}`);
  }
}
/* THE REVISION, AGAIN, IN THE TAIL. The head of a corpus run scrolls out of every terminal and out of every
   paste — the block a reader quotes is this one — so the identity is emitted where the numbers are and not
   only where the run began. Re-asked rather than reprinted, because the answer can have CHANGED: this checkout
   is shared, and a tree edited between the link and the summary means `git show HEAD` will not produce the
   program these numbers are about. Three answers, all written out, for the reason the corpus's own three are:
   "it did not move" is a POSITIVE finding and is what makes the head above quotable. */
for (const l of revisionLines(REV_AT_START)) console.log(l);
{
  const moved = revisionMoved(REV_AT_START);
  console.log(moved ? `[rev] AND IT MOVED WHILE THIS RAN — ${moved}`
                    : "[rev] the engine did not move during this run");
}
console.log(`  files ${files.length}   runs ${runs.length}   subtests ${pass + fail}   pass ${pass}` +
            `   fail ${fail}   aborted-runs ${aborted}   errored-runs ${errored}   unreadable-runs ${unread}` +
            `   undecided-files ${g_undecided}`);
console.log("===========================================================");
process.exit(fail || aborted || unread || g_undecided ? 1 : 0);
