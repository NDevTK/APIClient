#!/usr/bin/env bash
# Decisive test: gate GC off during a logged COW drive (g_grind_drive_active).
# Hypothesis: GC-during-drive frees -> discard reverts them -> free-list OOB traps.
# Criterion: thr -> 0 across fixtures (was branchless 4, body_async 6, ...) WHILE
# eps coverage held (branchless 3 incl branch_a, ce_lifecycle 12, body_async 7).
set -u
PORT="${PORT:-8765}"
BASE="http://localhost:${PORT}"
H="node testing/harness.js"

# fixtures server (start only if down)
if ! curl -s -o /dev/null "${BASE}/branchless_cold.html"; then
  node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 &
  sleep 2
fi

node engine/build.mjs cow > engine/.work/build-gcgate2.log 2>&1
BUILD=$?
CERR=$(grep -ciE "error:" engine/.work/build-gcgate2.log)
echo "BUILD=$BUILD cerr=$CERR"
if [ "$BUILD" != "0" ]; then echo "BUILD FAILED"; tail -40 engine/.work/build-gcgate2.log; exit 1; fi

for fx in branchless_cold body_async_shape cold_transport ce_lifecycle react_effect chain_nested; do
  $H restart >/dev/null 2>&1
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
  for i in $(seq 1 18); do
    st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"')
    case "$st" in *complete*|*stalled*) break;; esac
    sleep 3
  done
  stats=$($H worker "var r=self._whyRecords||[],t=0,c=0,s=0;for(const x of r){if(!x)continue;if(x.phase==='deep_callmain_throw')t++;else if(x.phase==='deep_recycle')c++;else if(x.phase&&(''+x.phase).indexOf('saturat')>=0)s++;}return JSON.stringify({thr:t,rec:c,sat:s})" 2>&1 | head -1)
  eps=$($H offscreen "var a=[];for(const x of globalStore.endpoints.values())a.push((x.method||'GET')+(x.path||x.url||'?'));return a.length+':'+JSON.stringify(a)" 2>&1 | head -1)
  printf "%-18s %s\n    eps=%s\n" "$fx" "$stats" "$eps"
done
echo "DONE"
