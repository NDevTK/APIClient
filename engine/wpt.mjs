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
import { existsSync, readdirSync, mkdtempSync, openSync, readFileSync, readSync,
         statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
import { tmpdir, cpus, loadavg } from "node:os";
import { gateRevision, revisionLines, revisionMoved } from "./gate_revision.mjs";
import { lexborNativeArchive } from "./lexbor_source.mjs";
import { childCpuSeconds, childCpuDelta, cpuText } from "./gate_cpu.mjs";

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
/* The meter every verdict below is measured in lives in engine/gate_cpu.mjs — ONE reader, because this file
   carried a hand-copied one that read the PARENT's own idle CPU and printed it as the killed child's. */
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
                   /* THE `.idl` FILES EVERY `idlharness` TEST FETCHES — WITHOUT WHICH THAT WHOLE FAMILY IS AN
                      EXCLUDED TEST THAT COLLECTS, RUNS, AND PASSES EVERY CHECK THIS FILE MAKES ABOUT ITS OWN
                      COLLECTION. It is the excluded-test failure at one remove, and it is the hardest-looking
                      shape of it yet: the idlharness files ARE named by the entries below, ARE classified as
                      tests by tools/manifest/sourcefile.py and by this gate alike, ARE handed their META
                      scripts, and DO run — and every one of them REPORTED EXACTLY TWO SUBTESTS, `idl_test
                      setup` and `<harness>`, because `resources/idlharness.js`'s `fetch_spec` is
                      `fetch('/interfaces/' + spec + '.idl')` and this corpus had no `interfaces` directory.
                      `css/cssom/idlharness.html` was the one that said so in words — `Error fetching
                      /interfaces/cssom.idl.` — and the rest timed out awaiting that same promise, which is the
                      same fact wearing the column that hides it. So the family's rows were a statement about
                      THIS LIST and not about the engine, while the total looked complete.
                      THE FLOOR IS THE TELL, AND IT IS THE PART THAT OUTLIVES THIS INCIDENT: when every member
                      of a family reports the SAME SMALL SUBTEST COUNT, that count is a DECLARED FIXTURE that
                      never arrived, because one rejected promise fails every member at the identical point. A
                      family scoring badly varies; a family that is not running does not. That is why
                      `idlFixtures` below resolves an `.idl` at COLLECTION and why each run prints its subtest
                      count beside its verdict — neither is decoration, and the second is what makes the first
                      falsifiable.
                      HOW MANY MEMBERS THE FAMILY HAS IS NOT WRITTEN HERE. It grows with WPT_PATHS, so a number
                      here is wrong the moment somebody widens the list — and the reader who wants it is the one
                      about to invalidate it. `idlFixtures` IS the derivation; run it rather than quoting a
                      count.
                      WHAT WAS THEREFORE NEVER ASKED IS THE ONLY THING IN THIS TREE THAT CAN ASK IT.
                      engine/idlgen.mjs, engine/idl_members.mjs and engine/idl_installed.mjs diff the spec's
                      member NAMES against the names each component installs, so they measure PRESENCE and
                      nothing else — `grep -c variadic` over all three is zero. TWO LATER TOOLS NARROWED THAT
                      GAP AND DID NOT CLOSE IT, and saying which is what stops the next reader building a third
                      copy: engine/argaudit.mjs joins a member's DECLARED argument types against its own BODY,
                      and engine/argtypegate.mjs joins that declaration against the IDL — both read the
                      DECLARATION, so neither can see what the engine actually INSTALLED. idlharness.js asserts the
                      properties a name cannot carry: a function object's `length` (the interface object's, an
                      operation's minimum argument count, a getter's 0 and a setter's 1), whether a name is an
                      accessor or a data property (attribute vs operation), the interface object's `name`, its
                      [[Prototype]] chain against the IDL's `inheritance`, the property attributes
                      {writable, enumerable, configurable} of every member, `[Exposed]` against the realm, and
                      `toString.length === 0`. A gate that cannot fail on any of those is what let 460 installs
                      each remember their own `length` and seven disagree with each other.
                      IT COSTS NO TEST AND NO SUBTREE. `interfaces` is FLAT at the pinned revision — 336 `.idl`
                      files plus META.yml and README.md, zero subdirectories — and an `.idl` is neither a
                      document nor a `.js`, so `testKind` answers null for every one of them and the stray
                      census at the foot of this file is unmoved. It is checked out to BE USED, with the same
                      standing as `resources` and `common`. Every spec name the collected files pass to
                      `idl_test` resolves there at the pinned revision — dom, html, url, streams, encoding,
                      FileAPI, IndexedDB, webcrypto and its two tentative siblings, fetch, referrer-policy, fs,
                      storage, storage-buckets, permissions, service-workers, xhr, cssom, cssom-view,
                      css-pseudo, pointerevents, uievents, SVG, mathml-core, fullscreen, wai-aria, webidl,
                      observable.tentative — so this one entry is what turns the family on.
                      THE PREDICTION THAT STOOD HERE WAS THAT THE NUMBERS WOULD BE LARGE AND BAD, AND THE HALF
                      IT GOT WRONG IS THE USEFUL HALF. Large is right — a member of this family reports subtests
                      in the hundreds, one per interface plus one per member. What it implied was that `length`
                      would be the finding, since arity is the axis the paragraph above says nothing local can
                      ask. IT IS NOT. The dominant failure is PROPERTY ATTRIBUTES, and the second is the BRAND
                      CHECK; arity is a rounding error beside them, because the `.length` values are mostly
                      right and it is what surrounds them that is wrong.
                      AND THE SPLIT IS PER-INSTALLER, NOT PER-INTERFACE — which is the structural fact, and the
                      one that predicts the next member without anybody counting. Web IDL §3.7.6 Attributes and
                      §3.7.7 Operations both build their descriptor with `[[Enumerable]]: true`, while §3.7.3's
                      @@toStringTag and the interface object on the global are `[[Enumerable]]: false`. A
                      component installed through core/idl_args.h's `idl_install_*` gets exactly that. A
                      component installed through quickjs's OWN `JS_SetPropertyFunctionList` gets ECMAScript
                      BUILT-IN attributes — non-enumerable members, which is right for `Array.prototype.map` and
                      wrong for every Web IDL member — and one whose interface object reaches the global through
                      `JS_SetPropertyStr` gets an ordinary [[Set]]'s writable+enumerable+configurable, wrong in
                      the OPPOSITE direction. So the failing set is not a list of interfaces; it is whichever
                      components still reach those two quickjs entries. GREP FOR THE INSTALLER, NEVER FOR THE
                      INTERFACE.
                      THE BRAND CHECK IS THE SAME QUESTION ASKED OF THE RECEIVER, and it is worse than a wrong
                      answer. idlharness calls every operation with `this = null` and `this = {}` and requires a
                      TypeError. Where the engine derives its record from `this_val` and DCHECKs that the derived
                      pointer is non-NULL, the receiver is a value THE PAGE SUPPLIED — input, not something this
                      codebase computed — so the assert stands on the one thing CLAUDE.md says an assert may
                      never stand on, and `X.prototype.m.call(null)` ABORTS the engine where the spec asks it to
                      throw. That is not a hypothetical: it is how a variant of dom/idlharness.window.js ends. */
                   "interfaces",
                   /* THE STANDARD, THEN ITS COMPONENTS — the same two-level shape as `dom` below, and for the
                      same reason twice over. `FileAPI/blob` and three siblings were listed and the STANDARD was
                      not, so ten test files living at FileAPI's own level — fileReader, unicode, idlharness —
                      were checked out by cone mode and collected by nothing, and FileReader's and BlobURL's
                      directories were not measured at all. */
                   "FileAPI", "FileAPI/blob", "FileAPI/file", "FileAPI/support", "FileAPI/url",
                   "FileAPI/BlobURL", "FileAPI/FileReader", "FileAPI/filelist-section",
                   "FileAPI/reading-data-section",
                   "encoding", "tools",
                   /* WEB CRYPTOGRAPHY. §10's Crypto is COMPLETE — `subtle`, `getRandomValues`, `randomUUID` —
                      and its oracle sat uncollected, which is the excluded-test failure this gate exists to
                      catch: `getRandomValues.any.js` checks the step ORDER that a hand-review cannot (a
                      Float32Array longer than 65536 bytes must take step 1's TypeMismatchError and NOT step 3's
                      QuotaExceededError), the zero-length view, the subclass receiver and the same-object
                      return, and `randomUUID.https.any.js` runs 256 draws through a collision Set — which is
                      the one property this engine's reproducible stream has to earn rather than assert.
                      IT CANNOT BE A WPT_OWN_LEVEL ENTRY, because nothing else puts this standard on disk and
                      that list adds nothing to the checkout. So the SUBTREE comes too: 201 files, 125 test
                      documents, all but the three at its own level exercising §14's SubtleCrypto, whose
                      members are absent pending §13's CryptoKey model. NOTHING IS PREDICTED HERE ABOUT WHAT
                      THEY SCORE — what each abort names is the work queue, read off the run. The subtree is
                      generateKey 41, derive_bits_keys 31, import_export 30, sign_verify 25, serialization 20,
                      encrypt_decrypt 18, digest 8, util 7, encap_decap 3, wrapKey_unwrapKey 2. */
                   "WebCryptoAPI",
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
                   /* HTML §8.5.4 "The innerHTML property" / §8.5.5 "The outerHTML property" — innerHTML,
                      outerHTML, insertAdjacentHTML and §13.4 "Parsing HTML fragments", which element.c
                      implements and which SPEC_STEPS.md §4 is the conversion target for.
                      THIS ROW HAS NOW BEEN WRONG TWICE, EACH TIME BY OUTLIVING THE ABSENCE IT DESCRIBED. First
                      it said DOMParser and XMLSerializer "live here too and are absent"; then, when §8.5.1's
                      DOMParser landed, it said DOMParser's "XML arm CRASHES by name" and that "every file that
                      reaches an XML type ABORTS on a capability neither interface can have until an XML parser
                      exists". core/xml/ IS that parser and it exists — `parseFromString(…, "text/xml")` returns
                      a Document — so the sentence sent its reader to build what was already built, which is
                      exactly the stale-DFAIL failure this file names one row up.
                      SO THE ROW STOPS NAMING WHICH CAPABILITY IS MISSING, because that is the sentence that
                      keeps going stale, and names instead what the directory IS — which does not change. It is
                      FOUR subjects that fail in four different places: §8.5.1's DOMParser over five
                      DOMParserSupportedTypes; §8.5.8's XMLSerializer, which is DOM Parsing and Serialization
                      §3.2.1's XML serialization algorithm and NOT §13.3's, so it fails where a namespace
                      prefix map or an empty-element tag does; the §8.5.4/§8.5.5/§8.5.6 markup members over
                      documents served as `application/xhtml+xml`, which reach HTML §7.5.3 "Loading XML
                      documents" and not the HTML parser; and `tentative/`, which is the WICG
                      declarative-partial-updates proposal
                      (`streamHTML`, `appendHTML` and their positional siblings) and is in no standard — WPT's
                      own sourcefile.py collects a tentative file as an ordinary test, so it is run and counted
                      here like any other, and it is the majority of this directory's subtests.
                      NO COUNT AND NO PREDICTION IS QUOTED HERE ON PURPOSE, for the reason the `dom` row above
                      gives: a row that pre-declares its own numbers is a row nobody reads a regression out of.
                      What each abort NAMES is the work queue, and it is read off the run. */
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
                      same shape: `css/mediaqueries/resources` and `css/css-scroll-snap/support` each sit under
                      a standard whose OWN LEVEL cone mode would then materialize — 106 and 92 blobs
                      respectively — so each is a decision about running that standard, not a helper that costs
                      nothing, and each belongs to whoever takes it. THE THIRD OF THEM, `css/css-fonts/support`,
                      IS NOW INSIDE A LISTED ENTRY, because that decision was taken: its standard is the row two
                      below this one, and 680 is that standard's own level rather than a cost this entry pays. */
                   "css/support",
                   /* CSS TYPED OM LEVEL 1 — §4.3.1 "Common Numeric Operations, and the CSSNumericValue
                      Superclass", §4.3.2 "Numeric Value Typing", §4.3.3 "Value + Unit: CSSUnitValue objects" and
                      §4.3.4 "Complex Numeric Values: CSSMathValue objects". Both section numbers and titles were
                      read out of the fetched draft, not recalled. Five built components — core/css/css_math.c,
                      css_math_value.c, css_numeric_value.c, css_unit_value.c and css_property_numeric.c, 4528
                      lines at the revision this entry lands on — install CSSNumericValue, CSSNumericArray,
                      CSSStyleValue, CSSUnitValue, CSSMathValue and the five CSSMath* subclasses, and the
                      standard's directory was on nobody's disk. `grep '"css/css-typed-om'` over this file
                      answered zero, which is the excluded-test failure the three css rows above already name:
                      the numeric-type algebra is a pure-function subject with an exact oracle, and nothing in
                      this checkout asked it a single question.
                      ITS FIXTURES WERE CHECKED BEFORE THE ENTRY WENT IN — now by the mechanism below rather than
                      by hand, which is the point of building it. Of the 381 files at the pinned revision, 378
                      are not `.yml` and total 518921 bytes; `testKind` answers document for 348 of them, script
                      for 11 `.any.js` and null for 19. Their declared fixtures resolve to exactly seven paths:
                      `/resources/testharness.js` (348 documents), `/resources/testharnessreport.js` (348),
                      `/resources/idlharness.js` and `/resources/WebIDLParser.js` (idlharness.html, the second
                      through SERVER_REWRITES), `css/css-typed-om/resources/testhelper.js` (334 documents and the
                      one `// META: script=` line the eleven scripts share), and
                      `css/css-typed-om/the-stylepropertymap/properties/resources/testsuite.js` (243) — the last
                      two INSIDE this entry, which is what makes it one entry and not three.
                      AND ONE THAT IS NOT THERE, WHICH IS SAID RATHER THAN DISCOVERED. Three files name
                      `../resources/comparisons.js` — stylevalue-normalization/transformvalue-normalization.
                      tentative.html, stylevalue-subclasses/cssMatrixComponent.tentative.html and
                      stylevalue-subclasses/cssTransformValue.tentative.html — and `git ls-tree -r` at bf4714d
                      finds no `comparisons.js` anywhere in the corpus except test262 staging. It is broken
                      UPSTREAM at this revision, exactly like back-forward-cache/pagehide-event-handler-
                      microtasks.window.js's `./resources/test-helper.js`, so no WPT_PATHS entry can supply it
                      and those three abort naming it.
                      THE `idlharness.html` FIXTURE SET WAS CHECKED TOO, and it is the largest in this corpus:
                      `idl_test(['css-typed-om'], ['cssom', 'SVG', 'geometry', 'html', 'dom', 'mathml-core'])`,
                      all SEVEN present under the listed `interfaces`. That file also carries `<meta
                      name="timeout" content="long">`.
                      IT MUST BE A WPT_PATHS ENTRY AND NOT A WPT_OWN_LEVEL ONE. Its two helper directories are
                      inside the subtree, `css`'s own level is already materialized by the three rows above, and
                      naming a deeper path — `stylevalue-subclasses/numeric-objects`, where the numeric-type
                      tests live — would put two intermediate own levels on disk that neither list accounts for,
                      which this gate hard-fails.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. Three of the standard's interfaces are
                      absent by name — CSSKeywordValue, CSSUnparsedValue and StylePropertyMap — so the two
                      thirds of this directory written about StylePropertyMap have no component to answer them
                      and each abort NAMES that, which is the work queue read off the run. */
                   "css/css-typed-om",
                   /* CSS FONTS LEVEL 4 §2.1 "Font family: the font-family property", §2.1.1 "Syntax of
                      <font-family-name>" and §2.1.2 "Syntax of <generic-font-family>". Section numbers and
                      titles read out of the fetched draft, not recalled. Two built things answer them and
                      NEITHER HAD A CONFORMANCE ORACLE IN THIS TREE: core/css/css_font_shorthand.c carries
                      §2.1.1's `<font-family-name> = <string> | <custom-ident>+` as the grammar §2.7's
                      `<'font-family'>#` term is validated against, and core/css/css_style_declaration.c's
                      `cssom_initial_value` answers §2.1's `Initial:` line — which that property's own
                      `Inherited: yes` makes the base case of every element in every document. The live cone
                      under `/css/` held css-typed-om, css-values, cssom, cssom-view and support and nothing
                      else, so `getComputedStyle(el).fontFamily` was judged by NO gate here at any value.
                      WHAT IT COSTS AND WHAT IT COLLECTS, DERIVED RATHER THAN ESTIMATED. At the pinned revision
                      the subtree is 2481 files, of which `testKind` answers `document` for 163, `script` for
                      NONE and `unreadable` for none — 83 under `parsing`, 45 at the standard's own level, 15
                      `animations`, 14 `variations`, 4 `math-script-level-and-math-style`, 1 `matching` and 1
                      `font-display`. The 2318 that collect are mostly not tests at all: 1438 `.glif` and the
                      rest of the 1525 under `support` are a UFO font source tree, which `nameIsNonTest` refuses
                      by path part. The derivation is `testKind` ITSELF read out of this file and run over
                      `git ls-tree -r bf4714d -- css/css-fonts`, calibrated first against the three counts the
                      css-typed-om row above already publishes (348 documents, 11 scripts, 19 non-`.yml` nulls)
                      and against `css/css-values`' 268 — reproduced to the digit, which is what makes 163 a
                      measurement and not an estimate.
                      IT MOVES THE DENOMINATOR AND THE NUMBER IS SAID RATHER THAN LEFT TO BE NOTICED. The walk
                      collects 5969 files (4907 documents, 1062 scripts), so this is +163 documents and +2.73%.
                      A pass count taken across this entry is a fraction of a different population from one
                      taken before it, and the two are not comparable as totals.
                      AND THAT DENOMINATOR IS DERIVED OVER THE PINNED TREE, NEVER OVER THE CHECKOUT. This gate's
                      first act is `sparse-checkout set ...WPT_PATHS` followed by a hard fail on any entry still
                      absent, so what a RUN collects is what the TREE holds for the listed entries — while the
                      directory on disk is free to lag the list, and did: three roots stood unmaterialized when
                      this entry was written, so the same walk read off DISK answers 5601 (4540, 1061), short by
                      exactly their 367 documents and 1 script. A denominator taken from the checkout a previous
                      run happened to leave behind is a figure about THAT RUN rather than about this gate — the
                      re-sparsification warning above, read from the other end, and a coverage figure whose
                      population is not the one it names. RETIRED when a run prints its own collected total per
                      entry, because the number is then re-derived by the gate instead of restated here.
                      EVERY FIXTURE THE 163 DECLARE RESOLVES, CHECKED BEFORE THE ENTRY WENT IN and by this
                      file's own `docScriptFixtures` rather than by eye. They name exactly ten paths:
                      `/resources/testharness.js` and `/resources/testharnessreport.js` (163 each),
                      `css/support/parsing-testcommon.js` (53), `computed-testcommon.js` (25),
                      `interpolation-testcommon.js` (16), `inheritance-testcommon.js` (1) and
                      `shorthand-testcommon.js` (1, reached as `../../support/…` from `parsing`), plus
                      `/resources/idlharness.js` and webidl2 through SERVER_REWRITES — all under the listed
                      `resources` and `css/support` — and `css/css-fonts/support/font-family-keywords.js` (3),
                      which is INSIDE this subtree. Nothing is unresolved and nothing is absent upstream.
                      IT MUST BE A WPT_PATHS ENTRY AND NOT A WPT_OWN_LEVEL ONE, for the css-typed-om row's
                      reason and for a second: that helper is inside the subtree, and 83 of the 163 — the
                      `parsing` directory, which is where the computed-value oracle lives — are not at the own
                      level at all. `css`'s own level is already materialized by the rows above and holds
                      `.htaccess` and `README.md`, neither a test, so this entry adds no intermediate own level
                      and the stray census stays at zero.
                      THE `idlharness.html` FIXTURE SET WAS CHECKED TOO: `idl_test(["css-fonts-5", "css-fonts"],
                      ["cssom"])`, and all three `.idl` files are present under the listed `interfaces`.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. CSS Fonts 4 §2.1 "Font family: the
                      font-family property" states a `Computed value:` that is a LIST — "list, each item a
                      string and/or <generic-font-family> keywords" — and this engine answers the SPECIFIED
                      text for a declared value, so `parsing/` is expected to disagree — but which files and
                      how is what the run says, and a number written here would be a guess sitting where the
                      next reader takes it for a fact. Per §A-DIRECTORY-THAT-ABORTS
                      a count arriving where there was no result is the first honest measurement of an area and
                      never a regression to revert. */
                   "css/css-fonts",
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
                   /* HTML §6.10 "Close requests and close watchers". It is a TOP-LEVEL directory upstream and
                      not an `html/…` one, which is why a reader looking for it under `html/interaction` finds
                      nothing — its 55 files sit at `close-watcher/`, and this checkout's own index at the
                      pinned revision is where that was read rather than from recollection.
                      IT IS THE ORACLE FOR §6.10.2's GROUP ALGEBRA AND FOR NOTHING ELSE IN THIS TREE. The
                      manager's allowance, its banking boolean and its join-the-last-group branch are arithmetic
                      no other corpus asks about, and `basic.html`, `abortsignal.html`, `event-properties.html`
                      and `inside-event-listeners.html` ask about them through the ONE interface that
                      establishes a watcher in this build. Its `esc-key/` and `user-activation/` subtrees ask
                      about a close request, which nothing here dispatches, and its `iframes/` subtree asks
                      across navigables — so a large part of this directory is measuring §6.10.1 and §4.11.4,
                      neither of which exists yet.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. Expect bad first numbers: what each
                      failure NAMES is the work queue, and a directory left out because its numbers would be
                      bad is the excluded test this gate exists to catch. */
                   "close-watcher",
                   /* HTML §8.1.7.3 "Processing model"'s rendering loop and §8.12 "Animation frames" —
                      core/rendering. §8.9 stood at both numbers here and is "User prompts", which owns
                      alert/confirm/print and nothing on this row. The
                      component did not exist, so neither did this row; `requestAnimationFrame` was one of the
                      ~1300 names browser/platform_names.h had the engine reporting as honestly ABSENT. Now
                      that the rendering task source, "update the rendering" and §8.12's map of animation
                      frame callbacks are built, a
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
                   /* HTML §6.12 "The popover attribute" and §6.5.1 "The ToggleEvent interface" —
                      core/html/popover.c, core/html/toggle_event.c, and the ordered set both of them move
                      elements through, core/css/top_layer.c. Three built components whose spec directory this
                      list did not name, which is the excluded-test failure this file catches twice above
                      wearing its hardest shape yet: the PARENT was already in a list. `html/semantics` is a
                      WPT_OWN_LEVEL entry, so the standard is on disk, appears in a list, and reports a row —
                      and `popovers/` was in NEITHER list, so a reader checking whether the area was accounted
                      for found the name and stopped. A parent in the own-level list is the strongest possible
                      false COMPLETE here, because that list's whole meaning is that the subtree stays absent.
                      WHAT IS ASKED ABOUT THOSE MEMBERS TODAY, COUNTED AT THE PINNED REVISION RATHER THAN
                      ESTIMATED. `togglePopover` is named by 11 files in this corpus, TEN of them under this
                      directory, and by ZERO files under any path either list carries — so §6.12's toggle
                      popover has no collected test that names it at all. `showPopover` is named by 203 files
                      and 7 of those are collected: four under `close-watcher/user-activation/`, one under
                      `dom/nodes/moveBefore/`, one under `shadow-dom/reference-target/tentative/`, and one
                      helper. `hidePopover` is 97 and 4, all four under
                      `shadow-dom/reference-target/tentative/`. Every one of those is a test written about a
                      DIFFERENT standard that reaches a popover on its way past, which is coverage by accident
                      — and accidental coverage is what makes a component look measured while the directory
                      written about it is not on disk.
                      ITS FIXTURES WERE CHECKED BEFORE IT WAS ADDED, which is the idlharness lesson applied in
                      the one direction that costs nothing. Its 133 files name 15 distinct scripts between
                      them and every one already resolves under a listed entry, so this is not another family
                      that collects, runs, and reports a floor of two subtests apiece because the thing it
                      fetches is on nobody's disk. Its own `resources/` holds five helpers and no test
                      document; 82 of the 133 load `/resources/testharness.js` and none is a `.any`/`.window`/
                      `.worker.js`.
                      IT COSTS 133 BLOBS AND 356 KiB, AND IT MUST BE A WPT_PATHS ENTRY. Naming it in
                      WPT_OWN_LEVEL would add nothing to the checkout — that is the whole difference between
                      the two lists — and nothing else puts this subtree on disk, since `html/semantics`'s own
                      level is materialized by the two rows directly above. Naming the PARENT instead is the
                      3970-file checkout that own-level list exists to avoid. Six files under
                      `interactive-elements/the-dialog-element` and one more name
                      `popovers/resources/popover-utils.js` and `popovers/resources/toggle-event-source-test.js`,
                      so this entry also serves them the day that directory is taken.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. The subtree is UNMEASURED, which is a
                      different statement from either passing or failing, and what each failure NAMES is the
                      work queue, read off the run. */
                   "html/semantics/popovers",
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
                   /* location.c — HTML §7.2.4 "The Location interface", whose own directory was never checked out even though
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
                   /* HTML §7.2.2 "The Window object" AND §7.4 "Navigation and session history" — WindowProxy and popups. `window.open`, `opener`, `parent`,
                      `top`, `frames`, named access, and what a cross-origin WindowProxy may expose. This
                      engine has just grown a WindowProxy member surface and §7.2.2.1 "Opening and closing windows"'s open(), and neither had a
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
                   /* AND THE THIRD ONE, WHOSE ABSENCE THIS GATE HAD BEEN REPORTING AS AN ABORT.
                      `back-forward-cache/weblocks-worker.https.window.js` names
                      `/html/browsers/browsing-the-web/remote-context-helper-tests/resources/test-helper.js`,
                      which is a REAL FILE at the pinned revision in a directory NO entry above reaches —
                      `remote-context-helper` and `remote-context-helper-tests` are two directories, and having
                      the first is not having the second. That is the excluded-test failure with a reason
                      attached, which this file calls the hardest kind to notice.
                      IT COSTS 27 TEST FILES AND THAT IS THE DECISION, NOT A SIDE EFFECT. Cone mode materializes
                      every directory ON THE PATH, so naming this `resources` puts
                      `remote-context-helper-tests`'s own level on disk — 27 `.window.js` files that test the
                      RemoteContextHelper itself (addWindow, addIframe, addWorker, navigation, bfcache), 32 RUNS
                      because `addIframe-urlType` declares four variants and `addWindow-urlType` three. They are
                      claimed by WPT_OWN_LEVEL below, because a test file on disk that neither list accounts for
                      fails this gate, and because leaving them out to keep one helper cheap would be buying a
                      smaller number with an excluded directory. Expect bad first numbers: every one of them
                      opens a window or a worker. */
                   "html/browsers/browsing-the-web/remote-context-helper-tests/resources",
                   /* `/html/resources/common.js` is named by a `<script src>` in dom/ranges, and a document
                      whose script 404s runs a test nobody wrote. Its last segment is `resources`, so it is
                      checked out to BE USED and contributes no test of its own. */
                   "html/resources",
                   /* HTML §8.4 DYNAMIC MARKUP INSERTION — `document.open()`, `document.write()`,
                      `document.writeln()`, `document.close()` and the HTML-unsafe markup members. Every one of
                      those algorithms is a built component here (core/html/document_open.c,
                      core/html/document_write.c, core/dom/element.c) and NOT ONE of their tests was checked
                      out, so the standard's own suite could not fail: the excluded-test defect with the total
                      still looking complete, which is the shape this list exists to make impossible.
                      THE STANDARD IS LISTED AND SO ARE ITS SEVEN SUBDIRECTORIES, the same two-level shape as
                      `dom` and `FileAPI` above and for both of their reasons. The standard-level entry is what
                      CHECKS OUT the subtree, so a subdirectory upstream adds later is collected rather than
                      silently missed — it holds no test file of its own at the pinned revision, which is a fact
                      about that revision and not a reason to leave it unlisted. The seven are REPORTING
                      refinements over subjects that fail in different places: opening-the-input-stream is
                      §8.4.1's document-replacing arm, document-write is §8.4.3's parser feed, html-unsafe-
                      methods is `setHTMLUnsafe`/`parseHTMLUnsafe`, and the innerHTML/outerHTML pair is the
                      §8.5.4 serializer reached through a different member. One row for all of them would be one
                      number in which none of them is visible.
                      NOTHING IS PREDICTED ABOUT WHAT THEY SCORE. Not one of these files has ever run in this
                      tree. What each abort NAMES is the work queue, and it is read off the run — expect the
                      §8.4.1 arm this engine CRASHES on by design to be well represented, because
                      core/html/document_open.c's step 5 DFAIL is what stands between a parse-time write and the
                      destructive half of an algorithm it must not reach. Its ancestor `html/webappapis` holds
                      no file at its own level, so this costs no own-level entry below. */
                   "html/webappapis/dynamic-markup-insertion",
                   "html/webappapis/dynamic-markup-insertion/closing-the-input-stream",
                   "html/webappapis/dynamic-markup-insertion/document-write",
                   "html/webappapis/dynamic-markup-insertion/document-writeln",
                   "html/webappapis/dynamic-markup-insertion/html-unsafe-methods",
                   "html/webappapis/dynamic-markup-insertion/opening-the-input-stream",
                   "html/webappapis/dynamic-markup-insertion/the-innerhtml-property",
                   "html/webappapis/dynamic-markup-insertion/the-outerhtml-property",
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
                   "service-workers/service-worker/resources",
                   /* AND THE SECOND HELPER THE SAME STANDARD'S OWN idlharness NAMES, WHICH THIS GATE HAD
                      ALREADY DIAGNOSED IN FULL AND NOBODY HAD ACTED ON. `service-workers/idlharness.https.any.js`
                      is the one member of that family that never ran at all: its META block names
                      `cache-storage/resources/test-helpers.js` beside the service-worker one, and the run
                      reported `ABORT [corpus] … it EXISTS at bf4714d81e and this checkout does not have it, so
                      WPT_PATHS is short of service-workers/cache-storage/resources: add that entry and the file
                      runs`. That message is this driver at full strength — it asked git, it distinguished a
                      checkout gap from a file the pinned corpus does not contain, and it named the entry to
                      add — and the entry still was not here, which is the excluded test surviving its own
                      diagnosis.
                      IT COSTS SEVENTEEN TEST FILES AND THAT IS THE DECISION, NOT A SIDE EFFECT — the same
                      arithmetic `remote-context-helper-tests` carries above. The `resources` directory holds
                      ten files and no test, but cone mode materializes every directory ON THE PATH, so naming
                      it puts `service-workers/cache-storage`'s own level on disk: twelve `.https.any.js` /
                      `.https.window.js` scripts and five documents that load testharness.js. They are claimed
                      by WPT_OWN_LEVEL below, because a test file on disk that neither list accounts for fails
                      this gate, and because leaving them out to keep one helper cheap would be buying a smaller
                      number with an excluded directory. Its `crashtests/` sibling is not on the path to
                      anything listed and stays absent, which is every unlisted path's standing statement:
                      untested rather than passing. Expect bad first numbers — every one of those seventeen
                      opens with `caches`, and there is no CacheStorage component. */
                   "service-workers/cache-storage/resources",
                   /* THE NAVIGATION API — HTML §7.2.6 "The navigation API". Seven built components and not one
                      collected test that names any of them: core/frame/navigation.c (§7.2.6.2 "The Navigation
                      interface", §7.2.6.3 "Core infrastructure", §7.2.6.4 "Initializing and updating the entry
                      list", §7.2.6.6 "The history entry list"), core/frame/navigation_history_entry.c
                      (§7.2.6.5 "The NavigationHistoryEntry interface"), core/frame/navigate_event_fire.c
                      (§7.2.6.10.4 "Firing the event"), core/events/navigate_event.c (§7.2.6.10.1 "The
                      NavigateEvent interface"), core/frame/navigation_destination.c (§7.2.6.10.3 "The
                      NavigationDestination interface"), core/events/navigation_current_entry_change_event.c
                      (§7.2.7.1 "The NavigationCurrentEntryChangeEvent interface") and
                      core/frame/navigation_abort.c.
                      WHAT IS ASKED ABOUT THEM TODAY, COUNTED AT THE PINNED REVISION RATHER THAN ESTIMATED.
                      `navigation.currentEntry` is named by 228 corpus files, 209 of them under this directory,
                      and by ZERO collected tests. `navigation.back` is 95 / 89 / zero, `navigation.traverseTo`
                      37 / 36 / zero, `navigateEvent` 30 / 26 / zero, `navigation.updateCurrentEntry` 14 / 14 /
                      zero. The four INTERFACE names each show one hit outside this directory and it is the same
                      file every time — `interfaces/html.idl`, for which `testKind` answers null because an
                      `.idl` is neither a document nor a `.js`. It is a FIXTURE, and no collected test passes
                      `html` to `idl_test` as a src, so even the interface surface is unasserted. This is the
                      popover shape without the accidental coverage: not a component that looks measured, a
                      component nothing in the checkout mentions.
                      ITS FIXTURES WERE CHECKED BEFORE IT WAS ADDED. Its 445 collected tests name 20 distinct
                      scripts between them and every one resolves: `resources/` (testharness, testharnessreport,
                      testdriver and its two siblings), `common/` (get-host-info.sub.js at 24 files, utils.js,
                      dispatcher, gc.js), seven helper paths inside this directory itself, and two that cross
                      into listed entries — `html/browsers/browsing-the-web/back-forward-cache/resources/
                      helper.sub.js` at nine files and `service-workers/service-worker/resources/
                      test-helpers.sub.js` at one, both of which this list already carries and wptserve already
                      substitutes. It declares no `idl_test`, so there is no `.idl` to be short of.
                      IT COSTS 492 BLOBS AND 591 KiB. All 445 collected tests are DOCUMENTS — this directory
                      holds no `.any`/`.window`/`.worker.js` at all. 452 of its documents load
                      `/resources/testharness.js` and the gate drops seven of those: six sit under a
                      `resources/` path segment and one carries the `-manual` type flag, both of which are
                      `nameIsNonTest`/`testKind` doing exactly what they are for.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. */
                   "navigation-api",
                   /* INTERSECTION OBSERVER §2.2 "The IntersectionObserver interface", §2.3 "The
                      IntersectionObserverEntry interface" and §3.2's processing model — core/
                      intersection_observer/intersection_observer.c and intersection_observer_entry.c, which
                      are 1692 and 357 lines and have no collected oracle between them.
                      COUNTED AT THE PINNED REVISION. `IntersectionObserver` is named by 155 corpus files, 141
                      under this directory, and by ZERO collected tests; `isIntersecting` 62 / 55 / zero,
                      `intersectionRatio` 29 / 27 / zero, `IntersectionObserverEntry` 19 / 18 / zero,
                      `rootMargin` 13 / 10 / zero. Every name shows exactly one hit outside this directory and
                      it is `interfaces/intersection-observer.idl` in all five cases — the fixture, not a test,
                      and one no collected file fetches, so it has sat in the checkout unread.
                      ITS FIXTURES WERE CHECKED. Eight distinct scripts across 143 collected tests, all
                      resolving: `resources/` (testharness, testharnessreport, idlharness, webidl2 under the
                      `/resources/WebIDLParser.js` rewrite this file's SERVER_REWRITES already carries),
                      `common/rendering-utils.js` and `common/get-host-info.sub.js`, its own
                      `resources/intersection-observer-test-utils.js` at 98 files, and one crossing into a
                      listed entry — `css/cssom-view/support/scroll-behavior.js`, under the listed
                      `css/cssom-view`. Its `idlharness.window.js` declares `idl_test(['intersection-observer'],
                      ['dom'])` and BOTH `interfaces/*.idl` are present, so this is not another family that
                      collects, runs and reports its floor of two subtests.
                      IT COSTS 171 BLOBS AND 268 KiB: 142 documents and one `.js`. 143 of its documents load
                      the harness and one is dropped for sitting under `resources/`.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. */
                   "intersection-observer",
                   /* THE CONSOLE STANDARD — §1.1 "Logging functions", §1.2 "Counting functions", §1.3
                      "Grouping functions", §1.4 "Timing functions" and §2.2 "Formatter(args)" — core/console/
                      console.c, 1004 lines, zero collected tests.
                      COUNTED AT THE PINNED REVISION, and the numbers are small because this standard is small:
                      `console.timeEnd` is named by 4 corpus files, 2 here, ZERO collected; `console.countReset`
                      2 / 1 / zero; `console.dir` 1 / 1 / zero; `console.table` occurs in NO corpus file at all,
                      which is a fact about the corpus and not about this list. The count map, the group stack
                      and the timer table that console.c holds per realm are written by exactly the members
                      nothing here asks about.
                      ITS FIXTURES WERE CHECKED. Seven distinct scripts across 12 collected tests: five under
                      `resources/` (testharness, testharnessreport, testdriver and its vendor shim, idlharness,
                      webidl2 via the WebIDLParser rewrite) and its own `console/helper.js` at four files.
                      `idlharness.any.js` declares `idl_test(['console'], [])` and `interfaces/console.idl` is
                      present, so its IDL assertions run.
                      IT COSTS 16 BLOBS AND 18 KiB: 5 documents and 7 `.any.js`. Six of its files are markup;
                      the sixth carries the `-manual` type flag AND loads no harness at all — a page that asks a
                      human to read the devtools console — so `testKind` drops it at the name, before the
                      content is ever read, which is both rules agreeing rather than one covering for the other.
                      This is the cheapest entry in this list after the one below it, and its component is the
                      largest per byte of corpus.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. */
                   "console",
                   /* INPUT DEVICE CAPABILITIES — §"The InputDeviceCapabilities interface" and §"Extensions to
                      the UIEvent interface and UIEventInit dictionary". THAT STANDARD NUMBERS NO SECTION: its
                      document is unnumbered `<h2>`s throughout, so a §number written beside these titles would
                      be an invention, and the title is the whole of the citation this one can carry.
                      core/events/input_device_capabilities.c is the interface and it is the TYPE of the
                      `sourceCapabilities` member core/events/ui_event.c installs, which UIEvent, FocusEvent,
                      MouseEvent and KeyboardEvent all inherit — so one uncollected directory is the oracle for
                      a member on four interfaces.
                      COUNTED AT THE PINNED REVISION. `InputDeviceCapabilities` is named by exactly 2 corpus
                      files: this directory's `idlharness.window.js` and `interfaces/input-device-capabilities.
                      idl`. `sourceCapabilities` and `firesTouchEvents` are named by ONE file each and it is the
                      `.idl` both times — the fixture, which `testKind` answers null for. So the whole corpus
                      says this interface exists in exactly one test, and this list did not have it.
                      ITS FIXTURES WERE CHECKED. Two scripts, both under `resources/`: idlharness.js and
                      webidl2 via the WebIDLParser rewrite. Its `idl_test(['input-device-capabilities'],
                      ['uievents', 'dom'])` names three specs and all three `.idl` files are present, so the
                      file asserts rather than reporting its floor of two subtests.
                      IT COSTS 2 BLOBS AND 413 BYTES — one META.yml and one `.window.js` — for ONE collected
                      test. It is the cheapest entry this list has ever taken.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. */
                   "input-device-capabilities",
                   /* FULLSCREEN API §2 "Model", §3 "API" and §5.1 ":fullscreen pseudo-class" — core/fullscreen/
                      fullscreen.c and the ordered set it moves elements through, core/css/top_layer.c, which is
                      the same set `html/semantics/popovers` above was taken for. Those are its two live
                      consumers, with core/html/close_watcher.c a third; core/html/html_dialog.c is NOT one,
                      because `showModal()` is honestly absent there and it is the only producer of the
                      modal flag — so the top layer's second measured consumer is this directory and no other.
                      COUNTED AT THE PINNED REVISION, and this one DOES have accidental coverage, which is the
                      shape that makes a component look measured. `requestFullscreen` is named by 105 corpus
                      files, 57 under this directory, and by THREE collected tests — `dom/nodes/moveBefore/
                      fullscreen-preserve.html` and the two `html/semantics/popovers/popover-top-layer-*.html`
                      — each written about a DIFFERENT standard and reaching fullscreen on its way past. The
                      four other cross-directory hits are not tests at all: two sit under `resources/`, one is
                      `interfaces/fullscreen.idl`, and one is corpus data under `tools/`. `exitFullscreen` is
                      73 / 48 / the same three; `fullscreenElement` 71 / 57 / one (the moveBefore file);
                      `fullscreenchange` 55 / 45 / ZERO, so §3's event is asked about by nothing in this
                      checkout.
                      ITS FIXTURES WERE CHECKED. Nine distinct scripts across 83 collected tests, all
                      resolving: `resources/` (testharness, testharnessreport, testdriver, testdriver-vendor,
                      testdriver-actions, idlharness, webidl2 via the rewrite), its own `trusted-click.js` at 51
                      files, and `common/top-layer.js` — which is under the listed `common` and is the shared
                      helper the popover entry above also leans on. `idl_test(['fullscreen'], ['dom', 'html'])`
                      names three specs and all three `.idl` files are present.
                      IT COSTS 110 BLOBS AND 147 KiB: 78 documents and 5 `.js`. All 78 markup files that load
                      the harness are collected — nothing here is dropped by name, so `markupOnly` and
                      `testKind` agree with each other on every one of them.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. 68 of its 83 collected tests pull
                      `testdriver.js` — 64 documents by `<script src>` and four `.window.js` by `// META:
                      script=`, counted apart because a total of 68 read as documents would have been wrong
                      about both halves — so expect what a corpus that wants a driver does in an engine that
                      has none, and what each abort NAMES is the work queue, read off the run. */
                   "fullscreen",
                   /* COOKIES — ADDED BECAUSE A LANDED FIX HAD NO ORACLE IN EITHER DIRECTION. RFC 6265 §5.3
                      step 5 refuses a `Domain` that is a public suffix, and until it was built this engine
                      STORED `Domain=com`; the reason it survived is that no gate here could see it. That is
                      the excluded-test failure in its purest form — not a test that fails, a test that is not
                      on disk, so the area reports nothing and the total looks complete.
                      THE SHAPE WORTH KEEPING: a cone entry is not a coverage preference, it is the set of
                      questions this tree is ABLE to ask. A component can be built, asserted and reviewed
                      against a standard nobody can score it against, and nothing anywhere says so — the
                      sparse cone is read by no audit and appears in no verdict. So when a diff implements a
                      step, ask whether its area is IN this list before predicting anything about a gate, and
                      say "unscorable here" rather than quoting a pass that was never attempted.
                      Widening this list is expected to ADD failures. Per §A-DIRECTORY-THAT-ABORTS, that is the
                      first honest measurement of an area and never a regression to revert. */
                   "cookies",
                   /* INTERACTIVE ELEMENTS — the same absence as `cookies` above, found the same way. A lane
                      built HTML §4.11.4's `show()`, `showModal()` and the dialog focusing steps and then
                      reported that NO gate here judges a single `<dialog>` test, so none of it has any
                      conformance oracle in this tree. It established that by reading
                      `.git/info/sparse-checkout` rather than the directory listing — the right instrument,
                      because a directory that is absent and a directory that is empty look alike from a `ls`.
                      Its sibling `html/semantics/popovers` IS here and gave the popover half an oracle all
                      along, which is exactly how a missing entry hides: the neighbouring area reports
                      normally and the total looks complete. */
                   "html/semantics/interactive-elements",
                   /* POINTER EVENTS — THE THIRD ENTRY FOUND THE SAME WAY, AND THE ONE THAT SHOWS WHY THE
                      TWO ABOVE ARE NOT ENOUGH ON THEIR OWN. A lane landed `PointerEvent` — the interface a
                      real bundle feature-detects and this engine answered false for — and the interface has
                      no oracle here: `pointerevents/` holds 214 files at the pinned revision, 191 of them
                      loading `resources/testharness.js`, and NOT ONE is checked out.
                      WHAT IT DOES HAVE IS ONE ROW OF SOMEBODY ELSE'S TABLE, which is the shape that makes an
                      absence look covered. `dom/nodes/Document-createEvent.https.html` lists "PointerEvent"
                      among its factory names and asserts the prototype identity and the initial Event state,
                      so `window.PointerEvent` existing is scored — and its constructor, its members, its
                      property attributes and its `[Exposed]` are scored by nothing. `interfaces/pointerevents.idl`
                      IS checked out and reads like the missing oracle and is not: `css/cssom-view/idlharness.html`
                      names it in `idl_test`'s SECOND argument, and `internal_add_dependency_idls` builds
                      `{ only: [] }` and marks what it pulls `untested` — a dependency IDL resolves types for
                      the specs under test and asserts nothing about itself. Read out of the corpus's own
                      `resources/idlharness.js` rather than assumed.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES, and the `fullscreen` entry above says
                      why that sentence is written rather than a number: 169 of the 191 reference
                      `testdriver` and 22 do not, so most of this area wants synthetic pointer input from an
                      engine that has no device — expect what that does, and read the work queue off what
                      each abort
                      NAMES. Per §A-DIRECTORY-THAT-ABORTS a count arriving where there was no result is the
                      first honest measurement of an area and never a regression to revert. */
                   "pointerevents",
                   /* THE CANVAS ELEMENT — HTML §4.12.5 "The canvas element" and §4.12.5.1 "The 2D rendering
                      context", whose §4.12.5.1.16 "Pixel manipulation" road has just landed as core/canvas/
                      canvas_rendering_context_2d.c, image_data.c, canvas_path.c, path_2d.c and svg_path_data.c.
                      Section numbers and titles read out of the fetched multipage spec, not recalled.
                      THE AREA WAS NOT MERELY UNLISTED, IT WAS ABSENT FROM DISK, which is the level BELOW an
                      uncollected test and the one this file's own header calls worse: a gate that walks what it
                      finds, collects correctly and reports honest pass counts is SILENT about everything
                      outside the cone. Measured rather than assumed, and BOTH halves separately because a
                      directory that is absent and a directory that is empty look alike from an `ls`: `grep -i
                      canvas .git/info/sparse-checkout` answers NOTHING against 99 lines, and `ls html/canvas`
                      answers ENOENT while `git ls-files html/canvas` answers 4688. The only canvas-named files
                      on disk were 9 service-worker tainting tests and one custom-elements reaction file, none
                      of them about this interface. So `getContext`, the context, ImageData and Path2D shipped
                      into a tree where no WPT subtest could score them at any value.
                      IT IS `element` AND NOT `html/canvas`, AND THE LINE IS A COMPONENT BOUNDARY RATHER THAN A
                      PREFERENCE ABOUT NUMBERS. The sibling `offscreen` is §4.12.5.3 "The OffscreenCanvas
                      interface" — a DIFFERENT interface, and one this engine does not have: NO component
                      declares or installs it, and its only non-comment occurrence anywhere in core is the
                      STRING LITERAL in html_canvas_element.c's placeholder-mode throw, whose own comment says
                      that arm is unreachable because the member entering that mode does not exist. (The name
                      occurs 8 times in core; seven are comments recording its absence, which is why the
                      question is asked of the INSTALLER and never of a `grep -c` over the interface.) That
                      subtree is 2481 files and 2058 collected tests, 1014 of them `.worker.js`/`.any.js` wanting a
                      DedicatedWorkerGlobalScope this engine also does not have, so naming it would roughly
                      double this entry to measure a component nobody has written. It is the `css` row's rule —
                      only the standards whose components exist — and its standing statement is the one every
                      unlisted path carries: untested rather than passing. It becomes a row the day
                      OffscreenCanvas does. `tools` is generator tooling that `nameIsNonTest` refuses by path
                      part, and it is on the path to nothing listed, so it stays absent too.
                      WHAT IS NOT NARROWED IS THE DRAWING ROAD, AND THAT IS DELIBERATE. canvas_rendering_
                      context_2d.h states that every drawing member is ABSENT rather than present-and-inert, so
                      `fill-and-stroke-styles` (257), `compositing`, `text` (108), `layers`, `shadows`,
                      `line-styles` and `transformations` will reach a member that is not there. Picking only
                      the directories whose members landed would be choosing the good numbers, which is the
                      excluded test this gate exists to catch; what each failure NAMES is the work queue.
                      `html/canvas/resources` COMES WITH IT AND IS NOT OPTIONAL. Every generated canvas test
                      carries `<script src="/html/canvas/resources/canvas-tests.js">` — 1145 of the 1252
                      documents — and that helper defines the `_addTest`/`_assertSame`/`_assertPixel` wrappers
                      the test bodies are written in. Without it this area is the idlharness family again: it
                      would collect, run, and report a floor per file because the thing it fetches is on
                      nobody's disk. Its last path part is `resources`, so `nameIsNonTest` refuses all 18 of its
                      files and it contributes NO test — checked out to BE USED, the same standing as
                      `css/support`, `html/resources` and `wai-aria/scripts`. It also carries `2x2.png`, which
                      is the only pixel source any of these tests are handed.
                      EVERY DECLARED FIXTURE WAS RESOLVED BEFORE THE ENTRY WENT IN, by extracting each
                      collected file's `<script src>` and `// META: script=` lines at the pinned revision. The
                      1253 tests name 14 distinct paths, and they add up: FIVE under `resources/` (testharness
                      and testharnessreport at 1252 each, testdriver and its two siblings at 5 each), THREE
                      under `common/` (media.js at 10, get-host-info.sub.js at 3, and namespaces.js, which is
                      the next paragraph), ONE at `dom/events/scrolling/scroll_support.js` under the listed
                      `dom/events`, THREE inside this subtree (canvas-display-p3.js at 11, imagebitmap's
                      common.sub.js at 8, CanvasWidgetWithImages.https.sub.js at 1), and TWO under
                      `html/canvas/resources` above — canvas-tests.js at 1145, wait-for-canvas-paint.js at 65.
                      Thirteen of the fourteen are PRESENT at the pinned revision, checked one by one.
                      AND ONE THAT IS NOT THERE, SAID RATHER THAN DISCOVERED. `manual/imagebitmap/
                      createImageBitmap-serializable.html` names `/common/namespaces.js`, and at bf4714d the
                      only `namespaces.js` in the whole corpus is `trusted-types/support/namespaces.js`. It is
                      broken UPSTREAM at this revision exactly like css-typed-om's `comparisons.js`, so no
                      WPT_PATHS entry can supply it and that ONE file aborts naming it.
                      IT COSTS 2161 BLOBS AND 3899 KiB — 2143 under `element` and 18 under `resources`.
                      IT MOVES THE DENOMINATOR AND THE NUMBER IS SAID RATHER THAN LEFT TO BE NOTICED: the walk
                      collects 6133 over the lists as they stand at this revision, and these three entries make
                      it 7390 — +1257 (+1253 documents, +4 scripts), +20.50%, the largest single move this list
                      has taken. A pass count across this entry is a fraction of a different population from one
                      taken before it, and the two are not comparable as totals. The derivation is `testKind`
                      and `nameIsNonTest` READ OUT OF THIS FILE and run over `git ls-tree -r bf4714d`, never
                      over the checkout, and it was calibrated first against the three counts the rows above
                      already publish — css-typed-om's 381/348/11 and its 19 non-`.yml` nulls, css-values' 268,
                      css-fonts' 2481/163/0/2318 — and against the 1062 scripts the css-fonts row states for
                      the whole walk. All reproduced to the digit, which is what makes these measurements and
                      not estimates. RETIRED when a run prints its own collected total per entry.
                      NOTHING IS PREDICTED HERE ABOUT WHAT IT SCORES. */
                   "html/canvas/element", "html/canvas/resources",
                   /* THE LAYOUT AREAS, ADDED BECAUSE THE SECTIONS THEY JUDGE WERE BUILT AND NOTHING COULD FAIL
                      ON THEM. This file's own header says the list "grows as areas are covered \u2014 an area
                      absent here is honestly untested, which is a different statement from \"passes\"", and that
                      sentence had been true and unacted-on for a whole session of layout work: CSS 2.1 \u00a710.1's
                      containing-block cases, \u00a710.3.7 and \u00a710.6.4 over the static position, css-flexbox-1 \u00a74's
                      anonymous item, \u00a78.3's `align-items`, \u00a79.3 in both axes and \u00a79.4 steps 7-11,
                      css-overflow-3 \u00a73.1.4's viewport propagation, css-writing-modes-4 \u00a76.4's axis rows,
                      css-sizing-3 \u00a75.1's replaced rules and HTML \u00a715.4.3's dimension hints all landed against
                      a cone holding css-fonts, css-typed-om, css-values, cssom and cssom-view \u2014 five areas,
                      NONE of which lays anything out. The engine's own DCHECKs were the only judge, and a
                      DCHECK exists only where an author thought to write one.
                      THE POPULATION IS MEASURED BY THE CORPUS'S OWN CLASSIFIER AND NOT BY A `find`, because
                      those two answer questions an order of magnitude apart. `find <area> -name "*.html" | wc -l`
                      reads 5254 across the seven; `python3 engine/wpt_classify.py engine/.work/wpt` — which is
                      WPT's own tools/manifest/sourcefile.py, the same oracle this gate already diffs its
                      collector against — names 1055 of them as tests, every one of which loads
                      `resources/testharness.js` (checked file by file, 1055 of 1055). The derivations are those
                      two commands and the figures are what they answered at this revision; run them rather than
                      quoting these. A pass count taken across this entry is a fraction of a different
                      population from one taken before it.
                      THE STRUCTURAL FACT IS THE HALF THAT DOES NOT ROT, AND IT PREDICTS THE NEXT CSS AREA
                      WITHOUT ANYBODY COUNTING: a CSS area is predominantly REFTESTS — a pair of documents and
                      a `<link rel=match>`, asserting that two RENDERINGS are pixel-identical — so a
                      testharness-only gate measures a MINORITY of any such area BY CONSTRUCTION, and the four
                      fifths it cannot see are not a gap in the checkout or in the collector. They are a
                      different INSTRUMENT: the reference-image kind, which is exactly what measures a
                      RASTERIZER rather than a layout number. This gate owns the testharness fifth. Nothing here
                      claims the rest is covered, and a reader who reads `css/css-flexbox` in this list as "the
                      flexbox area is tested" has read it as four times the statement it makes.
                      AND THE FOUR FIFTHS ARE COUNTED BY `engine/wpt_reftest_scope.py`, WHICH IS THE THIRD
                      COMMAND AND THE ONLY ONE THAT ASKS THE CLASSIFIER FOR ANYTHING BUT ITS TESTHARNESS
                      ANSWER. `wpt_classify.py` keeps one kind and drops the rest on its own second line, so
                      until that script landed nothing in this tree COUNTED a reftest — the `find` above is a
                      count of FILES and stands in for a count of tests, which is a proxy rather than a
                      classification. The new one walks the same tree with the same authority and reports the
                      whole breakdown, the reference GRAPH (relations, how many references a test names, and
                      how many of those references are themselves reftest nodes, which is what makes it a
                      graph and not a pair), and two population bounds a reader of this entry needs.
                      IT ALSO REPORTS THE ONE BAND THAT IS A FACT ABOUT THIS CHECKOUT RATHER THAN ABOUT THE
                      CORPUS, and that band refines the sentence above rather than contradicting it: the
                      reftest-ness of a CSS area is not a gap in the cone, and a large part of the reftest
                      population is nonetheless unrunnable here, because the files those tests name as their
                      references are not on disk. They are shared — a handful of reference documents carry
                      most of the population — and they live in `css/reference` and `css/CSS2/reference`,
                      which are real directories at the pinned corpus revision.
                      THOSE TWO DIRECTORIES AND `fonts` ARE NOW LISTED, WHICH RETIRES THE HALF OF THIS
                      PARAGRAPH THAT SAID THEY SIT OUTSIDE THE ENTRIES — see the three rows at the end of
                      this list for what each one supplies and what it leaves behind. What is NOT retired is
                      the sentence above it: the reftest population is still a population NOTHING IN THIS
                      TREE RUNS, and putting a reference document on disk does not change that by one test.
                      The absent band does not go to zero either, and the entries say which references
                      survive it and why each is a refusal rather than an oversight.
                      THIS IS THE SHAPE THE `idlharness` ENTRY ABOVE ALREADY RECORDS, ARRIVING AT A DIFFERENT
                      FAMILY, and it is worth naming here because the reading it invites is the expensive
                      one: a reftest whose reference is absent does not report a missing file, it reports a
                      FAILING TEST, and the component it names is the ENGINE. A first reference-image run
                      made against this cone would therefore hand back a number that is a statement about
                      WHICH DIRECTORIES ARE CHECKED OUT wearing the clothes of a rendering score. Run the
                      script and read its absent band before reading any such number.
                      NOTHING IS PREDICTED HERE ABOUT WHAT A REFERENCE-IMAGE RUN WOULD SCORE. No instrument
                      in this tree has ever compared two renderings, so a first one is a BASELINE.
                      AND THE FIFTH IT DOES OWN HAS A REAL LAYOUT ORACLE IN IT, which is why the entries are
                      worth the disk at all: 341 of the 1055 load `resources/check-layout-th.js`, whose
                      assertions read `offsetWidth`/`offsetHeight`/`offsetTop`/`offsetLeft`,
                      `clientWidth`/`clientHeight`/`clientTop`/`clientLeft`, `scrollWidth`/`scrollHeight` and
                      `getComputedStyle` off the laid-out tree and compare them against `data-expected-*`
                      attributes the test author wrote. That is a USED-VALUE oracle written by the people who
                      wrote the spec, which is the one thing neither the IDL audit (presence) nor the smoke
                      fixture (whatever its author thought of) can be. All eleven of those members exist in this
                      engine — `core/html/html_element_view.c`, `core/dom/element_view.c`,
                      `core/css/css_style_declaration.c` — so the family is not blocked at a fixture.
                      AND IF IT WERE, THE TELL IS THE ONE THE `idlharness` ROW ABOVE ALREADY NAMES: a family
                      whose members all report the SAME SMALL SUBTEST COUNT is a family that is not running,
                      because one missing member fails every one of them at the identical point, while a family
                      that is merely scoring badly VARIES. Read the subtest counts beside the verdicts before
                      reading any total over this entry.
                      THE DISK COST IS ZERO AND THAT IS A PROPERTY OF THE CLONE, not of the areas: `--filter=blob:none`
                      means a widened cone fetches no blobs until a file is READ, so `df` was unchanged across
                      all seven adds. The cost arrives on the first run that opens them.
                      NOTHING IS PREDICTED HERE ABOUT WHAT THEY SCORE. A first run over an area this engine has
                      never been measured against is a BASELINE, and a low one is the honest starting number
                      rather than a regression. */
                   "css/css-sizing", "css/css-flexbox", "css/css-position", "css/css-writing-modes",
                   "css/css-overflow", "css/css-display", "css/css-logical",
                   /* THE REFERENCE DOCUMENTS EVERY CSS REFTEST NAMES, AND THE TYPEFACE ITS TESTS MEASURE
                      THEMSELVES AGAINST. Three rows, one shape, and the shape is the one the `idlharness`
                      entry at the head of this list already records: a fixture the corpus DECLARES, that
                      resolves at the pinned revision, and that this cone does not hold — so the family
                      naming it does not report a missing file, it reports a result that names the ENGINE.
                      THE TWO ROWS ARE NOT THE SAME KIND OF GAIN AND MUST NOT BE READ AS ONE, WHICH IS THE
                      whole reason they carry separate paragraphs below. `fonts` supplies a fixture to tests
                      THIS GATE ALREADY COLLECTS AND RUNS. `css/reference` and `css/CSS2/reference` supply
                      references to a population NOTHING HERE RUNS. Only the first can move a verdict, and
                      conflating them is how a reader comes to believe this list now covers reftests.

                      `fonts` — 213 blobs, 5004445 bytes. THE ROW THAT REACHES THE TESTS ALREADY BEING RUN.
                      A CSS test makes a text run a predictable size by loading a typeface whose every glyph
                      is one em box, then asserting a used value the engine computed from it. When the
                      typeface does not arrive the element falls back to whatever the host has, and the
                      assertion is then about a font nobody chose — a declared fixture that never arrives,
                      failing inside a family that collects, runs and reports like any other. The derivation
                      is two commands and they are what the figures below came from:
                        python3 engine/wpt_classify.py engine/.work/wpt | cut -f1 | sort > /tmp/h
                        cd engine/.work/wpt && grep -rl '/fonts/' --include='*.html' --include='*.htm' \
                          --include='*.xht' --include='*.xhtml' --include='*.js' --include='*.svg' \
                          --include='*.xml' . | sed 's|^\./||' | sort | comm -12 - /tmp/h
                      That answered 174 COLLECTED TESTHARNESS TESTS naming a path under `/fonts/`, across
                      fourteen distinct URLs, every one of them absent from this checkout. Eleven of the
                      fourteen resolve at the pinned revision and this entry supplies them; the tests naming
                      ONLY those eleven number 171 of the 174, counted file by file rather than estimated.
                      AND THE OTHER THREE ARE A CORPUS FACT, NOT A MISSING ENTRY — exactly like the
                      `comparisons.js` residual this list already records for css-typed-om. They are
                      `/fonts/Rochester.otf` (named by css/css-values/lh-unit-003.html),
                      `/fonts/RobotoExtremo-VF.subset.ttf` (css/css-fonts/font-variation-settings-
                      serialization-002.html) and `/fonts/FontWithFancyFeatures.otf`
                      (css/css-fonts/idlharness.html), and `git ls-tree -r` at the pinned revision finds no
                      such path under `fonts` for any of the three. Each file EXISTS, under
                      `css/css-fonts/support/fonts/`, which is already on disk and is reached at a different
                      address — so the tests ask the server for a path the corpus does not serve, and no
                      entry in this list can supply it. Those three go on failing at a fixture and saying so
                      here is what stops the next reader adding a fourth directory to chase them.

                      `css/reference` — 25 blobs, 9242 bytes — and `css/CSS2/reference` — 25 blobs, 15821
                      bytes. THE REFERENCE HALF, AND WHAT IT DOES NOT BUY IS THE PART TO READ FIRST. The
                      derivation is one command:
                        python3 engine/wpt_reftest_scope.py engine/.work/wpt
                      Its ABSENT band answered 22 distinct references not on disk, blocking 1158 reftests
                      whole-corpus; over the seven CSS layout areas listed directly above, 18 references
                      blocking 1054. Two documents carry most of it. Attributing that band by directory —
                      the same command, its absent lines grouped by the directory each reference lives in —
                      puts 18 references and 1148 blocked tests in `css/reference`, two and two in
                      `css/CSS2/reference`, one and seven in `css/filter-effects/reference`, and one and one
                      in `compat`. These two entries take the first two rows, which is every reference the
                      seven areas above name: over those areas the band goes to nothing, and whole-corpus it
                      goes from 22 references to the two named below.
                      EVERY ONE OF THE 22 WAS RESOLVED AT THE PINNED REVISION BEFORE THIS LANDED, one by
                      one, with `git cat-file -e <rev>:<path>` against each absent path and `[ -e <path> ]`
                      beside it: 22 of 22 present in the revision, 22 of 22 absent from the working
                      checkout. That is the idlharness lesson applied rather than restated — a fixture claim
                      is checked at the revision, not inferred from a directory existing.

                      WHAT THIS DOES NOT FIX, AND IT IS THE LARGER HALF: NOTHING IN THIS TREE RUNS A
                      REFTEST. No instrument here has ever compared two renderings, and these entries put
                      documents on disk without building one. A reader who takes a `css/reference` row in
                      this list as "reftests are handled now" has read it as the opposite of what it says.
                      The reference-image driver is a separate subproblem and remains unbuilt; what these
                      two rows change is that a first such driver would no longer hand back a number that is
                      a statement about WHICH DIRECTORIES ARE CHECKED OUT wearing the clothes of a rendering
                      score. That is a precondition for measuring, not a measurement.

                      TWO DIRECTORIES IN THAT BAND ARE DELIBERATELY NOT LISTED, AND THE REASON IS THE SAME
                      ONE IN BOTH CASES: each would put test files on disk that nothing runs, which the
                      census at the foot of this file FAILS the gate for — so adding them to unblock a
                      reference is a widening smuggled in under another justification, and the price is
                      paid by a population nobody decided to measure.
                      `compat` supplies ONE reference and unblocks ONE test, and it holds 13 testharness
                      tests of its own. `css/filter-effects/reference` supplies ONE reference and unblocks
                      SEVEN, and cone mode materializes every directory ON THE PATH to a listed one, so
                      naming it drags `css/filter-effects`'s OWN LEVEL onto disk — 459 blobs, 411472 bytes,
                      of which SEVEN are testharness tests. Both are real gaps and both are decisions for a
                      diff that means to take those areas on. The derivation for each is the corpus's own
                      classifier over the directory in question, which is what produced those two counts:
                        git archive <rev> <dir> | tar -x -C <scratch> && ln -s <wpt>/tools <scratch>/tools
                        python3 engine/wpt_reftest_scope.py <scratch> <dir>   # read its KINDS line
                      THE SAME QUESTION WAS ASKED OF EVERY DIRECTORY THIS DIFF DOES LIST, BEFORE IT LISTED
                      THEM, because a widening that trips the gate's own assertion is worse than none. Over
                      `css/reference`, `css/CSS2/reference` and `fonts` that classifier answers ZERO
                      testharness — 3 reftests and 22 support, 25 support, and 213 support respectively —
                      and this file's own `testKind` agrees with it for the reason that matters at the
                      collector: none of those 263 files is a `.js`, and none of them holds a
                      `<script src=/resources/testharness.js>` element, which are `testKind`'s only two
                      positive arms. So all three collect nothing, exactly as `resources` and `interfaces`
                      do, and the stray census is unmoved.
                      THE PATHS WERE PRICED TOO, WHICH IS THE PART THE FILTERED CLONE MAKES EASY TO MISS.
                      `css/reference` sits under `css`, whose own level is two files and both already on
                      disk. `css/CSS2/reference` drags `css/CSS2`'s own level — 22 blobs, 100730 bytes,
                      which that same classifier calls 13 reftest, 6 support, 1 crashtest, 1 visual and ZERO
                      testharness. `fonts` is top level and drags nothing. Total for the three rows and the
                      one dragged level: 285 blobs, 5130238 bytes, which `df` will show on the first run
                      that opens them rather than at checkout, because the clone is filtered.
                      NOTHING IS PREDICTED HERE ABOUT WHAT ANY AREA SCORES. A fixture that starts arriving
                      changes what a family is measuring, so a count taken across this entry is a fraction
                      of a different population from one taken before it. */
                   "css/reference", "css/CSS2/reference", "fonts",
                   /* HTML'S FORM-OWNER SURFACE, WHOSE ORACLE THIS TREE HAS BEEN IMPLEMENTING AGAINST AND
                      COULD NOT RUN. `html/semantics/forms` was on no disk, so members landed for it were
                      measured by nothing: the `form` getters on HTMLOptionElement and HTMLLegendElement --
                      the second of which core/html/html_element.c installs BY NAME -- have their oracles at
                      `the-option-element/option-form.html` (whose own rel=help is
                      html.spec.whatwg.org/multipage/#dom-option-form) and
                      `the-legend-element/legend-form.html`, and the form-owner ASSOCIATION those getters
                      answer from is `form-control-infrastructure/association.window.js`. Three directories,
                      chosen because those are the files that settle members ALREADY INSTALLED -- not
                      because the area is due.
                      THE OWN LEVEL COMES WITH THEM, WHICH IS WHY THE WPT_OWN_LEVEL ROW BELOW IS PART OF THIS
                      ENTRY RATHER THAN A SEPARATE THOUGHT. Cone mode materializes every directory ON THE
                      PATH, so naming these three puts `html/semantics/forms`'s own level on disk -- 7 blobs,
                      9490 bytes -- and FIVE of those are testharness tests. Left unclaimed they are exactly
                      the stray the census at the foot of this file FAILS the gate for.
                      EVERY FIXTURE WAS RESOLVED BEFORE THE ENTRY WENT IN, which is this file's own procedure
                      and not a formality. Across all 33 files every `<script src>` is one of
                      /resources/testharness.js, testharnessreport.js, testdriver.js, testdriver-vendor.js --
                      all four already on disk under the `resources` row -- plus ONE relative src,
                      `option-label-value.js`, which ships inside `the-option-element` itself. No META
                      script, no `.sub` template, no fetched `.idl`: nothing here needs a row not already
                      listed.
                      WHAT THE COLLECTOR TAKES IS MEASURED, through the corpus's own classifier rather than
                      this file's port of it:
                        python3 engine/wpt_classify.py engine/.work/wpt
                      answers 8444 testharness files / 9828 runs WITHOUT these rows and 8468 / 9852 WITH
                      them, and the diff of the two listings is EXACTLY the 24 files these three directories
                      and that own level contribute -- nothing else moved either way. The count closes
                      against the checkout: 33 blobs = 24 collected + 5 META.yml/WEB_FEATURES.yml + 4 that
                      `testKind` drops, each for a different reason it already states --
                      `option-disabled-manual.html` at the NAME, `dynamic-content-change-rendering.html` and
                      its `-ref.html` for holding no testharness element, and `option-label-value.js` for
                      being a `.js` with no global in its meta.
                      THE REMAINING DIRECTORIES OF `forms` ARE DELIBERATELY NOT LISTED, on the same ground
                      the `compat` row above states: each would put test files on disk that a decision has to
                      be taken about first. The derivation for whoever takes them is the two commands that row
                      already names, and the count is NOT restated here -- the sentence this replaced said
                      EIGHTEEN where `git ls-tree --name-only <rev> html/semantics/forms/` answers TWENTY-TWO
                      subdirectories of which three were listed, so NINETEEN, while the same sentence\'s blob
                      and byte totals (847 and 1464338) reproduced to the digit. A count computed once and
                      edited around goes wrong where the totals beside it do not, so what is written here is
                      the command and not the number. */
                   "html/semantics/forms/the-option-element", "html/semantics/forms/the-legend-element",
                   "html/semantics/forms/form-control-infrastructure",
                   /* THE TWO LARGEST FORM-CONTROL DIRECTORIES AND THE SUBMISSION DIRECTORY ONE OF THEM
                      DEPENDS ON -- the decision the row above says has to be taken, taken for three of them.
                      WHY THESE THREE AND NOT THE OTHER SIXTEEN. Each names an engine component that ships
                      and is measured by nothing. `the-input-element` is the oracle for core/html/
                      input_value.c, input_number.c, input_picker.c and text_control_selection.c;
                      `the-select-element` is the oracle for the FC_SELECT row of html_form.c\'s form-control
                      table, and it is the SIBLING of the already-listed `the-option-element` -- an option
                      suite without a select suite is an incoherent pair, since an option\'s index, its
                      selectedness and its form owner are all answered from its select.
                      `form-submission-0` is the oracle for form_entry_list.c, form_data.c, form_data_event.c
                      and form_submission_attributes.c, AND it is a fixture: `the-input-element/
                      input-type-change-submit.html` names
                      `../form-submission-0/resources/targetted-form.js`, which is the ONE unmet reference in
                      all three directories. Listing it is what closes that, and it is listed as a WPT_PATHS
                      row rather than pointed at its `resources` subdirectory precisely because a
                      resources-only row would materialize `form-submission-0`\'s own level -- 35 testharness
                      tests -- as the stray the census at the foot of this file FAILS the gate for.
                      EVERY FIXTURE WAS RESOLVED BEFORE THE ENTRY WENT IN, mechanically rather than by eye,
                      and through THIS FILE\'S OWN resolution rather than a second copy of it: SCRIPT_EL,
                      markupOnly, scriptMetadata and SERVER_REWRITES read out of this file at run time, then
                      the rule metaScripts and docScriptFixtures already state -- rewrite first, a `/`-rooted
                      ref is corpus-root-relative, anything else resolves against the test\'s own directory,
                      a query or fragment cut before the lookup. Over the three directories: 789 script
                      references, ZERO that resolve outside cone+candidate, ZERO absent from the corpus, ZERO
                      naming another origin or a `{{}}` template. The checker was ARMED BEFORE ITS ZERO WAS
                      BELIEVED -- run against the already-listed `the-option-element` it answers 24
                      references and no finding, and run against that same directory with `resources` removed
                      from the cone it names testharness.js and testharnessreport.js, 11 files each. A zero
                      from a probe that has never spoken is a statement about the probe.
                      WHAT THE COLLECTOR TAKES IS MEASURED, through the corpus\'s own classifier rather than
                      this file\'s port of it:
                        python3 engine/wpt_classify.py <corpus>
                      answers 8468 testharness files / 9852 runs WITHOUT these rows -- reproducing to the
                      digit the figure the block above publishes, which is what licenses reading the rest --
                      and 8735 / 10119 WITH them. The diff of the two listings is EXACTLY the 267 files these
                      three directories contribute, 119 + 113 + 35, and nothing moved the other way.
                      THE COST IS 568 BLOBS AND 935313 BYTES, which `df` will show on the first run that
                      opens them rather than at checkout, because the clone is filtered. It drags NO new
                      directory: all three are siblings of rows already listed, so cone mode materializes no
                      ancestor level that was not already on disk, and the file set grows by exactly 568 --
                      every one of them named by one of these rows. The stray census is therefore unmoved BY
                      CONSTRUCTION rather than by measurement, which is the property a leaf row has and a
                      `resources` row does not.
                      NOTHING IS PREDICTED HERE ABOUT WHAT ANY OF THE 267 SCORES. */
                   "html/semantics/forms/the-input-element", "html/semantics/forms/the-select-element",
                   "html/semantics/forms/form-submission-0",
                   /* HTML §7.1.4 "Cross-origin embedder policies", SHIPPED END TO END AND JUDGED BY
                      NOTHING. This engine reads `Cross-Origin-Embedder-Policy` and its report-only sibling
                      at core/frame/embedder_policy.c, through core/fetch/structured_fields.c's item parse;
                      carries the result as HTML §7.1.7 "Policy containers"' container ITEM
                      (core/frame/policy_container.h's `embedder` field and its `policy_container_embedder`
                      accessor); turns it into HTML §7.1.3 "Cross-origin opener policies"'
                      `same-origin-plus-COEP` at core/frame/opener_policy.c; and ends at
                      HTML §7.3.2.3 "Groupings of browsing contexts"' cross-origin isolation mode
                      (core/frame/browsing_context_group.h) and HTML §8.1.7.1 "Definitions"'
                      `crossOriginIsolated`, which core/frame/agent_cluster.c INSTALLS as an accessor. Every
                      one of those was read AT THE FILE rather than matched on a name, which is the check the
                      row below records a previous reading of this tree getting wrong in the other direction.
                      Not one line of that chain has ever met an oracle: before this entry
                      `git grep -ci cross-origin-embedder-policy engine/wpt.mjs` answered 0, with an invented
                      control answering 0 beside it.
                      IT IS WORTH A ROW FOR A REASON THE CONFORMANCE ROWS ABOVE DO NOT HAVE. The isolation
                      mode is what decides `crossOriginIsolated`, which core/frame/agent_cluster.h names as a
                      value a page READS and a flow forks on, and it is the same bit SECURITY.md's own
                      per-instance key is stated against — so a wrong answer here is not only a divergence
                      from a browser, it is a fork this engine takes or fails to take. A component with a
                      consumer that deep and no oracle at all is CLAUDE.md's
                      ships-a-component-and-nothing-judges-it, which is a FINDING rather than a gap.
                      IT COSTS 153 BLOBS AND 258718 BYTES AT THE PINNED REVISION. Re-price with
                        git -C engine/.work/wpt ls-tree -r -l <rev> -- html/cross-origin-embedder-policy \
                          | awk '{n++;b+=$4} END{print n,b}'
                      IT MUST BE A WPT_PATHS ENTRY AND NOT A WPT_OWN_LEVEL ONE, and it drags NO new own
                      level: `/html/` is already a cone directory with its subdirectories excluded, so cone
                      mode materializes THIS subtree and no ancestor that was not already on disk. The stray
                      census is therefore unmoved BY CONSTRUCTION rather than by measurement, which is the
                      property a leaf row has and a `resources` row does not.
                      EVERY DECLARED FIXTURE RESOLVES INSIDE THE CONE THIS ROW JOINS, checked BEFORE the row
                      went in rather than after: 248 declared references over this directory's `.html` and
                      `.js` files, 14 distinct targets, ZERO outside cone+subtree. `/common/` (including
                      `common/dispatcher/`), `/resources/` and
                      `service-workers/service-worker/resources/` are the three it reaches outside itself and
                      all three are listed above. Re-derive with
                        for p in $(git -C engine/.work/wpt ls-tree -r --name-only <rev> -- \
                                   html/cross-origin-embedder-policy | grep -E '\.(html|js)$'); do \
                          git -C engine/.work/wpt show <rev>:$p | grep -oE \
                          '(// META: script=[^ ]*|<script[^>]+src="[^"]*")'; done | sort | uniq -c
                      THAT ZERO IS A FLOOR AND IT SAYS SO, for the same reason the row below states: it sees
                      a `<script src>` element and a `// META: script=` line and nothing else, so a helper a
                      test reaches by a runtime `fetch()`, an `<iframe src>`, a WORKER CONSTRUCTOR or a
                      `?pipe=` handler is outside what it can look at — and this directory reaches for all
                      four, which makes the floor looser here than at any row above.
                      `service-workers/service-worker/resources/test-helpers.sub.js`, named by 18 of these
                      files, is the one already-known hazard and it is RETIRED: it answered HTTP 500 until
                      engine/wptserve.py declared `wss`, which that file records at its own site.
                      WHAT THE COLLECTOR TAKES IS NOT STATED HERE, because the classifier cannot be run
                      against a directory that is not on disk and this entry was written before one existed.
                      A NAME CONVENTION IS A FLOOR AND NOT A CLASSIFICATION — WPT decides a test document by
                      what it LOADS — so the number is taken the way every other row's is,
                        python3 engine/wpt_classify.py engine/.work/wpt
                      run before and after this entry, and the DIFF of the two listings is what this row
                      contributes. The 153 above is a BLOB count and is not that number.
                      NOTHING IS PREDICTED HERE ABOUT WHAT ANY OF IT SCORES, AND THE FIRST RUN'S FIGURE IS A
                      FLOOR OF FAILURES RATHER THAN A DEFECT LIST. Not one of these files has ever run in
                      this tree; HTML §7.1.4.2 "Embedder policy checks"' reporting half is a NAMED RESIDUAL
                      rather than a built mechanism — core/frame/embedder_policy.c names the observable, which
                      is Reporting API §3.4.1 "Generate report of type with data";
                      and whether HTML §7.3.2.3's `concrete` arm is REACHED is a question a run answers
                      and this comment does not. THE ONE FAILURE THIS ENTRY CAN HAVE is a first run read as a
                      REGRESSION, and it is read as the first measurement of a component that had none.
                      ITS TWO SIBLINGS ARE NOT THE SAME DECISION AND ARE NAMED SO THEY ARE NOT SWEPT IN WITH
                      IT. `html/cross-origin-opener-policy` (192 blobs) has a component too —
                      core/frame/opener_policy.c — and is a row somebody should write, after resolving its
                      fixtures the way this one's were; it is left out because two unrun rows at once produce
                      a failure count with no name in it. `fetch/cross-origin-resource-policy` (17 blobs) is
                      the opposite case and must NOT be added: `git grep -ic cross-origin-resource-policy
                      -- 'engine/host/**'` answers 0 against a COEP control of 13, so there is no component
                      there and widening into it buys refusals this engine already gives honestly. */
                   "html/cross-origin-embedder-policy",
                   /* THE TWO STANDARDS THAT DECIDE AN `@S` VERDICT, AND WHICH NOTHING IN THIS TREE HAS EVER
                      JUDGED. Every other row here buys CONFORMANCE: a wrong answer is a divergence from a
                      browser. These two buy something else, and CLAUDE.md §@S names it in one line -- a PoC
                      has to run under the page's ACTUAL policy, and the worked example it gives is an
                      inline `onerror` that a `script-src 'self'` kills -- so a wrong CSP answer does not
                      surface as a failing subtest, it
                      surfaces as a FALSE SECURITY VERDICT in a report a human acts on. The `cspBlocks` field
                      is produced in solver/solve.c out of policy_allows_inline and
                      policy_allows_string_compilation, which run core/frame/csp_source_list.c's source-list
                      match; a defect anywhere under that call chain is a breakout badged clean, or a real
                      sink badged blocked, with no oracle anywhere that would say so.
                      IT IS NOT AN ABSENT FEATURE, WHICH IS WHY IT IS WORTH A ROW. A previous reading of this
                      tree reported that there is no named component for either standard, deriving it from WPT
                      AREA NAMES not matching directory names, which is a claim about the naming and not
                      about the engine. What is on disk, read rather than name-matched:
                      core/frame/csp_directive_list.c is one parse of the grammar, core/frame/csp_source_list.c
                      is the source-list grammar and its matching, core/html/html_meta_csp.c is
                      HTML §4.2.5.3 "Pragma directives", and core/html/trusted_types.c is the sink half.
                      Fetch §4.1 "Main fetch" step 7 asks ALL FOUR of its disjuncts at one site in
                      core/fetch/fetch.c, the third of them being
                      CSP §4.1.2 "Should request be blocked by Content Security Policy?", reached from four
                      production callers (html_image.c, html_link.c, xml_http_request.c, and the park in
                      solver/engine.c that covers `<script src>` and `fetch()`).
                      SO THE QUESTION THIS ROW OPENS IS NOT "IS IT BUILT" BUT "IS IT RIGHT", AND THE PARTS
                      THAT ARE NOT BUILT ARE ALREADY NAMED BY THE TREE ITSELF rather than by this comment,
                      which is what keeps them from rotting here:
                        `git grep -n policy_allows_inline -- '*.c'` and the same for
                      `policy_allows_string_compilation`
                      answers which of CSP's inline checks have a caller and which have none, and
                      csp_source_list.c's own DCHECK under 'strict-dynamic' states in its own words that this
                      engine runs the inline check over a `<style>` element and over an event-handler
                      attribute and over no inline `<script>` element at all. CSP §5.5 "Report a violation" is
                      unbuilt too, and is a NAMED RESIDUAL at core/frame/policy_container.c and
                      core/html/html_base_element.c rather than a crash. IT USED TO BE A `DCHECK` THAT FIRED
                      ONLY WHEN A BLOCKING POLICY DECLARED AN ENDPOINT, and that is recorded because this row
                      is what would have met it: a policy is a header whichever server served the document
                      sent, so the guard was an assert over a stranger's bytes, and §5.5 gates only the report
                      POST on `report-uri`/`report-to` while firing the `securitypolicyviolation` event
                      unconditionally — so it was also silent about the endpoint-free policies that owe an
                      event just the same. THIS ENTRY READ `FIFTEEN FILES` AND THE COMMAND BELOW ANSWERS 47,
                      recorded rather than quietly corrected because the count is the half a reader quotes
                      onward and the command is the half they were told to run: the abort surface those two
                      asserts covered was THREE TIMES what this entry claimed. Four narrower readings were
                      tried and none is fifteen — `report-to` alone 14, `report-uri` alone 35, excluding
                      `support`/`resources` 41, the non-`.sub.headers` half 0 — so it was wrong WHEN WRITTEN
                      rather than a different question asked, which is a fact about how it was derived and
                      not about the corpus, since a count over a PINNED revision cannot go stale.
                      RETIREMENT: this record goes when no count in this entry stands beside a command that
                      answers differently.
                      `base-uri/report-uri-does-not-respect-base-uri.sub.html` is one of the 47, which is
                      the second site's own subject. Re-derive with
                        for p in $(git -C engine/.work/wpt ls-tree -r --name-only <rev> -- \
                                   content-security-policy | grep \.headers$); do \
                          git -C engine/.work/wpt show <rev>:$p | grep -i '^Content-Security-Policy:' | \
                          grep -qiE 'report-uri|report-to' && echo $p; done | wc -l
                      Read those; do not take a list from here.
                      IT COSTS 1699 BLOBS AND 2492419 BYTES AT THE PINNED REVISION -- 1361 and 1835203 for
                      `content-security-policy`, 338 and 657216 for `trusted-types`. Both are top level, so
                      neither drags another directory's own level onto disk and neither can move the stray
                      census by itself. Re-price either with
                        git -C engine/.work/wpt ls-tree -r -l <rev> -- <dir> | awk '{n++;b+=$4} END{print n,b}'
                      BOTH ARE WPT_PATHS ENTRIES AND NOT WPT_OWN_LEVEL ONES, because neither standard is on
                      disk at all: an own-level entry adds nothing to a checkout, and there is nothing here
                      for it to have already materialized.
                      EVERY DECLARED FIXTURE WAS RESOLVED BEFORE THE ROW WENT IN, through THIS FILE'S OWN
                      resolution rather than a second copy of it -- SCRIPT_EL, markupOnly, scriptMetadata,
                      metaScripts and SERVER_REWRITES sliced out of this file at run time and applied to the
                      1116 files the corpus's own classifier names for the three rows this diff adds: 3475
                      declared references, ZERO outside the cone. Four resolve to a path the pinned revision
                      does not have, and three of those four are the TEST'S OWN SUBJECT -- `<script src="x">`,
                      `src="a">`, `src="v">` are bogus addresses those trusted-types files load on purpose to
                      provoke a violation, so a 404 is what a browser answers too. The fourth is
                      `content-security-policy/parsing/support/helper.sub.js`, which is absent from WPT at
                      this revision and is a fact about the corpus that no entry here can change. Eight more
                      are `{{sub}}` templates, a `data:` URL and cross-origin addresses, which this file
                      classifies as unresolved BY DESIGN because wptserve resolves them and the driver does
                      not.
                      THAT ZERO IS A FLOOR AND IT SAYS SO. It sees a `<script src>` element and a
                      `// META: script=` line and nothing else, so a fixture a test reaches by a runtime
                      `fetch()`, an `<iframe src>`, an `<img src>` or a `?pipe=` handler is outside what it
                      can look at. The TELL for one is the same one the `interfaces` row above is written
                      about: if every member of a family reports the SAME SMALL subtest count, that count is a
                      declared fixture that never arrived, and the per-file subtest column is what makes it
                      visible.
                      WHAT THE COLLECTOR TAKES IS MEASURED, through the corpus's own classifier rather than
                      this file's port of it:
                        python3 engine/wpt_classify.py engine/.work/wpt
                      NOTHING IS PREDICTED HERE ABOUT WHAT ANY OF IT SCORES. Not one of these files has ever
                      run in this tree, so a pass count stated here would be invented.
                      THE CLAUSE THAT RATED NAMING A CAPABILITY THE SAME KIND OF GUESS IS RETIRED, and is
                      recorded rather than deleted because it re-derives easily: which capability a file
                      NAMES is a count of a SPELLING over a PINNED revision, which this entry already says
                      cannot go stale. A score is a fact about a run nobody has made; a spelling is a fact
                      about bytes already on disk, and only the first is unavailable here.
                      311 OF THE 1126 RUNS REGISTER A `securitypolicyviolation` LISTENER, AN
                      `onsecuritypolicyviolation` HANDLER OR CONSTRUCT `SecurityPolicyViolationEvent`; 322
                      name it at all. CSP §5.5 "Report a violation" is what fires it.
                      THIS SENTENCE USED TO END "and it is UNBUILT -- a NAMED RESIDUAL at
                      core/frame/policy_container.c", AND IT IS REWRITTEN RATHER THAN DELETED BECAUSE THE
                      ARGUMENT AROUND IT IS UNCHANGED AND A READER WHO RE-DERIVES IT WILL RE-ADD IT. §5.5
                      IS BUILT: core/frame/csp_violation.c composes §2.4.2's violation object and fires
                      §5.1's event, and the residual that sentence pointed at is retired. An absence
                      asserted after it has been filled is the one stale claim whose only reader is
                      somebody deciding whether to BUILD the thing -- so it argues for a second copy of a
                      component that already exists, which is why it is corrected here rather than left
                      for the first run to contradict. Derive it rather than trusting this line:
                          git grep -c securitypolicyviolation engine/host/browser/core/frame/csp_violation.c
                      answers nonzero, with an invented spelling beside it answering 0 as the control.
                      WHAT THE COUNT NOW MEANS IS DIFFERENT AND SMALLER, AND NO SCORE IS PREDICTED FOR IT
                      EITHER: better than a quarter of what these rows collect still rests on ONE
                      observable, but that observable now EXISTS and its red -- if any -- is per-file
                      fidelity against a built component rather than one component's name. Several of its
                      fields are themselves named residuals reading "" or 0, so a first run's failures may
                      still cluster; that is a prediction nobody has earned and this entry does not make.
                      AND THE SHAPE OUTLIVES THE COUNT, WHICH IS WHY IT IS SAID APART FROM IT: that event is
                      the IDIOM THIS CORPUS USES TO ASSERT THAT A BLOCK HAPPENED, so it is not confined to
                      the areas named for it. `script-src` carries 62 of those runs, `style-src` 41 and
                      `connect-src` 30, against 16 in `securitypolicyviolation` itself and 9 in
                      `reporting-api`. A reader expecting the absence to fall in a reporting area and leave
                      the directive tests alone has it backwards, and no per-file reading recovers it.
                      RE-DERIVE IT, AND READ THE CONTROLS BEFORE THE NUMBER: materialize the three rows'
                      blobs into a scratch root, classify with the command above, and count the files each
                      pattern matches. An INVENTED event name must answer 0; a bare `addEventListener(`
                      answers 369 files, so the 311 is 84% of every file in these rows that registers any
                      listener at all, which is what shows the matcher can see one.
                      RETIREMENT: these paragraphs go when a run of these rows has been scored, because the
                      count is then a statement about a run rather than about the corpus. */
                   "content-security-policy", "trusted-types",
                   /* AND THE ONE HELPER THOSE ROWS NAME THAT IS NOT UNDER EITHER OF THEM.
                      `reporting/resources/report-helper.js` is declared by TEN of the files above -- nine
                      under `content-security-policy/report-hash` and one under
                      `content-security-policy/reporting` -- seven of them as a `// META: script=`, which is
                      a fixture the DRIVER hands over and therefore a run this gate REFUSES rather than
                      measures when it is absent.
                      IT IS A `resources` ROW, WHICH IS EXACTLY THE SHAPE THAT MOVES THE STRAY CENSUS. Cone
                      mode materializes every directory ON THE PATH, so naming it puts `reporting`'s own
                      level on disk -- 40 blobs, 53073 bytes -- and TWENTY-FIVE of those are testharness
                      tests. Left unclaimed they are precisely the stray the census at the foot of this file
                      FAILS the gate for, and the WPT_OWN_LEVEL row below is part of this entry rather than a
                      separate thought. `reporting`'s ONLY subdirectory is `resources`, so the pair costs 60
                      blobs and 69395 bytes and nothing else comes with it.
                      LISTING `reporting` ITSELF WOULD BE THE SAME CHECKOUT TODAY AND A DIFFERENT CLAIM. A
                      WPT_PATHS entry claims a SUBTREE, so it would silently absorb any directory upstream
                      adds under `reporting` later; the pair below claims the level that is on disk and
                      nothing more, which is what this row is actually buying. */
                   "reporting/resources",
                   /* AND THE FIXTURE DIRECTORY THE TWO ROWS ABOVE DECLARE AND NOTHING COULD SEE THEM DECLARE.
                      `content-security-policy/media-src` names `/media/A4.webm` and `/media/sound_5.oga` from
                      four of its test documents, both EXIST at the pinned revision, and neither was in the
                      checkout — because the collection-time fixture resolution above reads a `<script src>`
                      element and a `// META: script=` line and NOTHING ELSE, which `docScriptFixtures` now
                      says in its own words and measures rather than merely warning about.
                      WHAT AN ABSENT ONE COSTS IS NOT A SMALLER NUMBER, IT IS AN INVERTED DIAGNOSIS.
                      `media-src-7_1.html` is a POSITIVE test — `media-src 'self'` over a same-origin
                      `/media/A4.webm`, which must LOAD — and its error path calls
                      `assert_unreached("Media error handler should be triggered for non-allowed domain.")`.
                      A 404 from an unchecked-out fixture therefore fires `onerror`, fails the test, and prints
                      a message saying CSP blocked an address CSP allowed. That is a FALSE VERDICT about the
                      component this row exists to judge, arriving through the gate rather than through the
                      engine.
                      IT IS NOT A PURCHASE FOR THOSE FOUR FILES. Re-derived over the checkout as it already
                      stood, FIFTEEN distinct `/media/…` paths that exist at the pinned revision were named by
                      files ALREADY collected, across `css`, `html`, `custom-elements`, `service-workers` and
                      `xhr` — a declared-fixture gap live in five areas that no run has ever reported, because
                      no channel looked at the construct that declares it.
                      IT COSTS 40 BLOBS AND 14737680 BYTES, and it is the largest row here by bytes. It holds
                      ZERO test files — every entry is `.webm`, `.mp4`, `.mp3`, `.oga`, `.wav`, `.vtt`,
                      `.png`, `.jpg` or `META.yml` — so it adds nothing to the stray census, and it is TOP
                      LEVEL and flat, so it drags no other directory's own level onto disk. Re-price and
                      re-check both with
                        git -C engine/.work/wpt ls-tree -r -l <rev> -- media | awk '{n++;b+=$4} END{print n,b}'
                        git -C engine/.work/wpt ls-tree -r --name-only <rev> -- media | grep -cE '[.](html|js)$'
                      THE WIDENING WAS PRICED IN FALSE ACCUSATIONS BEFORE IT LANDED, which is the half a
                      coverage-gaining change usually leaves unmeasured. Over the whole checkout, every
                      `/media/…` string that resolves to nothing at the pinned revision is either an
                      EXTENSION-LESS STEM a test completes at run time (`/media/counting`, `/media/movie_300`,
                      `/media/sound_5` — the `canPlayType` shape) or a run inside a COMMENT or a JS `import`
                      specifier (`/media/Projects` in a NIST URL, `/media/capture` in a mojom path). The
                      reader below sees a markup `src` attribute on a media element and none of those is one,
                      so the measured price of this row is ZERO new accusations.
                      NOTHING IS PREDICTED HERE ABOUT WHAT THESE FILES SCORE. Adding the fixture makes four
                      media-src documents SCORABLE; whether they pass is a fact about a run nobody has made. */
                   "media"];

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
  /* The 27 files at remote-context-helper-tests's own level, on disk because its own `resources` is now listed
     above for the one helper `weblocks-worker.https.window.js` names. It has no subdirectory but that
     `resources`, so this entry and that one together account for the whole directory — 26 of the 27 name
     `./resources/test-helper.js`, which the WPT_PATHS entry supplies. */
  "html/browsers/browsing-the-web/remote-context-helper-tests",
  /* `accessibility_properties_basic.tentative.html` and `idlharness.window.js`, on disk because
     `wai-aria/scripts` is listed for shadow-dom/reference-target. */
  "wai-aria",
  /* The standard's own `idlharness.https.any.js`, and the 247 test files at service-worker's own level — the
     largest population this gate had on disk and did not run. Its five subdirectories
     (ServiceWorkerGlobalScope/, multi-globals/, navigation-preload/, tentative/ and the listed resources/) are
     otherwise not on disk and are claimed by nothing here. */
  "service-workers", "service-workers/service-worker",
  /* The seventeen files at cache-storage's own level, on disk because its own `resources` is now listed above
     for the one helper `service-workers/idlharness.https.any.js` names. A row of its own rather than folding
     into `service-workers`, for that list's own reason: `service-workers`'s row is the standard's single
     idlharness file, and burying seventeen CacheStorage tests inside it is a number in which neither subject is
     visible. */
  "service-workers/cache-storage",
  /* The four test files at `html/canvas`'s own level, on disk because `html/canvas/element` and
     `html/canvas/resources` are listed in WPT_PATHS and cone mode materializes every directory ON THE PATH:
     one document (`canvas-css-random.html`) and three scripts (`color.window.js`, `historical.any.js`,
     `historical.window.js`). Its two `-crash.html` siblings are dropped at the NAME by `testKind`, before any
     content is read. Its unlisted subtrees are `offscreen/` and `tools/`, which that WPT_PATHS row explains
     and which stay absent.
     THESE FOUR ARE NOT A SMALLER VERSION OF THE SUBTREE, WHICH IS WHY THE LEVEL IS CLAIMED RATHER THAN LEFT
     TO THE CENSUS. `historical.*` asks which canvas members must NOT exist — its one assertion is
     `assert_equals(OffscreenCanvasRenderingContext2D.prototype.commit, undefined)` — which is the opposite
     question from everything under `element/`, and in this engine that name is absent outright, so the file
     reaches its assertion through a ReferenceError rather than through a member. */
  "html/canvas",
  /* The five test files at `html/semantics/forms`'s own level, on disk because the three
     `html/semantics/forms/*` rows are listed in WPT_PATHS and cone mode materializes every directory ON THE
     PATH: `beforeinput.tentative.html`, `historical-search-event.html`, `historical.html`,
     `input-change-event-properties.html` and `setCustomValidity-normalize-newlines.html`. Its two remaining
     own-level entries are META.yml and WEB_FEATURES.yml, which `nameIsNonTest` refuses.
     A ROW OF ITS OWN RATHER THAN FOLDING INTO `html/semantics`, because an own level is the files DIRECTLY
     in a directory and those are two different sets: that entry claims `interfaces.html` and
     `rellist-feature-detection.html`, and this one claims five different files one directory down. Cone mode
     put both on disk and only a path claims either. */
  "html/semantics/forms",
  /* The twenty-five files at `reporting`'s own level, on disk because `reporting/resources` is listed above --
     ten files under `content-security-policy` name `report-helper.js`, seven of them as a `// META: script=`,
     which is a fixture the driver hands over and so a run this gate refuses rather than measures when it is
     absent. `reporting`'s only subdirectory IS `resources`, so this entry claims everything the pair put on
     disk and there is no unlisted sibling left to be a decision for anybody.
     A ROW HERE RATHER THAN `reporting` IN WPT_PATHS, although the two are the same 60 blobs today: a
     WPT_PATHS entry claims the SUBTREE, so it would absorb a directory upstream adds later without anyone
     deciding to measure it, and this list exists to say what is measured. */
  "reporting"];

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
/* WHICH SCHEME A TEST IS LOADED OVER — `tools/manifest/item.py`'s `https` property, ported like every other
   rule here, and read by `tools/wptrunner/wptrunner/wpttest.py`'s `server_protocol` for exactly this purpose.
   WPT's own file-name documentation states it in one line: `.https` "Indicates that a test is loaded over
   HTTPS" (web-platform-tests.org/writing-tests/file-names.html, "Test Features"). The two `serviceworker`
   flags come with it because item.py's property is that same disjunction — a service worker registration
   requires a secure context, so those files are https whether or not anybody wrote the flag twice.
   THIS IS NOT A PRESENTATION DETAIL AND IT WAS BEING DROPPED. A test's scheme decides whether its realm is a
   SECURE CONTEXT, and Web IDL §3.3.7 [Exposed] step 2 — "If realm's settings object is not a secure context,
   and construct is conditionally exposed on [SecureContext], then return false" — DELETES every such member
   from a realm that is not one. Running a `.https.` file at an `http://` address therefore does not merely
   mislabel it: it hands the file a platform surface with a hole in it and reports the hole as the engine's.
   Measured on WebCryptoAPI, where 123 of 125 test documents are `.https.`: every one begins
   `var subtle = crypto.subtle`, `crypto.subtle` is `[SecureContext]` (Web Cryptography §10.2.1 "The subtle
   attribute"), and the family read 9246 failures of "cannot read property 'importKey' of undefined" — a number
   about HOW the run was configured, reported as a defect in WHAT ran, which is this gate's own recurring
   defect and the third time it has been this exact shape. */
