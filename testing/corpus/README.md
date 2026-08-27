# Real-site corpus

The gate for "does this work on real webapps". Five instruments; the mirror tree
itself is NOT checked in (102 MiB) and is rebuilt from a site list.

    node list.mjs                       # (a module) the one reader of a site list
    SITES=apps.tsv node mirror.mjs      # fetch + freeze that list, write provenance.json
    node serve-faithful.mjs <id> <port> # serve one frozen site
    node site.mjs <id> <url> [pass]     # drive it in Chrome, emit one ROW of JSON
    LANE=/tmp/mylane ./run.sh a1                          # one pass, frozen bytes, sites.tsv
    LANE=/tmp/mylane SITES=apps.tsv AT=live ./run.sh r1   # one pass, live, the app pages
    SITES=apps.tsv node report.mjs census-r1.jsonl …      # the table + the ranked abort queue

## Two lists, and a census is a measurement OF one

`sites.tsv` is thirty LOGIN pages. `apps.tsv` is twelve JS-heavy APP pages — the list that produced this
project's standing headline (ten of twelve ENGINE-ABORT, `fin/n` 0 everywhere, `sinks: 0` on every
measurable site). That list and its driver existed only under `/tmp`, so the one artifact needed to
re-measure the headline was the one artifact the checkout did not have; both are now here.

`SITES` names the list and every instrument takes it — `run.sh` always did, `mirror.mjs` and `report.mjs`
each hard-coded `sites.tsv`. That was not untidiness. `report.mjs` looked all twelve app ids up in
`sites.tsv`, matched none, and filled every miss with `|| ''`, so a report handed the WRONG LIST printed
twelve complete-looking rows. It now refuses a census whose ids the list does not contain, and it PRINTS
each site's observed stack beside the signature it hit — which is both the reader that column never had and
the thing that makes a ranking generalise: three sites on one signature is a number, three sites on one
signature that all ship the same bundler is something to reproduce.

`AT=live` and `AT=frozen` are the same driver. §Testing is why they are not the same measurement: a
before/after belongs on frozen bytes; a live pass is for DISCOVERING signatures, and its endpoint column is
noise on any site that aborted.

`run.sh` needs a LANE: a directory holding a COPY of `testing/harness.js` and of `extension/`, because
harness.js derives its extension directory from its own location. That copy is what makes the browser
provably load an artifact no other lane can rebuild under you mid-pass.

`control/` is the INSTRUMENT CONTROL and is not a corpus site. It is a page whose `fetch` calls the engine
certainly sees — one inline, one in a subresource, one behind an absent-state flag, one in a function nobody
calls, one built from a config value — AND whose attacker sources reach a code-execution sink: an untainted
markup write, `location.search` into `innerHTML`, `location.hash` into `eval`, and a branch over a source
whose two arms both arrive. A corpus-wide zero is a finding only once this is non-zero **in the column being
read**; run it in the same lane, on the same artifact, beside every census.

THE @S HALF WAS ABSENT WHILE THE CENSUS PUBLISHED `sinks: 0` FOR TWELVE SITES, and the two halves are not
interchangeable: the control scored 6/6 on endpoints with `secSinks 0`, so a reader who took that as the
control passing had a control for the endpoint column standing behind a claim about the security column.
That is this directory's own rule read one column too wide, and it is exactly the reading a control exists
to make impossible — an instrument that answers one question is silence about the other, not evidence for
it. Every column a census prints needs a rung here that moves it, or that column's zero is unreadable.

## What each instrument refuses to do, and why

`mirror.mjs` saves every resource at its ORIGINAL path and keeps its query. An
earlier builder flattened them to `/sN.js`, which deletes two things a page
reads: the query (122 of 740 resources carry one, and a real site's first guard
was `params.affiliate || params.from` read off `document.currentScript`), and the
module graph (a module chunk's relative `import` resolves against ITS url). Real
Chrome learns nothing from flattened bytes either, so a zero measured there is a
fact about the fixture.

`serve-faithful.mjs` rewrites only the ORIGIN, so path and query survive
byte-for-byte, and recomputes the stored filename from the request's own query
using the builder's rule rather than a second index that could drift. A missing
resource 404s loudly and is counted: silently serving an empty body for a missing
chunk is the flattening defect wearing different clothes.

`serve-faithful.mjs` also refuses to answer a miss with PROSE. Its 404 body was the words `not mirrored`,
served as text/plain, and the engine — correctly — compiled it as a classic script: `not mirrored` is two
identifiers, so it raised a parse abort at line 1 column 5, which is where `mirrored` starts. That abort
ranked as the corpus's NUMBER ONE engine defect across five sites, and it was this file. The body is now
empty. The same census also found the two ways this server and the builder can point at bytes the origin
never served, and both are now closed: an attribute value is HTML-ESCAPED (figma's `src` held `&#47;` for
`/`, so 7 of 7 resources were saved as a 1.3 MiB 404 page), and a relative URL resolves against the DOCUMENT
BASE URL — HTML §2.4.3 "Document base URLs", the first descendant `base` element with an `href` (§4.2.3 "The
base element") — not against the document's own address. Three corpus documents declare one; material
.angular.dev ships `<base href="/">` and every one of its 12 scripts was fetched from the wrong path and
stored as `<!doctype html><title>Page Not Found</title>`. A fixture that stores an error page under a real
URL's name manufactures engine bugs, which is worse than measuring nothing.

`run.sh` refuses to drive a browser it has not proved is its own, and refuses to reuse a fixture server it
has not proved bound for THIS site — one port is reused per row, so a server that failed to die would answer
with the previous site's document, and no counter in the census could contradict it.

`site.mjs` reads the ENGINE's record for a crash, not the browser console -- the
renderer does not tee stdout, so a console scrape reads empty for a page that
learned a thousand endpoints. It reports the LAST log entry rather than a sum or
a max, because every entry carries the run's cumulative total and the log is not
cleared between page loads: summing counted one endpoint once per snapshot, and a
max over a non-decreasing log cannot report a fall. `distinctEndpoints` is the
headline. An ABSENT count and a zero are kept apart -- a run that reported no
document is not a page that was analysed and found clean. It also NAMES the
transcript it wrote (`logFile`, pass-qualified when a pass label is given), so
`report.mjs` reads the console this row's counters came out of instead of
reconstructing a filename: one path per site is overwritten by the next pass, and
a site that ran cleanly in pass 1 then inherits pass 3's abort.

## Measuring

A before/after belongs on frozen bytes, where the only thing that changed is the
engine. Live sites are for DISCOVERING signatures. State the load average with
every number -- `site.mjs` samples it at both ends of the dwell and puts it in the
row, because the dwell is WALL-CLOCK and a busy machine hands the engine less CPU
inside the same 60 seconds -- and record the artifact's sha256, not the build head,
which for a build of a dirty shared tree names a revision whose sources do not
contain the program that ran.

What the outcome column is worth: measured over three passes on one frozen
artifact, every site's ABORT-or-not was identical in all three, and so was every
endpoint count. Only `flows` moved, and it moved a lot (one site 9550-18521), which
is exactly the wall-clock/CPU variance above. So a single pass settles whether a
site aborts and on what; it settles nothing about how much work got done.
