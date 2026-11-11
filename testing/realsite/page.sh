#!/bin/bash
# One page of the real-site census: Chromium's loads, one engine replay of a recording, and their diff.
#   page.sh <snapshot> <store> <replay-out> <captures> <out-prefix> <id> <url> [budgets] [mem-cap MiB] [backstop s]
# The capture and the replay each hold the machine-wide heavy-run lock in turn. The capture loads the live
# page as a logged-out visitor would; the replay reaches no server. Written: the captures under
# <captures>/<id>, the engine's artifacts under <replay-out>/logs, and <out-prefix>.{chrome.out,engine.out,
# txt,json}. A step that fails is reported and the later steps still run on what exists. REALSITE_SKIP names
# a step to leave out (`chrome` or `engine`) when its artifacts already exist.
set -u
snap=$1; store=$2; rout=$3; caps=$4; out=$5; id=$6; url=$7; budgets=${8:-108e6,432e6}; cap=${9:-4096}; backstop=${10:-2400}
here=$(cd "$(dirname "$0")" && pwd)
slot=${REALSITE_SLOT:-/tmp/apiclient-frozen/slots/bigmem}
loads=${REALSITE_LOADS:-3}
skip=${REALSITE_SKIP:-}
if [ "$skip" != chrome ]; then
  rm -f "$caps/$id"/chrome-*.json
  # The backstop is inside the lock so that it ends the capture itself and the lock with it.
  QUIET_MS=${QUIET_MS:-20000} MAX_MS=${MAX_MS:-90000} flock "$slot" timeout 600 \
    node "$here/chrome_requests.cjs" "$caps" "$loads" "$id" "$url" > "$out.chrome.out" 2>&1
  echo "$id chromium exit $?: $(tail -n 1 "$out.chrome.out")"
fi
if [ "$skip" != engine ]; then
  (cd "$snap" && bash "$here/engine_replay.sh" "$snap" "$store" "$rout" "$id" "$budgets" "$cap" "$backstop") > "$out.engine.out" 2>&1
  echo "$id engine exit $?: $(tail -n 2 "$out.engine.out" | head -n 1)"
fi
wire=$rout/logs/$id.0.wire.jsonl
if [ ! -f "$wire" ]; then echo "$id no engine run to diff"; exit 1; fi
res=$rout/logs/$id.0.final.results.json
[ -f "$res" ] || res=$res.gz
node "$here/diff.mjs" "$caps/$id" "$wire" "$res" --json "$out.json" > "$out.txt" 2> "$out.diff.err"
echo "$id diff exit $?: $(sed -n 5,6p "$out.txt" | tr '\n' ' ')"