function testIsHttps(rel) {
  const parts = rel.split("/");
  const meta = fileFlags(parts[parts.length - 1]).meta;
  return meta.includes("https") || meta.includes("serviceworker") || meta.includes("serviceworker-module");
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
   matched. This mirrors `codeOnly()`'s rule for JS: strip what is not code before reading it as code.
   THAT SENTENCE NAMED A FUNCTION THIS TREE DID NOT CONTAIN, in every revision that has carried it — `codeOnly`
   occurred EXACTLY ONCE in the whole repository, here, describing itself as the established rule this one
   mirrors. It is the stale-claim failure in its quietest form: not a crash sending a reader to build what
   exists, but prose asserting a convention, so a reader looking for the JS-side rule finds nothing and cannot
   tell whether they have missed it or it was never written. It is answered the way that class is answered —
   by BUILDING the named thing rather than deleting the sentence, because the rule the sentence states is
   correct and the scan below needs exactly it. */
/* ONE PATTERN FOR THE SCRIPT ELEMENT, READ TWO WAYS. `markupOnly` throws the body away and keeps the tag;
   `inlineScripts` keeps the body and throws the tag away. They are the same question about the same bytes, so
   they are the same pattern — two copies would be free to disagree about what a script element is, which is
   the defect this file spends its length avoiding elsewhere. */
const SCRIPT_EL = "(<(?:[A-Za-z][\\w.-]*:)?script\\b[^>]*>)([\\s\\S]*?)<\\/(?:[A-Za-z][\\w.-]*:)?script\\s*>";
function markupOnly(src) {
  return src.replace(/<!--[\s\S]*?-->/g, "").replace(new RegExp(SCRIPT_EL, "gi"), "$1");
}
/* THE BODIES OF A DOCUMENT'S INLINE SCRIPTS — what a document's JavaScript actually is, for a reader that wants
   to read it AS JavaScript. A document's `idl_test(...)` call lives in one of these and nowhere else, and
   handing the whole file to a JS reader instead would have it tokenize markup: an unbalanced apostrophe in
   prose (`Ms2ger's`) opens a string literal that swallows the rest of the file. */
function inlineScripts(src) {
  return [...src.replace(/<!--[\s\S]*?-->/g, "").matchAll(new RegExp(SCRIPT_EL, "gi"))].map((m) => m[2]);
}
/* WHAT IS CODE IN A JAVASCRIPT FILE — the rule `markupOnly`'s comment above has claimed for three revisions.
 *
 * COMMENTS COME OUT AND STRING LITERALS STAY, and that asymmetry is the whole specification of this function
 * rather than an implementation detail of it. A caller reads this to find a CALL and the ARGUMENTS of that
 * call, and in `idl_test(['cssom'], ['dom'])` the arguments ARE string literals — so a stripper that blanked
 * them would delete the answer along with the noise. A comment, by contrast, can spell any call at all and
 * means none of them.
 *
 * IT IS A SCANNER AND NOT A CHAIN OF `replace`s, because the two constructs are mutually quoting: a STRING can
 * contain comment syntax (`'// not a comment'`) and a COMMENT can contain quote syntax (a block comment whose
 * text is `"not a string"`), so whichever regex runs first corrupts the other's input. One left-to-right pass
 * has no such order to get wrong.
 *
 * A `/` IS DIVISION UNLESS THE PRECEDING SIGNIFICANT CHARACTER SAYS OTHERWISE — the standard heuristic, and it
 * is used here for exactly one purpose: to avoid reading the inside of a regexp literal as code. Getting it
 * wrong cannot fabricate a call, because the fallback is to treat the text AS CODE, which is what a reader
 * that skipped this function entirely would have done. Every ambiguity therefore resolves toward the answer
 * this function was introduced to improve on, never past it. */
function codeOnly(src) {
  let out = "", i = 0;
  /* THE LAST CHARACTER OF CODE EMITTED, which is what decides the `/` above. Kept as we go rather than
     re-derived by scanning backwards, so its cost does not depend on how far the last token was. */
  let prev = "";
  const REGEXP_MAY_FOLLOW = "(,=:[!&|?{};+-*%~^<>";
  while (i < src.length) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "/") {                       /* a line comment: to the newline */
      while (i < src.length && src[i] !== "\n") i++;
      out += " ";
      continue;
    }
    if (c === "/" && src[i + 1] === "*") {                       /* a block comment: to its close */
      const end = src.indexOf("*/", i + 2);
      i = end < 0 ? src.length : end + 2;
      out += " ";
      continue;
    }
    if (c === '"' || c === "'" || c === "`") {                   /* a string: KEPT, escapes honoured */
      const q = c;
      let j = i + 1;
      for (; j < src.length; j++) {
        if (src[j] === "\\") { j++; continue; }
        if (src[j] === q) break;
      }
      out += src.slice(i, Math.min(j + 1, src.length));
      prev = q;
      i = j + 1;
      continue;
    }
    if (c === "/" && (prev === "" || REGEXP_MAY_FOLLOW.includes(prev))) {   /* a regexp literal: skipped whole */
      let j = i + 1, cls = false;
      for (; j < src.length && src[j] !== "\n"; j++) {
        if (src[j] === "\\") { j++; continue; }
        if (src[j] === "[") cls = true;
        else if (src[j] === "]") cls = false;
        else if (src[j] === "/" && !cls) break;
      }
      /* AN UNTERMINATED ONE WAS NEVER A REGEXP. A `/` that reaches the end of its line with no closing slash is
         division after all, and the heuristic simply guessed wrong; emitting the `/` as code and carrying on is
         the reading that cannot lose a call. */
      if (j >= src.length || src[j] === "\n") { out += c; prev = c; i++; continue; }
      out += " ";
      prev = "/";
      i = j + 1;
      continue;
    }
    out += c;
    if (!/\s/.test(c)) prev = c;
    i++;
  }
  return out;
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
/* LEXBOR, BUILT NATIVELY. Streams §5.4 gives every writable controller a real AbortSignal, which is an
   EventTarget, which reaches the solver's decision hook and through it the scheduler — and the scheduler's COW
   covers the DOM, so the gate needs the same tree the shipped build has. The .a is committed to nothing and
   built from the SOURCE tracked at engine/lexbor, which is no longer fetched.
   AND IT IS ASKED FOR RATHER THAN CACHED HERE. This file used to hold the cmake recipe and the identity check
   that guards it, and build.mjs's native arm held a `existsSync` over a hardcoded copy of the same path — two
   consumers of one artifact, one of which could hand its linker an archive compiled from a different source.
   `engine/lexbor_source.mjs` owns both now and carries the account of what that cost; what belongs in THIS file
   is only that the gate needs the archive, which is the line below. */
