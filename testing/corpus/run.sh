#!/bin/bash
# ONE CENSUS PASS: every site in sites.tsv, one virgin browser each, against FROZEN BYTES and a FROZEN
# ARTIFACT. Emits one JSON row per line; feed it to report.mjs.
#
#   LANE=/tmp/mylane ./run.sh pass1            # all sites
#   LANE=/tmp/mylane ./run.sh pass1 excalidraw # one site
#
# THE LANE IS THE WHOLE POINT AND IT IS REQUIRED, NOT DEFAULTED. It is a directory holding a COPY of
# `testing/harness.js` and a COPY of `extension/`, because harness.js derives EXT_DIR from its own location
# and Chrome's extension id from that path. Copying the pair is what makes the browser provably load an
# artifact nobody can rebuild under you: the shared checkout's qjs.wasm was replaced in the middle of an
# earlier pass, and the rows either side of it were two different programs wearing one corpus. Pointing this
# at the live checkout is therefore not a convenient shortcut, it is the defect, so there is no default.
#
#     mkdir -p $LANE/testing && cp testing/harness.js $LANE/testing/ && cp -a extension $LANE/
#
# ONE VIRGIN BROWSER PER SITE. An engine abort tears down the renderer pool, and the NEXT site in the same
# browser then reports counters belonging to a poisoned pool -- which is how an earlier census read runs=0
# for three healthy origins and called them dead. `restart` (never `start`) also wipes IndexedDB and the V8
# code cache, so no row inherits the previous row's frontier.
#
# THE FIXTURE SERVER IS PER SITE AND ITS MISSES ARE KEPT. serve-faithful 404s loudly for a resource the
# mirror lacks; that count goes to logs/<id>.serve so a row with a thin document can be told apart from a
# row whose engine learned nothing.
set -u
CORP="$(cd "$(dirname "$0")" && pwd)"
: "${LANE:?set LANE to a directory holding testing/harness.js and extension/ -- see the comment above}"
[ -f "$LANE/testing/harness.js" ] || { echo "no $LANE/testing/harness.js"; exit 2; }
[ -f "$LANE/extension/lib/qjs/qjs.wasm" ] || { echo "no $LANE/extension/lib/qjs/qjs.wasm"; exit 2; }

LABEL=${1:-pass}
ONLY=${2:-}
# SITES names the list to walk (default the corpus). A repair pass over the handful of sites whose fixture
# data was rebuilt is a different list, not a different script.
SITES=${SITES:-$CORP/sites.tsv}
[ -f "$SITES" ] || { echo "no site list at $SITES"; exit 2; }
PORT=${HARNESS_PORT:-9451}
FIXPORT=${FIXPORT:-8951}
export NODE_PATH=${NODE_PATH:-/home/user/APIClient/node_modules}
export HARNESS_PROFILE=$LANE/prof HARNESS_LOCK=$LANE/harness.lock HARNESS_PORT=$PORT
export HARNESS_EXT_DIR=$LANE/extension CDP=$PORT DWELL=${DWELL:-60000}
OUT=$CORP/census-$LABEL.jsonl
mkdir -p "$CORP/logs"
: > "$OUT"
echo "artifact $(sha256sum "$LANE/extension/lib/qjs/qjs.wasm" | cut -c1-64)"
echo "load at start: $(cut -d' ' -f1-3 /proc/loadavg)"

while IFS=$'\t' read -r id url stack; do
  [ -z "$id" ] && continue
  [ -n "$ONLY" ] && [ "$ONLY" != "$id" ] && continue
  echo "=== $(date -u +%H:%M:%S) $id   load $(cut -d' ' -f1 /proc/loadavg)"
  node "$CORP/serve-faithful.mjs" "$id" "$FIXPORT" >"$CORP/logs/$id.serve" 2>&1 &
  SRV=$!
  sleep 1
  # THE SERVER MUST BE *THIS* SITE'S. One fixture port is reused for every row, so a server that failed to die
  # would still be bound and would answer with the PREVIOUS site's document -- a row measuring the wrong site
  # under the right name, which no counter in the census could contradict. serve-faithful prints "<id> on
  # <port>" only after a successful listen, so that line is the proof, and its absence is fatal for the row.
  if ! grep -q "^$id on $FIXPORT " "$CORP/logs/$id.serve"; then
    echo "{\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"fixture server for $id did not bind $FIXPORT\"}" >> "$OUT"
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; continue
  fi
  ( cd "$LANE" && timeout 240 node testing/harness.js restart "$PORT" ) >"$CORP/logs/$id.restart" 2>&1
  # CONFIRM THE BROWSER IS OURS BEFORE DRIVING IT. `restart` can report "started" while its Chrome is
  # already gone, and one lane then silently drove another agent's browser and lost a whole pass.
  MYID=$(node -e "const c=require('crypto');const h=c.createHash('sha256').update(Buffer.from(process.argv[1],'utf8')).digest('hex').slice(0,32);let i='';for(const x of h)i+=String.fromCharCode(97+parseInt(x,16));console.log(i)" "$LANE/extension")
  if ! curl -s --max-time 5 "http://127.0.0.1:$PORT/json/list" | grep -q "$MYID"; then
    echo "{\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"port $PORT is not serving our extension $MYID\"}" >> "$OUT"
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; continue
  fi
  R=$(cd "$CORP" && timeout 360 node site.mjs "$id" "http://127.0.0.1:$FIXPORT/" 2>&1 | grep '^ROW ' | head -1)
  [ -z "$R" ] && R="ROW {\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"driver produced no row\"}"
  echo "${R#ROW }" >> "$OUT"
  kill -TERM $SRV 2>/dev/null; wait $SRV 2>/dev/null
done < "$SITES"

# TEAR DOWN BY THE PID IN *OUR* LOCK, never by a pattern. `pkill -f testing/harness.js` matches every lane's
# harness on a shared checkout, which is the exact way one agent has already killed another's browser.
MYPID=$(node -e "try{console.log(JSON.parse(require('fs').readFileSync(process.argv[1],'utf8')).pid)}catch{}" "$HARNESS_LOCK" 2>/dev/null)
[ -n "${MYPID:-}" ] && kill "$MYPID" 2>/dev/null
echo "DONE -> $OUT"
