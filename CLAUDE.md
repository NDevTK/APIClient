# APIClient — working spec

This file is the spec every agent receives at spawn. It holds RULES only. It carries no status, no incident narrative, no counts, no commit SHAs and no file:line coordinates, because every one of those is true when written and wrong soon after, and a stale line here is read as the standing description of where the work is.

The full history of how these rules were learned — every incident, measurement and retired argument — is preserved verbatim in `docs/claude-record.md`. It is not injected into agents; read it on demand. A comment in the tree that cites `CLAUDE.md §SOME-NAME` names a record in that file: grep it there. When a rule here seems wrong or under-specified for your case, grep the record for your ARTIFACT (the file, subsystem or concept you are touching), not for the principle — the record that names your exact artifact is the one worth reading.

This file has a size budget enforced by `engine/mdgate.mjs`. A new lesson goes in as one rule sentence in the right section; its incident goes in the commit message or at the code site it is about. If a rule cannot be stated in a few sentences, the code should probably assert it instead.

## The project

A browser with a BFS time-travel solver. Run a page's UNMODIFIED bundle on a patched QuickJS (with Lexbor as the DOM) under forced multi-path execution, and produce two views from one run: the app's API surface (including interesting UNUSED endpoints with computed example keys and values), and XSS/security verdicts proven by firing. The ideal is to learn the logged-in API surface while logged out, because SPAs ship the same bundle to everyone.

Every change must satisfy two viewpoints at once:
1. A Chromium/Blink engineer: one problem per file, offensive programming, Web IDL, established engine tooling and names.
2. A solver engineer: no bounds, BFS time travel, per-flow isolated state that still models shared and interprocedural code, solving example values, XSS payloads and conditionally-loaded JS.

## Disposition

- Best long-term design over speed, safety, passing tests and existing code. Never paper over a problem. Land at least one concrete diff per turn; a probe that localises a trap, or a verified narrowing, counts. Context length is not a stop signal.
- Biggest mechanism change first. A task's size is never a reason to defer. The bigger the goal, the smaller the first concrete diff — decompose and land the first member this turn.
- Regressions are acceptable and expected when replacing the wrong system. Never revert a correct fix to dodge one. No bulk sweeps.
- A superseded system is deleted in the same diff — never kept as a fallback, safety net or `if`. The new system stands alone and crashes where incomplete.
- Fix the root. Make an impossible state impossible, then delete the now-dead workaround. Do subproblems in order; never jump ahead of a subproblem.
- Comments are not follow-ups: if code is wrong now, it crashes (DCHECK/DFAIL) or it gets built. The one exception is a NAMED RESIDUAL — code that is correct but narrower than the spec — stated at the site with three clauses: what is not covered, what the next diff builds, and how its absence would show (an observation, never a specific instance). A residual is retired, and marked met, in the same commit that builds its thing.
- Push back. Agreeing because something was asserted — by the user, a lane, a coordinator or the existing code — rather than because it is spec-true is a design failure. Prove disagreements against the spec or the code.
- Deleting non-perfect code, riskiest first, is valued. Dead-code removal alone shows nothing.
- A legacy path not designed for suspend/resume is an unbuilt capability: convert it so the old body is deleted and the C entry becomes a DFAIL naming the route to fix. Never invent a counter or report that keeps a suite green while the old path runs.
- Avoid the word "genuinely".

## Shared checkout: git and process safety

Several agents share one working tree, one index and one machine. These rules are absolute.