const LEXBOR_SRC = join(ENGINE, "lexbor");
const LEXBOR_LIB = lexborNativeArchive(ENGINE, "wpt");

const cc = spawnSync("clang", ["-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow", "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer", "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration", "-DNDEBUG", "-D_GNU_SOURCE", '-DCONFIG_VERSION="wpt"',
  "-DAPICLIENT_DEV=1", "-DENABLE_DUMPS",
  "-I" + join(ENGINE, "qjs"), "-I" + join(ENGINE, "host"), "-I" + join(ENGINE, "host", "browser"),
  "-I" + join(LEXBOR_SRC, "source"),
  ...SRCS, LEXBOR_LIB, "-o", bin, "-lm", "-lpthread"], { encoding: "utf8" });
if (cc.status !== 0) { console.error("[wpt] runner build FAILED\n" + (cc.stderr || "")); process.exit(1); }


/* WHAT AN ABORT *IS*, WHICH THIS COLUMN USED TO REFUSE TO SAY.
 *
 * `aborted` counted three unrelated diagnoses under one number and a reader had to hand-cluster the log to
 * separate them: `html/browsers` reported `aborted-runs 25`, and 21 of those were DCHECKs naming capabilities to
 * build, 2 were a META helper the CHECKOUT does not have, and 2 were the CPU budget doing its job. Those ask for
 * work in three different files from three different people, and the sum is what two runs get compared on — so
 * "the abort column did not move" was read as "the same aborts fired", which it does not mean and cannot. It is
 * the identical defect this file already fixed for `errored` and for the CPU-vs-wall kill, stopping one step
 * short: the kill was given its own SIGNAL and then reported in the same COLUMN as the thing it is not.
 *
 * THE KIND IS A FACT THIS DRIVER HOLDS, NOT A PATTERN OVER ITS OWN OUTPUT. It knows whether IT installed the
 * limit that fired (SIGXCPU is the kernel naming this driver's own rlimit), whether ITS kill timer fired (node
 * reports `ETIMEDOUT` on the result — see the cascade for why that is not the same question as "was the signal
 * SIGTERM"), whether IT could resolve a META script, and whether the corpus HAS the file at the pinned
 * revision. Classifying by regex over an abort MESSAGE would be a recognizer that goes stale the day someone
 * rewords an assert — per-spelling plumbing, with every omission a silent reclassification.
 * ONE EXCEPTION, NAMED RATHER THAN HIDDEN: `gap`, `fatal` and the two `nodone`/`driver` kinds are read from the
 * CHILD's output, because a subprocess has no other channel. What is read there is a PROTOCOL MARKER this
 * project owns at both ends — check.h's `@WHY` and `@E`, the runner's `@WPTDONE`/`@WPTERR` — never the prose
 * inside it. The message text is DISPLAYED and never classified on, which is the whole difference: a reworded
 * assert changes what the reader sees and cannot change which column it lands in.
 *
 * CHECK.H OWNS TWO MARKERS AND THIS DRIVER READ ONE, WHICH IS HOW AN ABORT THAT NAMED A CAPABILITY IN FULL CAME
 * TO BE REPORTED AS ONE THAT "named NOTHING". `DCHECK`/`DFAIL`/`DCHECKF`/`DFAILF` emit `@WHY`; `CHECK`/
 * `CHECK_FAIL`/`CHECKF`/`CHECK_FAILF` emit `@E` — the SAME machine-readable `{"reason":…}` payload, and
 * quickjs-check.h emits the same pair in its plain-text shape. Matching only `@WHY` sent every always-fatal
 * assert down the final `else` and printed "died on SIGABRT and named NOTHING — no @WHY", which is a FALSE
 * statement about a child that had just written its reason on stdout, and it filed the work under "debug the
 * frame" instead of the work queue. MEASURED: all three of `domparsing`'s `crash` aborts were ONE `@E` at
 * core/xml/xml_document.c naming XML 1.0 §2.8's [28] doctypedecl and the DTD subsystem it asks for, so that
 * area's `crash` column was 3 and its true value is 0.
 * THEY STAY TWO COLUMNS RATHER THAN BECOMING ONE, because the two macros are a DECISION this project makes per
 * site and the columns are what make that decision legible: a `@WHY` is compiled out of a release build, so the
 * capability it names is missing only in dev, while an `@E` is fatal in RELEASE TOO — it is the site saying we
 * must not PROCEED even in production. Folding them would put three states behind one answer, which is the
 * defect this file's own `notrun` split exists to refuse.
 *
 * A KIND THIS DRIVER CANNOT NAME IS `UNKNOWN` AND IS LOUD. It is never folded into the largest bucket, because a
 * fourth kind arriving later would then land silently in the number that drives attention — the defaulted-field
 * defect performed in the instrument built to end it. The split is ASSERTED to sum to the count it decomposes
 * (see `abortSplit`), so a site that forgets to name its kind breaks the gate instead of shrinking a column. */
