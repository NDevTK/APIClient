# The live-Chrome harness

`testing/harness.js` drives ONE real Chrome with the unpacked extension loaded, and lets you
read the extension's own state out of it. It is a set of COMMANDS you run against a browser you
started and left running — not a batch scorer.

No harness code lives inside `extension/` — the extension runs unchanged.

## What this file used to say, and why that mattered

Every section below replaced prose describing a program that was deleted long ago: a batch
runner that walked `testing/sites.json`, wrote `testing/reports/<ISO>/<site>.json`, and scored
the result offline through `classify.js` + `extractors.js`. The runner and its three suites went
out when the harness became interactive; the analyzer, the extractor and this document did not,
and `npm run classify` stayed in `package.json` pointing at all of it.

That is worse than clutter, and it is the reason the whole pipeline is now gone rather than
repaired. `classify.js` read `report.dump` — a field nothing in the tree wrote — and read the
analysis record's `valueConstraints` / `protoFieldMaps` / `protoEnums` / `sourceMap` /
`dangerousPatterns`, five names the engine has never emitted and the offscreen stopped
fabricating as constant empties. Every one of those reads carried `|| []`, so the analyzer's
verdict line — `reqs=… learned=… findings=…` — was composed out of absences and would have
printed a clean bill for a page nothing had looked at. A document that describes a measurement
apparatus that cannot measure is the same defect as the defaults inside it: it reads as
authoritative and sends the next reader to run something that answers nothing.

## Setup

```
npm install          # puppeteer; first install fetches a bundled Chrome (~150 MB)
node engine/build.mjs
```

The Chrome profile lives at `testing/profile/` and is gitignored. It persists across runs —
cookies, the extension's IndexedDB, everything. Delete the directory to start fresh.

## Commands

```
node testing/harness.js <command> [args…]
```

| Command | What it does |
|---|---|
| `restart` | Kill any harness Chrome and start a fresh one. **Use this, not `start`** — `start` reuses a stale wasm and a poisoned IDB. |
| `restart-keep` | Same, but keeps IndexedDB. |
| `start` | Launch only if nothing is running. |
| `goto <url>` | Navigate the active tab. A dead fixture server CRASHES here rather than quietly loading a Chrome error page. |
| `page <expr>` | Evaluate in the page under analysis. |
| `popup <expr>` | Evaluate in the extension popup. Dogfood the RENDERED popup DOM here, not offscreen internals. |
| `pocrun [ms]` | Click a finding's PoC and poll the card for REAL EXPLOIT / NOT REPRO. |
| `offscreen <expr>` | Evaluate in the offscreen document (`ast-worker.html`) — where the brain, `globalStore` and `state.docs` live. |
| `sweval <expr>` | Evaluate in the MV3 service worker (`background.js`), which holds no state. |
| `capture <out.js>` | Save the page's served HTML. |
| `diag [secs] [reload]` | List CDP targets, then tee every extension console for N seconds. |
| `multitab <url…>` | Open each url in its own tab and poll learned endpoints per host — the concurrent-scheduler measurement. |
| `netdiff [--all] [--assets] [--unused]` | The moat's headline diagnostic. See below. |

## `netdiff`

Diffs what the page's live traffic did against what forced execution learned, read-only, over
the brain's own state.

* default — LIVE-NOT-LEARNED: requests that fired and were not learned. A coverage gap.
* `--unused` — LEARNED-NOT-LIVE: the API surface forced execution found and the page never
  fired, with the parameter keys and example values it recovered. **This is the value**;
  CLAUDE.md names it a diagnostic that the solver dominates the live page, never the thing to
  optimize.

Every run prints a `runs` census first — how many documents ended `complete`, `crashed`,
`nothing-to-run`, or have **not returned**. Read it before the counts under it: a missing
endpoint is a gap only for a document whose run has returned, and an absent count and a zero
count are different facts. The census is `_astRun`, which the offscreen writes once per
document at the terminal return; the caveat used to name a `learnstate` command whose worker
the offscreen's own CSP forbids from ever existing.

## Reading state directly

`offscreen <expr>` reaches the brain's live objects: `globalStore.endpoints`,
`globalStore.discoveryDocs`, `globalStore.securityFindings`, `state.docs`. Each document
carries `_astRun` (the run outcome), `_astResults` (present only when an engine document
arrived — its ABSENCE is the statement that none did), and `_resolverErrors` (absent means the
engine recorded no page error, which is why nothing may default it to `[]`).

## The served fixtures

`testing/fixtures/` is ignored WHOLE and its instruments are re-included BY NAME, each with the
argument for keeping it written beside the negation in `.gitignore`. That file is the index; there
is no other, and nothing anywhere directory-scans this folder. Derive the set with a command rather
than reading a figure here, because a figure rots and the command does not:

