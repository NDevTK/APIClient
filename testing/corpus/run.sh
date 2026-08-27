#!/bin/bash
# ONE CENSUS PASS: every site in the list, one virgin browser each, against a FROZEN ARTIFACT. Emits one JSON
# row per line; feed it to report.mjs.
#
#   LANE=/tmp/mylane ./run.sh pass1                      # frozen bytes, sites.tsv
#   LANE=/tmp/mylane ./run.sh pass1 excalidraw           # one site
#   LANE=/tmp/mylane SITES=apps.tsv AT=live ./run.sh q5  # the live app-page census
#
# WHERE THE BYTES COME FROM IS A PARAMETER, AND THERE IS ONE DRIVER. `AT=frozen` (the default) serves each
# site from the mirror through serve-faithful; `AT=live` drives the row's own URL over the internet. They are
# the SAME loop because everything that makes a row trustworthy — the lane, the virgin browser, the proof
# that the browser is ours, the pass-qualified transcript — is identical in both and belongs to neither. A
# SECOND SCRIPT FOR THE LIVE CASE IS WHAT THIS REPLACES, and the two copies had already drifted: the /tmp one
# had `--noproxy 127.0.0.1` on its identity check and this one did not, so on a box exporting `http_proxy`
# they were asking two different questions and only one of them was about our browser.
#
# AND WHAT `AT=live` MEASURES IS NOT WHAT `AT=frozen` MEASURES. CLAUDE.md §Testing: a before/after belongs on
# frozen bytes, where the only thing that changed is the engine; live sites are for DISCOVERING signatures.
# A live pass is therefore a signature hunt whose endpoint counts are noise on any site that aborts, and the
# report says so — it is not a cheaper frozen pass.
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
SITES=${SITES:-sites.tsv}
case "$SITES" in /*) ;; *) SITES=$CORP/$SITES;; esac
[ -f "$SITES" ] || { echo "no site list at $SITES"; exit 2; }
# WHERE THE BYTES COME FROM. `frozen` serves the mirror; `live` drives the row's own URL. Anything else is a
# typo and is fatal rather than silently taken as one of them -- a census that measured the other corpus
# under this one's label is a row no counter in the output could contradict.
AT=${AT:-frozen}
case "$AT" in frozen|live) ;; *) echo "AT must be frozen or live, not \`$AT\`"; exit 2;; esac
PORT=${HARNESS_PORT:-9451}
FIXPORT=${FIXPORT:-8951}
export NODE_PATH=${NODE_PATH:-/home/user/APIClient/node_modules}
export HARNESS_PROFILE=$LANE/prof HARNESS_LOCK=$LANE/harness.lock HARNESS_PORT=$PORT
export HARNESS_EXT_DIR=$LANE/extension CDP=$PORT DWELL=${DWELL:-60000}
OUT=$CORP/census-$LABEL.jsonl
mkdir -p "$CORP/logs"
: > "$OUT"
echo "artifact $(sha256sum "$LANE/extension/lib/qjs/qjs.wasm" | cut -c1-64)"
echo "list $SITES   at $AT"
echo "load at start: $(cut -d' ' -f1-3 /proc/loadavg)"

while IFS=$'\t' read -r id url stack; do
  [ -z "$id" ] && continue
  case "$id" in \#*) continue;; esac
  [ -n "$ONLY" ] && [ "$ONLY" != "$id" ] && continue
  echo "=== $(date -u +%H:%M:%S) $id   load $(cut -d' ' -f1 /proc/loadavg)"
  SRV=
  TARGET=$url
  if [ "$AT" = frozen ]; then
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
    TARGET=http://127.0.0.1:$FIXPORT/
  fi
  ( cd "$LANE" && timeout 240 node testing/harness.js restart "$PORT" ) >"$CORP/logs/$id.restart" 2>&1
  # CONFIRM THE BROWSER IS OURS BEFORE DRIVING IT. `restart` can report "started" while its Chrome is
  # already gone, and one lane then silently drove another agent's browser and lost a whole pass.
  MYID=$(node -e "const c=require('crypto');const h=c.createHash('sha256').update(Buffer.from(process.argv[1],'utf8')).digest('hex').slice(0,32);let i='';for(const x of h)i+=String.fromCharCode(97+parseInt(x,16));console.log(i)" "$LANE/extension")
  # POLLED, NOT ASKED ONCE, AND `--noproxy` BECAUSE THE ANSWER MUST COME FROM *THIS* BOX. `restart` prints
  # "started" when it has spawned Chrome, which is BEFORE the DevTools HTTP endpoint answers -- so a single
  # curl loses a race it was never meant to be in, and the row it kills is written into the census as a fatal
  # measurement rather than the flake it is. Measured: a control run that had ALREADY LAUNCHED (the restart
  # log's own last line named our extension id) was refused by this check one line later. The identity is
  # still the gate; only the number of chances it gets to answer changed. `--noproxy 127.0.0.1` because a box
  # exporting `http_proxy` would otherwise ask a proxy about our loopback browser.
  OURS=
  for _ in $(seq 1 20); do
    curl -s --noproxy 127.0.0.1 --max-time 5 "http://127.0.0.1:$PORT/json/list" | grep -q "$MYID" && { OURS=1; break; }
    sleep 0.5
  done
  if [ -z "$OURS" ]; then
    echo "{\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"port $PORT is not serving our extension $MYID\"}" >> "$OUT"
    [ -n "$SRV" ] && { kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; }
    continue
  fi
  # THE PASS LABEL GOES TO THE DRIVER, so its transcript is logs/<label>-<id>.log rather than one path per
  # site that the next pass overwrites. Without it every pass's rows are read against the LAST pass's console
  # and a site that ran cleanly in one pass inherits another pass's abort. report.mjs reads the name off the
  # row, so passing it here is what makes a multi-pass census one measurement per (site, pass).
  R=$(cd "$CORP" && timeout 360 node site.mjs "$id" "$TARGET" "$LABEL" 2>&1 | grep '^ROW ' | head -1)
  [ -z "$R" ] && R="ROW {\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"driver produced no row\"}"
  echo "${R#ROW }" >> "$OUT"
  [ -n "$SRV" ] && { kill -TERM $SRV 2>/dev/null; wait $SRV 2>/dev/null; }
done < "$SITES"

# TEAR DOWN BY THE PID IN *OUR* LOCK, never by a pattern. `pkill -f testing/harness.js` matches every lane's
# harness on a shared checkout, which is the exact way one agent has already killed another's browser.
MYPID=$(node -e "try{console.log(JSON.parse(require('fs').readFileSync(process.argv[1],'utf8')).pid)}catch{}" "$HARNESS_LOCK" 2>/dev/null)
[ -n "${MYPID:-}" ] && kill "$MYPID" 2>/dev/null
echo "DONE -> $OUT"
