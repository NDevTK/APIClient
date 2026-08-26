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

## Rules that are not style

* **One targeted minimal test at a time.** Never a bulk sweep against live sites.
* **Clear storage before concluding any bug.**
* **One run of a live site is not a measurement.** The bytes, the server's answers and the
  order orphans are reached all move under you. A before/after belongs on frozen bytes — a
  mirror, a fixture, a recorded payload replayed. Report a live number with its run count and
  its spread or do not report it as a comparison.
* **On a site that aborts, the endpoint count beside the crash is noise.** How much a run
  learned before it died is a function of where the crash landed. The crash record is the
  stable fact.
* Bulk gates (test262, WPT, solvergate) belong to the main agent and are run once, serialized.
  This harness is the targeted exercise, and it costs no compile.