- Banned: `git reset --hard`, `git checkout -- <paths>`, `git stash`, `git clean`, a private index (`GIT_INDEX_FILE`), `git add -A`, `git add .`. They destroy or sweep other agents' uncommitted work with no record.
- Never compose a read and a write of one file in one expression (`open(p,"w").write(open(p).read()…)`, or a redirect onto its own source): it truncates before reading. Read fully into a variable, then write. Restore a tracked file with `git show HEAD:<path> > <path>`.
- Stage and commit as one uninterrupted operation, and stage only your own hunks: `git diff -- <files> | git apply --cached -`, choosing hunks by content, never by position. The commit is bare: no `-q` (it prints no SHA) and no pathspec (it re-reads the working tree). Use `git commit -F <message-file>` with a lane-named message file. If the commit fails, retry immediately with nothing in between.
- Take the SHA from `git commit`'s own output, capture it once, never re-derive it, never use `HEAD` for it. Then read it back with `git show --stat <sha>` and compare the counts against what you staged.
- Push to both refs: `main` and the session branch. Publish with `engine/publish.mjs <sha> --expect <n>`, which fetches, inspects the range, refuses a split correcting pair, pushes and verifies by content. A command containing `git push` contains nothing else.
- A push publishes an ancestry. The publishable set is the prefix ending at the oldest commit whose lane has not reported, and never a cut between a lane's commit and a later commit of its own that corrects it. A lane's commit is published when the lane reports, not when it appears.
- Never rewrite pushed history to fix attribution; state a mis-attribution in the next commit message and in the report. A swept-in diff that is coherent is pushed as it stands.
- Before believing any file, diff or count, establish the checkout's relationship to `origin/main` (`git rev-list --left-right --count origin/main...HEAD`); behind and ahead take opposite bases. Read tree claims at `origin/main` (`git show origin/main:<path>`).
- When the stop hook reports uncommitted changes, attribute them by content to the agents that own them. Commit only your own; report the rest. Never commit a live lane's in-flight work.
- Name every scratch file and snapshot after the lane and revision, never after its role. Verify a commit from git, not from a scratch file.
- An untracked `.c` under `engine/host/` is compiled. Put scratch copies in the scratchpad.
- Address processes by PID, never by `pkill -f`/`pgrep -f` pattern. Attribute a process by its parent chain. Gate any destructive step on a check's exit status (`if check; then destroy; fi`), never `check; destroy`. Probe liveness by PID, not by output side effects.
- A backgrounded job's completion notice is about its wrapper. Wait on the PID, then read the job's own artifact and terminal line. A pipeline's status is its last stage's. A turn must not end with a backgrounded process as its only outstanding item.
- After any rejected or interrupted tool call, verify what the tree actually contains before claiming anything about it.
- Writable disk is a per-session allowance; on ENOSPC delete large stale artifacts (dead snapshots are found by marker and liveness, not by name).

## Claims, evidence and measurement

Most wasted work in this project came from believing something that had stopped being true, or had never been checked. These rules apply to your own findings as much as anyone else's.

