#!/bin/sh
# ONE NAVIGATION PER BROWSER, because the thing being measured must not be the previous run.
#
# A second navigation in one tab reaches the engine as a document REPLACEMENT, and until that
# path is clean a run that follows another run is measuring the wedge rather than the site. So
# this restarts between every (site, run) pair: the browser, its IndexedDB and its code cache
# are the same at the start of every row, and the only thing that differs is which document
# arrived.
#
# It is deliberately NOT a mean of anything. Each row is printed as live-run.js emitted it —
# one navigation, one engine, its own counters — and the spread across rows is the reader's to
# take, because §Testing forbids collapsing a live site's run-to-run drift into one number.
#
#   sh testing/live-matrix.sh <runs> <url> [url…]
set -u
RUNS="$1"; shift
PORT="${PORT:-9351}"
export HARNESS_PROFILE="${HARNESS_PROFILE:-testing/profile-m5ey}"
export HARNESS_LOCK="${HARNESS_LOCK:-testing/harness-m5ey.lock}"
i=0
while [ "$i" -lt "$RUNS" ]; do
  for u in "$@"; do
    node testing/harness.js restart "$PORT" >/dev/null 2>&1
    echo "### run=$i url=$u"
    LIVE_RUN_BUDGET_MS="${LIVE_RUN_BUDGET_MS:-90000}" node testing/live-run.js 1 "$u" 2>&1 \
      | grep -v '^#' | grep -v '^$'
  done
  i=$((i+1))
done