/* THE TEXT IS ONE LINE PER KIND, ON PURPOSE. It is printed as a table and a table whose rows wrap is a table
   nobody reads across; the argument for each kind is the paragraph above, where it can be as long as it needs. */
const ABORT_KINDS = {
  gap:     "the ENGINE named a capability it lacks (@WHY) — THE WORK QUEUE.        build it at the root",
  fatal:   "the ENGINE named an invariant it must not PROCEED past (@E).           build it; it is fatal in RELEASE too",
  crash:   "the ENGINE died on an unasked-for signal, naming NOTHING.              debug the frame",
  corpus:  "THE CORPUS could not present the test as written.                      each line says which; fix here",
  killed:  "a RESOURCE BACKSTOP fired (CPU rlimit / wall) — a cost, not a gap.     read the CPU on the line",
  nodone:  "the file RAN and testharness never completed.                          the line names the cause",
  driver:  "THIS GATE's own contract broke (report line, or the spawn).            fix engine/wpt.mjs",
  UNKNOWN: "NO fact this driver holds classifies it — a kind it does not model.    model it before reading it",
};
/* THE DECOMPOSITION, AND THE ASSERT THAT MAKES OMITTING A ZERO SAFE. A kind absent from the bracket is zero and
   PROVABLY zero, because the printed parts are checked to sum to the total they sit beside — which is also what
   catches an abort site that increments the total and names no kind. */