```
git ls-files testing/fixtures/                              # the set
git check-ignore -v --no-index testing/fixtures/<name>      # the entry that argues for it
node testing/fixtures_server.cjs &                          # serves the directory; FIX_PORT, default 8765
node testing/harness.js restart
node testing/harness.js goto http://127.0.0.1:8765/<name>
```

**NO DRIVER NAMES ANY OF THEM, AND THE TWO THAT LOOK DRIVEN ARE NOT.** Every document here is driven
BY HAND, with a URL argument, by whoever is asking its question, and each states what it predicts in
its own prose. The derivation is one command and it is the one to re-run rather than to believe:

```
for f in $(git ls-files testing/fixtures/); do b=$(basename $f);
  git grep -l -F "$b" . ':!testing/fixtures' ':!.gitignore'; done | sort -u
```

It answers five files and EVERY hit is prose. `wjp_absent.html` in `engine/host/solver/flow.c`,
`flow.h` and `result.c` and in `live-run.js` is quoted MEASUREMENTS naming their subject, plus one
drive command inside a comment; `scrstat_status_error.html` in `fixtures_server.cjs` is a comment
explaining which request header that document reads. The text at each hit TALKS ABOUT the document
rather than running it. So "which fixtures are named by something" is not a question that separates
one of these from another, and an instrument keyed on naming answers a question nobody has.

**THE DIRECTORY IS NAMED WHERE A BASENAME IS NOT, WHICH IS THE ROUTE THE COMMAND ABOVE CANNOT
SEE.** `engine/pagecensus.mjs` takes a document URL and knows nothing about any on-disk layout, and
its own usage line names this directory as a tracked subject that exists:
`(cd testing/fixtures && python3 -m http.server <port>) &`. That is a second drive route and it is
still a HAND drive — it selects nothing, it is handed a URL. **THE TWO SERVERS ARE NOT
INTERCHANGEABLE**: `python3 -m http.server` writes no access log, serves no `<file>.headers` sidecar
and honours no `?pipe=status(...)`, so a fixture whose oracle is the access line, whose arm is a
header-delivered policy, or whose subject is a chosen status must be served by
`testing/fixtures_server.cjs` and reads as its own opposite under the other one.

**THE ORACLE FOR A SERVED FIXTURE IS THE SERVER'S OWN ACCESS LOG** — `testing/fixtures_access.<port>.log`,
which `fixtures_server.cjs` truncates and stamps at startup and writes BEFORE it decides a status, so
a 404 path is recorded exactly as a 200 one is, a count is a count for THIS run, and an empty file
still carries its header (`the server ran and nothing asked it for anything` is thereby separable from
`no server ever ran`). That is a reader OUTSIDE the engine, which is what a witness needs; a console
line is not one, because the renderer deliberately does not tee its stdout.

**A WITNESS PATH IN ONE OF THESE DOCUMENTS IS A CONSTANT.** This engine's whole purpose is to make
values unknown, so a path composed from a computed value is not a concrete string and is never sent —
a witness built that way goes silent in exact proportion to how well the engine is working.

`testing/fixtures/GATE` does not exist and cannot yet. `engine/gate_collect.mjs`'s claim mechanism —
the thing that makes an uncollected file a NAMED FAILURE rather than a silent exclusion — requires the
runner a claim names to EXIST, and no runner enumerates this directory; `collectFixtures` walks
`engine/tests`. Two tracked scripts do compose fixture URLs from a variable, `testing/moat_survey.sh`
and `testing/scale_survey.sh`, and every name in their arrays is absent from the directory, so the one
shape a collector would have to cover currently covers nothing.

## Rules that are not style

* **One targeted minimal test at a time.** Never a bulk sweep against live sites.
* **Clear storage before concluding any bug.**
* **One run of a live site is not a measurement.** The bytes, the server's answers and the
  order orphans are reached all move under you. Report a live number with its run count, its
  spread, the ENGINE's revision and the HOUR, or do not report it as a comparison — the
  subject's own revision is not ours to state.
  This bullet used to answer that with `a before/after belongs on frozen bytes — a mirror, a
  fixture, a recorded payload replayed`, and the MIRROR half is RETIRED by the project owner:
  a page's scripts and styles are computed and fetched at runtime, so a frozen copy of somebody
  else's site is a program no visitor is served, and this repository carries none. The retired
  wording is kept because a reader who re-derives it from run-to-run variance will re-propose a
  mirror; `.gitignore`'s `testing/fixtures/*` block records the same retirement and the reason
  it does NOT reach a fixture this project WROTE, which is an instrument rather than a capture.
  What replaces the mirror needs no new mechanism: compare on the quantities that do not move
  with reach — a crash's IDENTITY, a conservation identity read WITHIN one sample, a count that
  cannot be true, a value that is wrong rather than small.
* **On a site that aborts, the endpoint count beside the crash is noise.** How much a run
  learned before it died is a function of where the crash landed. The crash record is the
  stable fact.
* Bulk gates (test262, WPT, solvergate) belong to the main agent and are run once, serialized.
  This harness is the targeted exercise, and it costs no compile.