- A claim about this tree is checked before it is acted on and re-checked before it is relayed: grep the entry a crash, comment, residual, brief or summary names, at `origin/main`. A DFAIL stays right about the spec and goes wrong about the tree. A name in a summary is the coordinate most likely to be invented.
- A relayed finding says where to look, never what is there. When briefing, state how each claim was obtained — fetched, grepped at a named revision, measured with a stated command, or inferred — and paste predicates and commands rather than paraphrasing them. When a lane refutes its brief, that is the mechanism working; record the correction where the claim was made.
- A remedy clause ("what to build") is a hypothesis; the spec half beside it is evidence. Before obeying a clause, read the callers of the site it names, fetch the section it cites, and check the population it assumes.
- A section number or title is fetched before it is written or relayed, including in a checklist. A quotation is pasted from fetched bytes. Verify against the edition the corpus records as its base. Levelled standards renumber; cite the level.
- A count names its unit and the command that produced it. Count constructs, not names (`DFAIL\(` not `DFAIL`); count occurrences (`grep -o | wc -l`) where lines are long; run prose greps with `-i`; use fixed-string search for literals with regex metacharacters; read the exit status (2 means the pattern was refused). State a sweep's result as a floor, name what it could not see, and prefer a runtime invariant over a textual sweep.
- An empty search result suggests a mis-addressed question first: grep the symbol with no path. `git grep` does not descend into gitlinks. Function-pointer tables, registries and macros do not spell the callee at the use.
- A negative probe needs an armed positive control and an invented negative control, run together, in the artifact the claim is about (a built binary shows what compiled; a log shows what fired).
- Read a counter's accessor before quoting it. A gauge and a lifetime counter cannot be summed or differenced together; a rate is a difference between samples, not a quotient of totals; a high-water mark plateaus like a ceiling. Read a series in file order, take the terminal census, sort numerically, and say which census you read.
- An absent value and a zero are different facts. A zero has several readings; count the arms that can produce it, including a callee's failure arm, and name which reading the evidence picks. Rows behind one gate are one reading, not several.
- A conservation identity holds within one sample. Check enumerations against the total printed beside them.
- A prediction states its reachability witness and the artifact it is read from. An absent crash is not a correct value; an unreached site leaves a prediction unscored, not confirmed. Score predictions by the construct's identity, not its line number. A stage that declares itself one interleaving is not a verdict about the revision.
- One live run is not a measurement. Live sites vary and real runs are bimodal; compare a crash's identity, a within-sample identity, or a count that cannot be true — never totals across runs whose terminal events or job counts differ. A fixture's workload proportions describe the fixture, not real pages.
- A gate, scan or build runs from a frozen snapshot at an explicit SHA (`engine/frozen_snapshot.sh`), and its result is quoted with that revision. Read an artifact's stamp, build mode and distance from the tip before trusting what it says about a revision.
- Read a run's whole log the first time; search the body for a refusal before theorising about a surprising number. A finding total is read beside its judged/verified denominator.
- A log is not final until its writer has exited; a final log can still be truncated or the output of a different mode. Check the terminal verdict and your own invocation.
- Retracting a method means re-running everything that method concluded.
- Your own measurement goes stale the moment someone fixes what it found. Re-derive it before relaying it.

## Offensive programming — `engine/host/check.h` (JS mirror: `extension/check.js`)

- Every dev-build crash is correct. It is either an unexpected state, to be made impossible at the root, or a missing feature, to be built. Never caught, swallowed, clamped or defaulted past. A page's own uncaught throw aborting boot is intentional.
- `DCHECK(cond,msg)`/`DFAIL(msg)`: dev-only invariants about this codebase's own logic; side-effect-free; compiled out in release. `CHECK`/`CHECK_FAIL`: always fatal, for OOM, physical floors, security boundaries and data integrity. When a diff makes a DCHECKed pointer load-bearing in release, promote the guard.
- Assert every invariant eagerly at its origin. Too few asserts is the failure.
- Assert only on values this codebase computed. Bytes a page, server or attacker supplied are input: refuse them with the spec's error, never assert on them. In the @S half the analysed page's own output is input.
- An assert must be able to fail: name both operands, and place it where both are in scope. A message conceding the state is legitimate is evidence the assert is wrong. Two seams over one primitive that disagree about an assert: the silent one is usually right.
- A shared helper reports its own line for every caller, so pass the caller's `__FILE__`/`__LINE__` to the check; invariants over transitions are macros expanded at each site.
- A consumer never defaults a producer's field (`|| 0`, `?? d`, `?.`, `catch {}`). DCHECK its presence and shape. A name read somewhere and written nowhere is a broken contract. When reviving a dead path, check what it would write.
- A predicate answering two questions is decided by the stricter one; split it into two predicates over one fact. A constant indexing two namespaces is two questions.
- A `default:` arm over a value this codebase enumerates is a guard, not work.
- An invariant over a gated operation counts the ask, not the outcome.
- Asserts must not rely on a structural property both populations share; record the act instead.
- The release arm beneath a DFAIL is a shipped path: design release arms of one algorithm together, and have the downstream half guard its own precondition.

## Architecture — the C engine is the browser

