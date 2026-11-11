#!/bin/bash
# The engine half of the real-site request diff: one live pass of a bench page through a frozen snapshot's native
# binary, its sends answered from the network and recorded (testing/bench/bench.mjs record) into a store of this
# lane's own, so the engine and Chromium are compared on what the site serves in the same hour.
#   engine_record.sh <snapshot> <store> <page> <work> <mem-cap MiB> <backstop s>
# It holds the machine-wide heavy-run lock for its whole run, and starts only with memory headroom (headroom.sh)
# of the cap plus 500 MiB and 600 MB of disk free; it stops its own run if free disk falls under 300 MB.
# Its artifacts are <store>/logs/<page>.rec*.{wire.jsonl,final.results.json,stderr.tail,progress.jsonl,
# census.tsv.gz} and one line appended to <store>/logs/record.jsonl.
set -u
# A live pass sends whatever the engine's egress lets out to real servers, forced arms' requests included, so
# it runs only when the person running it has said so; engine_replay.sh is the pass that reaches no server.
if [ "${REALSITE_LIVE_EGRESS:-}" != "I have read what this run may send" ]; then
  echo "engine_record.sh sends to real servers; set REALSITE_LIVE_EGRESS to the sentence in this script to run it, or use engine_replay.sh" >&2
  exit 5
fi
snap=$1; store=$2; page=$3; work=$4; cap=$5; backstop=$6
here=$(cd "$(dirname "$0")" && pwd)
slot=${REALSITE_SLOT:-/tmp/apiclient-frozen/slots/bigmem}
disk_start=${REALSITE_DISK_START_MB:-600}; disk_stop=${REALSITE_DISK_STOP_MB:-300}
exec 8>"$slot" || exit 4
flock 8
free_mb() { df --output=avail -BM / | tail -1 | tr -dc 0-9; }
if [ "$(free_mb)" -lt "$disk_start" ]; then echo "$page not started: $(free_mb) MB free, under $disk_start"; exit 3; fi
room=$("$here/headroom.sh")
if [ "$room" -lt $((cap + 500)) ]; then echo "$page not started: memory headroom $room MiB, under cap $cap + 500"; exit 3; fi
echo "$page start: $(free_mb) MB free, headroom $room MiB, $(uptime | sed 's/.*load/load/')"
setsid node "$snap/testing/bench/bench.mjs" record --store "$store" --snap "$snap" --pages "$page" \
  --work "$work" --mem-cap "$cap" --backstop "$backstop" 8>&- &
pid=$!
low=0
while kill -0 $pid 2>/dev/null; do
  if [ "$(free_mb)" -lt "$disk_stop" ] && [ $low = 0 ]; then
    low=1
    # bench.mjs spawns the zone as a process group of its own; its group is the run and nothing else.
    for q in $(ps -o pid= --ppid $pid); do kill -9 -- "-$q" 2>/dev/null; done
  fi
  sleep 5
done
wait $pid; st=$?
echo "$page done (bench exit $st, stopped for disk: $low): $(free_mb) MB free, $(uptime | sed 's/.*load/load/')"
exit $st
