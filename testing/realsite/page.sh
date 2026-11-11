#!/bin/bash
# One page of the real-site census: Chromium's loads, one live engine pass, and their diff.
#   page.sh <snapshot> <store> <captures> <out-prefix> <id> <url> [work] [mem-cap MiB] [backstop s]
# The capture and the engine pass each hold the machine-wide heavy-run lock in turn. Written: the captures
# under <captures>/<id>, the engine's artifacts under <store>/logs, and <out-prefix>.{chrome.out,engine.out,
# txt,json}. A step that fails is reported and the later steps still run on what exists.
set -u
snap=$1; store=$2; caps=$3; out=$4; id=$5; url=$6; work=${7:-432e6}; cap=${8:-4096}; backstop=${9:-2400}
here=$(cd "$(dirname "$0")" && pwd)
slot=${REALSITE_SLOT:-/tmp/apiclient-frozen/slots/bigmem}
loads=${REALSITE_LOADS:-3}
rm -f "$caps/$id"/chrome-*.json
# The backstop is inside the lock so that it ends the capture itself and the lock with it.
QUIET_MS=${QUIET_MS:-20000} MAX_MS=${MAX_MS:-90000} flock "$slot" timeout 600 \
  node "$here/chrome_requests.cjs" "$caps" "$loads" "$id" "$url" > "$out.chrome.out" 2>&1
echo "$id chromium exit $?: $(tail -n 1 "$out.chrome.out")"
(cd "$snap" && bash "$here/engine_record.sh" "$snap" "$store" "$id" "$work" "$cap" "$backstop") > "$out.engine.out" 2>&1
echo "$id engine exit $?: $(tail -n 2 "$out.engine.out" | head -n 1)"
wire=$(ls -t "$store/logs/$id".rec*.wire.jsonl 2>/dev/null | head -n 1)
if [ -z "$wire" ]; then echo "$id no engine run to diff"; exit 1; fi
node "$here/diff.mjs" "$caps/$id" "$wire" "${wire%.wire.jsonl}.final.results.json" --json "$out.json" > "$out.txt" 2> "$out.diff.err"
echo "$id diff exit $?: $(sed -n 4,5p "$out.txt" | tr '\n' ' ')"