- Lexbor (DOM) and QuickJS (JS) in C own all semantics: parsing, execution, the forced-exec scheduler, endpoint/@S detection, document identity, frontier logic. Components are isolated per document and flow (COW).
- JavaScript is only the irreducible platform edge, per SECURITY.md: the trusted `safeFetch` chokepoint, IndexedDB/`chrome.*` relays, WASM loading, and popup rendering. Working JS is not moved into C by rule; what belongs in C is what a flow needs mid-execution (it must fork per arm and park with the flow). Type sniffing stays in `safeFetch`. No per-file "becomes C" ledger.
- Every bridge edge DCHECKs the engine↔JS contract.
- Use established browser and engine names and tools: Chromium's DCHECK/CHECK, QuickJS's dump infrastructure (`JS_DumpValue`, `JS_DumpGCObject`, `JS_DUMP_*`), gdb, targeted ASan. No printf probes. Score sanitizer runs on `ERROR:` with no tool name (LeakSanitizer writes its own prefix).
- The engine is a clean QuickJS-ng delta at `engine/qjs` (tracked content, synced to upstream); the host/scheduler lives outside it in `engine/host`.

## Browser half — follow the spec, build like a browser

- Every semantic traces to a spec section, implemented as a component named like a real browser's, with a DCHECK on the spec invariant. A citation carries its section number and its title (`HTML §13.2.5.43 comment start state`), verified against fetched text. Run `engine/citegen.mjs` on what you write.
- A citation is owed to the document the standard's editors maintain (the Editor's Draft or living standard; a Recommendation only where editors have stopped). A sub-step number is written only where the step holds one list; count list items with depth tracked.
- Spec order and narrow edge behaviour are the spec: event loop ordering, insertion modes, script order, percent-encode sets, and the rest. Most "solver gaps" are the browser behaving slightly wrong.
- A per-realm fact is answered per realm: per-realm class protos built eagerly at realm creation through one list every realm uses. Never a module static.
- No stubs. A shape-only object or a bare opaque where the spec computes a value is a gap; an unbuilt API is honestly absent. Before installing an interface, check how real bundles feature-detect it: a partial install can flip a guard true and abandon a working fallback, so land the smallest diff that makes the guard's true branch survivable.
- Web IDL is a gap auditor (`engine/idlgen.mjs`), not a stub generator. Extended attributes modify an algorithm rather than replace it. Prefer an existing corpus (WPT, test262) as the oracle over a new auditor, but read its assertions.
- Model state without devices: mock-backed IO that round-trips (FSA, media, matchMedia, getComputedStyle). Opaque is reserved for the unknowable.
- A dispatch deciding what bytes are (which document/parser/decoder) is one component every entry point calls, with a consumer-side assert.
- An operation that becomes a work item takes its inputs with it; re-derive every field from the spec's order when making something asynchronous.
- A capability materialized per flow needs reclamation through the spec's destroy algorithm, never a flag that announces an outcome.

## Solver half — forced multi-path concolic execution on time-travel primitives

- Every value is concolic: source provenance, a constraint domain, and a concrete example when one is computed, pinned or learned. There is no binary opaque sentinel. The example propagates because the engine runs the real op on it — never a parallel tagged-value class or a recorded transform-expression.
- Forced multi-path execution plus a per-flow path constraint over inputs only. Concrete execution grounds shared state. Where the domain permits both outcomes both arms run; a contradicted arm is pruned; uncertainty keeps the arm. Concretize-on-pin. Retiring an arm needs an observation that falsified its premise, not merely one consistent with it. Count completions by observable outcome, including the one where nothing settles.
- A domain's facets must be able to express what the algorithm asks. A fork whose arms are chosen by what is cheap to mint, rather than by the spec's partition, is a narrowing.
- @H never invents: an equality-pinned value is concrete; a range/prefix-gated value is a domain-annotated shape carrying both provenance and domain. @S solves freely because every PoC is fire-verified.
- Re-execution discharges constraints: no external SMT, no taint tracker, no transform-expression, no state merging, no function summaries. Opacity survives numeric coercion.
- Encoding builtins (JSON, URI, base64, escape) run the real codec; a builtin that can throw forks success and throw arms.
- Learning from replies is the point: consumed replies are always fetched and JS bodies executed. Safety lives in `safeFetch`, never in the engine.

## Scheduler — BFS and time travel

