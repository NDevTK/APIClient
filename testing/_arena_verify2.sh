#!/usr/bin/env bash
# Verify the dlmalloc FREEZE (arena allocs + deferred frees + arena-copy reallocs). branchless_cold
# driving is flaky (some runs drive no orphan -> driveNmalloc:0), so retry until it actually drives
# (driveNmalloc>0), then read the trap. Signals: deferTotal>0 (frees deferred), arenaTotalKB>0
# (allocs routed), thr=0 (fork-snapshot fixed). sentry_cdn = real-bundle no-regression guard.
set -u
cd /d/APIClient || exit 1
BASE="http://localhost:8765"; H="node testing/harness.js"
curl -s -o /dev/null "${BASE}/sentry_cdn.html" || { node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; }
read_stats() {
  $H worker "var r=self._whyRecords||[];var c=r.filter(function(x){return x&&x.phase==='cow_stats';});var t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;var last=c.length?c[c.length-1]:{};return JSON.stringify({thr:t,tN:last.totNmalloc,dN:last.driveNmalloc,aKB:last.arenaTotalKB,dfT:last.deferTotal,cBH:last.cBelowHeap,nStats:c.length})"
}
for fx in branchless_cold sentry_cdn; do
  for attempt in 1 2 3 4; do
    $H restart >/dev/null 2>&1
    $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
    st=""
    for i in $(seq 1 34); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
    s=$(read_stats 2>&1 | head -1)
    echo "$fx attempt=$attempt $s state=$st"
    if [ "$fx" = "branchless_cold" ]; then case "$s" in *'"dN":0'*|*'"dN":null'*) continue;; esac; fi
    break
  done
done
echo DONE
