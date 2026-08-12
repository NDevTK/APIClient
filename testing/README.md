# Quality Harness

End-to-end quality harness for the API Security Researcher extension.
Drives a real Chrome with the unpacked extension loaded across a list of
complex popular sites, pulls the extension's learned state out, and
scores it.

No harness code lives inside `extension/` — the extension runs
unchanged. All driving, extraction, and scoring sits here.

## Setup

```
npm install
```

(Installs `puppeteer` as a devDependency. First install fetches a bundled
Chrome, ~150 MB.)

The extension needs its Babel bundle built once:

```
node build.js
```

## Run

```
npm run harness                            # all sites
npm run harness -- --only github_home      # one site
npm run harness -- --only github_home,reddit_home --dwell 25
```

Output:

```
testing/reports/<ISO>/
  run.json              run metadata + per-site status
  <site>.json           full state dump for that site
  <site>.scripts/       script sources referenced by security findings
                        (refetched so we can review the actual JS)
```

The Chrome profile lives at `testing/profile/` and is gitignored.
It persists across runs — cookies, IndexedDB for the extension, etc.
Delete the directory to start fresh.

## Analyze

```
npm run classify                           # newest run
npm run classify -- --dir testing/reports/2026-04-16T...
npm run classify -- --site github_home
```

Writes `analysis.json` into the run directory and prints a per-site
summary to stdout.

## What gets measured

### 1. API vs static classification

The classifier **ignores Content-Type and URL extensions**. Per the
user's requirement: an API can legitimately return an image. Each
captured response is labelled on body content alone:

* **magic bytes** for media/fonts/pdf/zip
* **JSON parse** (and GraphQL shape detection)
* **NDJSON** multi-line JSON
* **SSE** `event:`/`data:` frames
* **gRPC-Web frame** (`[0x00|0x80][be32 length]`)
* **Protobuf** wire-format varint head
* **HTML / JS / CSS** heuristics for static web assets

Then we compare the content-based label to what the extension learned
(i.e. whether the service ended up in a discovery doc). Two
disagreements are surfaced per site:

| Flag | Meaning |
|------|---------|
| `extSaysApiBodyStatic` | Extension tracked this as an API, body looks static. Possible noise. |
| `bodySaysApiExtMiss`   | Body strongly looks API, extension didn't learn. Possible miss. |

Both lists are in `analysis.json` — not pass/fail; triage leads.

### 2. Method naming quality

Per service: method count, name collisions, percentage of names
containing a CRUD verb, and names that look like bare IDs/UUIDs. Low
CRUD-verb percentage + high UUID count means names are mechanical
(probably `v1_by_id_123e4567`) rather than semantic.

### 3. Body-param learning

For every logged request with a JSON request body, counts the leaf
fields present vs. the fields learned into the service schema under
that method ID. Large gap = the learner missed fields.

### 4. Security findings — triage, not labels

**The user must review the actual JavaScript to decide whether each
finding is real.** The harness does not guess.

For every finding in every run, the classifier produces a triage
entry:

```json
{
  "kind": "sink",
  "sourceUrl": "https://example.com/app-12ab.js",
  "pageUrl": "https://example.com/",
  "type": "DOM XSS sink",
  "sink": "innerHTML =",
  "severity": "high",
  "sourceType": "user-controlled",
  "taintSource": "location.hash",
  "sanitized": false,
  "line": 4821,
  "col": 33,
  "codeWindow": "  4817    ...\n> 4821    el.innerHTML = location.hash;\n  4825    ...",
  "extensionContext": "..."
}
```

The `codeWindow` is ±4 lines of the actual script as fetched, so the
reviewer can judge real-vs-false-positive directly from
`analysis.json` without opening the file. The raw script is also
saved under `<site>.scripts/` keyed in `index.json`.

## Mechanics

The harness resolves the extension ID from Puppeteer's service-worker
target (no manifest `key` needed). It wakes the SW between sites,
then calls module-level state (`state.tabs`, `globalStore`,
`serializeTabData`) via `worker.evaluate()`. Those are exactly what
`popup.js` already reads via `chrome.runtime.sendMessage` — we take
the same data by a more direct route to avoid any message-gating
surprises.

Per-site flow:

1. Open blank tab, navigate to site (`domcontentloaded`).
2. Dwell N/2 seconds for AST + network capture.
3. Scroll the page to trigger lazy loads.
4. Dwell another N/2 seconds.
5. Ask the SW which tab id currently maps to this URL.
6. Dump `state.tabs[tabId]` + `globalStore` snapshot to JSON.
7. Refetch each script referenced by a security finding, save it.
8. Close tab, move on.

## Extending

* **Add sites**: edit `testing/sites.json`. Entries are
  `{ "name": "unique_slug", "url": "..." }`. Use real complex popular
  sites — toy vuln apps aren't useful here.
* **New score**: add a function to `classify.js`, wire it into
  `analyzeSite()`'s returned object. Everything offline runs against
  saved dumps, so new scoring can re-run over old runs.
* **New state field**: prefer editing `extractors.js` and reading
  what already exists in the SW — do **not** add code to the
  extension itself.