- Every code flow, at any depth, is suspend/resumable; nothing is driven to completion. A flow is a path isolated by its COW delta and captured as a snapshot.
- No bounds: no depth, step, time, run, memory, recursion or no-progress cap, watchdog, seen-set or fixpoint. Only emitted output proves a flow done. A rest granularity is a scheduler-owned policy at the unit the spec names, never a bound.
- One WFQ policy at both levels: accumulated emitted value, the flow's search fitness, a UCB optimism bonus, minus CPU aging. Order only, never dropping work. A fork is rank-neutral (asserted over the whole weight), and two arms forked at different instants must read a term the same — a share of something the family owns lives on the account; a fact about the executed prefix lives on the flow. A term whose denominator a departure can lower rewards departing. Credit and debit are kept at the same accounting unit.
- Level 2 orders flows within a document (preemptible per opcode); Level 1 orders per-page engines. One global, continuous, cross-session frontier persisted in IndexedDB, never reset. A drive of uncalled code carries a cross-session name for the function, and any recorded ordinal over a mutable set names the member, not its position.
- There is no grind and no second scheduler; orphan driving and @S candidate re-fires are ordinary flows on the one frontier.
- Attention: a per-opcode return to the scheduler, with a value yield and a cooperative-quantum yield measured in CPU where the host can, from an asynchronous source. A resume that is not byte-identical is a cap.
- Every runtime job is a scheduler flow; there is no job-queue drain. Awaits deliver settled values or rethrow. Code-loading async always executes.

## State isolation — per-flow COW delta

- Each flow owns the heap slots and DOM nodes it wrote, layered over a shared baseline. Context switches swap deltas; any number of flows interleave mid-write. The delta captures mutations and creations across the JS heap and the Lexbor DOM — one primitive for isolation, snapshot, eviction and resume.
- The document is per-flow. The browser owns the DOM API; the solver (`solver/dom_cow.c`) owns time-travel state, captured at writes.
- A component's own C record time-travels via `cow_capture_host_record` in its accessor, whose layout matches its finalizer. Raw byte capture is for POD latches only.
- Platform data a flow queues is a JS value, never malloc'd C.
- Only shared baseline state is captured; flow-local creations are skipped. Iterations over opaque input fork per iteration. Large shared accumulation is a chain of per-iteration flows. Async state rides the delta.

## The C stack is not a limit — trampolining and hooking

- All calls trampoline onto the heap stack; overflow on an unhooked path crashes loudly.
- Call-site-resolved builtins (`.bind/.call/.apply`, spread, Proxy traps with their invariants) are resolved at the operator site.
- Continuation-holding builtins (array callbacks, `replace`, `stringify`, getters, reactions) are step machines declared at their definition (`JS_CFUNC_STEP_DEF`) and driven from the one C-function invocation point. The legacy `JS_Call` loop body is deleted in the same diff. No recognizer, allowlist or per-call-site predicate; an unconverted path is a `@WHY`. Widen one consumer at a time and build what the failures name.
- A step state is complete before anything can throw; a callback frame owns its arguments; exercise callbacks with a rest parameter. No data pointer in `JSCFunctionType`. A caller-supplied sentinel is supplied by a definition macro, not by convention.
- Generator/async drivers self-resume through `async_func_resume_run` on a preempt. Boot is the first forking flow over a pre-boot baseline.

## Time-travel resume

- A parked flow resumes from its snapshot byte-identically at any depth, hot or cold, across sessions, by global value order. A resume re-derives example values from current sources.
- RAM pressure relocates the cold tail to IndexedDB. At disk, residency is the strict prefix of the value order within a configurable share; re-derivable entries may be shed; the only copy of unrecomputable work is never shed.

## @S — the PoC is constructed and proven by firing

- The breakout is derived from the real sink parse context (`solve_html.c`, `solve_js.c`), never a payload table. Solve path, context breakout, filter survival and firing jointly, from the real re-executed sink string and observed byte provenance.
- Only firing proves a PoC. Absence of a PoC is a parked search, never a "safe" verdict. Distance-directed search keeps a per-flow fitness comparator separate from the reward ledger, with an observation site before each rung.
- Cross-flow state is solved by re-running the producer. Execution environment (CSP, Trusted Types), stored/second-order, DOM clobbering and prototype pollution are part of the PoC. Live verify runs the engine's exact PoC; real Chrome is the oracle.

