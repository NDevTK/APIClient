# Real-site corpus

The gate for "does this work on real webapps". Four instruments; the mirror tree
itself is NOT checked in (102 MiB) and is rebuilt from `sites.tsv`.

    node mirror.mjs                     # fetch + freeze 30 sites, write provenance.json
    node serve-faithful.mjs <id> <port> # serve one frozen site
    node site.mjs <id> <url>            # drive it in Chrome, emit one ROW of JSON

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

`site.mjs` reads the ENGINE's record for a crash, not the browser console -- the
renderer does not tee stdout, so a console scrape reads empty for a page that
learned a thousand endpoints. It reports the LAST log entry rather than a sum or
a max, because every entry carries the run's cumulative total and the log is not
cleared between page loads: summing counted one endpoint once per snapshot, and a
max over a non-decreasing log cannot report a fall. `distinctEndpoints` is the
headline. An ABSENT count and a zero are kept apart -- a run that reported no
document is not a page that was analysed and found clean.

## Measuring

A before/after belongs on frozen bytes, where the only thing that changed is the
engine. Live sites are for DISCOVERING signatures. State the load average with
every number, and record the artifact's sha256 -- not the build head, which for a
build of a dirty shared tree names a revision whose sources do not contain the
program that ran.
