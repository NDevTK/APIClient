#!/bin/bash
# The engine half of the real-site request diff as a replay: one pass of a bench page through a frozen
# snapshot's native binary with every send answered from a recording (testing/bench/bench.mjs run), so nothing
# reaches a server. A send the recording lacks is answered as a network failure and counted as a miss.
#   engine_replay.sh <snapshot> <store> <out-dir> <page> [budgets] [mem-cap MiB] [backstop s]
# It holds the machine-wide heavy-run lock for its whole run, and starts only with 1500 MB of disk free and
# memory headroom (headroom.sh) of the cap plus 500 MiB; it stops its own run if free disk falls under 1300 MB.
# Its artifacts are <out-dir>/logs/<page>.0.{wire.jsonl,final.results.json,<budget>.results.json,stderr.tail,
# progress.jsonl,census.tsv.gz}, one line appended to <out-dir>/runs.jsonl, and bench's own output in
# <out-dir>/<page>.bench.out.
set -u
snap=$1; store=$2; out=$3; page=$4; budgets=${5:-108e6,432e6}; cap=${6:-4096}; backstop=${7:-2400}
here=$(cd "$(dirname "$0")" && pwd)
slot=${REALSITE_SLOT:-/tmp/apiclient-frozen/slots/bigmem}
exec 8>"$slot" || exit 4
flock 8
free_mb() { df --output=avail -BM / | tail -1 | tr -dc 0-9; }
if [ "$(free_mb)" -lt 1500 ]; then echo "$page not started: $(free_mb) MB free, under 1500"; exit 3; fi
room=$("$here/headroom.sh")
if [ "$room" -lt $((cap + 500)) ]; then echo "$page not started: memory headroom $room MiB, under cap $cap + 500"; exit 3; fi
echo "$page start: $(free_mb) MB free, headroom $room MiB, $(uptime | sed 's/.*load/load/')"
mkdir -p "$out"
setsid node "$snap/testing/bench/bench.mjs" run --store "$store" --snap "$snap" --out "$out" --runs 1 \
  --budgets "$budgets" --jobs 1 --backstop "$backstop" --mem-cap "$cap" --pages "$page" > "$out/$page.bench.out" 2>&1 8>&- &
pid=$!
low=0
while kill -0 $pid 2>/dev/null; do
  if [ "$(free_mb)" -lt 1300 ] && [ $low = 0 ]; then
    low=1
    # bench.mjs spawns the zone as a process group of its own; its group is the run and nothing else.
    for q in $(ps -o pid= --ppid $pid); do kill -9 -- "-$q" 2>/dev/null; done
  fi
  sleep 5
done
wait $pid; st=$?
head -n 1 "$out/$page.bench.out"
echo "$page done (bench exit $st, stopped for disk: $low): $(free_mb) MB free, $(uptime | sed 's/.*load/load/')"
exit $st