function abortSplit(m, n) {
  const sum = [...m.values()].reduce((a, b) => a + b, 0);
  if (sum !== n)
    throw new Error(`[wpt] ${n} abort(s) counted and ${sum} classified — an abort site incremented the total ` +
                    "without naming its kind, so this split is not a decomposition of anything");
  const parts = Object.keys(ABORT_KINDS).filter((k) => m.get(k)).map((k) => `${k} ${m.get(k)}`);
  return parts.length ? `  [${parts.join("  ")}]` : "";
}

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
  if (!a) areas.set(p, (a = { name: p, expected: 0, done: 0, runs: 0, pass: 0, fail: 0, notrun: 0, aborted: 0,
                              unread: 0, errored: 0, abPass: 0, abFail: 0, abNotrun: 0, abFiles: 0,
                              /* PER AREA, for the reason every other column is: an area whose aborts are all
                                 capability gaps and an area whose aborts are all missing helpers are the same
                                 number in a total, and they are not the same work. */
                              abKind: new Map(), lines: [] }));
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
              `  fail ${String(a.fail).padStart(7)}  notrun ${String(a.notrun).padStart(6)}` +
              `  aborted ${String(a.aborted).padStart(3)}` +
              `  errored ${String(a.errored).padStart(3)}  unread ${String(a.unread).padStart(3)}` +
              /* TRAILING, so the fixed columns stay aligned down the table and the ragged part is the part a
                 reader scans rather than reads across. */
              abortSplit(a.abKind, a.aborted));
  /* WHICH PART OF THE ROW ABOVE IS WORK THAT DID NOT FINISH — see the accumulation for the incident. Printed
     only when there IS such a part, because a row where every counted subtest came from a file that ran to
     completion is a row with nothing extra to say, and a line that reads `0` every time is a line nobody reads
     the one time it does not. */
  /* AND `notrun` IS DECOMPOSED HERE TOO, because the column above was added without it and that left the new
     number as the one thing on the row this line could not account for — which is the defect the column was
     added to remove, re-created one line below it. A file that ABORTS strands every subtest it had not reached,
     so an aborted file is a PRODUCER of NOTRUN rather than an unrelated neighbour of one: leaving it out meant
     an area could read `notrun 40` with nothing saying whether that was forty questions a component declined
     or one DCHECK firing early in a file that had forty.
     THE GUARD COUNTS IT AS WELL. It was `abFail || abPass`, so a file that aborted having produced ONLY
     stranded subtests printed no decomposition at all — the one case where the reader most needs the line, and
     silence exactly where the ABORT is the whole story. */
  if (a.abFail || a.abPass || a.abNotrun)
    console.log(`  ${" ".repeat(AREA_W)}    └─ of which ${a.abFail} fail / ${a.abPass} pass / ` +
                `${a.abNotrun} notrun came from ${a.abFiles} file(s) that ABORTED — counted, but not a ` +
                "finished measurement of anything");
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
/* WPTSERVE'S OWN ACCESS LOG, AND THIS LINE IS WHAT TURNS IT ON. engine/wptserve.py attaches a handler to the
   root logger when `WPT_ACCESS_LOG` names a file and does nothing when it does not — and NOTHING SET IT, so the
   instrument was built, landed, described in its own commit message, and inert. That is the mirror of the
   defaulted-field defect §Offensive-programming names: not a reader with no writer but a READER WITH NO CALLER,
   and it is the quieter direction, because an unset environment variable produces no diagnostic anywhere.
   IT IS A SECOND FILE, NOT THIS ONE, and that is a requirement rather than tidiness: SERVER_LOG is the server's
   stdout AND stderr, and it is the newline-anchored channel the READY promise above parses. A per-request line
   written into it is a second author of the file this driver polls for one pattern.
   WHY THE DRIVER WANTS IT AT ALL: a `fetch()` that never settles is THREE bugs wearing one TIMEOUT — the engine
   never ISSUED the request, it issued one the server did not answer, or the server answered and the reply
   reached no parked flow. The verdict, the subtest count and the message are identical in all three, so a search
   cannot be directed at any of them. The server is the only party that knows which, and this is it saying so. */
const ACCESS_LOG = join(dirname(bin), "wptserve.access.log");
writeFileSync(ACCESS_LOG, "");
const server = spawn("python3", [join(ENGINE, "wptserve.py"), WPT, "0"],
                     { stdio: ["ignore", serverLogFd, serverLogFd],
                       env: { ...process.env, WPT_ACCESS_LOG: ACCESS_LOG } });
/* THE READY LINE CARRIES TWO FACTS AND THIS DRIVER RELAYS BOTH. Where a client CONNECTS is a loopback socket;
   what the corpus is SERVED AS is `{{host}}:{{ports[http][0]}}` — the authority every `.sub` document asserts
   its own URLs against, which wptserve substitutes out of its own config. The runner used to compose the second
   one itself, as a literal `http://web-platform.test` with no port, so every `.sub` test asserting a RESOLVED
   URL compared the real authority against a port-less one and reported the ENGINE as wrong (32 failures in
   domparsing/innerhtml-mxss.sub.html alone). The port is EPHEMERAL — reserved by the server per run — so it
   cannot be a literal anywhere and the server is the only thing that can state it.
   THE NEWLINE IS PART OF THE MATCH: this reads a file another process is appending to, and a pattern that can
   match a half-written line would resolve with the port and no origin. */
/* THE ONE EXIT LISTENER, INSTALLED BEFORE ANYTHING WAITS ON IT AND LIVE FOR THE WHOLE RUN.
   IT USED TO BE REGISTERED INSIDE THE READY PROMISE'S EXECUTOR, WHICH MADE IT INERT THE MOMENT THE PROMISE
   SETTLED — a listener that calls `reject` on a settled promise is a no-op, so a wptserve that died at any
   point after READY was a death NOTHING IN THIS PROCESS OBSERVED. That is not a small hole and it is not
   hypothetical: this driver's last whole-corpus run had wptserve stop answering part-way through `shadow-dom`
   and then spawned 935 more children against a dead socket — storage, streams, url, wai-aria, webidl,
   webmessaging and xhr all read 100 % aborted, seven areas whose engine the same binary passes. The run did not
   end; it stopped MEASURING and kept REPORTING, which is §Testing's own defect at corpus scale.
   IT RECORDS AND DECIDES NOTHING, for the reason the quantum's signal handler decides nothing: the exit is an
   asynchronous fact and the verdict is a question somebody asks later, at a site that knows what it was doing
   when the answer came back. */
let g_serverExit = null;
server.on("exit", (code, signal) => { g_serverExit = { code, signal }; });
const ready = await new Promise((resolve, reject) => {
  const fail = setTimeout(() => reject(new Error("wptserve did not report READY within 60s")), 60_000);
  const poll = setInterval(() => {
    /* ASKED OF THE ONE RECORD ABOVE rather than of a second listener: two listeners for one event is two
       places for this file to disagree with itself about whether the server is up. */
    if (g_serverExit) {
      clearTimeout(fail); clearInterval(poll);
      reject(new Error(`wptserve exited (${serverEnded()}) before it reported READY`));
      return;
    }
    /* TWO PAIRS, one per scheme, each (dial, served-as) — see engine/wptserve.py's READY line for why the
       scheme is a second pair rather than a second field, and `testIsHttps` below for who chooses between
       them. A pattern that matched only the http pair would resolve the instant the line was written and
       silently leave every `.https.` test at an http address, which is the state this pair replaces. */
    const m = /READY (\d+) (\S+) (\d+) (\S+)\n/.exec(readFileSync(SERVER_LOG, "utf8"));
    if (m) {
      clearTimeout(fail); clearInterval(poll);
      resolve({ http:  { addr: "127.0.0.1:" + m[1], origin: m[2] },
                https: { addr: "127.0.0.1:" + m[3], origin: m[4] } });
    }
  }, 50);
}).catch((e) => { console.error("[wpt] " + e.message); process.exit(1); });
/* THE HTTP PAIR STAYS THIS DRIVER'S OWN, and that is a statement about who is speaking rather than a default.
   Everything below that uses these two names is the DRIVER talking to the server on its own account — the
   liveness probe, and the `.sub` substitution fetch that turns a template into the bytes a test is written
   against — and neither of those is a document with an address. Which origin a TEST is hosted at is a
   different question, asked per test at the spawn. */
const serverAddr = ready.http.addr, serverOrigin = ready.http.origin;
const serverHttps = ready.https;
console.log(`[wpt] wptserve on ${serverAddr}, serving the corpus as ${serverOrigin} and — over cleartext, ` +
            `which engine/wptserve.py names as its one fiction — as ${serverHttps.origin} on ` +
            `${serverHttps.addr}; its log: ${SERVER_LOG}`);
process.on("exit", () => server.kill());

function serverEnded() {
  if (!g_serverExit) return "still running";
  return g_serverExit.signal ? `killed by ${g_serverExit.signal}` : `exit code ${g_serverExit.code}`;
}

/* WHAT THE CORPUS SERVER WAS ASKED FOR WHILE ONE FILE RAN, AND WHAT IT ANSWERED.
 *
 * THE WINDOW IS THE CHILD'S OWN LIFETIME AND THAT IS EXACT, NOT APPROXIMATE: the run loop drives one test at a
 * time through `spawnSync`, which blocks this process until the child is reaped, so the bytes this log gains
 * between the call's two ends are that child's traffic and nobody else's. The one straggler a reader should know
 * about is a response line the server's own thread writes after the child has already exited; it lands in the
 * NEXT file's window, so a slice is a statement about a file's requests and never a proof that it made no other.
 *
 * IT IS READ BY BYTE OFFSET rather than by re-reading the file. A whole-corpus run makes millions of requests,
 * and `readFileSync(...).slice(seen)` per test is quadratic in the log — an instrument whose cost grows with the
 * thing it measures is one that gets switched off, which is how the last one came to be inert.
 *
 * THE TWO LINE SHAPES ARE WPTSERVE'S OWN, from tools/wptserve/wptserve/server.py: a REQUEST logs
 * `f"{request.method} {request.request_path}"` when it arrives, and a RESPONSE logs
 * `"%i %s %s (%s) %i" % (status, method, request_path, referer, length)` when it is answered. Both are prefixed
 * by engine/wptserve.py's `%(asctime)s`, which is two whitespace-separated tokens. The discriminator between
 * them is the first token after that prefix — a STATUS is digits and a METHOD is not — and never a count of
 * fields, because a referer with a space in it would move the others.
 * PARSED, NEVER CLASSIFIED ON. What comes out is a method, a path and a status: facts the server stated. No
 * verdict below reads the prose of any other line this log carries. */
const accessFd = openSync(ACCESS_LOG, "r");
let g_accessSeen = 0;
function corpusTrafficSince() {
  const size = statSync(ACCESS_LOG).size;
  /* A LOG THAT SHRANK IS NOT A LOG THIS DRIVER MAY KEEP READING. It is written by one process this driver
     started and truncated by nothing, so a smaller size than the offset already consumed means something else
     is writing it — and continuing would read one file's bytes as another's. It says so and stops measuring
     traffic rather than reporting a slice it cannot vouch for. */
  if (size < g_accessSeen) { g_accessSeen = size; return null; }
  if (size === g_accessSeen) return { asked: [], answered: [] };
  const buf = Buffer.allocUnsafe(size - g_accessSeen);
  const got = readSync(accessFd, buf, 0, buf.length, g_accessSeen);
  g_accessSeen += got;
  const asked = [], answered = [];
  for (const line of buf.toString("utf8", 0, got).split("\n")) {
    const m = /^\S+ \S+ (\S+) (\S+)(?: |$)/.exec(line);
    if (!m) continue;
    /* THE PATH MUST BE SERVER-ROOTED, and that clause is doing real work rather than restating the shape. This
       log carries wptserve's whole DEBUG stream, not only its access lines — `Route pattern: …`, `Found handler
       FileHandler`, `Starting http server on …` — and an all-caps first word in some future message would
       otherwise be read as a METHOD and its next word as a path, minting a request nobody made. A request_path
       always begins with `/`; a log sentence's second word almost never does. */
    if (/^[0-9]{3}$/.test(m[1])) {
      const r = /^\S+ \S+ ([0-9]{3}) (\S+) (\/\S*) /.exec(line);
      if (r) answered.push({ status: Number(r[1]), method: r[2], path: r[3] });
    } else if (/^[A-Z]+$/.test(m[1]) && m[2].startsWith("/")) {
      asked.push({ method: m[1], path: m[2] });
    }
  }
  return { asked, answered };
}

/* AND THE SAME QUESTION ASKED OF THE WHOLE RUN, WHICH IS THE ONLY WAY IT REACHES A FILE THAT PASSED.
 *
 * `trafficEvidence` below is EVIDENCE ON A VERDICT ALREADY REACHED, and it says so in its own banner: it is
 * printed under a file that failed to complete and never under one that finished. That is right for a verdict
 * and it leaves a hole exactly the shape of the one this gate exists to close. A test can ask the corpus server
 * for a fixture, be REFUSED, and go on to report a SMALL HONEST PASS — and its 404 is parsed by this driver,
 * read once, and dropped. §Testing names that shape by its cost: a test that is collected, runs, and reports is
 * still not a test that MEASURED anything, and the number it returns is the concealment.
 *
 * THE THREE STATIC CHECKS IN THE RUN LOOP COVER THE FIXTURES A TEST DECLARES — a META script the driver hands
 * over, an `.idl` the harness fetches, a `<script src>` a document names. Every OTHER way a test names a
 * resource is invisible to all three: a `fetch` of a `.py` handler, an `<iframe src>`, a worker script, an
 * `importScripts`. Adding a fourth kind, and a fifth, would be a list that only ever describes the last one
 * somebody hit — the shape this file already rejected for the runner's source list and for its type rules.
 *
 * SO IT IS NOT ENUMERATED, IT IS OBSERVED. The run's own traffic already states, in the SERVER'S own words,
 * every path a test asked for and what it was answered. Kept rather than dropped, that is a census of what this
 * checkout could not supply, derived from the run instead of from a table of ways to spell a reference — the
 * same reason `metaScripts` reads `// META: script=` out of the file rather than guessing at it, and the same
 * reason `idlFixtures` reads the source rather than the filename.
 *
 * ANSWERED LINES ONLY, BECAUSE A STATUS IS A FACT THE SERVER STATED. A path that was ASKED and never answered
 * is a hung socket; the per-file evidence reports that where the reader is standing when they need it, and
 * run-wide it cannot be told apart from a reply line written after this driver's last read.
 *
 * IT RUNS AT THE FREQUENCY OF WHAT IT OBSERVES: one map update per response line, on lines this driver has
 * already parsed for the block below. Nothing here spawns a process — git is asked once per surviving path, at
 * the foot of the run, where a corpus run has already spent an hour. */
const g_fixture = new Map();
let g_trafficUnvouched = 0, g_fixtureUndecodable = 0;
function corpusFixtureLedger(rel, traffic) {
  /* AN ABSENT MEASUREMENT IS COUNTED AS ONE. `null` is the log declining to vouch for this file's slice, and
     folding that into "this file asked for nothing" is the averaging §Testing forbids. The census says how many
     runs it could not see, rather than printing a total that quietly excludes them. */
  if (!traffic) { g_trafficUnvouched++; return; }
  for (const a of traffic.answered) {
    /* THE PATH AS THE CORPUS FILES IT — query and fragment stripped, for the reason `trafficEvidence` gives one
       block down: a query is a HANDLER'S argument and never part of a filename. */
    let file = a.path.split("?")[0].split("#")[0].replace(/^\/+/, "");
    if (file.includes("%")) {
      /* A REQUEST PATH IS THE TEST'S BYTES AND NOT THIS DRIVER'S, so a malformed escape is FOREIGN INPUT: the
         answer is to refuse it, never to judge a filename this driver invented by guessing past it. Counted, so
         the census reports what it could not decide instead of reading clean about it. */
      try { file = decodeURIComponent(file); } catch { g_fixtureUndecodable++; continue; }
    }
    if (!file) continue;
    let e = g_fixture.get(file);
    if (!e) g_fixture.set(file, e = { n: 0, statuses: new Set(), by: rel });
    e.n++;
    e.statuses.add(a.status);
  }
}

/* DOES THE PINNED CORPUS HAVE THIS PATH — the question that separates "this checkout is short an entry" from
   "the corpus does not contain it at all", and it has TWO callers now, so it is one function rather than two
   copies of an `ls-tree` invocation that must agree about which revision they ask at.
   `ls-tree` walks TREE objects only, so it costs milliseconds and — unlike `cat-file -e` — cannot trip the lazy
   blob fetch this `--filter=blob:none` clone would otherwise make over the network to answer a question about a
   path. Its exit status is 0 for present and absent alike, so the answer is the OUTPUT; a non-zero status is git
   DECLINING to answer and is reported as that third thing, never as a confident "the corpus does not have it".
   ASKED AT THE REVISION THIS RUN'S NUMBERS BELONG TO — CORPUS_AT_START's head, not the WPT_REV constant: those
   two agreeing is what the provisioning instructions ask for and not something this may assume. */
function corpusHas(p) {
  const revOf = /^[0-9a-f]{40}$/.test(CORPUS_AT_START.head) ? CORPUS_AT_START.head : null;
  if (revOf === null)
    return { known: false, at: null,
             why: `this driver could not ask git whether the corpus HAS it: the corpus identity read as ` +
                  JSON.stringify(CORPUS_AT_START.head) };
  const at = revOf.slice(0, 10);
  const ls = spawnSync("git", ["ls-tree", revOf, "--", p], { cwd: WPT, encoding: "utf8" });
  if (ls.status !== 0)
    return { known: false, at, why: `git declined to say whether the corpus has it: ${(ls.stderr || "").trim()}` };
  return { known: true, at, present: Boolean(ls.stdout.trim()) };
}

/* THE SENTENCE A TIMEOUT COULD NOT WRITE. A file whose harness never completed is asked here what it actually
 * put on the wire, and the answer splits the three states that used to share one verdict:
 *
 *   NO REQUEST for the thing it awaits  — the ENGINE never issued it. The gap is upstream of the network.
 *   A REQUEST and no reply line         — the server took it and did not answer; the run is blocked on a socket.
 *   A REPLY, and the flow still parked  — the bytes came back and reached no parked flow. That is the delivery
 *                                         seam, and it is the one state a reader would otherwise never suspect,
 *                                         because everything visible about it says "slow test".
 *
 * A NON-2xx IS DIAGNOSED AND NOT MERELY COUNTED, and the diagnosis is git's: a path the pinned corpus HAS is a
 * checkout this gate can fix and the message names the WPT_PATHS entry, while a path it does not have cannot be
 * supplied by any entry. That is the same split the META-script report makes, through the same `corpusHas`.
 * IT IS EVIDENCE ON A VERDICT ALREADY REACHED, NEVER A VERDICT OF ITS OWN. Plenty of WPT tests ask for a 404 on
 * purpose — a status handler's whole job is to return one — so a non-2xx is not by itself a defect and this is
 * never consulted for a file that finished. Printed only under a file that already failed to complete, it costs
 * no false positive and it is exactly where the reader is standing when they need it. */
function trafficEvidence(traffic) {
  if (!traffic)
    return "\n         corpus traffic: NOT MEASURED for this file — the access log moved under this driver, so " +
           "the slice could not be vouched for (this is an absent measurement, not an absence of requests)";
  if (!traffic.asked.length && !traffic.answered.length)
    return "\n         corpus traffic: the server logged NO request at all while this file ran, so whatever it " +
           "awaits was never ISSUED — the gap is upstream of the network, not in the reply path";
  const answeredFor = new Map();
  for (const a of traffic.answered) {
    if (!answeredFor.has(a.path)) answeredFor.set(a.path, []);
    answeredFor.get(a.path).push(a.status);
  }
  const parts = [`\n         corpus traffic: ${traffic.asked.length} request(s), ${traffic.answered.length} ` +
                 "answered — see below for the ones that are not an ordinary 200"];
  const unanswered = [...new Set(traffic.asked.map((a) => a.path))].filter((p) => !answeredFor.has(p));
  for (const p of unanswered.slice(0, 8))
    parts.push(`         ASKED AND NEVER ANSWERED: ${p} — the server took the request and wrote no reply line, ` +
               "so this run is blocked on that socket");
  const bad = [...answeredFor.entries()].filter(([, ss]) => ss.every((s) => s < 200 || s >= 300));
  for (const [p, ss] of bad.slice(0, 8)) {
    /* THE PATH AS THE CORPUS FILES IT. A request path is server-rooted and may carry a query, which is a
       HANDLER'S argument and never part of a filename — asking git about `status.py?code=404` would answer
       "absent" about a file that is present and turn a test's own subject into a checkout gap. */
    const file = p.split("?")[0].replace(/^\/+/, "");
    /* ON DISK IS ASKED FIRST, AND SKIPPING IT WOULD MAKE THIS LINE CONFIDENTLY WRONG. `corpusHas` answers
       whether the PINNED REVISION contains the path, which is not the same question as whether this checkout is
       missing it — and a `.py` handler is the case that separates them: `fetch/api/resources/status.py?code=404`
       is CHECKED OUT and answers 404 because returning one is its entire job. Measured against the live server
       while writing this. Reporting that as "WPT_PATHS is short of fetch/api/resources" would take a test's own
       subject and file it as this gate's checkout gap — a confident wrong diagnosis, which is worse than the
       silence it replaces. Present on disk means the status is the SERVER'S OWN ANSWER and nothing here. */
    const onDisk = existsSync(join(WPT, file));
    const has = onDisk ? null : corpusHas(file);
    parts.push(`         ${ss.join(",")} for ${p} — ` +
               (onDisk
                  ? `${file} IS in this checkout, so this status is the server's own answer to that request ` +
                    "(a handler's, very likely) and not a fixture this gate is failing to supply"
                : !has.known ? has.why
                : has.present
                  ? `${file} EXISTS at ${has.at} and this checkout does not have it: WPT_PATHS is short of ` +
                    `${dirname(file)}, and THIS IS A FIXTURE THE TEST NEEDS, not a result about the engine`
                  : `${file} does NOT EXIST at ${has.at}, so this status may well be what the test is asking ` +
                    "for and no WPT_PATHS entry can change it"));
  }
  return parts.join("\n");
}

/* WHY A `fetch` DID NOT HAPPEN, IN THE MOST SPECIFIC TERMS THE ERROR CARRIES. node's fetch rejects with a bare
   `TypeError: fetch failed` and puts every fact in `cause`, so reading `e.message` first prints that string and
   nothing else — measured: a refused connection and a rejected port number are both "fetch failed" at the top
   level and are `ECONNREFUSED` and "bad port" one level down. The order is code, then the cause's own message,
   then the top one; each step is a strictly less specific answer and the last is only reached when there is no
   cause at all, so this chain never stands in for a fact the error was carrying. */
function transportReason(e) {
  if (e && e.cause && e.cause.code) return String(e.cause.code);
  if (e && e.cause && e.cause.message) return String(e.cause.message);
  return String((e && e.message) || e);
}

/* IS THE CORPUS SERVER STILL SERVING? — ASKED OF THE SERVER, NEVER INFERRED FROM THE SYMPTOM.
 *
 * A run that could not get its own file off the corpus server is TWO facts wearing one shape, and the whole
 * value of this gate rests on telling them apart. Either the CORPUS cannot supply this file — a support path
 * the sparse checkout does not have, a handler that faults, a `.sub` template wptserve refuses — which is one
 * honest `corpus` row and the run carries on; or the SERVER IS GONE, in which case this row and every row after
 * it is about the server and nothing about the engine was measured by any of them. Guessing between them from
 * the text of the symptom is the recognizer shape: it goes stale, and its failure mode is the expensive one —
 * 935 rows of a confident wrong answer.
 *
 * SO THE DISCRIMINATOR IS A REQUEST. Any HTTP response at all — 200, 404, 500 — proves the server is accepting
 * connections and answering, which is the only property in question; the STATUS is not read, because a server
 * that 404s the corpus root is still a server. Three outcomes and all three are written out, because a driver
 * that cannot distinguish "the process exited" from "the port refuses" from "it accepted and said nothing"
 * hands the reader one number for three different pieces of work — the collapsed verdict §Testing refuses.
 *
 * AND THE `await` IS WHAT MAKES THE EXIT LISTENER ABOVE RELIABLE. This run is a sequence of `spawnSync` calls,
 * each of which blocks node's event loop for the whole of a test, so an `exit` event can sit undelivered in
 * libuv for minutes; awaiting the probe is a turn of the loop, so a death that has already happened is recorded
 * by the time the probe answers. The listener is what NAMES the death; the probe is what makes it arrive.
 *
 * ITS DEADLINE IS WALL TIME AND THAT IS CORRECT HERE, which is worth saying because §Testing bans a wall clock
 * as a verdict about the ENGINE. This measures neither the engine nor a test: it measures whether a socket on
 * loopback answers, and "it did not answer for twenty seconds" is the whole of the fact being reported. It is
 * also never the last word on a test — the run it interrupts already has its own verdict. */
const SERVER_PROBE_MS = 20_000;
async function serverHealth() {
  let answered = false, transport = null;
  try {
    const r = await fetch("http://" + serverAddr + "/", { signal: AbortSignal.timeout(SERVER_PROBE_MS) });
    /* THE BODY IS READ AND DROPPED so the connection is not left half-consumed for the next probe; the STATUS
       is deliberately not looked at — see above. */
    await r.arrayBuffer();
    answered = true;
  } catch (e) {
    transport = e && e.name === "TimeoutError" ? "timeout" : transportReason(e);
  }
  if (answered) return { serving: true, said: `the server answered this driver's probe (${serverEnded()})` };
  if (g_serverExit)
    return { serving: false,
             said: `wptserve is GONE — the process this driver spawned ${serverEnded()}, and its probe of ` +
                   `http://${serverAddr}/ then failed with ${transport}` };
  if (transport === "timeout")
    return { serving: false,
             said: `wptserve is RUNNING AND NOT ANSWERING — the process is still alive and did not answer a ` +
                   `request for http://${serverAddr}/ within ${SERVER_PROBE_MS / 1000}s. That is the wedge this ` +
                   "file's own header describes: a handler thread blocked with the connection it was serving " +
                   "still open" };
  return { serving: false,
           said: `wptserve is NOT ANSWERING — the process is still alive and a request for http://${serverAddr}/ ` +
                 `failed with ${transport}, so it is not accepting connections on the port it declared READY` };
}