## Attacker sources, values and discovery

- Each attacker source carries its real browser constraints (origin forgeability, per-component percent-encoding). Deliver candidates through the source's real transform.
- Run, don't match: values are computed by QuickJS, never pattern-matched. Absent injected app state is symbolic; app-owned resources are loaded and executed; config loaded for examples still forks branches.
- Active discovery from the learning document, CORS-bounded and one per endpoint. Static assets are not endpoints.
- A request carries the provenance of its values (observed, derived, forced) and its reply inherits it; forced replies are never merged into observed data.
- Egress is a policy in the trusted zone, configurable per origin, over computed signals (method, credential state, provenance, URL-carried authority, credential lineage, destination, intended invalidity). An undetermined signal is shown as unknown. The unconfigured default fires the page's own program and subresource loads and page-named child navigations as a browser would, same-origin and credentialed; everything else needs the person to widen the origin. A destructive-path deny list is a floor under the tool's own autonomy only, never over an operator's explicit act.
- Passive discovery is a comparison against the solver, never merged into its results. A seed is an address, never fetched bytes.
- Bind before build: host runtime, then engine intrinsic, then an existing Lexbor module, then a faithful spec port; hand-roll last.

## What the tool produces

- The product's razor is an address, key or value no parse of the served bytes can state because it exists only at run time — the `runtime-only` razor class, read per address. Door classes and `beyond`-a-markup-parse are diagnostics, not the bar. The static baseline is the competitor and should be made as strong as possible.
- `netdiff --unused` is a diagnostic, not an optimization target. Target JS-heavy app pages.

## Security → SECURITY.md

- Authorization keys on `sender.tab.url`; the credentialed-read principal is `MessageSender.origin`. Content script, website and the WASM bundle are untrusted. Every engine instance is a sandboxed renderer frame with an opaque origin. All network goes through `safe-fetch.js` or `pageContextFetch`; state lives in the offscreen document and IndexedDB, never `chrome.storage.local`.
- One WASM instance per origin-keyed agent cluster. Cross-instance state crosses as serialized, typed text; synchronous cross-instance reads are suspend points; the cross-origin WindowProxy allowlist is declared beside the members; cross-document messages are emissions carrying the trusted-zone-stamped sender origin and the sender's world.

## Build and testing

- `node engine/build.mjs` is a compile: it resolves the revision, compiles, links, stamps and installs. A compile/link red blocks; nothing else does.
- The gate suite (`node engine/build.mjs gates`) is a separate, slow ask. A red gate blocks nothing: it is an accepted risk carried as a work queue with a target of zero. Never wait on a test; ask for it, keep working, and read the result when it arrives. A gate run does not hold the builder slot.
- The main agent owns every build; a newer build supersedes an older one (stop the old one by PID). Subagents never build or run gate `.mjs` files (including `wpt.mjs`, which compiles everything). Subagents may read, grep, `curl`, run `clang -fsyntax-only`/`node --check`/`engine/prosediff.mjs`, and drive an already-built artifact with a bounded command. A subagent reports what needs building and at which revision.
- The DCHECKs are the gate; corpora exercise code so the asserts can fire. test262 runs with features always on, under forced time travel, and reports engagement and coverage with their denominators. Uncollected or uncheckout-ed tests are excluded tests. Use `test_forced.c` for targeted testing. Delete one-off regression tests after use.
- Verify on the real thing only when ready: `testing/harness.js restart` and `testing/live-run.js <runs> <url>`, one browser per case with storage cleared. Read what the shipped path writes, not harness prints.
- Measure CPU or work, never wall time; a wall-clock budget is a backstop with a distinct signal. A loaded box (including the build's own load and finished agents' leftover processes) corrupts measurements; quote load with them and do not start a load under a lane you told to measure.