/* THE RUN STOPPED MEASURING, SAID SO, AND KEPT ITS ROWS — the three halves of a truncated report.
 *
 * STOPPING IS THE CHOICE AND HERE IS THE ARGUMENT FOR IT. The alternative — carry on and label every remaining
 * row `corpus` — was measured: it produced 935 spawns of a 14 MB binary against a socket that answers nothing,
 * on a four-core box this project SHARES with every other gate, and §Testing's own loaded-machine incidents are
 * all one machine under load corrupting somebody's number. Those rows measure nothing by construction, so the
 * CPU they burn is spent making every concurrent measurement worse in exchange for no information at all.
 * RESTARTING THE SERVER WOULD BE WORSE STILL, and it is the tempting one: it is a recovery that hides the root
 * (why did wptserve die?), it straddles one run's rows across two servers on two ephemeral ports, and it is the
 * legacy-fallback shape — a mechanism that lets the report look complete while the thing it reports on is
 * broken.
 * WHAT IS KEPT is everything already measured: the areas that finished keep their rows, the area interrupted
 * mid-way prints its partial row and its held failure lines, and the summary runs. A truncated run that SAYS it
 * is truncated is more useful than one that dies — but only if the total below cannot be mistaken for a corpus,
 * which is what the shortfall on the last line and the exit status are for. */
let g_truncated = null;
async function serverGone(rel) {
  const h = await serverHealth();
  if (h.serving) return null;
  g_truncated = { rel, said: h.said };
  return h;
}

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
    /* THE REWRITE APPLIES WHEREVER THIS DRIVER RESOLVES A REFERENCE ON DISK, WHICH IS HERE AND AT
       `docScriptFixtures`. It used to say HERE AND NOWHERE ELSE, and that sentence was false the moment a
       document's `<script src>` became a disk question too — every idlharness DOCUMENT names
       /resources/WebIDLParser.js, which is on nobody's disk under that name, so a reader who believed the
       exclusivity would conclude those files must abort on a fixture the server serves perfectly well. An
       EXCLUSIVITY CLAIM IS THE STALE SENTENCE NOBODY GREPS, because it reads as a completed question rather
       than as something to check.
       WHAT IS STILL TRUE IS THE RULE UNDER IT, and it is the rule that decides where a future site belongs: a
       META script is a PROGRAM INPUT — the driver hands the runner its file to execute before the test — so
       this path is resolved against the disk and needs wptserve's rewrite table to find the webidl2 library
       under its historical name. Everything the test FETCHES goes through the real server, which applies its
       own rewrites; this table is not a second copy of those, it is what the driver must resolve for itself
       whenever it answers a disk question about a path the corpus states as a URL. */
    const ref = SERVER_REWRITES[v] || v;
    return ref.startsWith("/") ? join(WPT, ref.slice(1)) : join(dirname(file), ref);
  });
}

/* THE `.idl` FILES A TEST DECLARES — THE SECOND OF THE THREE KINDS OF "WHAT THIS FILE NEEDS BEFORE IT CAN
 * MEASURE ANYTHING". This comment used to call it "the other half", and there is no half: the third kind is a
 * document's own `<script src>`, resolved by `docScriptFixtures` below, and calling two of three a pair is how
 * the third one goes on being nobody's.
 *
 * A META script is a fixture the DRIVER hands over, and the block in the run loop below has always checked that
 * one. An `.idl` is a fixture the TEST FETCHES, and nothing checked it — which is how the whole idlharness
 * family came to collect, run, report, and pass every check this gate makes about its own collection while
 * never executing a single IDL assertion. `resources/idlharness.js` defines the mechanism in four lines:
 *
 *     function fetch_spec(spec) {
 *         var url = '/interfaces/' + spec + '.idl';
 *         return fetch(url).then(function (r) {
 *             if (!r.ok) { throw new IdlHarnessError("Error fetching " + url + "."); }
 *
 * and `idl_test(srcs, deps, setup)` does `Promise.all(srcs.concat(deps).map(globalThis.fetch_spec))`, so BOTH
 * array arguments are fetched and either one being absent rejects the whole setup. A missing `.idl` therefore
 * does not announce itself as a checkout gap: it lands in the FAIL column as a subtest named `idl_test setup`,
 * or — for the twenty-two files whose harness never got that far — as a TIMEOUT, which is the shape that hid
 * it. One `existsSync` per declared spec, asked here, turns a per-file harness timeout into a named entry.
 *
 * WHY THE ANSWER IS NOT "THE FAMILY IS ON NOW, SO THIS IS SILENT". It is silent today and that is the point of
 * having it: the family's fixtures were reachable through no mechanism, only through a list entry that nothing
 * asserted, so the day `interfaces` leaves WPT_PATHS — or a corpus bump renames a spec — the family returns to
 * its two-subtest floor and the total looks complete again. The check is what makes that a named abort instead
 * of a rediscovery.
 *
 * IT READS THE SOURCE, NEVER THE FILENAME. `FileAPI/idlharness.worker.js` declares no `// META: script=` line
 * at all — it calls `importScripts("/resources/WebIDLParser.js", "/resources/idlharness.js")` — so a detector
 * keyed on META misses it, and a detector keyed on the name `idlharness` would both miss a future test that
 * calls `idl_test` under another name and claim one that merely mentions it.
 *
 * WHAT IT CANNOT ANSWER, IT SAYS. A spec argument that is not a string literal cannot be resolved without
 * running the file, and reporting nothing for one would be this gate asserting reachability it never checked —
 * an absent measurement wearing the shape of a clean one. It is returned separately and reported separately. */
function idlFixtures(file, kind) {
  const raw = readCorpus(file);
  if (raw === null) return null;   /* readCorpus recorded WHY; the run loop reports it */
  /* THE CHEAP QUESTION FIRST, AND IT IS SOUND RATHER THAN MERELY FAST. `codeOnly` is a character scan, and this
     runs for every collected file in the corpus — thousands of them, most with no IDL in sight — so asking it
     of all of them would spend real CPU on a shared box to learn nothing, which §Testing names as the way a
     measurement becomes an artifact of the machine it ran on. The guard is exact and not a heuristic: `codeOnly`
     only ever REMOVES text, so a name absent from the raw bytes is absent from its output, and a file that
     passes this cannot have had a call that the scan below would have found. */
  if (!raw.includes("idl_test") && !raw.includes("fetch_spec")) return { specs: [], dynamic: [] };
  /* A DOCUMENT'S JAVASCRIPT IS ITS INLINE SCRIPT BODIES; a `.js` test's is the whole file. Either way what is
     read as code is put through `codeOnly` first, so a call spelled inside a comment cannot mint a fixture. */
  const code = (kind === "document" ? inlineScripts(raw) : [raw]).map(codeOnly).join("\n");
  const specs = new Set(), dynamic = [];
  /* `fetch_spec('x')` CALLED DIRECTLY, which `idl_test` is a wrapper over and which a test may use on its own. */
  for (const m of code.matchAll(/\bfetch_spec\s*\(\s*(['"])([^'"]+)\1\s*\)/g)) specs.add(m[2]);
  for (const m of code.matchAll(/\bidl_test\s*\(/g)) {
    /* THE CALL'S OWN EXTENT, by balancing from its `(` — an `idl_test` call's third argument is a setup
       FUNCTION whose body routinely holds arrays and braces of its own, so anything that stopped at the first
       `)` or `]` would read a fragment of that body as a spec list. */
    let depth = 0, end = -1;
    for (let k = m.index + m[0].length - 1; k < code.length; k++) {
      const ch = code[k];
      if (ch === "(" || ch === "[" || ch === "{") depth++;
      else if (ch === ")" || ch === "]" || ch === "}") { depth--; if (!depth) { end = k; break; } }
    }
    if (end < 0) { dynamic.push("an idl_test call whose parentheses do not balance"); continue; }
    const call = code.slice(m.index + m[0].length, end);
    /* THE FIRST TWO TOP-LEVEL ARRAY LITERALS ARE `srcs` AND `deps`, in idl_test's own parameter order, and the
       nested scan skips any parenthesised or braced argument whole so a setup function's arrays are never
       mistaken for them. */
    const arrays = [];
    for (let k = 0, d = 0, st = -1; k < call.length; k++) {
      if (call[k] === "[") { if (d === 0) st = k; d++; }
      else if (call[k] === "]") { d--; if (d === 0 && st >= 0) { arrays.push(call.slice(st + 1, k)); st = -1; } }
      else if (d === 0 && (call[k] === "(" || call[k] === "{")) {
        for (let dd = 0; k < call.length; k++) {
          if ("([{".includes(call[k])) dd++;
          else if (")]}".includes(call[k])) { dd--; if (!dd) break; }
        }
      }
    }
    for (const a of arrays.slice(0, 2))
      for (const part of a.split(",")) {
        const t = part.trim();
        if (!t) continue;
        const q = /^(['"])([^'"]+)\1$/.exec(t);
        if (q) specs.add(q[2]);
        else dynamic.push(t.length > 60 ? t.slice(0, 60) + "…" : t);
      }
  }
  return { specs: [...specs], dynamic };
}

/* THE THIRD KIND OF DECLARED FIXTURE — A DOCUMENT'S OWN `<script src>` — AND THE ONLY ONE STILL LEFT TO THE RUN
 * TO DISCOVER. §Testing states the rule the two blocks above already obey: "when a test declares fixtures (a
 * fetched IDL, a serviced resource, a substituted host), the reachability of those fixtures is part of
 * COLLECTING it, not a separate question the test is left to discover at runtime." A META script is checked at
 * collection; an `.idl` is checked at collection; a document's `<script src>` was checked NOWHERE — the file
 * ran, the engine reported `@WPTERR … <script src> did not load`, and the driver turned that into an abort
 * AFTER the fact. It is the same fixture question as the other two and it is answered here with them.
 *
 * WHAT MOVING IT EARLIER ACTUALLY BUYS, because "it already aborts" is the obvious objection and it is only
 * half true. (a) THE DIAGNOSIS. The runtime line says a path did not load and stops; every other fixture in
 * this file is split by `corpusHas` into "EXISTS at the pinned revision, so WPT_PATHS is short of <dir>" and
 * "does not exist at all, so no entry can supply it" — one of which is this gate's to fix and one of which is
 * not. Reporting them alike is what the META block's own comment records sending a reader to widen a list that
 * cannot help. (b) IT DEPENDS ON THE FILE RUNNING AT ALL. The runtime branch is guarded by `!abortedHere`, so a
 * document that aborts for any other reason never names the script it was missing, and a run truncated before
 * the file is reached names nothing about it either. A collection-time answer is a fact about the CORPUS and
 * does not need the engine's cooperation to be true. (c) IT IS THE CHECK A NEW AREA NEEDS BEFORE IT IS ADDED,
 * not after: every WPT_PATHS entry landed so far had its `<script src>` set resolved BY HAND before the entry
 * went in, which is a procedure that works exactly as long as everyone remembers it.
 *
 * THE RUNTIME BRANCH STAYS, AND IT IS NOT A FALLBACK. The two ask different questions about different axes —
 * this one asks whether the byte-source is IN THIS CHECKOUT, and the runtime one asks whether wptserve SERVED
 * it, which a file present on disk can still fail (a handler that 500s, a wrong content type). That is the
 * identical split the META block above draws between `unserved` (asked of the server) and `missing` (asked of
 * the disk), and neither there nor here does one subsume the other.
 *
 * RESOLUTION IS WPTSERVE'S, NOT A GUESS. A `/`-rooted src is corpus-root-relative and anything else resolves
 * against the test's own directory — the server's own rule, already stated at `metaScripts` — and
 * SERVER_REWRITES applies first, because `/resources/WebIDLParser.js` is on nobody's disk under that name and
 * every idlharness DOCUMENT names it. A query or fragment is cut before the lookup: `?pipe=sub` selects a
 * PIPELINE over a file that is still a file.
 *
 * WHAT IT CANNOT RESOLVE, IT SAYS, exactly as `idlFixtures` does for a non-literal spec argument. An absolute
 * or scheme-relative URL names another ORIGIN and no path in this checkout; a `{{…}}` src is a template whose
 * substituted value is the server's to compute. Those are returned separately and reported as an ABSENT
 * measurement for that file — never counted as a fixture that was found, which would be this gate asserting a
 * reachability it never checked.
 *
 * IT READS `markupOnly`, SO A `<script src>` WRITTEN AS TEXT IS NOT ONE. That is the same strip
 * `isTestDocument` makes and for the same reason — `the-script-element/execution-timing/102.html`
 * `document.write`s a whole harness document, so its bytes carry a literal script element no parser sees at
 * parse time. Sharing the strip is what stops the collector and this check disagreeing about what an element
 * is. */
function docScriptFixtures(file) {
  const raw = readCorpus(file);
  if (raw === null) return null;   /* readCorpus recorded WHY; the run loop reports it */
  const stripped = markupOnly(raw);
  const paths = new Set(), unresolved = [];
  const media = new Set(), mediaUnresolved = [];
  /* WPTSERVE'S RULE, WRITTEN ONCE AND ASKED BY BOTH LOOPS. It was inline in the script loop while the script
     loop was the only loop; a second construct reading it is a second reading of one rule, and the two would
     disagree about a `?pipe=` or a SERVER_REWRITES entry with nothing to say so. Returns the corpus-relative
     path, or null having recorded WHY it could not be placed. */
  const place = (raw_ref, unplaced) => {
    const ref = SERVER_REWRITES[raw_ref] || raw_ref;
    if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(ref) || ref.startsWith("//") || ref.includes("{{")) {
      unplaced.push(raw_ref.length > 60 ? raw_ref.slice(0, 60) + "…" : raw_ref);
      return null;
    }
    const bare = ref.split("?")[0].split("#")[0];
    if (!bare) return null;                               /* `src="?query"` names the document itself */
    return bare.startsWith("/") ? bare.slice(1)
                                : relative(WPT, join(dirname(file), bare)).split(sep).join("/");
  };
  const ATTR = /\ssrc\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))/i;
  for (const tag of stripped.matchAll(/<(?:[A-Za-z][\w.-]*:)?script\b[^>]*>/gi)) {
    const a = ATTR.exec(tag[0]);
    if (!a) continue;                                     /* an INLINE script declares no fixture */
    const raw_ref = (a[1] ?? a[2] ?? a[3]).trim();
    if (!raw_ref) continue;
    const p = place(raw_ref, unresolved);
    if (p !== null) paths.add(p);
  }
  /* THE MEDIA ELEMENTS, WHICH DECLARE A FIXTURE EXACTLY AS A `<script src>` DOES AND WERE READ BY NOTHING.
     They are collected APART from the script paths and not folded in with them, because the two failures are
     not the same failure and this gate has no business calling them one. A `<script src>` that does not load
     takes the test's own HARNESS with it, so the numbers afterwards are not the test's — which is why the
     block that consumes `paths` aborts the run. A `<video src>` that does not load takes the test's SUBJECT,
     so the file still runs and still reports; its answer is wrong rather than absent, and for a NEGATIVE test
     it can even be right by accident. Aborting on one would be this gate refusing a run over a fixture whose
     absence the run itself can survive, and the accusing direction is the one that needs the most suspicion.
     SO IT IS A BLIND SPOT WITH A SIZE RATHER THAN A FINDING — reported separately at the foot of the run,
     counted, and never summed into a failure total. */
  for (const tag of stripped.matchAll(/<(?:[A-Za-z][\w.-]*:)?(?:video|audio|source|track)\b[^>]*>/gi)) {
    const a = ATTR.exec(tag[0]);
    if (!a) continue;
    const raw_ref = (a[1] ?? a[2] ?? a[3]).trim();
    if (!raw_ref) continue;
    const p = place(raw_ref, mediaUnresolved);
    if (p !== null) media.add(p);
  }
  return { paths: [...paths], unresolved, media: [...media], mediaUnresolved };
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
  /* A TRANSPORT FAILURE IS NOT AN HTTP STATUS, AND LETTING IT THROW OUT OF HERE ENDED THE RUN WITH A BARE
     `TypeError: fetch failed` — no area rows, no summary, no revision, which is strictly worse than the tail it
     replaces. It is recorded as the third thing it is: the server gave no answer at all. The caller asks
     `serverGone` about it, because "the corpus server would not serve this helper" and "there is no corpus
     server" are two different pieces of work and only one of them is about this file. */
  let r;
  try {
    r = await fetch("http://" + serverAddr + path);
  } catch (e) {
    g_unserved.set(path, "no HTTP answer at all — " + transportReason(e));
    g_subbed.set(dep, null);
    return null;
  }
  if (!r.ok) { g_unserved.set(path, "HTTP " + r.status); g_subbed.set(dep, null); return null; }
  const out = join(dirname(bin), relative(WPT, dep).split(sep).join("__"));
  writeFileSync(out, await r.text());
  g_subbed.set(dep, out);
  return out;
}

let pass = 0, fail = 0, notrun = 0, aborted = 0, unread = 0, errored = 0;
/* HOW MANY OF `runs` THIS RUN ACTUALLY REACHED. It is the same number as `runs.length` for every run that ends
   normally, and the two differing is the ONLY thing that makes a truncated total unmistakable — printing
   `runs 5483` for a run that attempted 4548 of them is the "total that LOOKS complete" defect this gate exists
   to catch, performed by the gate itself. */
let attempted = 0;
const g_abKind = new Map();
/* THE ONE PLACE AN ABORT IS COUNTED, so that naming its kind is not a thing a site can forget: there is no
   `aborted++` left to write. The kind is checked against the table rather than trusted, because a typo at a call
   site would otherwise invent a bucket that the summary silently drops — and the split's own sum assert would
   then be the only thing to notice, one screen and several thousand runs later.
   THE KIND RIDES THE ABORT LINE TOO. A 1079-line log is read with grep, and `grep 'ABORT.*\[gap\]'` is the
   clustering a reader had to do by hand; putting the kind in the line is what makes the per-area numbers and the
   lines behind them the same fact rather than two things to reconcile. */
function abortRun(area, kind, rel, detail) {
  if (!Object.hasOwn(ABORT_KINDS, kind))
    throw new Error(`[wpt] an abort was raised with the kind ${JSON.stringify(kind)}, which is not one of ` +
                    `${Object.keys(ABORT_KINDS).join("/")} — an abort this driver cannot name arrives as ` +
                    "UNKNOWN, never as a bucket invented at a call site");
  aborted++; area.aborted++;
  g_abKind.set(kind, (g_abKind.get(kind) || 0) + 1);
  area.abKind.set(kind, (area.abKind.get(kind) || 0) + 1);
  area.lines.push(`  ABORT   ${rel}  [${kind}]\n         ${detail}`);
}
/* THE CENSUS BELOW IS A VERDICT, so it needs a home outside its own block: a test file on disk that neither list
   accounts for is an excluded test, and an excluded test is a failure — not a row a reader may skip. */
let g_undecided = 0;
/* AND THE FIXTURE CENSUS'S OWN, FOR THE SAME REASON AND ONLY FOR ITS FIRST ARM — a path the PINNED CORPUS HAS
   that a test asked for and this checkout could not serve is the same fact as a missing META script, which
   already aborts its file. Its second arm is reported and never counted; see the census for why. */
let g_refused = 0;
/* AND THE DECLARED-FIXTURE BLIND SPOT, WHICH IS A SIZE AND NOT A VERDICT. `docScriptFixtures` resolves the
   fixtures a document DECLARES, and until now it read one construct; the media elements declare one the same
   way and were read by nothing, which is how four `content-security-policy/media-src` documents came to name
   `/media/A4.webm` with no entry on any list to supply it.
   IT DOES NOT GATE, AND THAT IS THE CLAIM RATHER THAN A HEDGE. The two arms of `g_refused` and the
   `<script src>` abort are FINDINGS — a missing harness makes the numbers afterwards not the test's. A missing
   media fixture makes the numbers WRONG rather than absent: the file still runs, still reports, and a NEGATIVE
   test can even answer correctly without it. Summing the two would put a fact this gate cannot judge into a
   count a reader takes as a defect total, which is the shape the census above is banded against.
   IT PRINTS ON THE CLEAN DAY. A line that appears only when it is nonzero is a line nobody learns to look for,
   so the block below prints its zero and says what the zero is a zero OF — the population being the media
   `src` values, which is a FLOOR over the constructs that declare a fixture and not over every way a test can
   reach a byte. A runtime `fetch()`, an `<iframe src>`, an `<img src>` and a `?pipe=` handler are outside
   what any of this can look at, and that remains true after this. */
const g_mediaBlind = { absent: new Map(), unresolved: 0, files: new Set() };

/* WHY THE HARNESS ENDED THE WAY IT DID, OUT OF WHAT THE RUN ALREADY STREAMED.
 *
 * `status.message` is EMPTY for the status this gate meets most, and `(h.message || "")` turned that into a
 * plausible datum — the defaulted-field defect performed in a report. `html/browsers` printed
 * `testharness status TIMEOUT:` and then nothing, NINETY times, which was the single largest cause in the whole
 * area and named no component at all. Behind that one string were three different pieces of work, asking for
 * three different fixes in three different places: a file whose subtest was still AWAITING something (the
 * subtest's name is the name of what did not arrive), a file that registered NO subtest because the code that
 * creates them never ran, and a file whose subtests had ALL reported and whose COMPLETION never came — which is
 * not a missing answer at all. Three states behind one number is the shape §@S names for a search that cannot
 * be directed, and this is that shape in the instrument: every one of those rows read as "some browser feature
 * is missing" and the gate was holding the fact that separates them the whole time.
 *
 * IT STATES WHAT WAS OBSERVED, NEVER WHAT IS MISSING. The subtest statuses are testharness's own, streamed by
 * this run; the NAME is the one carried by a subtest that never settled, which is what points at the capability
 * without this file guessing at one. And where a subtest's own status IS the answer, the third arm names the
 * remaining conjuncts of `Tests.all_done()` rather than a cause: that predicate is `tests.length > 0 ||
 * remotes.length > 0`, `test_environment.all_loaded`, `num_pending === 0`, `!wait_for_finish`,
 * `!processing_callbacks` and no remote still `running` — so with every subtest resolved, `num_pending` was
 * zero and what was still false is one of the others. Naming the predicate is checkable; naming a component
 * would be a guess. */
function harnessCause(h, started, settled) {
  const of = (s) => [...settled.values()].filter((t) => t.status === s);
  const hung = of(2), notrunHere = of(3);
  const unsettled = [...started.keys()].filter((k) => !settled.has(k)).map((k) => started.get(k));
  const census = `[${settled.size} subtest(s) reported` +
                 (hung.length ? `, ${hung.length} TIMEOUT` : "") +
                 (notrunHere.length ? `, ${notrunHere.length} NOTRUN` : "") +
                 (unsettled.length ? `, ${unsettled.length} announced and never reported` : "") + "]";
  if (h.status === 2) {
    if (hung.length)
      return `: the file's own harness timeout fired while ${hung.length} subtest(s) were still awaiting — ` +
             `the first is ${JSON.stringify(hung[0].name)}, and what IT awaits is what did not arrive ${census}`;
    if (unsettled.length)
      return `: the harness timeout fired with ${unsettled.length} announced subtest(s) that never reached ` +
             `this driver — the first is ${JSON.stringify(unsettled[0])} ${census}`;
    if (settled.size === 0)
      return ": the harness timeout fired with NO subtest ever registered, so this file creates its tests out " +
             "of work that never ran — the missing capability is upstream of the first assertion";
    return `: every one of its ${settled.size} subtest(s) already carried a result and the harness still never ` +
           `completed — so \`num_pending\` was 0 and what \`Tests.all_done()\` was still missing is one of its ` +
           `other conjuncts: \`all_loaded\` (the window's \`load\` event), \`wait_for_finish\` (an ` +
           `\`explicit_done\` page's own \`done()\`), or a \`fetch_tests_from_window\` remote still \`running\` ` +
           `${census}`;
  }
  /* A MESSAGE THIS STATUS CARRIES IS THE REPORT; ITS ABSENCE IS A POSITIVE STATEMENT AND NOT A HOLE. */
  return (h.message ? `: ${String(h.message).slice(0, 200)}`
                    : ": testharness set this status and no message with it") + ` ${census}`;
}

console.log("\n==================== web-platform-tests ====================");
/* THE LEGEND GOES FIRST BECAUSE THE ROWS STREAM. Every ABORT line and every area row below carries a kind, and a
   reader watching a long run must be able to decode one without waiting for the block at the end. */
console.log("  an ABORT carries its KIND, and the kinds ask for work in different files: " +
            Object.keys(ABORT_KINDS).join(" / ") + " — see the breakdown at the foot of this run");
for (const { file: f, kind, variant } of runs) {
  /* THE RUN'S NAME CARRIES ITS VARIANT, because that is what distinguishes it from its siblings — four lines
     reading `url/url-constructor.any.js` with four different failures name nothing a reader can act on. The
     AREA is asked for the file's own path, since a variant does not live somewhere else. */
  const path = relative(WPT, f);
  const rel = path + variant;
  const area = byArea(path);
  /* WHERE THIS TEST IS HOSTED — decided from the FILE, once, here, so the two facts the spawn passes cannot
     come from different answers. It is asked of `path` and not of `rel`: a variant is a query string on the
     same file, and WPT reads the scheme flag off the file NAME. */
  const hosted = testIsHttps(path.split(sep).join("/")) ? serverHttps : { addr: serverAddr, origin: serverOrigin };
  const failures = area.lines;   /* held until this AREA finishes, not until the run does */
  area.runs++;
  attempted++;
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
      failures.push(`  UNREAD  ${rel}\n         the corpus file could not be read: ${g_unreadable.get(key)} — ` +
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
      /* AND WHETHER THERE IS STILL A SERVER TO HAVE REFUSED IT. A helper that 500s is a wptserve config fact
         about this file; a helper that got no answer at all is the whole run, and the two arrive here as the
         same empty `deps` slot. */
      const gone = await serverGone(rel);
      abortRun(area, "corpus", rel,
               "a .sub META script the corpus server would not serve: " +
               unserved.map((p) => `${p} (${g_unserved.get(p)})`).join(", ") +
               "\n         it is a TEMPLATE, so its bytes on disk are not the file this test is written " +
               "against — see the wptserve log named above for the traceback" +
               (gone ? `\n         AND ${gone.said}` : ""));
      if (gone) break;
      continue;
    }
    const missing = deps.filter((d) => !existsSync(d));
    if (missing.length) {
      /* A META script this driver cannot hand over is a CORPUS fact, never a test result — the file would run
         against a corpus it was not written for. But it is TWO corpus facts and only ONE of them is this gate's
         to fix, and reporting them alike sent a reader to widen a list that cannot help.
         MEASURED, on the two `html/browsers` runs that reported "META script not checked out":
         `back-forward-cache/weblocks-worker.https.window.js` names a helper that EXISTS at the pinned revision
         under `remote-context-helper-tests/resources`, in a directory WPT_PATHS did not reach — a checkout gap,
         now closed above. `back-forward-cache/pagehide-event-handler-microtasks.window.js` names
         `./resources/test-helper.js`, and NO SUCH FILE EXISTS AT THE PINNED REVISION: wptserve's own
         `_script_replacement` emits `<script src="./resources/test-helper.js">` into the wrapper it serves at the
         test's address, so the browser resolves it beside the test and 404s. That file is broken UPSTREAM at
         bf4714d and no sparse-checkout entry can supply it. One message for both would have been true of neither.
         ASKED OF GIT, WHICH IS WHERE THE ANSWER LIVES — the corpus is a checkout of a PINNED revision, so
         "checked out" and "exists at all" are two different questions, and the second is a fact about the TREE
         that this driver can simply look up. `corpusHas` is that lookup, shared with the fixture diagnosis on
         the run's own traffic below so the two cannot come to disagree about which revision they ask at. */
      const said = missing.map((d) => {
        const p = relative(WPT, d).split(sep).join("/");
        const has = corpusHas(p);
        if (!has.known) return `${p} — and ${has.why}`;
        return has.present
          ? `${p} — it EXISTS at ${has.at} and this checkout does not have it, so WPT_PATHS is short of ` +
            `${dirname(p)}: add that entry and the file runs`
          : `${p} — it does NOT EXIST at ${has.at} at all, so no WPT_PATHS entry can supply it. The test names a ` +
            "helper the pinned corpus does not contain and cannot run as written at this revision";
      });
      abortRun(area, "corpus", rel, "a META script this driver could not hand over:\n         " +
                                    said.join("\n         "));
      continue;
    }
    /* AND THE `.idl` FIXTURES THE TEST ITSELF WILL FETCH — the same question as the block above, asked about the
       other kind of fixture, answered through the same `corpusHas` so the two cannot come to disagree about
       which revision they ask at. See `idlFixtures` for why this is not the runtime path's job: a missing `.idl`
       arrives as a FAIL or a TIMEOUT on the file, in the column that reads as a result about the engine.
       IT IS A DISK QUESTION AND SAYS SO. What is checked is that the byte-source exists in this checkout; that
       wptserve then SERVES it is the run's own affair and shows up in `trafficEvidence` under a file that did
       not complete. Naming the axis is the point — a check that quietly meant something narrower than it read
       would be the concealment this block exists to remove. */
    const idl = idlFixtures(f, kind);
    if (idl && idl.specs.length) {
      const absent = idl.specs.filter((s) => !existsSync(join(WPT, "interfaces", s + ".idl")));
      if (absent.length) {
        const said = absent.map((s) => {
          const p = `interfaces/${s}.idl`;
          const has = corpusHas(p);
          if (!has.known) return `${p} — and ${has.why}`;
          return has.present
            ? `${p} — it EXISTS at ${has.at} and this checkout does not have it, so WPT_PATHS is short of ` +
              "interfaces: add that entry and this file's IDL assertions run"
            : `${p} — it does NOT EXIST at ${has.at} at all, so no WPT_PATHS entry can supply it. The test ` +
              `names a spec the pinned corpus does not carry under that name`;
        });
        abortRun(area, "corpus", rel,
                 "an .idl fixture this test fetches is not in the checkout:\n         " + said.join("\n         ") +
                 "\n         idlharness.js fetches every name in BOTH of idl_test's array arguments and rejects " +
                 "the whole setup if one is absent, so this file would report its floor of two subtests and " +
                 "nothing about the engine");
        continue;
      }
    }
    /* A SPEC NAME THIS DRIVER COULD NOT RESOLVE STATICALLY IS AN ABSENT ANSWER, NOT A CLEAN ONE, and it is
       printed rather than counted: the file is runnable and must not be aborted over it, but a reader who takes
       the silence above as "every fixture checked" would be trusting a check that skipped this file. */
    if (idl && idl.dynamic.length)
      failures.push(`  IDLDYN  ${rel}\n         ${idl.dynamic.length} idl_test spec argument(s) are not string ` +
                    `literals (${idl.dynamic.join(", ")}), so their reachability was NOT checked — this is an ` +
                    "absent measurement for this file, not a fixture that was found");
    /* AND THE `<script src>` A DOCUMENT DECLARES — the same disk question a third time, through the same
       `corpusHas`, so all three fixture reports agree about which revision they ask at. See
       `docScriptFixtures` for why this belongs here and not at the end of the run. A `.js` test declares its
       fixtures as `// META: script=` lines, which the block above already resolved, so this is asked only of
       documents. */
    const docfix = kind === "document" ? docScriptFixtures(f) : null;
    if (docfix && docfix.paths.length) {
      const absent = docfix.paths.filter((p) => !existsSync(join(WPT, p)));
      if (absent.length) {
        const said = absent.map((p) => {
          const has = corpusHas(p);
          if (!has.known) return `/${p} — and ${has.why}`;
          return has.present
            ? `/${p} — it EXISTS at ${has.at} and this checkout does not have it, so WPT_PATHS is short of ` +
              `${dirname(p)}: add that entry and this file runs as written`
            : `/${p} — it does NOT EXIST at ${has.at} at all, so no WPT_PATHS entry can supply it. The document ` +
              "names a helper the pinned corpus does not contain and cannot run as written at this revision";
        });
        abortRun(area, "corpus", rel, "a <script src> this document declares is not in the checkout:\n         " +
                                      said.join("\n         ") +
                                      "\n         the document would load without it and report whatever it " +
                                      "then managed — numbers from a test that is not the test");
        continue;
      }
    }
    /* A src THIS DRIVER COULD NOT RESOLVE TO A CORPUS PATH IS AN ABSENT ANSWER, NOT A CLEAN ONE — the same
       sentence as IDLDYN one line up, for the same reason. An absolute or scheme-relative URL names another
       origin, and a `{{…}}` src is a template the server substitutes; either way this file's reachability was
       not established and the report must not read as though it were. */
    if (docfix && docfix.unresolved.length)
      failures.push(`  SRCDYN  ${rel}\n         ${docfix.unresolved.length} <script src> value(s) do not name a ` +
                    `path in this corpus (${docfix.unresolved.join(", ")}), so their reachability was NOT ` +
                    "checked — this is an absent measurement for this file, not a fixture that was found");
    /* AND THE MEDIA FIXTURES THE SAME DOCUMENT DECLARES — COUNTED, NEVER THROWN. See `g_mediaBlind` for why
       this arm reports where the one above fails, and `docScriptFixtures` for why the two buckets are
       separate at the source rather than split here. */
    if (docfix) {
      for (const p of docfix.media)
        if (!existsSync(join(WPT, p))) {
          g_mediaBlind.absent.set(p, (g_mediaBlind.absent.get(p) || 0) + 1);
          g_mediaBlind.files.add(rel);
        }
      g_mediaBlind.unresolved += docfix.mediaUnresolved.length;
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
       destroy.
       AND IT IS READ FROM ONE PLACE NOW, because the copy that stood here indexed those fields TWO TOO LOW —
       `utime`/`stime`, this driver's OWN CPU — under a comment that correctly named fields 16 and 17. It was
       therefore not a slightly wrong number but a number about the wrong PROCESS: the parent's, consumed while
       it sat in `waitpid`, which is ~0 by construction. Every verdict below turned on it, and the SIGXCPU arm
       printed "this test consumed 0.0s of CPU and really is that expensive" — two assertions that cannot both
       be true, the second drawn from the one that contradicts the kernel. engine/gate_cpu.mjs holds the meter,
       names the fields by proc(5)'s own numbers so the comment and the code cannot disagree again, and answers
       `null` where a host cannot measure rather than a 0.0 that reads as a measurement. */
    /* DRAINED FIRST, SO THE WINDOW HOLDS THE CHILD'S TRAFFIC AND NOT THIS DRIVER'S. Everything logged up to this
       point belongs to somebody else: the `.sub` substitution fetch this driver makes on the test's behalf, a
       `serverGone` probe from the previous file, and the previous child's own straggler reply lines. Leaving
       them in would make "the server logged NO request while this file ran" — the sentence that says the ENGINE
       never issued one — unwritable, because the window would never be empty. */
    corpusTrafficSince();
    const cpu0 = childCpuSeconds();
    const r = spawnSync("/bin/sh",
                        ["-c", `ulimit -H -t ${CPU_BUDGET_S + 10} 2>/dev/null; ulimit -S -t ${CPU_BUDGET_S}; exec "$@"`, "sh", bin,
                         ...(kind === "document" ? ["--test-document"] : []),
                         ...(variant ? ["--variant", variant] : []), HARNESS, ...deps, f],
                        { encoding: "utf8", maxBuffer: 1 << 28, timeout: WALL_BACKSTOP_MS,
                          env: { ...process.env, WPT_SERVER: hosted.addr,
                                 /* WHERE TO CONNECT AND WHAT THE BYTES ARE SERVED AS — two facts, both the
                                    server's, neither derivable from the other. The runner asserts they name
                                    one port (wpt_derive_addresses) and has no literal to fall back to.
                                    WHICH PAIR is `testIsHttps`'s answer for THIS file, so the two stay one
                                    server: each listener binds the port its own origin names, which is what
                                    keeps that assert at full strength across both schemes. */
                                 WPT_TOP_ORIGIN: hosted.origin } });
    const cpuUsed = childCpuDelta(cpu0, childCpuSeconds());
    /* THE TRAFFIC THIS FILE GENERATED, taken here because `spawnSync` has just reaped the only process that
       could have generated any. See `corpusTrafficSince` for why the window is exact and what its one straggler
       is. `null` is the log saying it cannot vouch for the slice, and every reader below treats that as "no
       evidence" rather than as "no requests" — an absent measurement and a zero measurement are different facts
       and §Testing forbids averaging them. */
    const traffic = corpusTrafficSince();
    /* AND INTO THE RUN-WIDE LEDGER, HERE RATHER THAN AT ANY OF THE READERS BELOW, because every one of those is
       a FAILURE path: a file that completed cleanly reaches none of them, and a clean file's refused fixture is
       precisely the case the census exists for. See `corpusFixtureLedger`. */
    corpusFixtureLedger(rel, traffic);
    const out = (r.stdout || "") + (r.stderr || "");
    /* An ABORT is a result about this file, not an accident: it is a DCHECK naming a capability the browser half
       does not have, which is exactly what this gate is for. It is counted apart from a FAIL because the two ask
       for different work — a fail is a wrong answer, an abort is a missing one. */
    /* AN ABORT DOES NOT ERASE WHAT THE FILE ALREADY REPORTED. A DCHECK ends the process, but the results printed
       before it are results — and discarding them here counted a file that had failed four subtests and then
       leaked as a file that produced NOTHING, which is the "same failures with the count hidden" shape this gate
       exists to avoid. It hid them from me, too: I diagnosed that file as ending with no results and went looking
       for what it was waiting on. The abort is reported AND the subtests are counted; they are different facts. */
    /* THE TWO SHAPES *AND* THE TWO MARKERS — four spellings of one fact, because there are two emitters and two
       severities and losing any of them loses the name. The HOST's check.h prints a machine-readable JSON line
       whose `reason` carries the message; the ENGINE's quickjs-check.h prints `<marker> <msg> (file:line)` as
       plain text. Matching only the JSON shape reported the plain one as a bare "signal SIGABRT"; matching only
       `@WHY` did the same to every always-fatal `@E`, and that is the larger of the two losses because `@E`'s
       sites are the ones a release build still hits — see the kind table above for what it cost here. An abort
       with the name thrown away is the one thing an abort is FOR. `marker` is the SEVERITY and decides the
       column; `reason` is prose and is only ever DISPLAYED.

       AND THE JSON SHAPE IS PARSED AS JSON, WHICH IT WAS NOT. The read here was
       `/@(WHY|E) .*"reason":"([^"]*)/` — a `[^"]*` over a field check.h ESCAPES, so it stopped at the first
       `\"` INSIDE the reason and kept the dangling backslash. CLAUDE.md requires a citation to quote the
       standard's own words, so the asserts that name the most are exactly the ones that carry a quotation
       earliest, and exactly the ones this cut first: a §15.4 DFAIL whose reason is 1600 characters and states
       which two rendering rules to build reached the report as 119 of them, ending in a
       bare `\`. That is check.h's own documented failure — "the ONE field that names what to build was
       unreadable in exactly the asserts that named it best" — arriving in the READER instead of the emitter.
       AND `at` WAS NEVER READ AT ALL. check.h writes the assert's own `__FILE__:__LINE__` into the record and
       this gate printed only the reason, so a run naming hundreds of gaps named the file for none of them —
       a producer's field with no consumer, which is the defect CLAUDE.md makes greppable. It is reported now.
       A LINE THAT ANNOUNCES ITSELF AS JSON AND DOES NOT PARSE IS SAID OUT LOUD rather than quietly re-read by
       the looser pattern: that silent degrade is how the emitter's escaping bug survived on the bridge side,
       and a `@WHY` this gate cannot decompose is a defect in check.h, not a reason for a softer reader. */
    const rec = out.match(/@(WHY|E) (\{[^\n]*\})/);
    let named = null, at = null;
    if (rec) {
      try {
        const o = JSON.parse(rec[2]);
        named = [rec[0], rec[1], String(o.reason ?? "")];
        at = o.at ? String(o.at) : null;
      } catch (e) {
        named = [rec[0], rec[1],
                 `check.h emitted a record this gate could not parse (${e.message}), so the capability this ` +
                 `abort names is in the raw line and nowhere else: ${rec[0]}`];
      }
    } else {
      named = out.match(/^@(WHY|E) (.+)$/m);   /* the ENGINE's plain-text emitter carries its own (file:line) */
    }
    const marker = named ? named[1] : null;
    /* WHOLE, and it is not unbounded: check.h composes the record into one PIPE_BUF buffer and caps the reason
       at APICLIENT_REASON_CAP, appending "[reason truncated: N of M bytes]" when it runs out — so the bound is
       upstream, real, and self-declaring. The `.slice(0, 160)` that used to stand at the two arms below was an
       author's margin on top of that, and it was cutting the half of the sentence that says WHAT TO BUILD:
       a property-read @WHY threads the C reader's own file:line through the message and instructs the reader to
       "FIX IT AT THAT FILE:LINE", and this gate cut the line before the site every time it fired. */
    const reason = named ? (at ? `[${at}] ${named[2]}` : named[2]) : null;
    /* WHAT *THIS DRIVER* DID, ASKED OF THE RESULT RATHER THAN GUESSED FROM THE SIGNAL. node's `spawnSync` reports
       its own interventions as an errno on `result.error`, and the signal alone cannot tell them apart:
       MEASURED — a `timeout:` kill is `ETIMEDOUT` **with SIGTERM**, and a `maxBuffer` overflow is `ENOBUFS`
       **also with SIGTERM**. This cascade used to read SIGTERM as "the wall backstop" and would therefore have
       reported a test that printed more than 256 MB — exactly the runaway `css/css-values` files that
       re-registered one subtest 7663 times — as a 600-second block, complete with an invitation to attach gdb to
       a process that had in fact exited on its own output. A spawn that never started (ENOENT) came out the other
       end as "testharness.js itself never ran", which is a claim about the CORPUS made from a fact about this
       driver's own `/bin/sh`. Both are the same mistake: reading a downstream symptom instead of the thing that
       caused it, when the causing thing is right there on the result object. */
    const timedOut = Boolean(r.error) && r.error.code === "ETIMEDOUT";
    const spawnBroke = Boolean(r.error) && !timedOut;
    const abortedHere = Boolean(named || r.signal || r.error);
    if (abortedHere) {
      /* WHICH KIND, FROM FACTS THIS DRIVER HOLDS. Every arm below is something it KNOWS: whether its own spawn
         failed, whether its own kill timer fired, whether the kernel raised the rlimit IT installed, and how much
         CPU the child actually consumed. Nothing here reads an abort message to decide a column.
         `gap` and `fatal` are the arms read from the child, and they read a MARKER (check.h's `@WHY` and `@E`),
         never the prose in it — the message is displayed and cannot move a run between columns. */
      let kind, cause;
      if (spawnBroke) {
        kind = "driver";
        cause = `this driver could not run the test at all: spawnSync failed with ${r.error.code || r.error.message}` +
                (r.error.code === "ENOBUFS"
                  ? ` — the child printed more than the ${1 << 28} bytes this driver will hold, so its output ` +
                    "was truncated and NOTHING it reported can be trusted. That is a runaway in the test or in " +
                    "the engine, and it is this driver's limit that stopped it"
                  : " — the child may never have started, so nothing about the engine was measured");
      } else if (named && timedOut) {
        /* BOTH CANNOT BE TRUE OF ONE PROCESS: a @WHY and an @E both `abort()` on the line that emits them, so a
           child that wrote one was gone long before a 600 s timer could fire. Arriving here means this driver's
           own model of the run is wrong, and that is precisely what must not be quietly filed under the biggest
           bucket. */
        kind = "UNKNOWN";
        cause = `the child emitted a @${marker} (${reason.slice(0, 120)}) AND this driver's kill timer fired — ` +
                "both markers abort at once, so a process cannot do both, and this driver's accounting of the " +
                "run is therefore wrong";
      } else if (marker === "WHY") {
        kind = "gap";
        cause = reason;   /* WHOLE — see the read above for the bound and for what the clamp was cutting */
      } else if (marker === "E") {
        /* THE SAME READ, A DIFFERENT COLUMN — see the kind table. `@E` is CHECK/CHECK_FAIL, which is not
           compiled out, so this file's absence is one a release build hits too. */
        kind = "fatal";
        cause = reason;
      } else if (timedOut) {
        kind = "killed";
        /* "LOOK AT THE OTHER END" IS AN INSTRUCTION THIS DRIVER CAN CARRY OUT ITSELF, and the other end is
           nearly always the corpus server: a runner blocked with zero CPU is blocked in `read()` on a socket,
           and the only socket it has is wptserve's. Asked ONLY where a block is not RULED OUT — a STARVED test
           consumed real CPU and its diagnosis is the load average, which no probe improves. An UNMEASURED run
           rules nothing out, so it is probed and the probe's answer is reported as an OBSERVATION rather than
           promoted into a verdict the meter did not support. */
        const blocked = cpuUsed !== null && cpuUsed < 1;
        const gone = cpuUsed === null || cpuUsed < 1 ? await serverGone(rel) : null;
        /* AND WHAT WENT OVER THAT SOCKET, which turns the instruction below into an answer this driver already
           holds. "Look at what it is waiting on" is the right next step and it used to require a debugger and a
           process that no longer exists; the access log names the request the run was blocked on — or says
           there was none, which moves the whole diagnosis upstream of the network. Appended to the two arms
           where the wire can be the cause and NOT to the starved arm, whose verdict is the load average and
           whom no amount of traffic would inform. */
        const otherEnd = (gone ? `AND ${gone.said} — that is the other end, and THIS RUN IS TRUNCATED HERE`
                               : "The corpus server ANSWERED this driver's probe, so the other end is not " +
                                 "wptserve being gone: attach to it (gdb -p, /proc/PID/syscall) and look at " +
                                 "what it is waiting on") + trafficEvidence(traffic);
        cause = cpuUsed === null
          /* THE THIRD STATE, SAID OUT LOUD. Block and starvation are told apart by ONE number, so a host that
             cannot produce it cannot produce either verdict — and the failure of the old meter was exactly
             that it produced a `0.0` here, which is the block arm's whole evidence, for every run on every
             host. An absent number and a zero number are different facts and this arm keeps them apart. */
          ? `wall backstop at ${WALL_BACKSTOP_MS / 1000}s, and the CPU this child consumed is ` +
            `${cpuText(cpuUsed)} — so this driver CANNOT say whether it was blocked on something that never ` +
            `came or starved of the thread, which are the only two things it could be and want opposite work. ` +
            otherEnd
          : blocked
          ? `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuText(cpuUsed, 2)} of CPU — ` +
            `that is a BLOCK, not load: the process was asleep waiting for something that never came. ` +
            otherEnd
          : `wall backstop at ${WALL_BACKSTOP_MS / 1000}s having consumed ${cpuText(cpuUsed)} of CPU, ` +
            `load average ${loadavg()[0].toFixed(1)} on ${cpus().length} cores — starved, not blocked; RE-RUN THIS FILE ALONE`;
      } else if (r.signal === "SIGXCPU" && (cpuUsed === null || cpuUsed >= CPU_BUDGET_S - 1)) {
        kind = "killed";
        cause = `exceeded the ${CPU_BUDGET_S}s CPU budget — SIGXCPU is the kernel raising the SOFT rlimit this ` +
                `driver installed, so it is not wall time and not load: this test consumed ${cpuText(cpuUsed)} ` +
                "of CPU and really is that expensive";
      } else if (r.signal === "SIGXCPU") {
        /* THE METER AND THE KERNEL DISAGREEING IS A DEFECT IN THIS DRIVER, NOT A FACT ABOUT THE TEST, and it is
           asserted here because the alternative is what stood here before: SIGXCPU means the kernel watched
           this child spend the SOFT rlimit, so a reading materially below the budget is impossible and printing
           it beside the signal produced one sentence asserting two things that cannot both be true — "SIGXCPU
           is the kernel raising the rlimit ... this test consumed 0.0s of CPU and really is that expensive" —
           with the conclusion drawn from the half that contradicts the signal. The kernel is the ground truth
           and the meter is derived, so the derived one being wrong is this driver's bug to fix, and it lands in
           the column this file already keeps for its own accounting being wrong. */
        kind = "UNKNOWN";
        cause = `the kernel raised SIGXCPU, which it does only when this child spent the ${CPU_BUDGET_S}s SOFT ` +
                `rlimit this driver installed — and this driver's own CPU meter read ${cpuText(cpuUsed, 2)} for ` +
                "it. Both cannot be true of one process. The signal is the kernel's and the number is derived " +
                "from /proc/self/stat's cutime/cstime, so the number is the one that is wrong and engine/" +
                "gate_cpu.mjs is where it is read. Nothing about this test was measured.";
      } else if (r.signal === "SIGKILL" && cpuUsed !== null && cpuUsed >= CPU_BUDGET_S) {
        kind = "killed";
        cause = `exceeded the ${CPU_BUDGET_S}s CPU budget and did not die of SIGXCPU, so the kernel escalated to ` +
                `SIGKILL at the ${CPU_BUDGET_S + 10}s HARD rlimit — ${cpuText(cpuUsed)} of CPU consumed`;
      } else if (r.signal === "SIGKILL" && cpuUsed === null) {
        /* THE SAME THIRD STATE ONE ARM DOWN. This driver's own hard rlimit and an external kill arrive as the
           SAME signal and are told apart by the CPU alone, so without it the two collapse — and the arm below
           would then assert "NOT a limit this driver installed" about a kill this driver installed. */
        kind = "UNKNOWN";
        cause = `SIGKILL, and the CPU this child consumed is ${cpuText(cpuUsed)} — so this driver cannot tell ` +
                `its OWN ${CPU_BUDGET_S + 10}s HARD rlimit from a kill that came from outside the run (the OOM ` +
                "killer is the usual one, and dmesg names it). Those are the only two candidates and they ask " +
                "for opposite work.";
      } else if (r.signal === "SIGKILL") {
        /* NOT THE BUDGET, AND SAYING SO IS THE POINT. This used to read `SIGXCPU || SIGKILL` and reported every
           SIGKILL as "this test really is that expensive" — a sentence about the test, asserted for a signal
           that is the OOM killer's usual one. The CPU consumed is the discriminator and this driver has it —
           though for a long time it did not: the meter it asked read the PARENT's idle CPU, so this arm caught
           every hard-rlimit escalation as well and told the reader to go and read dmesg about a kill this
           driver had installed itself. */
        kind = "UNKNOWN";
        cause = `SIGKILL after only ${cpuText(cpuUsed, 2)} of CPU, which is far below the ${CPU_BUDGET_S}s budget ` +
                "— so it is NOT a limit this driver installed. Something outside this run killed the process; " +
                "the OOM killer is the usual one (dmesg names it)";
      } else {
        kind = "crash";
        cause = `died on ${r.signal} and named NOTHING — no @WHY, no @E, and no limit this driver installed. ` +
                "That is a crash to debug at its own frame, not a capability to build";
      }
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
      abortRun(area, kind, rel, cause + census.map((l) => `\n         ${l}`).join(""));
      /* THE ROW IS RECORDED FIRST AND THE RUN STOPS SECOND — a truncation must never cost the report of the
         file it was discovered on, which is the one row that names where the server went. */
      if (g_truncated) break;
    }
    /* THE `@WPTHANDLER` BRANCH IS GONE, not disabled. It excused a test that asks for a wptserve `.py` handler
       back when this driver served the corpus off disk — and engine/wptserve.py now runs WPT'S OWN server, which
       imports those handlers and calls their `main`, so nothing has emitted that marker since. A branch whose
       emitter no longer exists is not a safety net, it is a reader's mistake waiting to happen: it says this gate
       cannot run handler tests, and this gate can. */
    /* A `<script src>` THE SERVER WOULD NOT SERVE. It is the same fact as the two blocks above — this file ran
       against a corpus it was not written for — so it is the same ABORT, and its partial subtests are dropped
       for the reason the count exists at all: they are numbers from a test that is not the test.
       WHAT IT ASKS IS NOW THE NARROWER HALF, AND THE SENTENCE THAT USED TO STAND HERE IS RETIRED. This said a
       `<script src>` the corpus does not HAVE was "the only one of the two that went uncounted", and that
       argument died the moment `docScriptFixtures` began resolving every document's declared srcs against the
       checkout BEFORE the file is run: a src that is absent on disk is now a named collection-time abort
       carrying the `corpusHas` split, so it never reaches here. What is left for this branch is exactly what a
       disk check cannot answer — a file that IS in the checkout and did not arrive: a handler that 500s, a
       wrong content type, a server that stopped. That is the same axis split the META block draws between
       `unserved` and `missing`, and neither half subsumes the other, which is why both stand. */
    /* `!abortedHere` — A FILE IS COUNTED ONCE. The block above does not `continue` (deliberately: an abort does
       not erase what the file already reported), so this fired IN ADDITION to it and a file that both aborted
       and asked for a script the corpus lacks was counted TWICE, in a column whose whole meaning is one per
       file. Measured: moveBefore/fullscreen-preserve.html, which is why the area read 332 over 331 runs. Its
       abort already names the file and its cause; the missing path rides that row rather than opening a second. */
    const noscript = out.match(/^@WPTERR .*<script src> did not load: (.*)$/m);
    if (noscript && !abortedHere) {
      abortRun(area, "corpus", rel, `a <script src> the corpus does not serve: ${noscript[1]}`);
      continue;
    }
    /* AND THE RUN'S OWN FILE, WHICH IS THE SAME FACT ABOUT THE ONE INPUT THAT MATTERS MOST. The runner GETs the
       test off wptserve — a `.sub.` file is a TEMPLATE, so its bytes on disk are not the test — and a server
       that answers nothing leaves it with nothing to run. It used to say so with an always-fatal `CHECK`, which
       is a claim about the ENGINE, and the cost was a whole run's tail: this driver's last whole-corpus run
       came back with storage, streams, url, wai-aria, webidl, webmessaging and xhr at 100 % aborted — 935 files
       — after wptserve stopped answering part-way through shadow-dom, and every one of them was reported as an
       engine crash. Re-running those files against a live server with the SAME binary passes them.
       READING THE MARKER IS NOT THE FIX AND MUST NOT BE MISTAKEN FOR IT: `@E` now lands in `fatal`, whose own
       row tells the reader to build an engine invariant that is not missing. The runner names which of the
       three states this is, at the site that knows; this line is the column it names.
       IT IS NOT GUARDED BY `!abortedHere` LIKE THE ONE ABOVE, and the difference is a fact about the emitter
       rather than a preference: the runner reports this and STOPS, so there is no test left to abort and no
       partial result to preserve — a file carrying both markers is a contract break, not a second diagnosis. */
    /* AND THE TAIL IS NO LONGER LEFT FOR A READER TO NOTICE. "If a whole tail of this run says the same thing,
       wptserve stopped answering" was the whole of the old remedy, and it is an instruction to a human to do
       what this driver can do in one request: ASK THE SERVER. A live server means this is a fact about the FILE
       and the run carries on; a dead one means this row and every row after it is about the server, and the run
       stops rather than spending the corpus on it. */
    const unservedTest = out.match(/^@WPTERR (.*): the corpus server did not serve this run's own file$/m);
    if (unservedTest) {
      const gone = await serverGone(rel);
      abortRun(area, "corpus", rel,
               `the corpus server did not serve this run's own file: ${unservedTest[1]} — nothing about the ` +
               "engine was measured. " +
               (gone ? gone.said + " — THIS RUN IS TRUNCATED HERE"
                     : `The server ANSWERED this driver's probe straight afterwards, so it is up and this file ` +
                       `is the fact: the corpus cannot supply it (wptserve's log: ${SERVER_LOG})`));
      if (gone) break;
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
    /* testharness's OWN Test.statuses, and the split is this gate's own §Testing rule applied one level down.
       This was `t.status === 0 ? pass : fail`, which folded FOUR verdicts into two and inflated exactly the
       column a reader compares two runs on. NOTRUN is the one that does the damage and it is not rare: it is
       what the harness stamps on every subtest a file never reached, so ONE subtest timing out early marks all
       its siblings — `clear-window-name.https.html` read 1 pass and 11 fails when what happened was 1 pass, 1
       TIMEOUT and 10 subtests that never ran and said nothing about this engine. A component that answered ten
       questions wrong and one that was never asked them are the same number in that column, which is the
       collapsed verdict this file's header refuses for aborts and kills.
       WHAT COUNTS AS A FAILURE IS WHAT THE ENGINE OWED AND DID NOT DELIVER: FAIL is a wrong answer and TIMEOUT
       is an answer that never came — both are this engine's. NOTRUN is a consequence of a sibling's verdict and
       PRECONDITION_FAILED is the TEST declining to run (testharness stamps it for an OptionalFeatureUnsupported
       throw), so neither is a measurement of the engine and neither may sit in `fail`.
       THE KIND IS NAMED ON THE LINE TOO. Every one of these used to print `FAIL`, so a subtest that hung and a
       subtest that computed the wrong string were the same row and the reader had to infer which from the
       message — and a NOTRUN carries no message at all, so it inferred as a silent wrong answer. */
    const ST = { 0: "PASS", 1: "FAIL", 2: "TIMEOUT", 3: "NOTRUN", 4: "PRECONDITION_FAILED" };
    /* WHERE THIS FILE'S OWN LINE GOES. It is written below, once every count is final, and SPLICED in here so a
       reader meets the file before its subtests rather than after them. */
    const lineMark = failures.length;
    let filePass = 0, fileFail = 0, fileNotrun = 0;
    for (const t of settled.values()) {
      const kind = ST[t.status];
      /* A STATUS THIS GATE DOES NOT KNOW IS NOT SILENTLY A FAILURE. testharness owns this enum and may add to
         it; counting an unknown code as a wrong answer is the defaulted-field defect — a plausible datum where
         there is none — so it is reported by NUMBER and counted with the failures it cannot be distinguished
         from, saying so on its own line. */
      if (t.status === 0) { filePass++; continue; }
      if (t.status === 3 || t.status === 4) {
        fileNotrun++;
        failures.push(`  ${kind === "NOTRUN" ? "NOTRUN " : "PRECOND"} ${rel} :: ${t.name}\n         ` +
                      (kind === "NOTRUN"
                        ? "the harness never ran this subtest — a sibling ended the file first, so it says " +
                          "nothing about the engine"
                        : "the test declined to run: an optional feature it needs is unsupported") +
                      ((t.message || "") ? `: ${(t.message || "").slice(0, 200)}` : ""));
        continue;
      }
      fileFail++;
      failures.push(`  ${(kind || "STATUS " + t.status).padEnd(7)} ${rel} :: ${t.name}\n         ` +
                    (kind ? "" : "testharness reported a subtest status this gate does not know — ") +
                    (t.message || (kind === "TIMEOUT" ? "the subtest never settled" : "")).slice(0, 200));
    }
    /* THE FIRST UNCAUGHT ERROR THAT IS STILL STANDING — and "still standing" is the whole of what this loop
       adds. A stream cannot withdraw a line it has printed, so solver/result.c corrects one by APPENDING
       `@WPTERR-RETRACTED` with the same payload: HTML §8.1.6.4 "HostPromiseRejectionTracker(promise,
       operation)" step 7.4 fires when the page attaches a handler in a LATER task, and the rejection this
       runner already reported turns out to be about a file that did nothing wrong. Quoting it as the reason a
       file errored is a diagnosis of something that did not happen, which is worse than no diagnosis.
       PAIRED IN ORDER, NOT SET-SUBTRACTED, because the producer's latch re-announces a pair after correcting
       it — reported, retracted, reported again stands ONCE, and set arithmetic would say zero. An unmatched
       correction is a BROKEN PRODUCER CONTRACT and throws: result.c prints RETRACTED only on the falling edge
       of a pair it announced, so an unmatched one means that latch is gone and this column has stopped
       meaning anything. (A killed run truncates the TAIL, so it cannot produce one.) */
    const standingErrs = [];
    for (const m of out.matchAll(/^@WPTERR(-RETRACTED)? (.*)$/gm)) {
      if (!m[1]) { standingErrs.push(m[2]); continue; }
      const k = standingErrs.indexOf(m[2]);
      if (k < 0)
        throw new Error("[wpt] an @WPTERR-RETRACTED line names a page error no @WPTERR line reported — " +
                        "solver/result.c prints the retraction only when the last STANDING occurrence of a " +
                        "(message, throw site) pair it already announced is taken back, so an unmatched one " +
                        "means that latch is broken: " + m[2].slice(0, 200));
      standingErrs.splice(k, 1);
    }
    const err = standingErrs.length ? standingErrs[0] : null;
    if (!abortedHere && err && !filePass && !fileFail && !fileNotrun) {
      /* NOT AN ABORT. The file ran and threw before registering a subtest — a result about the PAGE, where an
         abort is a capability the engine does not have. Folding it in is the very thing the column rule above
         forbids, and it is what made that column exceed the run count. */
      errored++; area.errored++;
      failures.push(`  ERROR   ${rel}\n         ${err.slice(0, 200)}`);
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
    /* AND THE KIND FOLLOWS THAT SPLIT RATHER THAN SITTING ABOVE IT. The four causes are not four flavours of one
       finding: the second is described here, in this file's own words, as THIS GATE'S BUG, so it is counted as
       `driver` and not among the file's own unfinished work. Leaving it in with the others would put a defect in
       the driver into the column a reader mines for engine work. */
    if (!abortedHere && !/^@WPTDONE /m.test(out)) {
      const hanging = [...started.keys()].filter((k) => !settled.has(k)).map((k) => started.get(k));
      const [kind, cause] =
        hanging.length
          ? ["nodone",
             `${settled.size} of ${started.size} subtest(s) reached a result; ${hanging.length} never did, so ` +
             `something they await never settled: ${hanging.slice(0, 8).map((n) => JSON.stringify(n)).join(", ")}` +
             (hanging.length > 8 ? `, +${hanging.length - 8} more` : "")]
        : started.size
          ? ["driver",
             `all ${started.size} of its subtest(s) reached a result and the harness STILL never completed — ` +
             "testharness's completion path did not reach this runner's report hook, which is this gate's own bug"]
        : err
          ? ["nodone", `it registered no subtest at all and threw on the way: ${err.slice(0, 200)}`]
          : ["nodone",
             "it registered no subtest and never completed — testharness.js itself never ran, or the report hook " +
             "is not installed in this run's programs"];
      /* AND WHAT IT PUT ON THE WIRE, which is the half of this diagnosis the driver used to have no way to ask.
         `hanging` names the subtests that never settled; `trafficEvidence` names what the corpus server was
         asked for and answered while they hung, so "awaiting something that never settled" stops being the end
         of the sentence and becomes a request that was never issued, never answered, or answered and dropped. */
      abortRun(area, kind, rel, `the harness never completed — no @WPTDONE. ${cause}` + trafficEvidence(traffic));
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
    /* PARSED ONCE, READ TWICE — by the verdict below and by the file's own subtest line after it. Two matches of
       one marker is two places for this file to disagree with itself about what the harness reported. */
    const d = out.match(/^@WPTDONE (\{.*\})$/m);
    let h = null;
    if (d) { try { h = JSON.parse(d[1]); } catch { h = null; } }
    {
      if (d && !h) {
        /* THE RUNNER'S OWN REPORT LINE, MALFORMED — a break in the contract between this driver and the program
           it built, so it is this gate's work and never the engine's. */
        abortRun(area, "driver", rel, `@WPTDONE carried no readable JSON: ${d[1].slice(0, 200)}`);
        continue;
      }
      const NAME = { 0: "OK", 1: "ERROR", 2: "TIMEOUT", 3: "PRECONDITION_FAILED" };
      if (h && h.status !== 0) {
        fileFail++;
        failures.push(`  FAIL    ${rel} :: <harness>\n         testharness status ` +
                      `${NAME[h.status] || h.status}${harnessCause(h, started, settled)}` +
                      /* AND THE WIRE EVIDENCE, ON *THIS* LINE TOO — because a harness TIMEOUT is the shape the
                         `nodone` branch above never sees. A file that awaits a fetch which never settles does
                         NOT usually vanish: testharness's own timer fires, `done()` runs, and the file reports
                         `@WPTDONE` with status TIMEOUT and whatever subtests it had. So it takes this path, not
                         that one, and attaching the traffic only to the missing-@WPTDONE case would have left
                         the instrument silent in precisely the case it was built for — the idlharness family,
                         every member of which reports a floor of two subtests and a TIMEOUT while awaiting
                         `/interfaces/<spec>.idl`. Asked only for TIMEOUT: an ERROR is an uncaught exception and
                         a PRECONDITION_FAILED is the test declining, and neither is a question about the wire. */
                      (h.status === 2 ? trafficEvidence(traffic) : ""));
      }
      /* `count` IS HOW MANY SUBTESTS THE HARNESS HOLDS, cross-checked because losing a line is the one failure
         mode a streamed report adds and it would make every number above wrong. Not asked of a file that
         ABORTED: a process killed between two @WPT lines really does hold more than arrived, and the abort
         line already says so. */
      if (h && !abortedHere && h.count !== settled.size) {
        fileFail++;
        failures.push(`  FAIL    ${rel} :: <harness>\n         the harness holds ${h.count} subtest(s) and ` +
                      `${settled.size} reached this driver — the report and the run disagree about the file`);
      }
    }
    /* THE FILE'S OWN SUBTEST COUNT, BESIDE ITS VERDICT, FOR EVERY FILE THAT RAN — INCLUDING THE ONES THAT PASSED.
     *
     * §Testing asks for exactly this line and names the reason: a test that is collected, runs, and reports is
     * still not a test that MEASURED anything, because its own fixtures can be unreachable — and then it returns
     * a SMALL HONEST NUMBER that reads as a small honest result. The whole idlharness family sat at exactly two
     * subtests apiece, one for `idl_test setup` and one for `<harness>`, having never executed a single IDL
     * assertion; each of the twenty-three contributed its two to a different area's total, where two is
     * unremarkable, and nothing anywhere printed the per-file figure that makes twenty-three identical floors
     * look like the one fact they are.
     *
     * IT IS PRINTED FOR CLEAN FILES OR IT DOES NOT CATCH THIS AT ALL. Every other line this gate emits is a
     * failure, an abort or an unread file, so a file that reports two passing subtests and stops emits NOTHING —
     * which is indistinguishable from a component with nothing to answer for. That silence was the whole of the
     * concealment, and a report that only speaks up about failures cannot break it.
     *
     * ITS COST IS ONE LINE PER FILE against a stream that already carries one per failing SUBTEST — `encoding`
     * alone answers three quarters of a million of those — so this is the small column in the report, not a new
     * expense. It is buffered into the AREA's lines like every other, so it arrives with the row it belongs to
     * and a truncated run still prints what it measured.
     *
     * NO FLOOR IS HARDCODED AND NONE SHOULD BE. What counts as suspiciously few subtests is a fact about a test,
     * not about this driver, and a threshold here would be a bound that goes stale the first time a family's
     * floor is three. The count is REPORTED; the reader is who notices that a family shares one. */
    {
      const total = filePass + fileFail + fileNotrun;
      /* THE HARNESS'S OWN VERDICT, from the one parse above. A status this table does not name is printed as its
         NUMBER rather than folded into a word — testharness owns that enum and may add to it, and a status
         reported as something it is not is the defaulted-field defect on the line built to expose one. */
      const hstat = h ? ({ 0: "OK", 1: "ERROR", 2: "TIMEOUT", 3: "PRECONDITION_FAILED" }[h.status] ?? `status ${h.status}`)
                      : "none";
      failures.splice(lineMark, 0,
        `  RAN     ${rel}  subtests ${String(total).padStart(5)}` +
        `  [pass ${filePass}  fail ${fileFail}  notrun ${fileNotrun}]  harness ${hstat}` +
        /* SAID ON THE LINE ITSELF, because a count from a file that stopped early is not the file's surface and
           a reader scanning this column must not have to cross-reference the ABORT line to learn that. */
        (abortedHere ? "  — THE FILE ABORTED, so this is what it reached and not what it holds" : ""));
    }
    pass += filePass;
    fail += fileFail;
    notrun += fileNotrun;
    area.pass += filePass;
    area.fail += fileFail;
    area.notrun += fileNotrun;
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
    if (abortedHere) {
      area.abPass += filePass; area.abFail += fileFail; area.abNotrun += fileNotrun; area.abFiles++;
    }
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
/* THE TRUNCATION, STATED WHERE THE ROWS END AND AGAIN ON THE LAST LINE — because the block a reader quotes is
   never the one they scrolled past. It carries the REVISION PAIR and the CORPUS revision beside the verdict,
   for the reason §Testing gives: a result quoted without the revision it came from is not a measurement, and a
   TRUNCATED result quoted without one is worse, since the number it carries is not even about a whole corpus.
   It names the run it reached, which is what turns "the tail is all corpus rows" into a place to look. */
if (g_truncated) {
  console.log("");
  console.log("  ================ THIS RUN IS TRUNCATED — IT IS NOT A MEASUREMENT OF THE CORPUS ================");
  console.log(`  ${g_truncated.said}`);
  console.log(`  it reached ${g_truncated.rel} — run ${attempted} of ${runs.length}, so ` +
              `${runs.length - attempted} run(s) were NEVER ATTEMPTED and are absent from every column below`);
  /* THE `qjs` CLAUSE THAT USED TO SIT BETWEEN THESE TWO IS GONE WITH THE FIELD. `engine/qjs` is tracked
     content since the subtree merge, so the engine has no commit of its own and `head` names the whole
     program; `corpus` is the WPT checkout, which IS a separate repository and keeps its own. */
  console.log(`  engine ${REV_AT_START.head}   corpus ${CORPUS_AT_START.head}`);
  console.log(`  wptserve's own log is at ${SERVER_LOG} — its last lines are why it stopped`);
  console.log("  the rows below are the runs that DID happen; nothing here says anything about the rest");
  console.log("  ===============================================================================================");
}

console.log("  ---- summary");
{
  const names = [...areas.keys()].sort();
  for (const n of names) {
    const a = areas.get(n);
    /* AN UNFINISHED AREA IS THIS DRIVER MISCOUNTING — UNLESS THE RUN WAS TRUNCATED, in which case it is exactly
       what a truncated run looks like and reporting it as broken accounting would be a confident wrong
       diagnosis of the gate's own making. Its held failure lines are flushed here rather than dropped: they are
       results the run already had, and losing them to the truncation would be the "same failures with the count
       hidden" shape twice over. */
    if (a.done !== a.expected || a.lines.length) {
      if (!g_truncated)
        throw new Error(`[wpt] area ${n} finished ${a.done} of ${a.expected} runs with ${a.lines.length} ` +
                        "unreported line(s) — the per-area accounting is wrong, so this table cannot be trusted");
      for (const l of a.lines) console.log(l);
      a.lines.length = 0;
      console.log(`  ${n} — PARTIAL: ${a.done} of ${a.expected} runs, the rest never attempted`);
    }
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

/* THE DECLARED-FIXTURE BLIND SPOT, AS A SIZE AND ON EVERY RUN. See `g_mediaBlind`. It is printed BEFORE the
   refused-fixture census below and apart from it, because that one is a FINDING with an exit status behind it
   and this one is a statement about what was not looked at — and a reader who meets them in one block reads the
   second as the first. */
{
  const n = [...g_mediaBlind.absent.values()].reduce((a, b) => a + b, 0);
  if (!n && !g_mediaBlind.unresolved) {
    console.log("  ---- declared media fixtures: every <video>/<audio>/<source>/<track> src a collected " +
                "document names resolves to a file in this checkout");
  } else {
    console.log(`  ---- BLIND SPOT — ${n} declared media fixture reference(s) in ${g_mediaBlind.files.size} ` +
                `document(s) name a path this checkout does not have, and ${g_mediaBlind.unresolved} more could ` +
                "not be placed at all. This does NOT fail the gate: the file still runs and still reports, so " +
                "its answer is WRONG rather than absent — which is exactly why it is counted here instead of " +
                "being summed into a failure total. A positive media test whose fixture 404s fails with a " +
                "message blaming the policy.");
    for (const [p, c] of [...g_mediaBlind.absent.entries()].sort((a, b) => b[1] - a[1]))
      console.log(`       ${String(c).padStart(4)}  /${p}`);
  }
  /* THE FLOOR, SAID WHERE THE NUMBER IS. A count a reader cannot price is read as a total. */
  console.log("       (a FLOOR: this reads a <script src>, a // META: script= line and a media element's src, " +
              "and nothing else — a fixture reached by a runtime fetch(), an <iframe src>, an <img src> or a " +
              "?pipe= handler is outside every channel here)");
}

/* AND WHAT THE CORPUS SERVER REFUSED FOR A PATH THIS CHECKOUT DOES NOT HAVE — the fixture question asked of the
   RUN rather than of a list of ways to declare one. See `corpusFixtureLedger` for why the traffic is kept past
   the file that generated it; this is what it is kept FOR.
   TWO FILTERS, IN THIS ORDER, AND THE FIRST IS THE WHOLE REASON THIS IS NOT A FALSE-ALARM MACHINE. A non-2xx is
   not by itself a defect: a `.py` status handler's entire job is to return one, and plenty of tests ask for a
   404 on purpose. So a path that IS ON DISK is the SERVER'S own answer and nothing this gate has to say about
   it. What survives is a path this checkout cannot serve out of any file, which is a different sentence.
   THEN GIT SPLITS THE REMAINDER INTO THREE FINDINGS THAT MUST NOT READ ALIKE, through the same `corpusHas` the
   META-script and `.idl` reports use, so all three agree about which revision they ask at. A path the PINNED
   REVISION HAS is a checkout this gate can fix, and the entry that would supply it is named. A path it does NOT
   have cannot be supplied by any entry — it is the test's own subject, or a reference the pinned corpus does not
   satisfy, and widening WPT_PATHS would do nothing either way. And git DECLINING to answer is the third thing,
   written out rather than folded into the second, because "the corpus does not have it" is a positive claim and
   an unanswered question is not evidence for it.
   ONLY THE FIRST ARM FAILS THE GATE, AND THE ASYMMETRY IS THE ARGUMENT FOR IT. A refused fixture the corpus HAS
   is the same fact as a META script this driver could not hand over, which already aborts its file: the test ran
   against a corpus it was not written for, so its number is a statement about WPT_PATHS and not about the
   engine. The second arm is printed and not counted, because failing on it would be a verdict this driver cannot
   support — nothing here can tell a deliberate 404 from a broken reference, and a gate that stops the tree on a
   guess is worse than one that prints it.
   IT PRINTS ON THE CLEAN DAY TOO. A line that appears only when something is wrong is one nobody learns to look
   for, and its silence then reads as a clean bill rather than as a question nobody asked — which is exactly how
   a family of tests can report a floor of two subtests for as long as nobody prints the floor. */
{
  const refused = [...g_fixture.entries()]
    /* EVERY status non-2xx, never ANY: a path answered 200 once and 404 once was served, and the 404 is that
       test's own business. */
    .filter(([, e]) => [...e.statuses].every((s) => s < 200 || s >= 300))
    .filter(([p]) => !existsSync(join(WPT, p)))
    .sort((a, b) => b[1].n - a[1].n);
  const supply = [], cannot = [], unknown = [];
  for (const [p, e] of refused) {
    const has = corpusHas(p);
    (!has.known ? unknown : has.present ? supply : cannot).push([p, e, has]);
  }
  g_refused = supply.length;
  /* THE DENOMINATOR ON THE SAME LINE AS THE FINDINGS, because a count of findings falls both when defects are
     fixed and when the instrument stops looking. How many distinct paths the server answered at all is what
     says which of those two a smaller number is. */
  console.log(`  ---- corpus fixtures: the server answered ${g_fixture.size} distinct path(s) this run` +
              (g_trafficUnvouched ? `; ${g_trafficUnvouched} run(s) whose traffic slice could not be vouched for` +
                                    " — an absent measurement for those, not an absence of requests" : "") +
              (g_fixtureUndecodable ? `; ${g_fixtureUndecodable} response path(s) carried an escape this driver ` +
                                      "could not decode and were NOT judged" : ""));
  if (!refused.length)
    console.log("       every path answered outside 2xx is present on disk, so each of those statuses is the " +
                "corpus's own answer and none of them is a fixture this checkout is short of");
  if (supply.length) {
    console.log(`       REFUSED — ${supply.length} path(s) a test asked for, answered outside 2xx, absent from ` +
                "this checkout and PRESENT at the pinned revision. The test ran against a corpus it was not " +
                "written for, so its numbers are about WPT_PATHS. This FAILS the gate.");
    for (const [p, e, has] of supply)
      console.log(`       ${String(e.n).padStart(4)}  /${p}  [${[...e.statuses].sort().join(",")}]  first asked ` +
                  `while running ${e.by} — it EXISTS at ${has.at}, so WPT_PATHS is short of ${dirname(p)}`);
  }
  if (cannot.length) {
    console.log(`       ${cannot.length} refused path(s) do NOT exist at the pinned revision at all, so no ` +
                "WPT_PATHS entry can supply them: each is a test's own subject (a deliberate 404) or a " +
                "reference the pinned corpus does not satisfy. Reported, not counted — see the block comment.");
    for (const [p, e] of cannot.slice(0, 20))
      console.log(`       ${String(e.n).padStart(4)}  /${p}  [${[...e.statuses].sort().join(",")}]  first asked ` +
                  `while running ${e.by}`);
    if (cannot.length > 20) console.log(`       … and ${cannot.length - 20} more`);
  }
  for (const [p, , has] of unknown)
    console.log(`       UNDECIDED  /${p} — ${has.why}, so this driver does not know which of the two above it ` +
                "is and is not guessing");
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
/* AND WHAT THE ABORTS WERE, WHICH IS THE QUESTION THIS COLUMN USED TO ANSWER BY MAKING THE READER CLUSTER THE
   LOG. `aborted-runs 25` was 21 capability gaps, 2 missing corpus helpers and 2 CPU-budget kills, and finding
   that out took a hand pass over a thousand lines — for a number whose whole purpose is to say how much engine
   work is waiting. Each row here is a decomposition of the total printed below, asserted to sum to it, so a kind
   that does not appear is zero and provably zero.
   A POSITIVE STATEMENT WHEN THERE ARE NONE, for the reason the UNDECIDED census carries one: "no run aborted" is
   a finding, and a block that simply vanishes reads the same as a block that was never written. */
{
  const total = [...g_abKind.values()].reduce((a, b) => a + b, 0);
  if (total !== aborted)
    throw new Error(`[wpt] ${aborted} abort(s) counted and ${total} classified — abortRun is the only site that ` +
                    "may count one, so this driver's own accounting is broken");
  if (!aborted) {
    console.log("  ---- no run aborted");
  } else {
    console.log(`  ---- ${aborted} aborted run(s), classified by what THIS DRIVER KNOWS about each — never by ` +
                "matching the text of an abort message:");
    for (const k of Object.keys(ABORT_KINDS)) {
      const n = g_abKind.get(k);
      if (!n) continue;
      console.log(`       ${k.padEnd(7)} ${String(n).padStart(4)}  ${ABORT_KINDS[k]}`);
    }
    /* THE ONE THAT MUST NEVER BE READ AS A ROW AMONG ROWS. An unclassifiable abort means a kind this driver does
       not model has arrived, and the whole point of the split is that it did not land silently in `gap`. */
    if (g_abKind.get("UNKNOWN"))
      console.log(`       ^^^^ ${g_abKind.get("UNKNOWN")} ABORT(S) THIS DRIVER COULD NOT CLASSIFY. Each names ` +
                  "what it observed; a kind this gate does not model is a hole in the instrument and is fixed " +
                  "here, in engine/wpt.mjs, before its number is read as anything else.");
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
/* `subtests` IS WHAT THE ENGINE WAS MEASURED ON, so it is pass+fail and not pass+fail+notrun: a NOTRUN subtest
   produced no verdict about this engine, and adding it to the denominator would move the ratio every time an
   unrelated sibling timed out. It is printed beside them, never inside them.
   IT IS NOT AN EXIT CONDITION EITHER, and that is not leniency: a NOTRUN is always DOWNSTREAM of something that
   already fails this gate — the timeout, abort or harness error that ended the file first — so failing on it
   again would report one defect twice and make the count of failing areas depend on how many siblings each
   defect happened to strand. */
/* AND `aborted-runs` CARRIES ITS SPLIT ON THE LINE ITSELF, because this is the line that gets quoted. The block
   above scrolls with everything else; what ends up in a report, a commit message or a message to another agent is
   this one, and a bare sum there re-creates the defect one line below the fix — two runs whose 25 are 21/2/2 and
   4/19/2 compare equal on it. Same order as the block, same assert. */
/* `runs` IS WHAT WAS ATTEMPTED, NOT WHAT WAS COLLECTED, AND THE SHORTFALL RIDES THE SAME LINE. This is the line
   that gets quoted into a report, a commit message or another agent's context, so a truncated run whose last
   line reads like a whole corpus is the defect back at full strength however loud the block above it was. */
console.log(`  files ${files.length}   runs ${attempted}${attempted === runs.length ? "" : ` of ${runs.length}`}` +
            `   subtests ${pass + fail}   pass ${pass}` +
            `   fail ${fail}   notrun ${notrun}   aborted-runs ${aborted}${abortSplit(g_abKind, aborted)}` +
            `   errored-runs ${errored}` +
            `   unreadable-runs ${unread}   undecided-files ${g_undecided}` +
            /* ON THE QUOTED LINE, because this is the line that ends up in a report or another agent's context,
               and a fixture the checkout could not serve is a statement about what these numbers MEASURED. */
            `   refused-fixtures ${g_refused}` +
            /* ON THE QUOTED LINE AND NOT IN THE EXIT STATUS, which is the whole distinction this column
               exists to carry: a reader quoting this line gets the gate's FINDINGS and its BLIND SPOT side by
               side, and can tell which of the two a number is. */
            `   unserved-media ${[...g_mediaBlind.absent.values()].reduce((a, b) => a + b, 0)}` +
            (g_truncated ? `   *** TRUNCATED at ${g_truncated.rel}: ${runs.length - attempted} run(s) never ` +
                           "attempted — this is NOT a corpus measurement ***" : ""));
console.log("===========================================================");
/* A TRUNCATED RUN FAILS THE GATE WHATEVER ITS COLUMNS SAY. Its numbers are a strict subset of a corpus and the
   thing that stopped it is a defect in how this gate is being run, so a green exit here would be the gate
   certifying a measurement it did not take. */
process.exit(fail || aborted || unread || g_undecided || g_refused || g_truncated ? 1 : 0);
